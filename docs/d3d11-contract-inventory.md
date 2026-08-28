# Native D3D11 driver contract inventory

This document is the implementation gate for replacing SCGL's OpenGL backend with a native
Direct3D 11/DXGI backend. It records what is known, what is only inferred, and the validation
required before broader rendering work.

## Sources and confidence

- **Confirmed:** SCGL source in this repository, including every driver and extension implementation.
- **Confirmed:** symbolized Mac `nSGLDX7::cGDriver` names and intent.
- **Confirmed:** Windows `SimCity 4.exe` 1.1.641 functions, offsets, IIDs, and calling behavior cited below.
- **Confirmed:** current `sc4-render-services` and its `gzcom-dll` interfaces, inspected from commit
  `main` on 2026-08-23.
- **Unverified at runtime:** all D3D11 behavior until the DLL is installed and exercised in SC4.

Mac addresses below explain intent only. Windows addresses are the authority for Windows ABI facts.
No Ghidra database changes were made.

## Architecture map

### DLL and class registration

- `cGDriversCOMDirector` derives from `cRZCOMSlimDllDirector` and exports through
  `RZGetCOMDllDirector()`.
- It registers one class, `0xBADB6906`, using `cGDriver::FactoryFunctionPtr2`, and enumerates it with
  version/priority `1,000,000`. This is the native DirectX driver's class ID, so GZCOM selects SCGL as
  the higher-version DirectX implementation without requiring `-d:OpenGL`.
- The factory allocates the driver, calls `QueryInterface`, and deletes it if the requested interface
  is unavailable. `GetGZCLSID()` returns the same `0xBADB6906` class ID.

### Interfaces queried by SC4

| Interface | IID | SCGL status | Windows 1.1.641 evidence |
|---|---:|---|---|
| `cIGZUnknown` | `0x00000001` | implemented | not returned by the native DirectX QueryInterface path inspected |
| `cIGZGDriver` | `0xA4554849` | implemented | returned at `this + 0x0C` |
| buffer-region extension | `0x669565FE` | implemented | returned at `this + 0x00` |
| lighting extension | `0x87E2B87D` | implemented | returned at `this + 0x08` |
| snapshot extension | `0xE69BFE2A` | mandatory and implemented | returned at `this + 0x04` |
| vertex-buffer extension | `0x09CD86F9` | implemented and exposed | native DirectX returns the separately held interface pointer at object offset `0x1C` |

Windows evidence: the native DirectX primary-interface thunk at `0x00886440` adjusts `this` by
`-0x0C` and enters QueryInterface at `0x00882A00`. The previously cited `0x0087D670` function belongs
to the OpenGL implementation, not `nSGLDX7`.

### Ownership and reference counting

- Every successful `QueryInterface` adds one reference.
- Each multiple-inheritance interface must return its correctly adjusted subobject pointer.
- `cRZRefCount` supplies `AddRef` and `Release`. The factory begins with zero references and the
  successful query establishes the caller-owned reference.
- Partial initialization must release only objects actually acquired. Shutdown must be safe when
  called repeatedly and the destructor must call it as a final safety net.
- The future D3D11 access interface will return AddRef'd device/context/backbuffer-facing objects;
  callers release them and use a monotonically increasing generation to reject stale resources.

### Initialization and shutdown

- `Init` initializes driver bookkeeping and mode enumeration, but the real rendering device belongs
  to the selected video mode/window lifecycle.
- Windows `Init` at `0x00881DF0` is guarded by an initialized byte and calls viewport initialization
  once. `Shutdown` at `0x00881E40` clears the byte and tears the viewport down once.
- Windows viewport initialization at `0x00888A30` registers `GDriverClass--DirectX`, probes the API,
  builds driver information, and enumerates modes. Shutdown at `0x008880F0` first deselects the mode,
  destroys its window, unregisters the class, and releases API objects.
- D3D11 does not need SCGL's hidden bootstrap window/context. `Init` should probe D3D11 capability and
  enumerate modes without creating a swap chain. `SetVideoMode` creates the actual device and swap
  chain. All ABI entry points catch failures internally; exceptions never cross into SC4.

### Window and canvas integration

- `SetVideoMode(index, wndProc, showWindow, unknown)` receives SC4's WndProc, not an existing HWND.
  The graphics driver creates and owns `GDriverWindow--<backend>` and installs that WndProc.
- The window's client area is the render size. Window styles and `AdjustWindowRectEx` affect only the
  outer rectangle.
- The main HWND later becomes available through `cIGZFrameWorkW32::GetMainHWND`, as used by current
  render-services.
- The first D3D11 mode is windowed. Zero client width/height and minimized windows suspend resize and
  presentation work without destroying device-wide resources.

### Display modes and resolution selection

- `sGDMode` is 52 bytes (`0x34`) with width/height/depth, color masks, fullscreen flag, feature flags,
  texture-stage count, and `isInitialized` at `0x30`.
- SCGL enumerates desktop modes with `EnumDisplaySettings`, deduplicates width/height/depth, and emits
  fullscreen and windowed entries. The D3D11 driver initially retains this contract but must not cap
  width at 2048.
- Required windowed validation sizes are 1920x1080, 2048x1152, 2560x1600, and 3200x1800. Texture caps
  and display dimensions are independent.

The D3D11 mode list explicitly adds the four required 32-bit windowed sizes independently of
fullscreen monitor-mode enumeration. `VideoModeUtilsTest` verifies mapping, deduplication, flags, and
stable indices.

### Frame begin, end, and present

- `Clear`, `ClearColor`, `ClearDepth`, and `ClearStencil` cache values and/or clear the bound color and
  depth-stencil targets according to SC4's mask bits (`0x4000` color, `0x1000` depth, `0x2000`
  stencil).
- `Flush` is SC4's externally visible frame boundary. SCGL swaps buffers there. The native D3D7
  implementation ends queued scene work, presents, restarts scene work, and performs device-loss
  recovery there (Mac intent: `Flush` at `0x004225E2`).
- D3D11 `Flush` presents through DXGI. It must report `DXGI_ERROR_DEVICE_REMOVED` and
  `DXGI_ERROR_DEVICE_RESET` with `GetDeviceRemovedReason`, invalidate device resources, and recover at
  a safe frame boundary.

### Textures, surfaces, palettes, and render targets

- Observed SCGL internal texture formats: RGB5, RGB8, RGBA4, RGB5A1, RGBA8, BC1/DXT1, BC2/DXT3,
  BC3/DXT5.
- Observed upload formats: RGB, RGBA, BGR, BGRA, alpha, luminance, luminance-alpha, and BC1/2/3,
  combined with the 16 SC4 scalar/packed element types.
- `LoadTextureLevel` supports full-level and sub-rectangle updates and receives an explicit row length;
  D3D11 uploads must honor source pitch and block-compression alignment.
- `SetTexture`/`GetTexture` operate on two observed texture stages. Texture parameters currently cover
  min/mag filtering and U/V addressing.
- Buffer regions copy color or depth rectangles to persistent offscreen storage. They are used by SC4
  to avoid full redraws and cannot be treated as optional merely because modern presentation differs.
- The symbolized Mac D3D7 implementation leaves all six `BitBlt`/`StretchBlt` variants unsupported
  (`0x0041ECBA` through `0x0041ED14`) and sets driver error 3. They remain unsupported here unless a
  Windows caller or runtime capture proves that the D3D11 path needs them.
- Swap-chain resize recreates only the backbuffer RTV, depth-stencil texture/view, and viewport-sized
  buffer-region resources. Immutable shaders, input layouts, and descriptor-cached states survive.
- Palette behavior is not yet evidenced and remains an explicit investigation item.

### Vertex and index submission

- The primary path is client-memory `InterleavedArrays` followed by `DrawArrays` or `DrawElements`.
- Confirmed standard formats contain position (3 floats), optional normal (3 floats), BGRA8 diffuse,
  and zero, one, or two float2 texture coordinates. Known strides are 12 through 44 bytes.
- Confirmed primitive vocabulary: triangles, triangle strip/fan, points, lines, line strip, quads, and
  quad strip. D3D11 has no quad topology, so observed quad paths require a bounded index expansion.
- Index scalar types come from SC4's type enum; D3D11 submission must reject unsupported widths and
  invalid stride/count combinations.
- Start with reusable dynamic vertex/index buffers that grow geometrically. Do not create per-draw
  D3D11 buffers.

### Matrices, viewport, and coordinates

- SC4 supplies model-view, projection, and per-stage texture matrices. The native D3D7 initialization
  establishes a top-left 2D projection: X maps to `[-1,1]`, Y is inverted, and Z is reversed for its
  chosen convention.
- Native DirectX `SetViewport(x, y, width, height)` at `0x008837E0` converts the public bottom-left Y
  coordinate to DirectX's top-left coordinate. The D3D11 implementation must preserve that conversion.
- `SetViewport()` selects the full client area and disables clipping. The rectangle overload sets both
  viewport and scissor. `GetViewport` returns left/top/right/bottom, not x/y/width/height.
- Half-pixel behavior and the exact GL/D3D depth conversion require screenshot/runtime evidence before
  any compensating offset is added.

### Render state and texture stages

- Confirmed capability toggles: alpha test, depth test, stencil test, culling, blending, texture 2D,
  and fog.
- Confirmed state vocabulary: eight compare functions; five stencil ops; eleven blend factors; flat or
  smooth shading; color/depth masks; polygon depth offset; fog mode/color/density/start/end/source;
  vertex color ambient/diffuse flags; color and alpha multipliers.
- Two texture stages support replace, modulate, add, add-signed, interpolate, dot3, and NV combine4
  vocabulary, four sources/operands, RGB/alpha scales, texture coordinate selection, and texture
  matrices.
- The initial fixed-function emulation uses one generic shader pair plus constants where correct.
  Additional shader variants are added only when an observed state cannot be represented safely.
- Rasterizer, blend, depth-stencil, and sampler objects are cached by their actual D3D11 descriptors
  and never created per draw. Shaders are compiled at build time or initialization, never per frame.

### Lighting and materials

- The lighting extension exposes global enable, individual lights, ambient model, ambient/diffuse/
  specular colors, position/direction, and material ambient/diffuse/specular/emission/shininess.
- SCGL notes SC4 does not query the extension in its observed path but still expects a default
  directional light. Windows QI nevertheless exposes it, so D3D11 must preserve the contract.

### Capability reporting

- `isInitialized` must be true or SC4 rejects the hardware driver.
- Report two texture stages initially, stencil support only when the chosen depth format supplies it,
  and BC/DXT support only after `CheckFormatSupport` confirms the formats used.
- Do not report the obsolete D3D7 2048 texture/display limit. D3D11 feature-level limits are reported
  truthfully and independently from window dimensions.
- The meanings of `sGDMode::__unknown2`, `__unknown3`, and `__unknown5` still need Windows caller
  recovery. SCGL's copied values are hypotheses, not proof.

### Readback, screenshots, cursors, and GDI

- The snapshot extension is mandatory; SCGL states omission crashes during load.
- `CopyColorBuffer` creates or reuses an SC4 `cIGZBuffer`, requires A8R8G8B8, clips to the viewport,
  reads the backbuffer, vertically normalizes the result, and writes opaque pixels.
- D3D11 implements this with a staging texture and row-pitch-aware copy. It must preserve the supplied
  buffer's ownership and return behavior.
- The game WndProc remains SC4's, preserving cursor/input behavior. No GDI interop is required until an
  observed caller demonstrates it.

### Failure and recovery

- Every HRESULT is checked and logged with operation context. Initialization uses one cleanup path and
  leaves the object reusable after failure.
- Resize releases all backbuffer references before `ResizeBuffers`, skips zero-sized clients, then
  rebuilds backbuffer-dependent resources.
- Device removal invalidates all device objects, increments the public generation, notifies registered
  consumers at a frame-safe point, and attempts full device recreation only when the window is valid.
- Unsupported enum/format logging is category-based and deduplicated. Optional capture mode records the
  set of encountered states/formats without permanent per-draw logging.

## First end-to-end milestone

The smallest milestone that proves integration is:

1. Keep the current GZCOM registration, `cIGZGDriver` ABI, mode enumeration, and game-owned WndProc.
2. Replace OpenGL bootstrap state with direct `ID3D11Device`, immediate context, `IDXGISwapChain`,
   backbuffer RTV, and depth-stencil texture/view ownership.
3. In windowed `SetVideoMode`, create the SC4 graphics window, use its client size, create the swap
   chain and targets, and make partial failure unwind safely.
4. Implement clear masks and `Flush`/`Present`; make shutdown idempotent.
5. Build Win32 Debug and Release. Install manually and verify SC4 selects the driver and presents a
   stable clear color through menu startup, resize, minimize/restore, and shutdown.

This milestone deliberately excludes shaders, geometry, texture uploads, ImGui, fullscreen, and
device recreation. Those begin only after registration and presentation are proven in SC4.

## Implementation and validation gates

| Gate | Implementation | Evidence required before advancing |
|---|---|---|
| 0 | Contract inventory | this document; baseline Win32 build |
| 1 | registration + clear/present | Debug/Release build; SC4 log; visible window; clean shutdown |
| 2 | basic geometry | one untextured and one textured observed vertex format; debug layer clean |
| 3 | primary state translation | captured state vocabulary; region and city screenshots at multiple zooms |
| 4 | textures, buffer regions, screenshot | format/pitch checks; screenshot extension; region/city redraw correctness |
| 5 | resize/high resolution/recovery | 1920x1080, 2048x1152, 2560x1600, 3200x1800; minimize/restore; Alt-Tab |
| 6 | native D3D11 service | queried narrow interface; AddRef ownership test; device generation test |
| 7 | ImGui integration | official `imgui_impl_dx11`; render-safe callback; service lifecycle validation |
| 8 | compatibility acceptance | menu, region, city, rotations/zooms, day/night, water, terrain, buildings,
  props, automata, shadows, transparency, UI/cursor, restart; comparison screenshots |

Runtime gates require a real SC4 1.1.641 process and therefore remain manual unless the user explicitly
authorizes process launch/debugging. A successful build alone is never reported as visual correctness.

## Open investigations that block final acceptance, not Gate 1

- Recover IID `0xA455484A` vtable and all Windows callers.
- Recover Windows meanings and callers for the unknown `sGDMode` flags.
- Confirm the complete set of formats, packed vertex encodings, index widths, state combinations, and
  whether palettes/GDI paths are exercised in region or city view.
- Confirm exact half-pixel, depth-range, culling, and color-channel behavior against known-good D3D7.
- Recover Windows frame/reset notifications used by the native D3D7 `Flush` path and align the D3D11
  service notification point.
