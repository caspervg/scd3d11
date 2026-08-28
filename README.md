# SCD3D11 · a Direct3D 11 renderer for SimCity 4

SCD3D11 replaces SimCity 4's native DirectX 7 hardware renderer with a Direct3D 11 implementation, to improve
compatibility with modern GPUs, drivers, Windows versions, and high-resolution displays.

It began as [SCGL](https://github.com/nsgomez/scgl) by Nelson Gomez, an OpenGL driver for the same interface, and was
renamed once the backend became Direct3D 11.

Not affiliated with or endorsed by EA Games.

## Requirements

* SimCity 4 Deluxe Edition 1.1.641 (Windows).
* A GPU supporting Direct3D 11 feature level 10.0 or better. The driver requests 11.1 → 11.0 → 10.1 → 10.0 and uses
  the first that succeeds.
* A 32-bit toolchain to build it. SimCity 4 is a 32-bit process, so a 64-bit build is useless and CMake refuses it.

## Installing

Copy `SCD3D11.dll` into a SimCity 4 plugins folder — either `Documents\SimCity 4\Plugins` or the `Plugins` folder of
whatever user directory you launch with.

The driver registers GZCOM class `0xBADB6906` — the class ID of the game's own DirectX driver — at a higher version,
so SC4 selects it automatically. No `-d:` switch is needed.

If you keep plugins outside `Documents\SimCity 4`, pass `-UserDir:` and **quote it**. SC4 splits an unquoted value at
the first space, silently loads the wrong plugin folder, and falls back to its built-in DirectX driver:

```
"SimCity 4.exe" -UserDir:"D:\My Stuff\SimCity 4\" -CustomResolution:enabled -r1920x1080x32 -w
```

## Building

CMake is the only build. Install [Visual Studio 2022](https://visualstudio.microsoft.com/#vs-section) or later with
the desktop C++ components, then from an **x86** Developer Command Prompt:

```
cmake -S . -B build\debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build\debug
ctest --test-dir build\debug --output-on-failure
```

`Debug`, `Release` and `MinSizeRel` are all supported and tested in CI. Debug builds run slower: they enable the D3D11
debug layer plus extra logging and validation.

Visual Studio and CLion both open this folder as a CMake project directly; there is no `.sln` to open.

## Presentation modes

SCD3D11 implements SC4's native windowed and exclusive-fullscreen modes. Add `-Borderless` (or
`-FullscreenMode:Borderless`) alongside a fullscreen launch to get a monitor-sized borderless window instead; the
selected game resolution is scaled to fit it.

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
`SimCity 4.exe`), tagged by category: `init`, `caps`, `swapchain`, `resource`, `grid`, `unsupported`. Each category is
capped per run so a repeating failure cannot fill the disk. Debug builds also forward D3D11 debug-layer messages.

Set `SC4D3D11_RECORD_STATES=1` before launching to additionally record observed render-state combinations to
`SC4D3D11-states.log`.

## Development scripts

Under `scripts/` — all default to a local SC4 install and plugin folder, so check the parameters before running:

* `Run-SC4D3D11.ps1` — builds, runs the tests, deploys the DLL, launches SC4, captures a screenshot and the logs into
  `runtime-captures\`.
* `Debug-SC4D3D11.ps1` — launches SC4 under `cdb` with symbols, so an access violation yields a real stack instead of
  SC4's symbol-less exception report. Accepts `-ScriptFile` for a cdb command script.
* `trace-combiner.cdb` — an example `-ScriptFile` that traces the driver's texture-combiner entry points.

## Contributing notes

`docs/` records what the driver has to honour and how it was verified: `d3d11-contract-inventory.md` for the SC4-side
contract (vtable layout, interfaces, ownership, resource semantics) and `d3d11-validation.md` for the validation
steps.

One trap worth knowing before touching `cIGZGDriver.h`: **MSVC lays out same-name virtual overloads in reverse
declaration order.** The interface's overload groups are therefore declared in the opposite order to their vtable
slots, and the trailing comment on each line is the slot it actually lands in. Sorting a group by slot number silently
swaps the methods at runtime — and where the overloads differ in argument count, it also corrupts the caller's stack.

## Third-party components

Included in the `vendor` folder:

* [gzcom-dll](https://github.com/nsgomez/gzcom-dll) (Expat License) — registers the graphics implementation with the game.
* [Scion](https://github.com/nsgomez/scion) (LGPLv2.1) — compatibility with game components.

## License

Licensed under the [GNU Lesser General Public License, version 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html)
or, at your option, any later version published by the Free Software Foundation.

You may dynamically link it with proprietary software such as SimCity 4, but changes you make to SCD3D11 must also be
shared under the LGPLv2.1 or later.
