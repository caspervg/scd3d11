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
| `-GPU:default` | high-performance GPU | Use the adapter Windows picks instead of the high-performance one |
| `-ParallelCull:<mode>` | `parallel` | `parallel`, `serial`, `off` (also `0`, `false`), or diagnostics `passthru` / `tailonly`; see [Parallel render cull](#parallel-render-cull). The `SC4D3D11_PARALLEL_CULL` environment variable is used when the switch is absent |
| `-SimTickCap:<ms>` | `32` | Per-tick simulation budget, clamped to `15`–`500`; `off` or `0` disables; see [Sim tick budget](#sim-tick-budget) |
| `-GridDebug` | off | Log the texture state of the terrain grid pass once per second (`grid` category) |
| `-ReShade:off` | integration on | Leave ReShade at its default behaviour (effects over the whole frame, UI included); see [ReShade](#reshade) |

Standard SimCity 4 switches that SCD3D11 reacts to or that the scripts in `scripts/` use:

| Switch | Effect |
|--------|--------|
| `-w` / `-f` | Windowed / fullscreen |
| `-CustomResolution:enabled` `-r<width>x<height>x32` | Render at an arbitrary resolution |
| `-UserDir:"<path>"` | User directory; with `SCD3D11.dll` in its `Plugins` folder, `SC4D3D11.log` is written there |
| `-CPUPriority:<level>` | Process priority. When present, SCD3D11 leaves priority alone instead of raising it to high |
| `-CPUCount:<n>` | Limits the cores the game uses. With `1`, the parallel cull starts no workers if the game applies the limit before SCD3D11 loads (check `workers=` in the log) |

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

## CPU scheduling

At startup SCD3D11 raises SimCity 4 to high priority (skipped when `-CPUPriority:` is on the command line), opts the
process out of Windows power throttling (EcoQoS), and on hybrid Intel CPUs keeps its threads on the performance cores.
Check `SC4D3D11.log` for the `cpu:` lines.

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
core. The pool is sized to the cores the process may use, so when the `SC4CPUOptions`
plugin (or `-CPUCount:1`) pins SimCity 4 to a single core, no workers start and the
gather stays on the render thread — remove that plugin, or configure it to allow
multiple cores, if you want the parallel path to do anything.

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
`Documents\SimCity 4`), tagged by category: `init`, `caps`, `swapchain`, `resource`, `grid`, `unsupported`. The log is
recreated each session. Debug builds also forward D3D11 debug-layer messages and record each newly observed
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
