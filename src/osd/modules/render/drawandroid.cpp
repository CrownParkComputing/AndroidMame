// license:BSD-3-Clause

#include "render_module.h"

#include "modules/osdmodule.h"

#if defined(SDLMAME_ANDROID) && defined(OSD_SDL) && !defined(SDLMAME_SDL3)

#include "window.h"

#include "emucore.h"
#include "render.h"

#include <SDL2/SDL_system.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace osd {
namespace {

class renderer_android_native : public osd_renderer
{
public:
	renderer_android_native(osd_window &window) : osd_renderer(window) { }
	~renderer_android_native() override { destroy(); }

	int create() override;
	int draw(int update) override;
	int xy_to_render_target(int x, int y, int *xt, int *yt) override;
	render_primitive_list *get_primitives() override;

private:
	void destroy();
	bool init_egl();
	bool init_gl();
	bool ensure_texture(render_texinfo const &tex);
	void upload_texture(render_primitive const &prim);
	void draw_primitive(render_primitive const &prim);
	ANativeWindow *get_android_window();

	static GLuint compile_shader(GLenum type, char const *source);
	static uint32_t rgb15_to_rgba(uint16_t src);
	static uint32_t rgb_to_rgba(uint32_t src, uint8_t alpha);
	static uint16_t rgb_to_rgb565(uint32_t src);

	EGLDisplay m_display = EGL_NO_DISPLAY;
	EGLSurface m_surface = EGL_NO_SURFACE;
	EGLContext m_context = EGL_NO_CONTEXT;
	ANativeWindow *m_native_window = nullptr;

	GLuint m_program = 0;
	GLuint m_texture = 0;
	GLint m_pos_attr = -1;
	GLint m_tex_attr = -1;
	GLint m_sampler_uniform = -1;

	osd_dim m_blit_dim = osd_dim(0, 0);
	int m_tex_width = 0;
	int m_tex_height = 0;
	int m_tex_seqid = -1;
	bool m_texture_rgb565 = false;
	std::vector<uint32_t> m_rgba;
	std::vector<uint16_t> m_rgb565;
};

GLuint renderer_android_native::compile_shader(GLenum type, char const *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

uint32_t renderer_android_native::rgb_to_rgba(uint32_t src, uint8_t alpha)
{
	return ((src >> 16) & 0xff) | (src & 0x00ff00) | ((src & 0x0000ff) << 16) | (uint32_t(alpha) << 24);
}

uint16_t renderer_android_native::rgb_to_rgb565(uint32_t src)
{
	return uint16_t(((src & 0xf80000) >> 8) | ((src & 0x00fc00) >> 5) | ((src & 0x0000f8) >> 3));
}

uint32_t renderer_android_native::rgb15_to_rgba(uint16_t src)
{
	uint32_t r = (src & 0x7c00) >> 7;
	uint32_t g = (src & 0x03e0) >> 2;
	uint32_t b = (src & 0x001f) << 3;
	r |= r >> 5;
	g |= g >> 5;
	b |= b >> 5;
	return r | (g << 8) | (b << 16) | 0xff000000;
}

ANativeWindow *renderer_android_native::get_android_window()
{
	JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
	if (!env)
		return nullptr;

	jclass cls = env->FindClass("org/libsdl/app/SDLActivity");
	if (!cls)
		return nullptr;

	jmethodID method = env->GetStaticMethodID(cls, "getNativeSurface", "()Landroid/view/Surface;");
	if (!method)
	{
		env->DeleteLocalRef(cls);
		return nullptr;
	}

	jobject surface = env->CallStaticObjectMethod(cls, method);
	env->DeleteLocalRef(cls);
	if (!surface)
		return nullptr;

	ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
	env->DeleteLocalRef(surface);
	return window;
}

bool renderer_android_native::init_egl()
{
	m_native_window = get_android_window();
	if (!m_native_window)
		return false;

	m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (m_display == EGL_NO_DISPLAY || !eglInitialize(m_display, nullptr, nullptr))
		return false;

	EGLint config_attribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};

	EGLConfig config;
	EGLint count = 0;
	if (!eglChooseConfig(m_display, config_attribs, &config, 1, &count) || !count)
		return false;

	EGLint format = 0;
	eglGetConfigAttrib(m_display, config, EGL_NATIVE_VISUAL_ID, &format);
	ANativeWindow_setBuffersGeometry(m_native_window, 0, 0, format);

	EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, context_attribs);
	m_surface = eglCreateWindowSurface(m_display, config, m_native_window, nullptr);
	if (m_context == EGL_NO_CONTEXT || m_surface == EGL_NO_SURFACE)
		return false;

	if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context))
		return false;

	eglSwapInterval(m_display, 0);
	return true;
}

bool renderer_android_native::init_gl()
{
	static char const *const vertex_shader =
			"attribute vec2 aPos;\n"
			"attribute vec2 aTex;\n"
			"varying vec2 vTex;\n"
			"void main() { vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
	static char const *const fragment_shader =
			"precision mediump float;\n"
			"varying vec2 vTex;\n"
			"uniform sampler2D uTex;\n"
			"void main() { gl_FragColor = texture2D(uTex, vTex); }\n";

	GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader);
	if (!vs || !fs)
		return false;

	m_program = glCreateProgram();
	glAttachShader(m_program, vs);
	glAttachShader(m_program, fs);
	glLinkProgram(m_program);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint linked = GL_FALSE;
	glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
	if (!linked)
		return false;

	m_pos_attr = glGetAttribLocation(m_program, "aPos");
	m_tex_attr = glGetAttribLocation(m_program, "aTex");
	m_sampler_uniform = glGetUniformLocation(m_program, "uTex");

	glGenTextures(1, &m_texture);
	glBindTexture(GL_TEXTURE_2D, m_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	return true;
}

int renderer_android_native::create()
{
	if (!init_egl() || !init_gl())
		fatalerror("Unable to create Android native EGL renderer\n");

	osd_printf_verbose("Using Android native EGL/GLES screen renderer\n");
	return 0;
}

void renderer_android_native::destroy()
{
	if (m_display != EGL_NO_DISPLAY)
		eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (m_texture)
		glDeleteTextures(1, &m_texture);
	if (m_program)
		glDeleteProgram(m_program);
	if (m_display != EGL_NO_DISPLAY && m_context != EGL_NO_CONTEXT)
		eglDestroyContext(m_display, m_context);
	if (m_display != EGL_NO_DISPLAY && m_surface != EGL_NO_SURFACE)
		eglDestroySurface(m_display, m_surface);
	if (m_display != EGL_NO_DISPLAY)
		eglTerminate(m_display);
	if (m_native_window)
		ANativeWindow_release(m_native_window);
}

bool renderer_android_native::ensure_texture(render_texinfo const &tex)
{
	bool const use_rgb565 = true;
	if (tex.width == m_tex_width && tex.height == m_tex_height && use_rgb565 == m_texture_rgb565)
		return true;

	m_tex_width = tex.width;
	m_tex_height = tex.height;
	m_tex_seqid = -1;
	m_texture_rgb565 = use_rgb565;
	if (m_texture_rgb565)
		m_rgb565.resize(size_t(m_tex_width) * size_t(m_tex_height));
	else
		m_rgba.resize(size_t(m_tex_width) * size_t(m_tex_height));
	glBindTexture(GL_TEXTURE_2D, m_texture);
	if (m_texture_rgb565)
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_tex_width, m_tex_height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, nullptr);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_tex_width, m_tex_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	return true;
}

void renderer_android_native::upload_texture(render_primitive const &prim)
{
	render_texinfo const &tex = prim.texture;
	if (!tex.base || !ensure_texture(tex) || m_tex_seqid == tex.seqid)
		return;

	uint32_t const fmt = PRIMFLAG_GET_TEXFORMAT(prim.flags);
	for (int y = 0; y < tex.height; y++)
	{
		if (m_texture_rgb565)
		{
			uint16_t *dst = &m_rgb565[size_t(y) * size_t(tex.width)];
			if (fmt == TEXFORMAT_ARGB32 || fmt == TEXFORMAT_RGB32)
			{
				uint32_t const *src = static_cast<uint32_t const *>(tex.base) + (size_t(y) * tex.rowpixels);
				for (int x = 0; x < tex.width; x++)
					dst[x] = rgb_to_rgb565(src[x]);
			}
			else
			{
				uint16_t const *src = static_cast<uint16_t const *>(tex.base) + (size_t(y) * tex.rowpixels);
				for (int x = 0; x < tex.width; x++)
				{
					if (fmt == TEXFORMAT_PALETTE16 && tex.palette)
						dst[x] = rgb_to_rgb565(tex.palette[src[x]]);
					else
					{
						uint16_t const p = src[x];
						dst[x] = uint16_t(((p & 0x7c00) << 1) | ((p & 0x03e0) << 1) | ((p & 0x03e0) >> 4) | (p & 0x001f));
					}
				}
			}
		}
		else if (fmt == TEXFORMAT_ARGB32 || fmt == TEXFORMAT_RGB32)
		{
			uint32_t *dst = &m_rgba[size_t(y) * size_t(tex.width)];
			uint32_t const *src = static_cast<uint32_t const *>(tex.base) + (size_t(y) * tex.rowpixels);
			for (int x = 0; x < tex.width; x++)
				dst[x] = rgb_to_rgba(src[x], fmt == TEXFORMAT_ARGB32 ? uint8_t(src[x] >> 24) : 0xff);
		}
		else
		{
			uint32_t *dst = &m_rgba[size_t(y) * size_t(tex.width)];
			uint16_t const *src = static_cast<uint16_t const *>(tex.base) + (size_t(y) * tex.rowpixels);
			for (int x = 0; x < tex.width; x++)
				dst[x] = fmt == TEXFORMAT_PALETTE16 && tex.palette ? rgb_to_rgba(tex.palette[src[x]], 0xff) : rgb15_to_rgba(src[x]);
		}
	}

	glBindTexture(GL_TEXTURE_2D, m_texture);
	if (m_texture_rgb565)
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex.width, tex.height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, m_rgb565.data());
	else
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex.width, tex.height, GL_RGBA, GL_UNSIGNED_BYTE, m_rgba.data());
	m_tex_seqid = tex.seqid;
}

void renderer_android_native::draw_primitive(render_primitive const &prim)
{
	upload_texture(prim);

	float const w = std::max(1, m_blit_dim.width());
	float const h = std::max(1, m_blit_dim.height());
	float const x0 = (prim.bounds.x0 / w) * 2.0f - 1.0f;
	float const y0 = 1.0f - (prim.bounds.y0 / h) * 2.0f;
	float const x1 = (prim.bounds.x1 / w) * 2.0f - 1.0f;
	float const y1 = 1.0f - (prim.bounds.y1 / h) * 2.0f;

	float const vertices[] = {
		x0, y0, prim.texcoords.tl.u, prim.texcoords.tl.v,
		x1, y0, prim.texcoords.tr.u, prim.texcoords.tr.v,
		x0, y1, prim.texcoords.bl.u, prim.texcoords.bl.v,
		x1, y1, prim.texcoords.br.u, prim.texcoords.br.v
	};

	glUseProgram(m_program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_texture);
	glUniform1i(m_sampler_uniform, 0);
	glVertexAttribPointer(m_pos_attr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices);
	glEnableVertexAttribArray(m_pos_attr);
	glVertexAttribPointer(m_tex_attr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices + 2);
	glEnableVertexAttribArray(m_tex_attr);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

int renderer_android_native::draw(int update)
{
	if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context))
		return 1;

	osd_dim const dim = window().get_size();
	if (dim != m_blit_dim)
	{
		m_blit_dim = dim;
		notify_changed();
	}

	glViewport(0, 0, m_blit_dim.width(), m_blit_dim.height());
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	window().m_primlist->acquire_lock();
	for (render_primitive const &prim : *window().m_primlist)
	{
		if (prim.type == render_primitive::QUAD && prim.texture.base && PRIMFLAG_GET_SCREENTEX(prim.flags))
			draw_primitive(prim);
	}
	window().m_primlist->release_lock();

	eglSwapBuffers(m_display, m_surface);
	return 0;
}

int renderer_android_native::xy_to_render_target(int x, int y, int *xt, int *yt)
{
	*xt = x;
	*yt = y;
	return x >= 0 && y >= 0 && x < m_blit_dim.width() && y < m_blit_dim.height();
}

render_primitive_list *renderer_android_native::get_primitives()
{
	osd_dim const dim = window().get_size();
	if (dim != m_blit_dim)
	{
		m_blit_dim = dim;
		notify_changed();
	}
	window().target()->set_bounds(m_blit_dim.width(), m_blit_dim.height(), window().pixel_aspect());
	return &window().target()->get_primitives();
}

class video_android_native : public osd_module, public render_module
{
public:
	video_android_native() : osd_module(OSD_RENDERER_PROVIDER, "androidnative") { }

	int init(osd_interface &osd, osd_options const &options) override
	{
		osd_printf_verbose("Using Android native renderer module\n");
		return 0;
	}

	std::unique_ptr<osd_renderer> create(osd_window &window) override
	{
		return std::make_unique<renderer_android_native>(window);
	}

protected:
	unsigned flags() const override { return FLAG_INTERACTIVE; }
};

} // anonymous namespace
} // namespace osd

#else

namespace osd { namespace { MODULE_NOT_SUPPORTED(video_android_native, OSD_RENDERER_PROVIDER, "androidnative") } }

#endif

MODULE_DEFINITION(RENDERER_ANDROID_NATIVE, osd::video_android_native)
