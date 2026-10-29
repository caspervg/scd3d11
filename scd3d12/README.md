# SCD3D12

SCD3D12 moves SimCity 4's rendering into a separate 64-bit Direct3D 12 process. The game, its window, input,
simulation and UI logic stay in `SimCity 4.exe`. `SCD3D11.dll` becomes a thin shim that sends the game's drawing to the
server and answers the game's queries itself. When the server is missing or dies, the shim falls back to the
in-process Direct3D 11 renderer.

```
SimCity 4.exe (x86)                         SCD3D12Server.exe (x64)
  SCD3D11.dll shim  ── shared memory ring ──▶  draw lane: replays SimGL calls in D3D12
   state copy, answers Get*                    scene lane: the city as GPU instances
   occupant and model hooks                    asset store: DBPF, S3D and FSH by TGI
   D3D11 fallback  ◀── replies, heartbeat ───  upscaling, frame generation, ray tracing
```

The live stream and a recorded capture are the same bytes. Every capture replays offline as a regression test.

## Milestones

| | Milestone | Done when |
|---|---|---|
| M0 | Gates and the unattended benchmark | The five questions below are answered. `scripts/bench_sc4.py` drives the game alone |
| M1 | Wire format, recorder, offline D3D12 replay | Captures replay to images that match SCD3D11 |
| M2 | Live remote draw lane | An hour of play on the server matches SCD3D11, killing the server falls back cleanly |
| M3 | Identity | Nearly every draw is tied to an occupant or a known category, motion vectors work |
| M4 | Scene lane | Buildings, props, flora, terrain, networks and automata come from the server's own copy of the city |
| M5 | Presentation and upscaling | DirectComposition on SC4's window, UI as its own layer, DLAA, DLSS, FSR, XeSS, frame generation |
| M6 | Light | Continuous time of day, ray-traced shadows and occlusion, night lights, reflections |
| M7 | Free camera | Smooth zoom, 360° rotation, some tilt, every tool still works |
| M8 | Ecosystem | A scene API for mods, HD texture packs, a protocol OpenSC4 could drive |

## Gates (M0)

1. **Picking under a free camera.** Answered: four hooks, not one. `cS3DCamera::GetEyeRay` (0x00800340) covers
   terrain picks and ray-based tools. Occupant picks also need `GetViewportRayContainerRect` (0x007C20E0), which
   chooses candidates from the screen point through the fixed orthographic view. The network tool's elevated picks
   need 0x00644FD0. UI anchored to the city needs `ProjectPointToViewportCoordinates2D` (0x007F78E0) and
   `ProjectPointToViewportCoordinates` (0x007F7850). `Project` and `UnProject` need no hook. The orthographic cull
   (0x007F7E50) and the visible ground area also follow the game's own camera, so a free camera needs its own culling.
2. **Bandwidth.** Answered: no problem. Big City Tutorial at 1920x1080, scrolling, zooming and rotating: on average
   20 MB of geometry per second of which 1 MB the cache had not seen, peaks of 90 MB and 9.5 MB, textures up to 57 MB
   per second while loading, the largest single frame 19 MB (while loading). About 220 draws per frame at rest and
   600 while scrolling. A heavy NAM city is still to be measured.
3. **Presenting from another process.** Answered: yes, and frame generation needs a child window instead of
   composition. With `ServerPresent=composition` the server's own flip-model swap chain sits on a DirectComposition
   surface the shim attaches to SC4's window, and the live game showed the server's frames that way. DLSS frame
   generation does not work on composition swap chains. Streamline's factory passes them straight to DXGI, without
   the hook frame generation sets itself up in, and it has no `IDXGIFactoryMedia` at all. Wrapped afterwards, such a
   swap chain never gets a generated frame (measured in replay: none on either kind). Frame generation does work on a
   window swap chain made through Streamline's factory on another process's child window, as long as that process
   started the server, as SC4 does. Streamline only generates frames while the foreground window belongs to its own
   process or to its parent. A replay started by a stand-in host and presenting into the host's child window drew 60
   frames and presented 120 per second under a 120 fps Reflex cap. So live frame generation needs the shim to give
   the server a child window of SC4's window to present into.
4. **Plugin folder scanning.** Answered: SC4 does not load DLLs from subfolders of Plugins (a test DLL in
   `Plugins\SCD3D12\` was never loaded). The server and its own DLLs can live there.
5. **Address space.** Answered: worth it already. Big City Tutorial just after loading, SC4's own process: 884 MB
   virtual and 468 MB private with local drawing, 858 MB and 314 MB with Renderer=server, the 64 MB ring
   included. The shim still keeps a full Direct3D 11 device and every texture, so more is to come.

## Status

| | State |
|---|---|
| M0 | Done. `scripts/bench_sc4.py` runs the game unattended through its HTTP command listener |
| M1 | Done. The Direct3D 12 driver (`cGDriver12`) renders a 57,835-frame capture bit-identically to SCD3D11 on every frame compared, in 26 s |
| M2 | The live game runs drawn by the server. `Renderer=server` works end to end: the stream goes through a 64 MB shared ring, frames come back through two shared textures and fences, and the shim falls back to drawing locally when the server stops (killed mid-session, the game carried on drawing locally at the same frame rate). `ServerPresent=composition` presents from the server through DirectComposition. Coverage captures (night, every zoom and rotation, panels and dialogs, the query tool, demolition with explosions, plopped lots, roads, zones and construction, exclusive fullscreen where the UI and the region view arrive as blits) and a synthetic stream with stencil, fog, lighting, TexImage2D and every blit kind replay bit-identically to SCD3D11 on all 499 frames compared. Big City Tutorial at 1920x1080, scrolling at zoom 3 with VSync off: 313 fps drawn locally, 280 fps with the server and 288 fps with composition (186 and 208 while the shim waited for each frame it had just sent). The game's render thread spends about 3.3 ms per frame either way, the server 1.0 ms of CPU and 0.1 to 0.3 ms of GPU. Repeated state calls stay out of the stream, which halves the records and makes captures a third smaller |
| M3 | Occupant inserts, removes and moves are streamed (plus a listing of every occupant once a city loads), and `DrawOwnerHooks` brackets every grid and view object draw with its owner. In a test session 78% of all draws were bracketed (the rest are UI, background and loading screens) and 99.97% of bracketed occupants matched the occupant records. Each model instance's S3D key and world transform follow its OwnerBegin. Motion vectors are next |
| M4 | Asset readers (DBPF, QFS, S3D, FSH, exemplars) done. `replay --scene substitute` draws the game's S3D models (buildings, props, flora, automata) from the server's own copy of the game data in place of the game's draws: meshes kept on the GPU, textures from the FSH files, render state from the S3D materials and draw attributes, night lights from the texture id + 0x8000. On four captures 98.6% to 99.96% of model draws are the server's and all 324 frames compared are bit-identical. Left to the game: cars it repaints with its own vertex colours and highlighted models. The tint the game lays over close flora on stage 1 is a 16x16 alpha texture it makes itself (no package holds it), so the lane keeps the game's stage 1 for those draws. Plain network pieces and lot cell textures are drawn from records the shim reads off each drawable (`NetworkPiece`, `CellTexture`): the record's world-space vertices with the texture from the store, the rest of the state the game's. A cell on a slope is drawn by the terrain, so there only the texture is swapped. On a Big City Tutorial capture 96% of network draws and 91% of cell draws are the server's, the rest being the first frame after a zoom or rotation (the game rebuilds those vertices inside the draw, after the record is read), and all 108 frames compared are bit-identical. Live, `ServerScene=substitute` (or `check`) does the same in `serve`: the shim names the folders SC4 loaded its packages from (`GameFolders`, also in captures, so replays need no `--game`), and the server indexes them on a thread of its own (0.11 s for the vanilla game) and starts substituting at the next frame. Still the game's: animated sprites (0x00ABD1D0, a third of the grid draws) and three other lot models (0x00AA20D0, 0x00AA2808, 0x00AA2EA8, likely foundations and retaining walls). `--scene owned` (`ServerScene=owned` live) keeps the whole static view and draws it every frame without SC4's backing store (see Owned scene). `--scene check` also compares each draw's state and vertices with the game's, and that every game texture stands for one store texture |
| M5 | DLAA and DLSS through NVIDIA Streamline run on the retained scene in replay, and frame generation in a window the replay presents to. See [Upscaling](#upscaling). Not live yet, and the server does not draw at a lower render size yet |
| M6 | Stage 1 done in the draw lane, verified offline and live: `--relight <hour>` for replay, `ServerRelight` live (seen working in the game). The server keeps a light layer next to the back buffer, through SC4's backing-store copies, and relights each frame with sun shadows traced over the orthographic depth, in about 0.13 ms at 1920x1080. Off, the output stays bit-identical. Stage 2 done offline on the retained scene (`--light`, see Light): ray traced sun shadows, smooth terrain normals, night lights as light sources and a sun that stands still in the world. Limits: SC4's lighting baked into BAT textures is only divided out roughly, and stage 2 is not wired into `serve` yet |

## Retained scene

`replay --scene retained` (`RetainedScene.h`) keeps the city's static models (buildings, props, flora) on the server
and draws them every frame with SC4's camera, instead of taking them from SC4's backing store.

SC4 draws its static view (passes 0 to 2) only when it has to: the whole window after a zoom or rotation, the strip a
scroll uncovers, the rectangle around something that changed. Each such redraw ends with a copy of colour and depth
into two buffer regions, and every frame starts by copying them back, shifted by whole pixels while scrolling. The
retained scene takes the models the scene lane draws in pass 1 out of that pass, so the backing store only holds
what the scene does not cover (terrain, water, networks, lot textures, overlays, models the lane leaves to the game).
At the first dynamic owner of each frame, after the restore and the static redraws, it draws every instance on
screen with the frame's camera, depth tested against the restored depth. The dynamic passes then test against the
instances as they did against the game's own.

Instances are keyed by occupant and model. They go when the zoom or rotation changes (other models), when SC4 redraws
a rectangle that holds their whole box without drawing them, and when their occupant is removed or moved, once SC4
redraws over them (until then SC4 still shows them).

The camera is read from the matrices SC4 loads before the dynamic passes: an orthographic projection and a view that
turns about world up and tilts. Big City Tutorial at 1920x1080 shows yaw 22.5, 112.5, -157.5 and -67.5 degrees for
the four rotations, a pitch of 45 degrees at 0.0716, 0.143 and 0.286 metres per pixel and 30 degrees at 2.61 metres
per pixel. View x world matches SC4's own modelview to the last bit of its translation (0.001 at 10 km).

Against `--scene substitute` on scene-tutorial, cov-night-ui, cov-build-region and net-live, a full view differs in
0.0002% to 0.5% of the channels, scrolled frames in 0.1% to 0.3%. Most of it is a line of pixels along the foot of
models: SC4's own shadows (pass 2) are drawn while the models are missing from depth. With those shadows left out
(`--relight`), a full view differs in 3 channels. The rest are places SC4 itself left stale in its backing store, a
model it redrew in part with newer lighting, or a removed occupant it still shows.

It also writes motion vectors (previous position minus position now, in pixels) from the depth buffer and both
cameras, and from the previous transform for instances that moved. Warping the previous frame by them reproduces 97%
of a 21 pixel scroll exactly. The rest are cars, the UI and animated props.

Time per frame for the retained draw at 1920x1080: 362 instances at 0.072 metres per pixel take 0.10 ms of CPU and
0.04 ms of GPU, 4,600 at 0.286 take 1.4 ms and 0.10 ms, 15,400 at 2.61 take 3.9 ms and 1.0 ms. Each instance is still
one draw through the driver's state machine.

## Light

`replay --scene retained --relight <hour> --light rays,terrain,lamps,world` (`WorldLight.h`) adds light from the
server's own copy of the city to the relighting of stage 1. Without `--light` it is off and the output stays as before.

- `rays`: a ray from every city pixel towards the sun, through the retained instances and the terrain (DXR 1.1 ray
  queries in the sunlight pass). Each mesh gets a bottom level structure once, and cut-out meshes test the texel's
  alpha at each candidate hit. The terrain's structure comes from the heights in SC4's terrain draws (a 16 metre grid),
  so it only casts from 8 metres off the surface: near the pixel it would shade the ground it stands for. Each pixel
  of a 4x4 tile aims at another point of the sun's disc, which the relight's blur turns into a soft edge.
- `terrain`: terrain pixels take smooth normals from those heights instead of the flat ones from depth.
- `lamps`: the night textures of the lit instances are read once per mesh on the GPU for where the mesh glows and how
  much. Each glowing side becomes a point light, binned into 32 pixel screen tiles. Lamps fade in as the sky darkens.
- `world`: the sun stands still in the world instead of turning with the camera.

The structures of the city and of the terrain are double buffered: a frame is traced with the structure built in an
earlier frame while the next one is built. Tracing a structure in the command list that built it hung the GPU (an RTX
5080) now and then. The retained scene counts its changes, so a frame where nothing changed reuses the last frame's
instances and lights.

GPU time of the lighting pass at 1920x1080 on an RTX 5080, full captures (average, with sun at 15:00 and 22:00):

| Capture | Stage 1 | rays, terrain, lamps at 15:00 | at 22:00 | of which the sunlight trace |
|---|---|---|---|---|
| scene-tutorial (5,400 instances) | 0.10 ms | 0.18 ms | 0.21 ms | 0.13 to 0.17 ms |
| net-live (7,200 instances, 2,300 lights) | 0.09 ms | 0.17 ms | 0.22 ms | 0.12 to 0.15 ms |
| cov-night-ui (8,100 instances) | 0.10 ms | 0.18 ms | 0.23 ms | 0.15 to 0.18 ms |

CPU per frame for the world light: 0.01 to 0.06 ms median, 0.05 to 0.26 ms on average, about 50 ms on the frame that
first builds a city's mesh structures. Rebuilding the city's structure (38 to 245 times per capture) costs under 1.7
ms of GPU at worst. No capture hung at full length with all features, day or night.

Not done yet: `serve` has no switch for it, SC4's own street lamps are not light sources (only the night textures of
models are), and the terrain heights sit up to 2 metres off some lots, which the 8 metre gap hides.

## Upscaling

`replay --scene retained --dlss dlaa|quality|balanced|performance|ultra` runs NVIDIA DLSS through Streamline
(`Dlss.h`) on the retained scene's frames. The build copies Streamline's DLLs next to the server, which loads them
only when asked to and only once their NVIDIA signature checks out. The server hooks nothing: Streamline only sees the
device, the resources it is handed and, for frame generation, the swap chain. Without `--dlss` the output stays
bit-identical.

What DLSS needs from a frame is in `UpscalerInputs`: the colour before the UI, depth, motion vectors, the jitter, the
camera now and in the previous frame, the render size and the output size. The retained scene fills it. For DLAA it
draws its instances with an 8 phase Halton jitter. The rest of the frame comes out of SC4's backing store without
jitter, so only the instances get smoothed. The other modes need the server to draw at a lower size, which it does not
yet. A jittered render size copy of each full frame stands in for that (`DlssTest.h`), to judge quality and cost.

net-live at 1920x1061 on an RTX 5080, four frames per mode, compared with the same frames without DLSS:

| Mode | Render size | PSNR | GPU time of the DLSS pass per frame |
|---|---|---|---|
| DLAA | 1920x1061 | 36.4 to 38.8 dB | 0.45 ms |
| Quality | 1280x707 | 36.4 to 37.9 dB | 0.46 ms |
| Balanced | 1114x615 | 36.1 to 37.7 dB | 0.45 ms |
| Performance | 960x531 | 35.5 to 37.3 dB | 0.46 ms |
| Ultra performance | 640x354 | 35.1 to 36.6 dB | 0.63 ms |

A bilinear upscale from the same render sizes scores 28.5 to 33.8 dB on two of those frames, where DLSS scores 35.1 to
37.9. For DLAA the difference is mostly the smoothing itself.

`--present window|composition|surface` shows the replay in a window through one of three kinds of swap chain,
`--frame-generation on` adds DLSS frame generation, `--fps <n>` caps the frame rate through Reflex and `--into
<window>` presents into another process's window. See gate 3 for what works. Frame generation also stops while the
window is not in the foreground, without saying so.

## Owned scene

`replay --scene owned` and `ServerScene=owned` (`OwnedScene.h`) keep the whole static view on the server and draw
all of it every frame with the frame's camera. SC4's backing store is not used: its region copies are skipped, and
nothing SC4 draws in the static window (from the frame's start to the first dynamic owner) reaches the screen.

Every draw of the static window is taken whole (`cGDriver12::DrawCapture`): the triangles in world space, and the
state the GPU needs (pipeline, constants, textures pinned so the game can delete or reload them). What is kept:

| | From | Kept as | Goes when |
|---|---|---|---|
| models | the scene lane's meshes | instances of one mesh per state | a redraw holds their whole box without them, or any part once their occupant left, or a new view |
| terrain and water | the terrain view object (0x00AB4480), passes 0 and 2 | the triangles of each 16 metre cell, every layer | a newer redraw draws the cell, or holds it whole without drawing it |
| background | the 128 metre quads a full redraw starts with | what they draw | a redraw overlaps them without drawing them the same |
| overlays | 0x00AB39D0, shadows and decals | what they draw, texture coordinates worked out once | as the background |
| grid drawables | lot textures, networks, foundations, cars the lane leaves to the game | the triangles each drawable draws | as models |

The terrain's draws cover the cells a viewport sees, so a cell, not a draw, is what stays the same from redraw to
redraw. A cell drawn the same again keeps its geometry on the GPU. Lot layers on the same spot keep the order SC4
drew them in. With relighting the overlays are left out, as the driver leaves SC4's shadows out.

Drawing: draws with the same state share one pipeline, one set of constants in memory that lasts, and their
textures. Models are instances of their mesh (DrawIndexedInstanced, the modelview per instance), everything else is
merged per state and 256 metre tile into buffers that grow in place, with pieces that go zeroed out until the buffer
is built again. Stages that take their coordinates from view positions (water, shadows, decals) get them computed
once into the vertices, so shadows of different buildings share a state too. Each frame: the redraw rules, the
buckets' changes copied to the GPU, the instances written, one draw per state and visible tile or per mesh state.

Against plain replay of scene-tutorial at 1920x1080, channels that differ:

| Frame | | Differ |
|---|---|---|
| 2253, 2300 | far zoom, after a full redraw | 0.005%, 0.02% |
| 3607, 6426, 8063 | near zooms, after full redraws | 0.59%, 0.72%, 0.45% |
| 11166, 12248 | full redraws | 0.22%, 0.27% |
| 12800, 13000, 13500 | while scrolling, the owned view drawn every frame | 1.4%, 2.4%, 5.7% |
| 15000, 15600, 17000, 21000 | after more scrolling | 0.7%, 0.8%, 0.4%, 1.9% |

Full redraws differ in shadow edges (texture coordinates now computed on the CPU) and blended model parts drawn in
another order. While scrolling the differences grow in the area the scroll uncovered: lots whose cells SC4 redraws in
pieces, and places SC4 itself left stale in its backing store (a highlight it still shows, a building it redrew
since). Motion vectors of a 1 and a 2 pixel scroll average 1.000 and 1.999 pixels.

`--look <x>,<y>` draws the chosen frames from a camera moved along the screen: at frame 13800 of scene-tutorial,
960 pixels to the left shows the blocks SC4 drew at the start of the scroll and no longer draws.

Time per composite (replay of scene-tutorial at 1920x1080; the retained scene's medians are from net-live):

| Metres per pixel | Draws | Instances | CPU median (average) | GPU median (average) | Retained before (CPU, GPU) |
|---|---|---|---|---|---|
| 0.072 | 418 | 601 | 0.10 ms (0.10) | 0.67 ms (0.53) | 0.15, 0.04 (models only) |
| 0.286 | 1,459 | 5,407 | 0.80 ms (3.1) | 0.41 ms (2.3) | 0.97, 0.06 (models only) |
| 2.613 | 5,504 | 19,969 | 1.06 ms (1.7) | 0.20 ms (0.21) | 3.65, 0.99 (models only) |

The averages at 0.286 come from the scroll, where new cells are added to the buckets and each strip looks for what it
removed. That search goes through an index of where everything kept lies on screen (64 metre bins, big items in a
list of their own): 0.75 ms per composite on average, from 1.34 ms when it walked every kept item. Live, Big City Tutorial with VSync on, the server spends 1.7 to 9
ms of CPU per frame (substitute: about 1 ms) and the composite 1.4 to 2.4 ms of GPU.

Still SC4's: the static draws themselves when it redraws (the server keeps them, but only what SC4 drew exists), the
camera, the dynamic passes (automata, effects, animated sprites, which have no motion vectors of their own) and the UI.
Models are the ones SC4 picked for its zoom and rotation, so a rotation SC4 never drew shows the kept models from the
wrong side: picking them from the occupants' exemplars is not done yet.

### For M5 (temporal upscaling and frame generation)

`OwnedScene::Inputs()` gives, for each frame the scene drew: the city before the UI (copied after the last
post-dynamic view object), the depth buffer, a motion target (previous position minus position now, in pixels, RG16F,
camera motion only), the cameras of this frame and the previous one without jitter, and render and output sizes.
`OwnedScene::SetJitter(x, y)` moves every owned draw by a fraction of a pixel (replay: `--jitter <x>,<y>`), and
`SaveInputs` writes the colour and motion as images (replay saves them for the chosen frames). `UpscalerCamera` hands the same to DLSS (`UpscalerInputs`, the HUD-less colour included): `replay --scene owned --dlss dlaa` runs DLAA on the owned view with its Halton jitter, 0.45 ms of GPU per frame on scene-tutorial. Gaps: the dynamic
passes are not jittered, moving instances get no motion of their own, and the render size is the output size until
the city is drawn to a target of its own.

### Stopping SC4's static draws

In owned mode SC4 still draws the static view when it redraws, and the server needs those draws: they are where the
kept scene comes from. Only drawables the server can make itself could be skipped: for a model, the ModelInstance
record holds everything (the lane draws the mesh from its own copy), so DrawOwnerHooks could send the record and return
without calling Draw. Picking would not notice (it goes through GetViewportRayContainerRect and the occupants, not
through the draws), but anything that reads SC4's own frame would: the city thumbnails and snapshots (CopyColorBuffer)
would lose their models. Not done yet.
