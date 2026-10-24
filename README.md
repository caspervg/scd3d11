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

## Command-line switches

Add these to the SimCity 4 launch arguments. Switch names are case-sensitive except `-Borderless`, so type them as shown.

Added by SCD3D11:

| Switch | Default | Effect |
|--------|---------|--------|
| `-Borderless` (or `-FullscreenMode:Borderless`) | off | With `-f`, a monitor-sized borderless window instead of exclusive fullscreen |
| `-VSync:off` | vsync on | Present without waiting for vsync |
| `-FlipModel:off` | flip model | Windowed and borderless: present with the legacy DISCARD swap chain instead of the flip model |
| `-GPU:default` | high-performance GPU | Use the adapter Windows picks instead of the high-performance one |
| `-ViewportCull:off` | cull on | Submit every draw of a scroll redraw instead of dropping those that lie entirely outside its small scissored viewport |
| `-DrawStats` | off | Log a per-second summary of frame times, draws, culled draws, scroll and full redraws and texture churn, plus a line for each frame over 100 ms (`stats` category) |
| `-GridDebug` | off | Log the texture state of the terrain grid pass once per second (`grid` category) |
| `-LogLevel:trace` | off | Also log high-volume events: geometry cache segment recycling, capture changes, window z-order moves |
| `-ReShade:off` | integration on | Leave ReShade at its default behaviour (effects over the whole frame, UI included); see [ReShade](#reshade) |

Standard SimCity 4 switches that SCD3D11 reacts to or that the scripts in `scripts/` use:

| Switch | Effect |
|--------|--------|
| `-w` / `-f` | Windowed / fullscreen |
| `-CustomResolution:enabled` `-r<width>x<height>x32` | Render at an arbitrary resolution |
| `-UserDir:"<path>"` | User directory; with `SCD3D11.dll` in its `Plugins` folder, `SC4D3D11.log` is written there |

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

On systems with both an integrated and a discrete GPU, SCD3D11 renders on the high-performance GPU. Add `-GPU:default` to use the adapter Windows picks instead. `SC4D3D11.log` names the adapter in use on its `adapter:` line.

## CPU options

SCD3D11 no longer touches CPU scheduling, the render cull or the simulation tick. Process priority, P-core/E-core
scheduling, power throttling, the parallel render cull and the sim tick budget are handled by the separate
[SC4 CPU Options](https://github.com/0xC0000054/sc4-cpu-options) plugin.

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

## ReShade

Install [ReShade](https://reshade.me) 6.0 or later for `SimCity 4.exe` and pick the **Direct3D 10/11/12** API. The
regular build is enough; the "full add-on support" build is not needed. SCD3D11 registers itself with ReShade as an
add-on and:

- renders the effects as soon as the city view is drawn, so the UI stays sharp and untouched;
- supplies the city's depth buffer, so depth effects (ambient occlusion, depth of field, fog) work without setting
  anything up. SC4's camera is orthographic, and the depth is encoded to match whatever
  `RESHADE_DEPTH_LINEARIZATION_FAR_PLANE` is set to. Leave the other `RESHADE_DEPTH_INPUT_*` definitions at `0`.

In the ReShade overlay's Add-ons tab, disable **Generic Depth**; SCD3D11 overrides its choice anyway. Outside the city
view (menus, region view) ReShade behaves as usual, except that effects can stay off in the region view after leaving
a city. Requires SimCity 4 1.1.641. Check `SC4D3D11.log` for
`reshade: add-on registered`; `-ReShade:off` turns the integration off.

## Diagnostics

The driver writes `SC4D3D11.log` to the parent of the plugins folder holding `SCD3D11.dll` (normally
`Documents\SimCity 4`), tagged by category: `init`, `caps`, `swapchain`, `resource`, `grid`, `unsupported`, `window`. The log is
recreated each session.

`window` records focus, activation, size, visibility and system-command messages on the game window (Alt+Tab and the
Windows key included), and `swapchain` records changes in the Present result, paused presentation and the exclusive
fullscreen state. A watchdog logs `watchdog: no frame for N ms` with the render thread's current phase, instruction
pointer and likely return addresses when no frame completes for 5 seconds once rendering is under way (the startup
plugin load is ignored), and `watchdog: frames resumed` afterwards. Debug builds also forward D3D11 debug-layer messages and record each newly observed
render-state, vertex-format and texture-format combination, tagged `state`.

## Third-party components

Included in the `vendor` folder:

* [gzcom-dll](https://github.com/nsgomez/gzcom-dll) (LGPLv2.1) — registers the graphics implementation with the game.
* [Scion](https://github.com/nsgomez/scion) (LGPLv2.1) — compatibility with game components.
* [xxHash](https://github.com/Cyan4973/xxHash) (BSD 2-Clause) — fingerprints geometry for the upload cache.

## License

Licensed under the [GNU Lesser General Public License, version 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html)
or, at your option, any later version published by the Free Software Foundation.

You may dynamically link it with proprietary software such as SimCity 4, but changes you make to SCD3D11 must also be
shared under the LGPLv2.1 or later.
