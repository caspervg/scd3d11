# D3D11 validation

Runtime correctness has not been tested from the development environment. A successful build or conversion test is not evidence that SimCity 4 renders correctly.

## Build checks

Run from an x86 Visual Studio developer prompt:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Repeat with `-DCMAKE_BUILD_TYPE=Release`. The resulting DLL must be PE32/x86. Debug testing must use the D3D11 debug layer; absence of the layer is recorded in `SC4D3D11.log`.

## Required capture set

For every run, retain:

- `SC4D3D11.log` from the game directory.
- The complete debugger/DebugView stream containing D3D11 debug-layer output.
- `config-log.txt` and the SC4 command line.
- Windows version, GPU, driver version, monitor topology, desktop resolution, and DPI scaling.
- A screenshot for every numbered visual checkpoint below.

Any D3D11 warning or error fails the run unless the exact message, cause, and justification are added to this document. Repeated resource or unsupported-state messages fail the bounded-logging check.

## Comparison procedure

1. Use the same region, city, save, camera position, zoom, rotation, simulation time, graphics settings, resolution, and UI state for both runs.
2. Capture a known-good reference using SC4's D3D7 renderer through dgVoodoo2.
3. Capture the D3D11 renderer after a fresh process start.
4. Save files as `<checkpoint>-<resolution>-{d3d7,d3d11}.png`; do not use JPEG.
5. Compare at native size and with an absolute-difference view. Record expected differences and investigate every unexplained difference using captured render-state/resource diagnostics before changing rendering behavior.

## Manual test matrix

Run the full matrix in both Debug and Release at 1920x1080, 2560x1600, and 3200x1800. Also run 2048x1152 once because it crosses the legacy 2048 boundary.

| # | Checkpoint | Action | Pass evidence |
|---|---|---|---|
| 1 | Startup/menu | Cold-start SC4, select the D3D11 driver, reach the main menu, open and close one dialog. | Driver selected; no fallback; correct background, UI, text, cursor, transparency; init/capability log present. |
| 2 | Region view | Load the reference region and pan across it. | Terrain, water, borders, city tiles, labels and UI match the reference closely. |
| 3 | City load | Load the reference developed city and wait for simulation to resume. | No blank frame, hang, missing resources, or recurring diagnostic. |
| 4 | Camera | At every zoom level, rotate through all four orientations and pan. | Geometry, culling, depth, texture coordinates and screen-space UI remain correct. |
| 5 | Day/night | Run across a complete day-to-night-to-day transition. | Lighting, emissive/night textures, shadows and color modulation transition without stale state. |
| 6 | Terrain/water | Inspect coast, transparent water, slopes and terrain-detail boundaries at several zooms. | Depth, alpha, fog, filtering and animated water match the reference. |
| 7 | Objects | Inspect dense buildings, props, flora, pedestrians, road traffic, rail and effects. | Textures, vertex colors, animation and ordering remain correct. |
| 8 | Shadows/transparency | Inspect overlapping shadows, smoke, light cones, trees and semitransparent UI. | Blend factors, alpha test, depth writes and ordering match the reference. |
| 9 | UI/cursors | Exercise menus, tooltips, query panels, data views, placement previews and every cursor type used by the test. | Pixel alignment, clipping, color-key/alpha behavior and cursor updates are correct. |
| 10 | Resize | Repeatedly drag both window edges across small, 2048-wide and target high-resolution sizes. | No crash or stale frame; each nonzero client size produces one bounded swap-chain event and correct output. |
| 11 | Minimize/restore | Minimize for ten seconds, restore, then resize. | Zero-sized presentation is skipped; resources and rendering recover without debug-layer output. |
| 12 | Alt-tab | Alt-tab away and back ten times, including during city load and night transition. | Focus and presentation recover; no device/resource leak or recurring error. |
| 13 | Shutdown/restart | Exit from region and city views, then restart and repeat startup three times. | Clean shutdown each time; no stale window/hook, live-object warning, or increasing resource count. |

For Gate 1 specifically, stop after verifying registration, window creation, clear color, presentation, resize/minimize/restore, and shutdown. Scene rendering is expected to remain unsupported until the geometry/state milestones land; that limitation must not be reported as runtime correctness.
