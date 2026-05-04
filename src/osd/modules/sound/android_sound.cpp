// license:BSD-3-Clause

#include "sound_module.h"

#include "modules/osdmodule.h"
#include "osdcore.h"

#if defined(SDLMAME_ANDROID)

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace osd {
namespace {

class sound_android : public osd_module, public sound_module
{
public:
	sound_android() : osd_module(OSD_SOUND_PROVIDER, "androidaudio"), sound_module() { }

	int init(osd_interface &osd, osd_options const &options) override;
	void exit() override;

	bool external_per_channel_volume() override { return false; }
	bool split_streams_per_source() override { return false; }

	uint32_t get_generation() override { return 1; }
	osd::audio_info get_information() override;
	uint32_t stream_sink_open(uint32_t node, std::string name, uint32_t rate) override;
	void stream_close(uint32_t id) override;
	void stream_sink_update(uint32_t id, int16_t const *buffer, int samples_this_frame) override;

private:
	struct stream_info
	{
		stream_info(uint32_t id, uint32_t channels, uint32_t rate, uint32_t frames)
			: m_id(id)
			, m_buffer(channels, rate)
			, m_frames(frames)
			, m_mix(frames * channels * 4)
		{
		}

		uint32_t m_id;
		abuffer m_buffer;
		uint32_t m_frames;
		std::vector<int16_t> m_mix;
		uint32_t m_mix_index = 0;
		std::mutex m_mutex;

		SLObjectItf m_player = nullptr;
		SLPlayItf m_play = nullptr;
		SLAndroidSimpleBufferQueueItf m_queue = nullptr;
	};

	static void queue_callback(SLAndroidSimpleBufferQueueItf queue, void *context);
	static void enqueue_buffer(stream_info &stream);

	SLObjectItf m_engine_object = nullptr;
	SLEngineItf m_engine = nullptr;
	SLObjectItf m_output_mix = nullptr;
	uint32_t m_next_id = 1;
	std::map<uint32_t, std::unique_ptr<stream_info>> m_streams;
};

int sound_android::init(osd_interface &osd, osd_options const &options)
{
	SLresult result = slCreateEngine(&m_engine_object, 0, nullptr, 0, nullptr, nullptr);
	if (result != SL_RESULT_SUCCESS)
		return -1;

	result = (*m_engine_object)->Realize(m_engine_object, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS)
		return -1;

	result = (*m_engine_object)->GetInterface(m_engine_object, SL_IID_ENGINE, &m_engine);
	if (result != SL_RESULT_SUCCESS)
		return -1;

	result = (*m_engine)->CreateOutputMix(m_engine, &m_output_mix, 0, nullptr, nullptr);
	if (result != SL_RESULT_SUCCESS)
		return -1;

	result = (*m_output_mix)->Realize(m_output_mix, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS)
		return -1;

	osd_printf_verbose("Audio: using Android native OpenSL ES backend\n");
	return 0;
}

void sound_android::exit()
{
	m_streams.clear();
	if (m_output_mix)
	{
		(*m_output_mix)->Destroy(m_output_mix);
		m_output_mix = nullptr;
	}
	if (m_engine_object)
	{
		(*m_engine_object)->Destroy(m_engine_object);
		m_engine_object = nullptr;
		m_engine = nullptr;
	}
}

osd::audio_info sound_android::get_information()
{
	osd::audio_info result;
	result.m_generation = 1;
	result.m_default_sink = 1;
	result.m_default_source = 0;
	result.m_nodes.resize(1);
	result.m_nodes[0].m_name = "android";
	result.m_nodes[0].m_display_name = "Android native audio";
	result.m_nodes[0].m_id = 1;
	result.m_nodes[0].m_rate = audio_rate_range{ 48000, 8000, 48000 };
	result.m_nodes[0].m_sinks = 2;
	result.m_nodes[0].m_sources = 0;
	result.m_nodes[0].m_port_names.emplace_back("FL");
	result.m_nodes[0].m_port_names.emplace_back("FR");
	result.m_nodes[0].m_port_positions.emplace_back(osd::channel_position::FL());
	result.m_nodes[0].m_port_positions.emplace_back(osd::channel_position::FR());
	return result;
}

uint32_t sound_android::stream_sink_open(uint32_t node, std::string name, uint32_t rate)
{
	if (!m_engine || !m_output_mix || node != 1)
		return 0;

	uint32_t const channels = 2;
	uint32_t const frames = 1024;
	auto stream = std::make_unique<stream_info>(m_next_id++, channels, rate, frames);

	SLDataLocator_AndroidSimpleBufferQueue loc_bufq = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 4 };
	SLDataFormat_PCM format_pcm = {
		SL_DATAFORMAT_PCM,
		channels,
		rate * 1000,
		SL_PCMSAMPLEFORMAT_FIXED_16,
		SL_PCMSAMPLEFORMAT_FIXED_16,
		SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
		SL_BYTEORDER_LITTLEENDIAN
	};
	SLDataSource audio_src = { &loc_bufq, &format_pcm };

	SLDataLocator_OutputMix loc_outmix = { SL_DATALOCATOR_OUTPUTMIX, m_output_mix };
	SLDataSink audio_sink = { &loc_outmix, nullptr };

	SLInterfaceID ids[] = { SL_IID_BUFFERQUEUE };
	SLboolean req[] = { SL_BOOLEAN_TRUE };
	SLresult result = (*m_engine)->CreateAudioPlayer(m_engine, &stream->m_player, &audio_src, &audio_sink, 1, ids, req);
	if (result != SL_RESULT_SUCCESS)
		return 0;

	result = (*stream->m_player)->Realize(stream->m_player, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS)
		return 0;

	result = (*stream->m_player)->GetInterface(stream->m_player, SL_IID_PLAY, &stream->m_play);
	if (result != SL_RESULT_SUCCESS)
		return 0;

	result = (*stream->m_player)->GetInterface(stream->m_player, SL_IID_BUFFERQUEUE, &stream->m_queue);
	if (result != SL_RESULT_SUCCESS)
		return 0;

	(*stream->m_queue)->RegisterCallback(stream->m_queue, queue_callback, stream.get());
	(*stream->m_play)->SetPlayState(stream->m_play, SL_PLAYSTATE_PLAYING);

	uint32_t const id = stream->m_id;
	enqueue_buffer(*stream);
	enqueue_buffer(*stream);
	enqueue_buffer(*stream);
	enqueue_buffer(*stream);
	m_streams[id] = std::move(stream);
	return id;
}

void sound_android::stream_close(uint32_t id)
{
	auto it = m_streams.find(id);
	if (it == m_streams.end())
		return;
	if (it->second->m_player)
		(*it->second->m_player)->Destroy(it->second->m_player);
	m_streams.erase(it);
}

void sound_android::stream_sink_update(uint32_t id, int16_t const *buffer, int samples_this_frame)
{
	auto it = m_streams.find(id);
	if (it == m_streams.end())
		return;
	std::lock_guard<std::mutex> lock(it->second->m_mutex);
	it->second->m_buffer.push(buffer, samples_this_frame);
}

void sound_android::enqueue_buffer(stream_info &stream)
{
	std::lock_guard<std::mutex> lock(stream.m_mutex);
	int16_t *target = &stream.m_mix[size_t(stream.m_mix_index) * stream.m_frames * stream.m_buffer.channels()];
	stream.m_buffer.get(target, stream.m_frames);
	(*stream.m_queue)->Enqueue(
			stream.m_queue,
			target,
			stream.m_frames * stream.m_buffer.channels() * sizeof(int16_t));
	stream.m_mix_index ^= 1;
}

void sound_android::queue_callback(SLAndroidSimpleBufferQueueItf queue, void *context)
{
	auto *stream = reinterpret_cast<stream_info *>(context);
	if (stream)
		enqueue_buffer(*stream);
}

} // anonymous namespace
} // namespace osd

#else

namespace osd { namespace { MODULE_NOT_SUPPORTED(sound_android, OSD_SOUND_PROVIDER, "androidaudio") } }

#endif

MODULE_DEFINITION(SOUND_ANDROID, osd::sound_android)
