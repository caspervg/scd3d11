# SCD3D11 - a Direct3D 11 renderer for SimCity 4

SCD3D11 replaces SimCity 4's native DirectX 7 hardware renderer with a Direct3D 11 implementation, to improve
compatibility with modern GPUs, drivers, Windows versions, and high-resolution displays.

Compared with the game's own renderer, it offers:

* rendering on today's Direct3D 11 drivers instead of the long-deprecated DirectX 7 path
* borderless fullscreen, a vsync switch and flip-model presentation
* rendering on the high-performance GPU of laptops with both an integrated and a discrete GPU
* [ReShade](#reshade) effects that leave the UI untouched and get the city's depth buffer without any setup

It began as [SCGL](https://github.com/nsgomez/scgl) by Nelson Gomez, an OpenGL driver for the same interface, and was
renamed once the backend became Direct3D 11.

Not affiliated with or endorsed by EA Games.

## Requirements

* SimCity 4 Deluxe Edition 1.1.641 (Windows).
* Windows 10 or later.
* A GPU supporting Direct3D 11 or better.
* The [Microsoft Visual C++ 2015–2022 Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe). The
  32-bit version is needed even on 64-bit Windows, because SimCity 4 is a 32-bit game.

## Installing

Download `SCD3D11-<version>.zip` from the [releases page](https://github.com/caspervg/scd3d11/releases) and copy
`SCD3D11.dll` and `SCD3D11.ini` into a SimCity 4 plugins folder — either `Documents\SimCity 4\Plugins` or the
`Plugins` folder of whatever user directory you launch with. The INI holds the [settings](#settings). Without it, the
defaults below apply.

The driver registers GZCOM class `0xBADB6906` — the class ID of the game's own DirectX driver — at a higher version,
so SC4 selects it automatically. No `-d:` switch is needed. Do not launch with `-d:Software`, which bypasses it.

If you keep plugins outside `Documents\SimCity 4`, pass `-UserDir:` and **quote it**. SC4 splits an unquoted value at
the first space, silently loads the wrong plugin folder, and falls back to its built-in DirectX driver.

To check that it is active, start the game and open `SC4D3D11.log` next to the plugins folder (normally
`Documents\SimCity 4\SC4D3D11.log`). Its `adapter:` line names the GPU in use.

## Uninstalling

Delete `SCD3D11.dll` and `SCD3D11.ini` from the plugins folder. SimCity 4 goes back to its built-in DirectX driver on the next launch.

## Settings

Set these in `SCD3D11.ini` next to `SCD3D11.dll`, as `Name=value` under `[SCD3D11]`. A launch argument of the same
name overrides the INI for one session: `-Name:value`, or `-Name` alone for `on`. Values are `on` or `off` (also
`enabled`/`disabled`, `true`/`false` and `1`/`0`), in any case. `SC4D3D11.log` lists every setting in effect and where
it came from.

| Setting | Default | Effect |
|---------|---------|--------|
| `Borderless` | off | With `-f`, a monitor-sized borderless window instead of exclusive fullscreen |
| `VSync` | on | Wait for vsync before presenting |
| `FlipModel` | on | Windowed and borderless: present with the flip model. Off uses the legacy DISCARD swap chain |
| `GPU` | `high-performance` | `default` uses the adapter Windows picks instead of the high-performance one |
| `ViewportCull` | on | Drop the draws of a scroll redraw that lie entirely outside its small scissored viewport |
| `LiveShadows` | off | Real cast shadows along SC4's own sun: buildings, trees, props and network pieces shade each other, slopes and walls, and hills shade the land behind them. Off by default because of the rendering cost |
| `NetworkShadowMasks` | off | With `LiveShadows`, keep the flat FSH shadow masks of prebuilt network pieces (the NAM ships thousands). They are removed by default because those pieces already cast live, and the two would darken the same ground twice |
| `ReShade` | on | Off leaves ReShade at its default behaviour (effects over the whole frame, UI included). See [ReShade](#reshade) |

Standard SimCity 4 switches that SCD3D11 reacts to:

| Switch | Effect |
|--------|--------|
| `-w` / `-f` | Windowed / fullscreen |
| `-CustomResolution:enabled` `-r<width>x<height>x32` | Render at an arbitrary resolution |
| `-UserDir:"<path>"` | User directory. With `SCD3D11.dll` in its `Plugins` folder, `SC4D3D11.log` is written there |

### Presentation modes

SCD3D11 implements SC4's native windowed and exclusive-fullscreen modes. Add `-Borderless` alongside a fullscreen
launch to get a monitor-sized borderless window instead.

On systems with both an integrated and a discrete GPU, SCD3D11 renders on the high-performance GPU. Set `GPU=default`
to use the adapter Windows picks instead. `SC4D3D11.log` names the adapter in use on its `adapter:` line.

### CPU options

SCD3D11 does not touch CPU scheduling, the render cull or the simulation tick. Process priority, P-core/E-core
scheduling, power throttling, the parallel render cull and the sim tick budget are handled by the separate
[SC4 CPU Options](https://github.com/0xC0000054/sc4-cpu-options) plugin.

## ReShade

Install [ReShade](https://reshade.me) 6.0 or later for `SimCity 4.exe` and pick the **Direct3D 10/11/12** API. The
regular build is enough. The "full add-on support" build is not needed. SCD3D11 registers itself with ReShade as an
add-on and:

- renders the effects as soon as the city view is drawn, so the UI stays sharp and untouched
- supplies the city's depth buffer, so depth effects (ambient occlusion, depth of field, fog) work without setting
  anything up. SC4's camera is orthographic, and the depth is encoded to match whatever
  `RESHADE_DEPTH_LINEARIZATION_FAR_PLANE` is set to. Leave the other `RESHADE_DEPTH_INPUT_*` definitions at `0`.

In the ReShade overlay's Add-ons tab, disable **Generic Depth**. SCD3D11 overrides its choice anyway. Outside the city
view (menus, region view) ReShade behaves as usual, except that effects can stay off in the region view after leaving
a city. Requires SimCity 4 1.1.641. Check `SC4D3D11.log` for `reshade: add-on registered`. `ReShade=off` turns the
integration off.

## Known issues

* With ReShade, effects can stay off in the region view after leaving a city.
* `LiveShadows` costs frame rate in large cities, most of all while scrolling.

## Reporting bugs

[Open an issue](https://github.com/caspervg/scd3d11/issues/new/choose) and attach `SC4D3D11.log` from the session that
went wrong. The log is recreated each time the game starts, so copy it before relaunching.

## Development

### Building

CMake is the only build. Install [Visual Studio 2022](https://visualstudio.microsoft.com/#vs-section) or later with
the desktop C++ components, then from an **x86** Developer Command Prompt:

```
git submodule update --init
cmake -S . -B build\debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build\debug
ctest --test-dir build\debug --output-on-failure
```

### Diagnostics

The driver writes `SC4D3D11.log` to the parent of the plugins folder holding `SCD3D11.dll` (normally
`Documents\SimCity 4`), tagged by category: `init`, `caps`, `swapchain`, `resource`, `grid`, `unsupported`, `window`.
The log is recreated each session.

`window` records focus, activation, size, visibility and system-command messages on the game window (Alt+Tab and the
Windows key included), and `swapchain` records changes in the Present result, paused presentation and the exclusive
fullscreen state. A watchdog logs `watchdog: no frame for N ms` with the render thread's current phase, instruction
pointer and likely return addresses when no frame completes for 5 seconds once rendering is under way (the startup
plugin load is ignored), and `watchdog: frames resumed` afterwards. Debug builds also forward D3D11 debug-layer
messages and record each newly observed render-state, vertex-format and texture-format combination, tagged `state`.

These settings add more detail:

| Setting | Effect |
|---------|--------|
| `DrawStats` | Log a per-second summary of frame times, draws, culled draws, scroll and full redraws and texture churn, plus a line for each frame over 100 ms (`stats` category) |
| `LogLevel=trace` | Also log high-volume events: geometry cache segment recycling, capture changes, window z-order moves |

The scripts in `scripts/` launch, debug and capture the game with these switches.

### Frame callback API

Other plugins can observe the live D3D11 device, context, swap chain and back-buffer RTV through the exports in
[`scd3d11/SCD3D11Service.h`](scd3d11/SCD3D11Service.h):

```c
BOOL __stdcall SCD3D11RegisterFrameCallback(SCD3D11FrameCallback callback, void* userData);
BOOL __stdcall SCD3D11UnregisterFrameCallback(SCD3D11FrameCallback callback, void* userData);
```

The callback receives an `SCD3D11FrameContext` for either `SCD3D11_EVENT_RENDER` (once per presented frame, with the
immediate context borrowed for the duration of the call) or `SCD3D11_EVENT_BEFORE_DEVICE_DESTROY`. `deviceGeneration`
increments whenever the device is recreated, so stale resources can be detected and dropped.

## Third-party components

Included in the `vendor` folder:

* [gzcom-dll](https://github.com/nsgomez/gzcom-dll) (LGPLv2.1) — registers the graphics implementation with the game.
* [sc4-dll-utilities](https://github.com/0xC0000054/sc4-dll-utilities) (LGPLv2.1) — game version detection and
  reading `SCD3D11.ini`.
* [Windows Implementation Library](https://github.com/microsoft/wil) (MIT) — used by sc4-dll-utilities.
* [xxHash](https://github.com/Cyan4973/xxHash) (BSD 2-Clause) — fingerprints geometry for the upload cache.
* [ReShade](https://github.com/crosire/reshade) add-on API headers (BSD 3-Clause) — the [ReShade](#reshade) integration.

Their license notices are reproduced in [`THIRD_PARTY_NOTICES.txt`](THIRD_PARTY_NOTICES.txt), which ships with every
release.

## License

Licensed under the [GNU Lesser General Public License, version 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html)
or, at your option, any later version published by the Free Software Foundation.

You may dynamically link it with proprietary software such as SimCity 4, but changes you make to SCD3D11 must also be
shared under the LGPLv2.1 or later.
