#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy>=1.26", "tqdm>=4.66"]
# ///
"""Bake shadow masks for every network-piece model in a Plugins tree.

Run it with uv, which fetches numpy itself:

    uv run tools/shadowbake/bake_nam.py "C:/Users/you/Documents/SimCity 4/Plugins/800-nam" -o out

The mapping needs no lookup table. In the NAM's own data 2073 of its 2583
shadow-mask instances are *the same id* as an S3D model instance, so a piece's
mask belongs at `{0x7AB50E44, 0x2BC2759A, <model instance>}`. Bake the model,
store it under its own id, and the game finds it.

Masks are tile-aligned, not fitted to the shadow. `UpdateShadow` passes `16.0f`
as the decal size at 0x0061E724 and every NAM piece model measures exactly
x,z in [-8, 8], so the texture covers that one tile and anything projecting
past the edge is clipped, which is what the stock masks show.

Geometry lying on the ground plane is dropped. Most network models are a flat
quad covering the whole tile - the road surface - and projecting those produced
a solid square over 90% of the tile. Only what stands above the ground casts a
shadow.

Everything hot runs in numpy: triangle fill, the box dilate and the DXT3
encode. The DBPF/FSH/S3D containers come from sc4shadow.py, which stays
dependency-free, and the FSH bytes are built by the same routine whose selftest
reproduces a stock Maxis mask byte for byte.
"""

from __future__ import annotations

import argparse
import collections
import os
import random
import struct
import sys
import time
from multiprocessing import Pool

import numpy as np
from tqdm import tqdm

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sc4shadow as S

TILE_BOUNDS = (-8.0, -8.0, 8.0, 8.0)
PACKAGE_SUFFIXES = (".dat", ".sc4model", ".sc4lot", ".sc4desc")

# colour0 = 0x0000, colour1 = 0x0001, indices = 0: the constant black half of
# every DXT3 block, exactly as the stock masks store it.
_COLOUR_BLOCK = np.frombuffer(struct.pack("<HHI", 0x0000, 0x0001, 0x00000000), dtype=np.uint8)


def mask_size_for(instance: int) -> int:
    """Texture size for a model instance.

    Maxis and the NAM both number LOD families `base + 0..4` and size them
    `8 << zoom`. Plenty of NAM instances do not end in 0-4, and for those the
    resolution is a free choice because the game only samples the texture, so
    they get a middle-of-the-road 64.
    """
    zoom = instance & 0xF
    return S.mask_size(zoom) if zoom <= 4 else 64


def supersample_for(triangle_count: int) -> int:
    """Drop supersampling on very dense meshes.

    Cost is proportional to triangles times the squared supersample factor, and
    a handful of bridge models carry tens of thousands of triangles. At that
    density the silhouette is already smooth, so 2x costs a quarter as much and
    looks the same, while a sparse mesh keeps the full 4x where the edges show.
    """
    if triangle_count > 16000:
        return 1
    if triangle_count > 4000:
        return 2
    return 4


def rasterise(triangles, size: int, supersample: int = 4) -> np.ndarray:
    """Fill projected triangles into an antialiased alpha map, tile-aligned.

    Each triangle is tested only over its own bounding box, so cost follows the
    shadow's area rather than the texture's. The supersampled grid is then
    block-reduced, which is where the antialiasing comes from.
    """
    minimum_x, minimum_z, maximum_x, maximum_z = TILE_BOUNDS
    high = size * supersample
    scale_x = high / (maximum_x - minimum_x)
    scale_z = high / (maximum_z - minimum_z)
    grid = np.zeros((high, high), dtype=bool)

    corners = np.asarray(triangles, dtype=np.float64)  # (N, 3, 2)
    if corners.size == 0:
        return np.zeros((size, size), dtype=np.uint8)
    px = (corners[:, :, 0] - minimum_x) * scale_x
    pz = (corners[:, :, 1] - minimum_z) * scale_z

    for index in range(corners.shape[0]):
        x0, x1, x2 = px[index]
        z0, z1, z2 = pz[index]
        area = (x1 - x0) * (z2 - z0) - (x2 - x0) * (z1 - z0)
        if abs(area) < 1e-9:
            continue
        low_x = max(int(np.floor(min(x0, x1, x2))), 0)
        high_x = min(int(np.ceil(max(x0, x1, x2))), high)
        low_z = max(int(np.floor(min(z0, z1, z2))), 0)
        high_z = min(int(np.ceil(max(z0, z1, z2))), high)
        if low_x >= high_x or low_z >= high_z:
            continue
        xs = np.arange(low_x, high_x) + 0.5
        zs = (np.arange(low_z, high_z) + 0.5)[:, None]
        w0 = ((x1 - x0) * (zs - z0) - (xs - x0) * (z1 - z0)) / area
        w1 = ((xs - x0) * (z2 - z0) - (x2 - x0) * (zs - z0)) / area
        inside = (w0 >= 0) & (w1 >= 0) & (w0 + w1 <= 1)
        grid[low_z:high_z, low_x:high_x] |= inside

    reduced = grid.reshape(size, supersample, size, supersample).sum(axis=(1, 3))
    return (reduced * (255 // (supersample * supersample))).clip(0, 255).astype(np.uint8)


def dilate(alpha: np.ndarray) -> np.ndarray:
    """Grow the mask by one texel at half strength.

    A shadow that is slightly too generous still reads as a shadow; one that
    drops a viaduct support reads as a bug, so the bias is deliberate.
    """
    padded = np.pad(alpha, 1)
    best = np.zeros_like(alpha)
    for dz in (0, 1, 2):
        for dx in (0, 1, 2):
            np.maximum(best, padded[dz:dz + alpha.shape[0], dx:dx + alpha.shape[1]], out=best)
    return np.where(alpha == 0, best // 2, alpha).astype(np.uint8)


def encode_dxt3(alpha: np.ndarray) -> bytes:
    """DXT3 for a black RGB / explicit-alpha image.

    A shadow mask has a constant colour, so the colour half of every block is
    fixed and nothing is fitted: the encode is exact to the 4-bit alpha grid.
    """
    height, width = alpha.shape
    nibbles = ((alpha.astype(np.uint16) * 15 + 127) // 255).astype(np.uint8)
    blocks = (nibbles.reshape(height // 4, 4, width // 4, 4)
              .transpose(0, 2, 1, 3)
              .reshape(-1, 16))
    packed = (blocks[:, 0::2] | (blocks[:, 1::2] << 4)).astype(np.uint8)
    colour = np.broadcast_to(_COLOUR_BLOCK, (packed.shape[0], 8))
    return np.concatenate([packed, colour], axis=1).tobytes()


def find_packages(root: str) -> list[str]:
    found = []
    for dirpath, _, names in os.walk(root):
        for name in names:
            if name.lower().endswith(PACKAGE_SUFFIXES):
                found.append(os.path.join(dirpath, name))
    return sorted(found)


def scan_package(path: str):
    """Return (model instances, existing mask instances) for one package."""
    try:
        reader = S.DbpfReader(path)
    except Exception:
        return [], []
    try:
        models, masks = [], []
        for tgi in reader.entries:
            if tgi.type == S.S3D_TYPE and tgi.group == S.S3D_GROUP:
                models.append(tgi.instance)
            elif tgi.type == S.SHADOW_TYPE and tgi.group == S.SHADOW_GROUP:
                masks.append(tgi.instance)
        return models, masks
    finally:
        reader.close()


_READER_CACHE: dict[str, object] = {}


def _reader_for(path: str):
    """One open reader per package per worker, so a split package is parsed once."""
    reader = _READER_CACHE.get(path)
    if reader is None:
        if len(_READER_CACHE) >= 2:
            _, stale = _READER_CACHE.popitem()
            stale.close()
        reader = _READER_CACHE[path] = S.DbpfReader(path)
    return reader


def bake_package(job):
    """Bake one batch of models from one package. Runs in a worker process.

    Every model is timed and the worst one is reported back, so a run that ends
    with a single busy core names the model responsible instead of leaving it
    to guesswork.
    """
    path, wanted = job
    try:
        reader = _reader_for(path)
    except Exception:
        return [], collections.Counter({"unreadable": 1}), (path, 0.0, 0, 0.0)
    results = []
    stats = collections.Counter()
    batch_started = time.perf_counter()
    worst_instance = 0
    worst_seconds = 0.0
    for instance in wanted:
        model_started = time.perf_counter()
        tgi = S.Tgi(S.S3D_TYPE, S.S3D_GROUP, instance)
        try:
            meshes = S.parse_s3d(reader.read(tgi))
        except Exception:
            stats["parse failed"] += 1
            _elapsed = time.perf_counter() - model_started
            if _elapsed > worst_seconds:
                worst_seconds, worst_instance = _elapsed, instance
            continue
        if not meshes or not any(m.positions for m in meshes):
            stats["no geometry"] += 1
            _elapsed = time.perf_counter() - model_started
            if _elapsed > worst_seconds:
                worst_seconds, worst_instance = _elapsed, instance
            continue
        triangles = S.project(meshes, 0)
        if not triangles:
            stats["flat, casts nothing"] += 1
            _elapsed = time.perf_counter() - model_started
            if _elapsed > worst_seconds:
                worst_seconds, worst_instance = _elapsed, instance
            continue
        size = mask_size_for(instance)
        alpha = dilate(rasterise(triangles, size, supersample_for(len(triangles))))
        covered = int(np.count_nonzero(alpha))
        if covered == 0:
            stats["empty mask"] += 1
            _elapsed = time.perf_counter() - model_started
            if _elapsed > worst_seconds:
                worst_seconds, worst_instance = _elapsed, instance
            continue
        image = S.FshImage(size, size, bytearray(), f"{instance:08X}")
        results.append((instance, S.build_fsh(image, payload=encode_dxt3(alpha))))
        stats["baked"] += 1
        stats["coverage"] += covered * 100 // (size * size)
        elapsed = time.perf_counter() - model_started
        if elapsed > worst_seconds:
            worst_seconds, worst_instance = elapsed, instance
    return (results, stats,
            (path, time.perf_counter() - batch_started, worst_instance, worst_seconds))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("root", help="Plugins folder to scan")
    parser.add_argument("-o", "--output", required=True, help="directory for the generated packages")
    parser.add_argument("--limit", type=int, default=0, help="stop after N models (0 = all)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--chunk", type=int, default=20000, help="masks per output package")
    parser.add_argument("--batch", type=int, default=64,
                        help="models per work item; smaller balances the workers better")
    parser.add_argument("--overwrite-existing", action="store_true",
                        help="also bake models that already ship a mask")
    args = parser.parse_args(argv)

    started = time.time()
    packages = find_packages(args.root)
    print(f"scanning {len(packages)} packages under {args.root}")

    existing: set[int] = set()
    # SC4 resolves duplicate TGIs from the last-loaded package. Keep the last
    # lexical source for each model too, instead of baking duplicate instances
    # concurrently and letting worker completion order choose the winner.
    model_sources: dict[int, str] = {}
    with Pool(args.jobs) as pool:
        stream = pool.imap(scan_package, packages, chunksize=8)
        for path, (models, masks) in tqdm(zip(packages, stream), total=len(packages),
                                          desc="scanning", unit="pkg"):
            for instance in models:
                model_sources[instance] = path
            existing.update(masks)
    package_models: dict[str, list[int]] = collections.defaultdict(list)
    for instance, path in model_sources.items():
        package_models[path].append(instance)
    total = len(model_sources)
    print(f"  {total} S3D models, {len(existing)} instances already have a mask "
          f"({time.time() - started:.0f}s)")

    # One job per package leaves a long tail: a handful of NAM packages hold
    # tens of thousands of models, so the last worker grinds through one of
    # them alone while the rest idle. Split every package into fixed-size
    # batches and hand out the largest packages first (longest-processing-time
    # first), which keeps all workers busy to the end.
    batches: list[tuple[str, list[int]]] = []
    queued = 0
    for path, models in sorted(package_models.items()):
        wanted = sorted({m for m in models if args.overwrite_existing or m not in existing})
        if args.limit:
            wanted = wanted[:max(0, args.limit - queued)]
        if not wanted:
            continue
        queued += len(wanted)
        for start in range(0, len(wanted), args.batch):
            batches.append((path, wanted[start:start + args.batch]))
        if args.limit and queued >= args.limit:
            break
    # Cost per model varies by orders of magnitude - a road tile is four
    # triangles, a bridge span is thousands - and it correlates with the source
    # package. Any package-ordered schedule therefore ends with every worker
    # idle but one, which is exactly what a run ordered by package size did.
    # Shuffling deterministically spreads the expensive models evenly, so the
    # tail is bounded by a single batch.
    random.Random(0x5C4).shuffle(batches)
    jobs = batches
    print(f"  baking {queued} masks in {len(jobs)} batches of <= {args.batch} "
          f"on {args.jobs} processes")

    os.makedirs(args.output, exist_ok=True)
    stats = collections.Counter()
    pending: dict[S.Tgi, bytes] = {}
    # The DLL manifest is an availability list, not an ownership list. Include
    # masks supplied by the input plugins as well as newly generated masks so
    # a gate-relaxed piece can use an existing mask that this run skipped.
    manifest: list[int] = list(existing)
    written = 0
    done = 0

    def flush():
        nonlocal pending, written
        if not pending:
            return
        name = os.path.join(args.output, f"zzz_ShadowMasks_{written:02d}.dat")
        count = len(pending)
        write_started = time.time()
        S.write_dbpf(name, pending)
        tqdm.write(f"  wrote {os.path.basename(name)} ({count} masks, "
                   f"{time.time() - write_started:.1f}s)")
        pending = {}
        written += 1

    slowest: list[tuple[float, int, str]] = []
    with Pool(args.jobs) as pool:
        stream = pool.imap_unordered(bake_package, jobs, chunksize=1)
        with tqdm(total=len(jobs), desc="baking", unit="batch") as progress:
            for results, package_stats, timing in stream:
                stats.update(package_stats)
                done += 1
                path, batch_seconds, worst_instance, worst_seconds = timing
                if worst_seconds > 0.25:
                    slowest.append((worst_seconds, worst_instance, os.path.basename(path)))
                for instance, payload in results:
                    pending[S.Tgi(S.SHADOW_TYPE, S.SHADOW_GROUP, instance)] = payload
                    manifest.append(instance)
                if len(pending) >= args.chunk:
                    flush()
                progress.update(1)
                progress.set_postfix(masks=stats["baked"], slow=f"{max(slowest, default=(0,))[0]:.1f}s")
    tqdm.write("  writing the final package...")
    flush()

    manifest_path = os.path.join(args.output, "SC4ShadowMasks.txt")
    with open(manifest_path, "w", encoding="ascii") as handle:
        handle.write("# Generated shadow-mask instances for -NativeShadowMasks.\n")
        handle.write(f"# {len(set(manifest))} available masks found or baked from {args.root}\n")
        for instance in sorted(set(manifest)):
            handle.write(f"{instance:08X}\n")

    baked = stats.pop("baked", 0)
    coverage = stats.pop("coverage", 0)
    print(f"\nbaked {baked} masks in {time.time() - started:.0f}s")
    if baked:
        print(f"  mean coverage {coverage // baked}% of the tile")
    print(f"  skipped: {dict(stats)}")
    if slowest:
        slowest.sort(reverse=True)
        print("  slowest models:")
        for seconds, instance, package in slowest[:8]:
            print(f"    {seconds:7.2f}s  {instance:08X}  {package[:50]}")
    print(f"  manifest: {manifest_path}")
    print(f"\nInstall: copy zzz_ShadowMasks_*.dat into your Plugins folder, and")
    print(f"  SC4ShadowMasks.txt next to SCD3D11.dll, then run the game with")
    print(f"  -NativeShadowMasks:network")
    return 0


if __name__ == "__main__":
    sys.exit(main())
