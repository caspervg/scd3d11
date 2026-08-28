# SCD3D11 - a Direct3D 11 renderer for SimCity 4

SCD3D11 replaces SimCity 4's native DirectX 7 hardware renderer with a Direct3D 11 implementation, to improve
compatibility with modern GPUs, drivers, Windows versions, and high-resolution displays.

It began as [SCGL](https://github.com/nsgomez/scgl) by Nelson Gomez, an OpenGL driver for the same interface, and was
renamed once the backend became Direct3D 11.

Not affiliated with or endorsed by EA Games.

## Requirements

* SimCity 4 Deluxe Edition 1.1.641 (Windows).
* A GPU supporting Direct3D 11 or better.

## Installing

Copy `SCD3D11.dll` into a SimCity 4 plugins folder — either `Documents\SimCity 4\Plugins` or the `Plugins` folder of
whatever user directory you launch with.

## Building

CMake is the only build. Install [Visual Studio 2022](https://visualstudio.microsoft.com/#vs-section) or later with
the desktop C++ components, then from an **x86** Developer Command Prompt:

```
cmake -S . -B build\debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build\debug
ctest --test-dir build\debug --output-on-failure
```

## Presentation modes

SCD3D11 implements SC4's native windowed and exclusive-fullscreen modes. Add `-Borderless` alongside a fullscreen launch to get a monitor-sized borderless window instead.

## Frame callback API

Other plugins can observe the live D3D11 device, context, swap chain and back-buffer RTV through the exports in
[`scd3d11/SCD3D11Service.h`](scd3d11/SCD3D11Service.h):

```c
BOOL __stdcall SCD3D11RegisterFrameCallback(SCD3D11FrameCallback callback, void* userData);
BOOL __stdcall SCD3D11UnregisterFrameCallback(SCD3D11FrameCallback callback, void* userData);
```

The callback receives an `SCD3D11FrameContext` for either `SCD3D11_EVENT_RENDER` (once per presented frame, with the
immediate context borrowed for the duration of the call) or `SCD3D11_EVENT_BEFORE_DEVICE_DESTROY`. `deviceGeneration`
increments whenever the device is recreated, so stale resources can be detected and dropped.

> **Renamed in this release.** These were `SCGLRegisterD3D11FrameCallback` / `SCGLUnregisterD3D11FrameCallback` in
> `SCGL.dll`. Consumers that resolve the module by filename or the functions by export name must be updated.

## Diagnostics

The driver appends to `SC4D3D11.log` in the game's working directory (normally the `Apps` folder next to
`SimCity 4.exe`), tagged by category: `init`, `caps`, `swapchain`, `resource`, `grid`, `unsupported`. Debug builds also forward D3D11 debug-layer messages.

Set `SC4D3D11_RECORD_STATES=1` before launching to additionally record observed render-state combinations to
`SC4D3D11-states.log`.

## Third-party components

Included in the `vendor` folder:

* [gzcom-dll](https://github.com/nsgomez/gzcom-dll) (LGPLv2.1) — registers the graphics implementation with the game.
* [Scion](https://github.com/nsgomez/scion) (LGPLv2.1) — compatibility with game components.

## License

Licensed under the [GNU Lesser General Public License, version 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html)
or, at your option, any later version published by the Free Software Foundation.

You may dynamically link it with proprietary software such as SimCity 4, but changes you make to SCD3D11 must also be
shared under the LGPLv2.1 or later.
