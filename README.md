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

SCD3D11 implements SC4's native windowed and exclusive-fullscreen modes. Add `-Borderless` alongside a fullscreen
launch to get a borderless window instead.

Both fullscreen modes keep the client area at exactly the selected resolution, so mouse coordinates line up with the
game's UI. Pick your desktop resolution to fill the screen in borderless mode; a smaller resolution is centred on the
monitor rather than stretched.

Add `-Monitor:<n>` to put either fullscreen mode on a specific display, numbered from 1 in the order Windows enumerates
them. The default (`0`) uses the primary monitor.

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

## Parallel render cull

SCD3D11 hooks SimCity 4's 3D-view redraw (`cSC43DRender::DrawStaticView` /
`DrawDynamicView`) and runs the quad-grid visibility gather + sort-key computation
across a worker pool instead of on the render thread. Sorting and draw submission
are unchanged, so the rendered image is identical to the stock path. It engages
only on large scenes and falls back to the stock code on anything unexpected.

Requires SimCity 4 1.1.641. Control it with the `-ParallelCull:<mode>` command
line argument (`parallel` is the default):

| Mode | Effect |
|------|--------|
| `parallel` | gather on the worker pool for large scenes, render thread otherwise |
| `serial` | reimplemented gather, always on the render thread |
| `off` (also `0`, `false`) | do not hook; stock code runs untouched |
| `passthru`, `tailonly` | hook diagnostics |

The worker pool only helps if the game process is allowed to run on more than one
core. The `SC4CPUOptions` plugin pins SimCity 4 to a single core by default, which
leaves the workers time-slicing the render thread's core and cancels any benefit —
remove that plugin, or configure it to allow multiple cores, if you want the
parallel path to do anything.

To confirm what engaged, check `SC4D3D11.log` (see Diagnostics below) for a line
like `parallel cull installed: mode=parallel workers=7 static=ok dynamic=ok`.

## Sim tick budget

`cSC4Simulator::OnTick` busy-loops the simulation for up to a speed-dependent
budget (33/50/66 ms) before returning to the frame loop, so a heavy sim on the
faster speeds starves rendering and input. SCD3D11 clamps that per-tick budget to
a fixed ceiling — the sim still advances every frame, just less per frame, and the
frame loop presents in between. On a large city the sim clock can lag wall-clock a
little more in exchange for a steady frame rate.

Requires SimCity 4 1.1.641. Tune with `-SimTickCap:<ms>` (default `32`; values are
clamped to `15`–`500`; `-SimTickCap:off` or `-SimTickCap:0` disables the patch).
Lower values favour frame rate over sim speed.

To confirm it engaged, check `SC4D3D11.log` for `sim tick budget installed: cap=32 ms`.

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
