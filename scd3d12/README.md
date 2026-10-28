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
3. **Presenting from another process.** Half answered: yes. With `ServerPresent=composition` the server's own
   flip-model swap chain sits on a DirectComposition surface the shim attaches to SC4's window, and the live game
   showed the server's frames that way. Streamline frame generation on such a swap chain is still to be tried.
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
| M4 | Asset readers (DBPF, QFS, S3D, FSH, exemplars) done. `replay --scene substitute` draws the game's S3D models (buildings, props, flora, automata) from the server's own copy of the game data in place of the game's draws: meshes kept on the GPU, textures from the FSH files, render state from the S3D materials and draw attributes, night lights from the texture id + 0x8000. On four captures 98.6% to 99.96% of model draws are the server's and all 324 frames compared are bit-identical. Left to the game: cars it repaints with its own vertex colours and highlighted models. The tint the game lays over close flora on stage 1 is a 16x16 alpha texture it makes itself (no package holds it), so the lane keeps the game's stage 1 for those draws. Plain network pieces and lot cell textures are drawn from records the shim reads off each drawable (`NetworkPiece`, `CellTexture`): the record's world-space vertices with the texture from the store, the rest of the state the game's. A cell on a slope is drawn by the terrain, so there only the texture is swapped. On a Big City Tutorial capture 96% of network draws and 91% of cell draws are the server's, the rest being the first frame after a zoom or rotation (the game rebuilds those vertices inside the draw, after the record is read), and all 108 frames compared are bit-identical. Live, `ServerScene=substitute` (or `check`) does the same in `serve`: the shim names the folders SC4 loaded its packages from (`GameFolders`, also in captures, so replays need no `--game`), and the server indexes them on a thread of its own (0.11 s for the vanilla game) and starts substituting at the next frame. Still the game's: animated sprites (0x00ABD1D0, a third of the grid draws) and three other lot models (0x00AA20D0, 0x00AA2808, 0x00AA2EA8, likely foundations and retaining walls). `--scene check` also compares each draw's state and vertices with the game's, and that every game texture stands for one store texture |
| M6 | Stage 1 done in the draw lane, verified offline and live: `--relight <hour>` for replay, `ServerRelight` live (seen working in the game). The server keeps a light layer next to the back buffer, through SC4's backing-store copies, and relights each frame with sun shadows traced over the orthographic depth, in about 0.13 ms at 1920x1080. Off, the output stays bit-identical. Limits: SC4's lighting baked into BAT textures is only divided out roughly, terrain is shaded per triangle, and night lights are not yet light sources |

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
