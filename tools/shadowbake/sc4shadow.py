#!/usr/bin/env python3
"""SimCity 4 shadow-mask baker: DBPF, QFS, FSH and S3D in one dependency-free module.

The game's prebuilt-network shadows are terrain decals textured by an FSH at
`{0x7AB50E44, 0x2BC2759A, <piece id> | <zoom>}`. Those masks are pure alpha:
RGB is black in every texel of every stock mask, and only the alpha channel
carries the silhouette, antialiased at the edges. The instance's low nibble is
the zoom, and the texture is `8 << zoom` square, so one piece needs five files.

This module reads stock masks, writes new ones, and bakes them from an S3D
model by projecting it along the game's own shadow direction.

Shadow direction, read out of the running game at `GetShadowDirection`
(`0x007D6B50`) by `scd3d11/NativeShadowDiagnostics.cpp`:

    rotation k: (cos(a), -1/sqrt(2), sin(a)) * ... with a = 67.5 + 90k degrees

Measured samples were `(-0.65328145, -0.707106769, 0.270598054)` and
`(-0.270598054, -0.707106769, -0.65328145)`, exactly 90 degrees apart. The
elevation is exactly 45 degrees (`y = -1/sqrt(2)`) and the horizontal part has
the same length, so **a point at height h lands exactly h units away** along the
azimuth. That is the whole projection.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from dataclasses import dataclass, field

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SHADOW_TYPE = 0x7AB50E44
SHADOW_GROUP = 0x2BC2759A
S3D_TYPE = 0x5AD0E817
S3D_GROUP = 0xBADB57F1

# 8, 16, 32, 64, 128 for zoom 0..4, confirmed against the stock masks.
ZOOM_LEVELS = (0, 1, 2, 3, 4)


def mask_size(zoom: int, footprint: float | None = None,
              texels_per_unit_at_zoom4: float = 2.0) -> int:
    """Texture size for a zoom, optionally scaled to the piece's footprint.

    Stock families are not all `8 << zoom`: 0D014E and 08AA30 run 8..128, but
    02F0106 runs 8, 8, 8, 16, 32 because the piece is small. Maxis sized the
    mask by world footprint and capped it, so the baker does the same.
    """
    if footprint is None:
        return 8 << zoom
    wanted = footprint * texels_per_unit_at_zoom4 * (2.0 ** (zoom - 4))
    size = 8
    while size < wanted and size < 128:
        size *= 2
    return size


def mask_instance(base: int, zoom: int) -> int:
    """UpdateShadow builds the key as base + zoom (`ADD EDI,EDX` at 0x0061E53C).

    It is addition, not an OR: the NAM piece logged as 0x57925119 at zoom 4 is
    base 0x57925115, which an OR would never produce.
    """
    return base + zoom


# Sun azimuth per camera rotation. 67.5 degrees plus 90 per rotation, matching
# the two captured samples; the elevation is 45 degrees for all of them.
def shadow_direction(rotation: int) -> tuple[float, float, float]:
    azimuth = math.radians(67.5 + 90.0 * (rotation % 4))
    horizontal = math.sqrt(0.5)
    return (
        math.cos(azimuth) * horizontal,
        -math.sqrt(0.5),
        math.sin(azimuth) * horizontal,
    )


# ---------------------------------------------------------------------------
# QFS / RefPack
# ---------------------------------------------------------------------------


def qfs_decompress(data: bytes) -> bytes:
    """Decode an EA RefPack stream, with or without the DBPF size prefix."""
    pos = 0
    if len(data) > 9 and data[4] == 0x10 and data[5] == 0xFB:
        pos = 4  # DBPF stores the compressed length in front of the stream
    if data[pos] != 0x10 or data[pos + 1] != 0xFB:
        raise ValueError("not a QFS stream")
    uncompressed = int.from_bytes(data[pos + 2:pos + 5], "big")
    pos += 5
    out = bytearray()
    while pos < len(data):
        control = data[pos]
        if control < 0x80:
            a = data[pos + 1]
            pos += 2
            plain = control & 0x03
            copy = ((control & 0x1C) >> 2) + 3
            offset = ((control & 0x60) << 3) + a + 1
        elif control < 0xC0:
            a, b = data[pos + 1], data[pos + 2]
            pos += 3
            plain = (a >> 6) & 0x03
            copy = (control & 0x3F) + 4
            offset = ((a & 0x3F) << 8) + b + 1
        elif control < 0xE0:
            a, b, c = data[pos + 1], data[pos + 2], data[pos + 3]
            pos += 4
            plain = control & 0x03
            copy = ((control & 0x0C) << 6) + c + 5
            offset = ((control & 0x10) << 12) + (a << 8) + b + 1
        elif control < 0xFC:
            plain = ((control & 0x1F) + 1) * 4
            copy = 0
            offset = 0
            pos += 1
        else:
            plain = control & 0x03
            copy = 0
            offset = 0
            pos += 1
        out += data[pos:pos + plain]
        pos += plain
        if copy:
            start = len(out) - offset
            for i in range(copy):
                out.append(out[start + i])
        if control >= 0xFC:
            break
    if len(out) != uncompressed:
        raise ValueError(f"QFS length mismatch: {len(out)} != {uncompressed}")
    return bytes(out)


# ---------------------------------------------------------------------------
# DBPF
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Tgi:
    type: int
    group: int
    instance: int

    def __str__(self) -> str:
        return f"{self.type:08X}-{self.group:08X}-{self.instance:08X}"

    @staticmethod
    def parse(text: str) -> "Tgi":
        parts = text.replace("_", "-").split("-")
        if len(parts) != 3:
            raise ValueError(f"expected TYPE-GROUP-INSTANCE, got {text!r}")
        return Tgi(*(int(p, 16) for p in parts))


class DbpfReader:
    """Reads DBPF 1.0 with a 7.0 index, which is every SC4 package."""

    def __init__(self, path: str, lazy: bool = True):
        """Read the index. With lazy=True the bodies are seeked on demand.

        Slurping whole packages is fine for one model but not for a plugin
        folder: the NAM is about a gigabyte and the bulk baker opens every
        package twice, once to scan and once to bake.
        """
        self.path = path
        self._handle = None
        if lazy:
            self._handle = open(path, "rb")
            self.data = self._handle.read(4096)
            header_only = True
        else:
            with open(path, "rb") as handle:
                self.data = handle.read()
            header_only = False
        if self.data[:4] != b"DBPF":
            raise ValueError(f"{path} is not a DBPF package")
        major, minor = struct.unpack_from("<II", self.data, 4)
        if (major, minor) != (1, 0):
            raise ValueError(f"unsupported DBPF version {major}.{minor}")
        # DBPF 1.0 header: index major at 0x20, count/offset/size at 0x24/28/2C,
        # index minor at 0x3C. Entries are 20 bytes, or 24 when the minor
        # version is 2 and an instance-high dword is present.
        index_major = struct.unpack_from("<I", self.data, 32)[0]
        count, offset, size = struct.unpack_from("<III", self.data, 36)
        index_minor = struct.unpack_from("<I", self.data, 60)[0]
        if index_major != 7:
            raise ValueError(f"unsupported index major version {index_major}")
        stride = 24 if index_minor == 2 else 20
        if header_only:
            self._handle.seek(offset)
            index = self._handle.read(count * stride)
        else:
            index = self.data[offset:offset + count * stride]
        self.entries: dict[Tgi, tuple[int, int]] = {}
        for i in range(count):
            base = i * stride
            if stride == 24:
                t, g, inst, _high, location, length = struct.unpack_from("<IIIIII", index, base)
            else:
                t, g, inst, location, length = struct.unpack_from("<IIIII", index, base)
            self.entries[Tgi(t, g, inst)] = (location, length)
        self._compressed: set[Tgi] | None = None

    def close(self) -> None:
        if self._handle is not None:
            self._handle.close()
            self._handle = None

    def raw(self, tgi: Tgi) -> bytes:
        location, length = self.entries[tgi]
        if self._handle is not None:
            self._handle.seek(location)
            return self._handle.read(length)
        return self.data[location:location + length]

    def read(self, tgi: Tgi) -> bytes:
        payload = self.raw(tgi)
        if len(payload) > 9 and payload[4] == 0x10 and payload[5] == 0xFB:
            return qfs_decompress(payload)
        return payload

    def find(self, type_id: int | None = None, group_id: int | None = None) -> list[Tgi]:
        return sorted(
            (tgi for tgi in self.entries
             if (type_id is None or tgi.type == type_id)
             and (group_id is None or tgi.group == group_id)),
            key=lambda t: (t.type, t.group, t.instance),
        )


def write_dbpf(path: str, entries: dict[Tgi, bytes]) -> None:
    """Write an uncompressed DBPF 1.0 / index 7.0 package.

    Uncompressed is deliberate: SC4 reads both, the masks are a few hundred
    bytes each, and leaving QFS out keeps the output byte-for-byte reproducible
    so a rebake with unchanged inputs produces an identical file.
    """
    header = bytearray(96)
    header[0:4] = b"DBPF"
    struct.pack_into("<II", header, 4, 1, 0)  # version 1.0
    # Index 7.0 with 20-byte entries. Every Maxis and NAM package on disk
    # writes minor 0 here; minor 1 is a later-EA variant that SC4 Reader will
    # not parse, so a package written that way shows its entries as raw text
    # even though the FSH inside is byte-identical.
    struct.pack_into("<I", header, 32, 7)
    struct.pack_into("<I", header, 60, 0)
    body = bytearray()
    index = []
    offset = 96
    for tgi, payload in sorted(entries.items(), key=lambda kv: (kv[0].type, kv[0].group, kv[0].instance)):
        index.append((tgi, offset + len(body), len(payload)))
        body += payload
    index_offset = 96 + len(body)
    index_bytes = bytearray()
    for tgi, location, length in index:
        index_bytes += struct.pack("<IIIII", tgi.type, tgi.group, tgi.instance, location, length)
    struct.pack_into("<III", header, 36, len(index), index_offset, len(index_bytes))
    with open(path, "wb") as handle:
        handle.write(header)
        handle.write(body)
        handle.write(index_bytes)


# ---------------------------------------------------------------------------
# FSH
# ---------------------------------------------------------------------------

FSH_DXT1 = 0x60
FSH_DXT3 = 0x61


@dataclass
class FshImage:
    width: int
    height: int
    alpha: bytearray  # width*height, 0..255; RGB is black by definition
    name: str = ""
    # The stock masks disagree with each other here - (8,0) dominates, but
    # (8,176), (8,64) and (1,0) all occur across families and LODs - so the
    # field is not read by the game. (8,0) is written to match the majority.
    misc: tuple[int, int] = (8, 0)


def _dxt3_encode_alpha(image: FshImage) -> bytes:
    """Encode a black RGB / explicit-alpha DXT3 payload.

    A shadow mask has a constant black colour, so the colour half of every
    block is fixed and no colour fitting is needed: this is exact, not lossy.
    The stock masks use colour0 = 0x0000 and colour1 = 0x0001 with all indices
    zero, which is what is reproduced here.
    """
    if image.width % 4 or image.height % 4:
        raise ValueError("DXT3 needs multiples of 4")
    out = bytearray()
    for block_y in range(0, image.height, 4):
        for block_x in range(0, image.width, 4):
            alpha_bits = bytearray(8)
            for row in range(4):
                for column in range(4):
                    value = image.alpha[(block_y + row) * image.width + block_x + column]
                    nibble = (value * 15 + 127) // 255
                    index = row * 4 + column
                    if index % 2 == 0:
                        alpha_bits[index // 2] |= nibble
                    else:
                        alpha_bits[index // 2] |= nibble << 4
            out += alpha_bits
            out += struct.pack("<HHI", 0x0000, 0x0001, 0x00000000)
    return bytes(out)


def _dxt3_decode_alpha(data: bytes, width: int, height: int) -> bytearray:
    alpha = bytearray(width * height)
    pos = 0
    for block_y in range(0, height, 4):
        for block_x in range(0, width, 4):
            bits = data[pos:pos + 8]
            pos += 16
            for row in range(4):
                for column in range(4):
                    index = row * 4 + column
                    byte = bits[index // 2]
                    nibble = byte & 0x0F if index % 2 == 0 else byte >> 4
                    alpha[(block_y + row) * width + block_x + column] = nibble * 17
    return alpha


def build_fsh(image: FshImage, directory_id: bytes = b"G264",
              payload: bytes | None = None) -> bytes:
    """Build a single-element, single-image DXT3 FSH.

    The layout mirrors the stock masks exactly, which is why `selftest`
    reproduces `0D014E00` byte for byte: a 16-byte SHPI header, one 8-byte
    directory entry, the 8-byte "Buy ERTS" tag, then a block of
    `16 + data + 16` bytes, then a 16-byte 0x70 name attachment.
    """
    if payload is None:
        payload = _dxt3_encode_alpha(image)
    entry_name = (image.name or "0000")[:4].ljust(4, "0").encode("ascii")
    block_size = 16 + len(payload) + 16
    total = 16 + 8 + 8 + block_size + 16

    out = bytearray()
    out += b"SHPI"
    out += struct.pack("<II", total, 1)
    out += directory_id
    out += entry_name
    out += struct.pack("<I", 32)  # the single entry always starts at 0x20
    out += b"Buy ERTS"
    out += struct.pack("<I", FSH_DXT3 | (block_size << 8))
    out += struct.pack("<HH", image.width, image.height)
    out += struct.pack("<HH", image.width // 2, image.height // 2)
    out += struct.pack("<HH", image.misc[0], image.misc[1])
    out += payload
    out += bytes(16)
    out += struct.pack("<I", 0x70)
    out += (image.name or "").encode("ascii")[:12].ljust(12, b"\0")
    assert len(out) == total, (len(out), total)
    return bytes(out)


def parse_fsh(data: bytes) -> list[FshImage]:
    if data[:4] != b"SHPI":
        raise ValueError("not an FSH")
    count = struct.unpack_from("<I", data, 8)[0]
    images = []
    for i in range(count):
        name, offset = struct.unpack_from("<4sI", data, 16 + i * 8)
        code = data[offset]
        block = int.from_bytes(data[offset + 1:offset + 4], "little")
        width, height = struct.unpack_from("<HH", data, offset + 4)
        misc = struct.unpack_from("<HH", data, offset + 12)
        body = data[offset + 16:offset + 16 + (block - 32 if block else len(data))]
        if code == FSH_DXT3:
            alpha = _dxt3_decode_alpha(body, width, height)
        elif code == FSH_DXT1:
            # DXT1 blocks are 8 bytes; 1-bit alpha lives in the colour indices.
            alpha = bytearray(width * height)
            pos = 0
            for by in range(0, height, 4):
                for bx in range(0, width, 4):
                    c0, c1, idx = struct.unpack_from("<HHI", body, pos)
                    pos += 8
                    for row in range(4):
                        for column in range(4):
                            code2 = (idx >> (2 * (row * 4 + column))) & 3
                            opaque = not (c0 <= c1 and code2 == 3)
                            alpha[(by + row) * width + bx + column] = 255 if opaque else 0
        else:
            raise ValueError(f"unsupported FSH record 0x{code:02X}")
        images.append(FshImage(width, height, alpha, name.decode("ascii", "replace"), misc))
    return images


# ---------------------------------------------------------------------------
# S3D
# ---------------------------------------------------------------------------


@dataclass
class S3dMesh:
    positions: list[tuple[float, float, float]] = field(default_factory=list)
    indices: list[int] = field(default_factory=list)


def parse_s3d(data: bytes) -> list[S3dMesh]:
    """Extract the meshes used by an S3D model's animation frames.

    S3D block length fields are not safe offsets (stock files contain values
    larger than the entire decoded entry), so blocks must be decoded in their
    defined order. ANIM supplies the essential mapping between VERT, INDX and
    PRIM blocks; pairing those arrays by ordinal loses valid geometry whenever
    their group counts differ.
    """
    if data[:4] != b"3DMD":
        raise ValueError("not an S3D model")

    cursor = 8

    def take_tag(expected: bytes) -> None:
        nonlocal cursor
        if data[cursor:cursor + 4] != expected:
            raise ValueError(f"expected {expected.decode()} at 0x{cursor:X}")
        cursor += 8  # tag and advisory block length

    take_tag(b"HEAD")
    major, minor = struct.unpack_from("<HH", data, cursor)
    cursor += 4
    if major != 1 or minor not in range(1, 6):
        raise ValueError(f"unsupported S3D version {major}.{minor}")

    take_tag(b"VERT")
    vertex_groups = []
    group_count = struct.unpack_from("<I", data, cursor)[0]
    cursor += 4
    for _ in range(group_count):
        _flags, count = struct.unpack_from("<HH", data, cursor)
        cursor += 4
        if minor >= 4:
            vertex_format = struct.unpack_from("<I", data, cursor)[0]
            cursor += 4
            coords, colours, textures = _vertex_format_counts(vertex_format)
            stride = coords * 12 + colours * 4 + textures * 8
        else:
            _vertex_format, stride = struct.unpack_from("<HH", data, cursor)
            cursor += 4
        if stride < 12 or cursor + count * stride > len(data):
            raise ValueError("invalid S3D vertex buffer")
        vertex_groups.append([
            struct.unpack_from("<fff", data, cursor + index * stride)
            for index in range(count)
        ])
        cursor += count * stride

    take_tag(b"INDX")
    index_groups = []
    group_count = struct.unpack_from("<I", data, cursor)[0]
    cursor += 4
    for _ in range(group_count):
        _flags, stride, count = struct.unpack_from("<HHH", data, cursor)
        cursor += 6
        if stride != 2:
            raise ValueError(f"unsupported S3D index stride {stride}")
        index_groups.append(list(struct.unpack_from(f"<{count}H", data, cursor)))
        cursor += count * stride

    take_tag(b"PRIM")
    primitive_groups = []
    group_count = struct.unpack_from("<I", data, cursor)[0]
    cursor += 4
    for _ in range(group_count):
        count = struct.unpack_from("<H", data, cursor)[0]
        cursor += 2
        group = []
        for _ in range(count):
            primitive_type, first, length = struct.unpack_from("<III", data, cursor)
            cursor += 12
            group.append((primitive_type, first, length))
        primitive_groups.append(group)

    # Material records are irrelevant to silhouette generation. ANIM is the
    # next required block and provides the buffer indices for each mesh/frame.
    cursor = data.find(b"ANIM", cursor)
    if cursor < 0:
        raise ValueError("S3D has no ANIM block")
    take_tag(b"ANIM")
    frame_count, _rate, _mode = struct.unpack_from("<HHH", data, cursor)
    cursor += 6
    _flags, _displacement = struct.unpack_from("<If", data, cursor)
    cursor += 8
    mesh_count = struct.unpack_from("<H", data, cursor)[0]
    cursor += 2

    mappings = []
    for _ in range(mesh_count):
        name_length, _flags = struct.unpack_from("<BB", data, cursor)
        cursor += 2 + name_length
        for _ in range(frame_count):
            mappings.append(struct.unpack_from("<HHHH", data, cursor)[:3])
            cursor += 8

    meshes = []
    seen = set()
    for vertex_index, index_index, primitive_index in mappings:
        key = (vertex_index, index_index, primitive_index)
        if key in seen:
            continue
        seen.add(key)
        if vertex_index >= len(vertex_groups) or index_index >= len(index_groups):
            continue
        indices = index_groups[index_index]
        if primitive_index < len(primitive_groups) and primitive_groups[primitive_index]:
            selected = []
            for _kind, first, length in primitive_groups[primitive_index]:
                selected.extend(indices[first:first + length])
            indices = selected
        if len(indices) < 3:
            continue
        meshes.append(S3dMesh(vertex_groups[vertex_index], indices))
    return meshes


def _vertex_format_counts(vertex_format: int) -> tuple[int, int, int]:
    if vertex_format & 0x80000000:
        return vertex_format & 3, (vertex_format >> 8) & 3, (vertex_format >> 14) & 3
    return {
        1: (1, 1, 0),
        2: (1, 0, 1),
        3: (1, 0, 2),
        10: (1, 1, 1),
        11: (1, 1, 2),
    }.get(vertex_format, (1, 0, 1))


# ---------------------------------------------------------------------------
# Projection and rasterisation
# ---------------------------------------------------------------------------


def project(meshes: list[S3dMesh], rotation: int,
            minimum_height: float = 0.25) -> list[tuple[tuple[float, float], ...]]:
    """Flatten every triangle onto y = 0 along the shadow direction.

    Elevation is 45 degrees and the horizontal component has the same length as
    the vertical one, so a vertex at height h simply slides h units along the
    azimuth. Vertices below the ground plane are left where they are, which is
    what keeps tunnel mouths and sunken pieces from projecting upwards.

    Triangles that lie entirely on the ground plane are dropped. Most network
    piece models are a flat quad covering the whole tile - the road surface
    itself - and projecting those produced a solid black square over 90% of the
    tile. Ground-level geometry casts no shadow; only what stands above it
    does, so `minimum_height` is what separates a viaduct deck from its
    roadway.
    """
    dx, dy, dz = shadow_direction(rotation)
    scale = 1.0 / abs(dy)
    triangles = []
    for mesh in meshes:
        for i in range(0, len(mesh.indices) - 2, 3):
            corner = []
            elevated = False
            for offset in range(3):
                index = mesh.indices[i + offset]
                if index >= len(mesh.positions):
                    corner = []
                    break
                x, y, z = mesh.positions[index]
                if y > minimum_height:
                    elevated = True
                travel = max(y, 0.0) * scale
                corner.append((x + dx * travel, z + dz * travel))
            if len(corner) == 3 and elevated:
                triangles.append(tuple(corner))
    return triangles


def rasterise(triangles, size: int, bounds, supersample: int = 4, dilate: int = 1) -> bytearray:
    """Rasterise projected triangles into an antialiased alpha coverage map.

    Coverage is exact along x and supersampled along z, so there is no
    full-resolution pixel loop: each sub-scanline contributes one span, written
    into a per-row difference array in constant time and prefix-summed once.
    That is what makes baking a whole plugin folder practical in plain Python.

    The result is then dilated by one texel. A shadow that is slightly too
    generous still reads as a shadow, while one that drops a viaduct support
    reads as a bug, so the bias is deliberate.
    """
    minimum_x, minimum_z, maximum_x, maximum_z = bounds
    scale_x = size / max(maximum_x - minimum_x, 1e-6)
    scale_z = size / max(maximum_z - minimum_z, 1e-6)
    rows = [None] * size
    share = 1.0 / supersample

    for (ax, az), (bx, bz), (cx, cz) in triangles:
        points = sorted((((ax - minimum_x) * scale_x, (az - minimum_z) * scale_z),
                         ((bx - minimum_x) * scale_x, (bz - minimum_z) * scale_z),
                         ((cx - minimum_x) * scale_x, (cz - minimum_z) * scale_z)),
                        key=lambda p: p[1])
        (x0, z0), (x1, z1), (x2, z2) = points
        if z2 - z0 < 1e-9:
            continue
        first = max(int(z0 * supersample), 0)
        last = min(int(z2 * supersample) + 1, size * supersample)
        for step in range(first, last):
            z = (step + 0.5) / supersample
            if z < z0 or z >= z2:
                continue
            # The long edge, plus whichever short edge this scanline crosses.
            edge_a = x0 + (x2 - x0) * (z - z0) / (z2 - z0)
            if z < z1:
                edge_b = x0 + (x1 - x0) * (z - z0) / (z1 - z0) if z1 - z0 > 1e-9 else x0
            else:
                edge_b = x1 + (x2 - x1) * (z - z1) / (z2 - z1) if z2 - z1 > 1e-9 else x1
            left, right = (edge_a, edge_b) if edge_a <= edge_b else (edge_b, edge_a)
            left = max(left, 0.0)
            right = min(right, float(size))
            if right <= left:
                continue
            row = int(z)
            if row < 0 or row >= size:
                continue
            accumulator = rows[row]
            if accumulator is None:
                accumulator = rows[row] = [0.0] * (size + 2)
            _add_span(accumulator, left, right, share, size)

    alpha = bytearray(size * size)
    for row in range(size):
        accumulator = rows[row]
        if accumulator is None:
            continue
        running = 0.0
        base = row * size
        for column in range(size):
            running += accumulator[column]
            if running > 0.002:
                alpha[base + column] = 255 if running >= 1.0 else int(running * 255.0)
    for _ in range(dilate):
        alpha = _dilate(alpha, size)
    return alpha


def _add_span(accumulator, left: float, right: float, share: float, size: int) -> None:
    """Add one scanline span to a row's difference array with exact x coverage."""
    first = int(left)
    last = int(right)
    if first == last:
        accumulator[first] += (right - left) * share
        accumulator[first + 1] -= (right - left) * share
        return
    head = (first + 1 - left) * share
    accumulator[first] += head
    accumulator[first + 1] += share - head
    if last < size:
        tail = (right - last) * share
        accumulator[last] += tail - share
        accumulator[last + 1] -= tail
    else:
        accumulator[size] -= share


def _dilate(alpha: bytearray, size: int) -> bytearray:
    out = bytearray(alpha)
    for y in range(size):
        for x in range(size):
            if alpha[y * size + x]:
                continue
            best = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < size and 0 <= nx < size:
                        best = max(best, alpha[ny * size + nx])
            if best:
                out[y * size + x] = best // 2
    return out

# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

# The stock 8x8 mask 7AB50E44-2BC2759A-0D014E00, uncompressed in SimCity_2.dat.
# It is the reference the builder is checked against.
STOCK_0D014E00 = bytes.fromhex(
    "53485049900000000100000047323634304430312000000042757920455254"
    "5361600000080008000400040008 00B000A1ED609A304A204A0000010000000000"
    "886A4BA5BC378ACD0000010000000000304A609AA1EDD3DE0000010000000000"
    "BC474C95887ACB020000010000000000000000000000000000000000000000"
    "0070000000304430313445303400000000".replace(" ", "")
)


def command_selftest(_args) -> int:
    images = parse_fsh(STOCK_0D014E00)
    assert len(images) == 1, images
    image = images[0]
    print(f"stock 0D014E00: {image.width}x{image.height} name={image.name!r} misc={image.misc}")
    rebuilt = build_fsh(FshImage(image.width, image.height, image.alpha, "0D014E04", image.misc),
                        directory_id=b"G264")
    # The directory entry name is the first four characters of the label.
    reference = bytearray(STOCK_0D014E00)
    if rebuilt == bytes(reference):
        print("build_fsh reproduces the stock mask byte for byte")
        return 0
    print(f"MISMATCH: rebuilt {len(rebuilt)} bytes vs stock {len(reference)}")
    for i, (a, b) in enumerate(zip(rebuilt, reference)):
        if a != b:
            print(f"  first difference at 0x{i:02X}: got 0x{a:02X}, stock 0x{b:02X}")
            break
    return 1


def _checker_alpha(size: int) -> bytearray:
    """An unmistakable test pattern: a bordered diagonal wedge."""
    alpha = bytearray(size * size)
    for y in range(size):
        for x in range(size):
            edge = x < size // 8 or y < size // 8 or x >= size - size // 8 or y >= size - size // 8
            wedge = x + y > size
            alpha[y * size + x] = 255 if (edge or wedge) else 0
    return alpha


def command_testmask(args) -> int:
    base = int(args.instance, 16)
    entries = {}
    for zoom in ZOOM_LEVELS:
        size = mask_size(zoom)
        instance = mask_instance(base, zoom)
        image = FshImage(size, size, _checker_alpha(size), f"{instance:08X}")
        entries[Tgi(SHADOW_TYPE, SHADOW_GROUP, instance)] = build_fsh(image)
    write_dbpf(args.output, entries)
    print(f"wrote {len(entries)} masks to {args.output}")
    for tgi in sorted(entries, key=lambda t: t.instance):
        print(f"  {tgi}  {mask_size(tgi.instance & 0xF)}px")
    return 0


def command_bake(args) -> int:
    reader = DbpfReader(args.package)
    model = Tgi.parse(args.model) if "-" in args.model else Tgi(S3D_TYPE, S3D_GROUP, int(args.model, 16))
    meshes = parse_s3d(reader.read(model))
    print(f"{model}: {len(meshes)} mesh groups, "
          f"{sum(len(m.positions) for m in meshes)} vertices")
    base = int(args.instance, 16)
    entries = {}
    manifest = {"source": str(model), "rotation": args.rotation, "masks": []}
    triangles = project(meshes, args.rotation)
    if not triangles:
        print("no triangles projected; nothing to bake")
        return 1
    xs = [p[0] for tri in triangles for p in tri]
    zs = [p[1] for tri in triangles for p in tri]
    pad = args.padding
    bounds = (min(xs) - pad, min(zs) - pad, max(xs) + pad, max(zs) + pad)
    for zoom in ZOOM_LEVELS:
        size = mask_size(zoom)
        instance = mask_instance(base, zoom)
        alpha = rasterise(triangles, size, bounds, supersample=args.supersample)
        image = FshImage(size, size, alpha, f"{instance:08X}")
        entries[Tgi(SHADOW_TYPE, SHADOW_GROUP, instance)] = build_fsh(image)
        covered = sum(1 for a in alpha if a) * 100 // (size * size)
        manifest["masks"].append({"zoom": zoom, "instance": f"{instance:08X}",
                                  "size": size, "coverage_percent": covered})
        print(f"  zoom {zoom}: {size}x{size}, {covered}% covered -> {instance:08X}")
    manifest["bounds"] = {"min_x": bounds[0], "min_z": bounds[1],
                          "max_x": bounds[2], "max_z": bounds[3]}
    write_dbpf(args.output, entries)
    with open(args.output + ".json", "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)
    print(f"wrote {args.output} and {args.output}.json")
    return 0


def command_show(args) -> int:
    reader = DbpfReader(args.package)
    tgi = Tgi.parse(args.tgi)
    for image in parse_fsh(reader.read(tgi)):
        print(f"{tgi}: {image.width}x{image.height} name={image.name!r} misc={image.misc}")
        step = max(1, image.width // 64)
        for y in range(0, image.height, step):
            row = "".join(" .:-=+*#%@"[min(9, image.alpha[y * image.width + x] * 10 // 256)]
                          for x in range(0, image.width, step))
            print("  " + row)
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("selftest", help="check the FSH writer against a stock mask")

    p = sub.add_parser("testmask", help="emit a recognisable mask set for one piece id")
    p.add_argument("instance", help="shadow instance, e.g. 5DA40200 (low nibble is the zoom)")
    p.add_argument("-o", "--output", default="shadow-testmask.dat")

    p = sub.add_parser("bake", help="bake masks from an S3D model")
    p.add_argument("package", help="DBPF package holding the model")
    p.add_argument("model", help="model instance or full TYPE-GROUP-INSTANCE")
    p.add_argument("instance", help="shadow instance base, low nibble ignored")
    p.add_argument("-o", "--output", default="shadow-baked.dat")
    p.add_argument("-r", "--rotation", type=int, default=0, choices=(0, 1, 2, 3))
    p.add_argument("-p", "--padding", type=float, default=0.5)
    p.add_argument("-s", "--supersample", type=int, default=4)

    p = sub.add_parser("show", help="print an FSH mask as ASCII")
    p.add_argument("package")
    p.add_argument("tgi")

    args = parser.parse_args(argv)
    return {
        "selftest": command_selftest,
        "testmask": command_testmask,
        "bake": command_bake,
        "show": command_show,
    }[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
