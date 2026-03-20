# Cemu Libretro

Cemu (Wii U emulator) as a libretro core for RetroArch.

## Build

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install -y cmake gcc g++ ninja-build nasm libpulse-dev libgtk-3-dev \
  libsecret-1-dev libgcrypt20-dev libsystemd-dev libbluetooth-dev freeglut3-dev

# Clone with submodules
git clone --recursive https://github.com/WizzardSK/cemu-libretro.git
cd cemu-libretro

# Configure (vcpkg handles dependencies automatically)
cmake -S . -B build -DCMAKE_BUILD_TYPE=release \
  -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -G Ninja -DENABLE_LIBRETRO=ON -DENABLE_WXWIDGETS=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_CUBEB=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON

# Build
cmake --build build --target cemu_libretro

# Result: bin/cemu_libretro.so
```

## Install

```bash
cp bin/cemu_libretro.so ~/.config/retroarch/cores/libcemu_libretro.so
cp cemu_libretro.info ~/.config/retroarch/cores/libcemu_libretro.info
```

## Setup

- Place `keys.txt` in RetroArch system directory under `Cemu/` (e.g. `~/.config/retroarch/system/Cemu/keys.txt`)
- MLC storage is at `<system_dir>/Cemu/mlc01/`
- Shared fonts: place `CafeStd.ttf`, `CafeCn.ttf`, `CafeKr.ttf`, `CafeTw.ttf` in `<system_dir>/Cemu/resources/sharedFonts/`
- Graphic packs: place in `<system_dir>/Cemu/graphicPacks/` (symlinks supported)
  - Packs with `default = 1` are auto-enabled (e.g. NSMBU crash fix)
- Supported formats: `.wud`, `.wux`, `.wua`, `.rpx`, `.elf`

## Architecture

- Uses OpenGL 4.5 Core Profile HW rendering via `RETRO_ENVIRONMENT_SET_HW_RENDER`
- Creates a separate shared GLX context for Cemu's GPU thread
- Video pipeline: GPU thread renders to custom FBO -> `glReadPixels` to CPU buffer -> frontend thread uploads to texture -> `glBlitFramebuffer` to RetroArch FBO
- GL_QUADS/GL_QUAD_STRIP converted to triangles for Core Profile compatibility (indexed quads use `glMapBuffer`)
- Audio routed through `LibretroAudioAPI` (accumulates samples, flushed each frame)
- Input: libretro joypad -> VPAD (GamePad) buttons + analog sticks + touchscreen (RETRO_DEVICE_POINTER)
- Graphic packs loaded and auto-enabled (default=1) at startup

## Current Status (2026-03-18)

### Working
- Core loads in RetroArch, games boot and run with full rendering
- OpenGL 4.5 Core Profile with shared GLX context for GPU thread
- Video output with correct orientation, survives fullscreen/windowed toggles
- GL_QUADS -> GL_TRIANGLES conversion with proper index buffer mapping
- Input: joypad buttons, analog sticks, touchscreen (mouse -> GamePad touch)
- Audio routing via LibretroAudioAPI
- Graphic packs loading with workaround patches (NSMBU crash fix etc.)
- Shared fonts for Japanese/CJK text
- Clean exit via Esc key

### Known Issues
- **Audio crackling** - audio sync between Cemu and RetroArch needs improvement
- **DRC (GamePad) screen** - currently shows TV output only
- **Some games may crash** - depends on game complexity and required HLE functions

### TODO
1. Fix audio synchronization/crackling
2. Add core option to switch between TV and GamePad (DRC) display
3. Implement core options (CPU mode, resolution, etc.)
4. Save states support
5. Test with more games

### Key files
- `src/libretro/libretro_core.cpp` - Main libretro core (GL context, video, input, game loading)
- `src/libretro/LibretroAudioAPI.h/cpp` - Audio backend
- `src/libretro/LibretroWindowSystem.cpp` - WindowSystem without wxWidgets
- `src/Cafe/HW/Latte/Renderer/OpenGL/OpenGLRendererCore.cpp` - GL_QUADS to triangles conversion
- `src/Cafe/OS/libs/vpad/vpad.cpp` - Libretro input integration (buttons + touch)
- `src/Cafe/GraphicPack/GraphicPack2.cpp` - Symlink-aware graphic pack loading
