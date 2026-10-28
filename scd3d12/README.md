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
| M2 | The live game runs drawn by the server. `Renderer=server` works end to end: the stream goes through a 64 MB shared ring, frames come back through two shared textures and fences, and the shim falls back to drawing locally when the server stops. `ServerPresent=composition` presents from the server through DirectComposition |
| M3 | Occupant inserts, removes and moves are streamed (plus a listing of every occupant once a city loads), and `DrawOwnerHooks` brackets every grid and view object draw with its owner. In a test session 78% of all draws were bracketed (the rest are UI, background and loading screens) and 99.97% of bracketed occupants matched the occupant records. Each model instance's S3D key and world transform follow its OwnerBegin. Motion vectors are next |
| M4 | Asset readers (DBPF, QFS, S3D, FSH, exemplars) in progress |
| M6 | Stage 1, relight every frame: with `--relight <hour>` (or `ServerRelight`) the draw lane also keeps a light layer (SC4's light factor and a city weight from the owner brackets) that moves with every buffer region, and a lighting pass relights each frame from it and the depth buffer, with screen-space sun shadows, into a frame of its own. UI passes through untouched. 0.13 ms per frame at 1920x1080 on an RTX 5080. Off, the output is bit-identical |
| M6 | Stage 1 done in the draw lane, verified offline: `--relight <hour>` for replay, `ServerRelight` live (not yet run live). The server keeps a light layer next to the back buffer, through SC4's backing-store copies, and relights each frame with sun shadows traced over the orthographic depth, in about 0.13 ms at 1920x1080. Off, the output stays bit-identical. Limits: SC4's lighting baked into BAT textures is only divided out roughly, terrain is shaded per triangle, and night lights are not yet light sources |
