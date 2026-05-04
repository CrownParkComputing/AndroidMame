# MAME Arcade Android Build

## Build Scripts

| Script | Purpose |
|--------|---------|
| `./build_arcade.sh` | Build native lib (arm64/arm/x86/x64) |
| `./build_apk.sh` | Build APK from native lib |

## Quick Start

```bash
# 1. Build native library (arcade-only, SDL3)
./build_arcade.sh

# 2. Build APK
./build_apk.sh

# 3. Install
adb install android-project/app/build/outputs/apk/debug/app-debug.apk
```

## Requirements

- NDK: `/opt/android-sdk/ndk/28.2.13676358`
- SDL3: `android-project/app/src/main/libs/arm64-v8a/libSDL3.so`
- Arcade filter: `src/mame/arcade.flt` (auto-generated)

## Features

### Arcade Whitelist Filtering
- Uses `arcade_supported_sets.txt` (16,479 games) for accurate filtering
- Blocks fruit machines, poker, slots, casino games automatically
- Only shows games that match MAME's arcade driver classification

### Controller Button Mapping
- **A Button**: P1 Button 1 (action/jump)
- **B Button**: Save State (was SELECT/exit)
- **X Button**: P1 Button 3
- **Y Button**: P1 Button 4
- **L1/R1**: Shoulders
- **L2/R2**: Triggers
- **Start**: Pause/Start game
- **D-Pad**: Movement

### Save States
- Stored at: `/storage/emulated/0/mame/sta/gamename/`
- Press **B** to save to slot 0
- Use in-game UI (TAB menu) to manage slots 0-9
- States persist across sessions

### Notes
- Uses **SDL3** (not SDL2) - `ANDROID_OSD=sdl3`
- 16KB page alignment enforced via `LDOPTS`
- Runtime parity warning from SDL3 compat structs is benign - MAME uses native SDL3 API
