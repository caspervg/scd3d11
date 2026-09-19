#!/usr/bin/env python3
"""Turn a SC4ShadowDiag.log trace into the Phase 1 comparison.

The trace answers one question: when the native projector produces a bad
shadow, is the mesh at fault or are the model, transform, LOD, bounds or
texture arguments wrong? That only shows up by putting a stock power pole
next to a network piece and a True3D prop in the same table, which is what
this script builds.

Usage: summarize-shadow-diag.py <SC4ShadowDiag.log> [--all]
"""

from __future__ import annotations

import collections
import re
import sys

# Occupant type values the shadow code and the network factory already name.
KNOWN_TYPES = {
    0x2890D4DE: "power pole",
    0xC772BF98: "cSC4NetworkOccupant",
    0x49C1A034: "prebuilt network model",
    0x49CC1BCD: "bridge network",
    0x08A4BD52: "tunnel network",
    0x6534284A: "exemplar",
}

# CreateOccupantShadow callers, so a record says which path reached it.
KNOWN_CALLERS = {
    0x0049445D: "AddModels power-pole case",
    0x00491A00: "AddModels helper 0x004919F0",
    0x00491A40: "AddModels helper 0x00491A30",
}

FIELD = re.compile(r"(\w+(?:\.\w+)?)=(\[[^\]]*\]|\([^)]*\)|\S+)")


def parse(path):
    records = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                if line.startswith("# totals"):
                    records.append(("totals", {"raw": line}))
                continue
            kind, _, rest = line.partition(" ")
            fields = dict(FIELD.findall(rest))
            fields["raw"] = line
            records.append((kind, fields))
    return records


def as_int(value, default=0):
    if value is None:
        return default
    try:
        return int(value, 0) if isinstance(value, str) else int(value)
    except ValueError:
        return default


def describe_type(value):
    return KNOWN_TYPES.get(value, "unknown")


def bounds_span(text):
    """'4[a,b,c..d,e,f]' -> (count, (dx, dy, dz)) so extents can be compared."""
    match = re.match(r"(\d+)\[(.*)\]$", text or "")
    if not match:
        return None, None
    count = int(match.group(1))
    low, _, high = match.group(2).partition("..")
    try:
        low = [float(v) for v in low.split(",")]
        high = [float(v) for v in high.split(",")]
    except ValueError:
        return count, None
    if len(low) != len(high):
        return count, None
    return count, tuple(h - l for l, h in zip(low, high))


def format_span(span):
    if span is None:
        return "-"
    return " x ".join(f"{v:.3g}" for v in span)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    show_all = "--all" in argv[2:]
    records = parse(path)

    occupants = {}
    add_shadows = []
    net_shadows = []
    net_maps = []
    totals = None
    for kind, fields in records:
        if kind == "occupant":
            occupants[as_int(fields.get("seq"))] = fields
        elif kind == "addshadow":
            add_shadows.append(fields)
        elif kind == "netshadow":
            net_shadows.append(fields)
        elif kind == "netmap":
            net_maps.append(fields)
        elif kind == "totals":
            totals = fields["raw"]

    print(f"trace: {path}")
    print(
        f"records: occupant={len(occupants)} addshadow={len(add_shadows)} "
        f"netshadow={len(net_shadows)} netmap={len(net_maps)}"
    )
    if totals:
        print(totals)

    # ---- Path A: what reached the mesh projector -------------------------
    print("\n== AddShadow, grouped by occupant class ==")
    if not add_shadows:
        print("  none. CreateOccupantShadow never reached AddShadow in this run.")
    groups = collections.defaultdict(list)
    for record in add_shadows:
        context = occupants.get(as_int(record.get("seq")), {})
        key = (context.get("vtbl", "?"), as_int(record.get("type") or context.get("type")))
        groups[key].append((record, context))

    header = (
        f"{'class vtable':<12}{'type':<12}{'what':<24}{'n':>4}{'zoom':>6}{'meshes':>8}"
        f"{'verts':>8}{'height':>9}{'pos extent (x,y,z)':>26}{'uv extent':>18}{'scale':>8}{'decal':>7}"
    )
    print(header)
    print("-" * len(header))
    for (vtable, type_value), entries in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        record, context = entries[0]
        counts, span = bounds_span(record.get("pos"))
        _, uv_span = bounds_span(record.get("uv"))
        meshes = len({as_int(r.get("mesh")) for r, _ in entries})
        zooms = sorted({as_int(c.get("zoom"), -1) for _, c in entries if c})
        decals = {as_int(r.get("decal"), -1) for r, _ in entries}
        height = span[1] if span and len(span) > 2 else None
        print(
            f"{vtable:<12}{type_value:#010x}  {describe_type(type_value):<24}{len(entries):>4}"
            f"{','.join(str(z) for z in zooms[:3]):>6}{meshes:>8}{counts or 0:>8}"
            f"{(f'{height:.3g}' if height is not None else '-'):>9}"
            f"{format_span(span):>26}{format_span(uv_span):>18}"
            f"{record.get('xf.s', '-'):>8}"
            f"{('ok' if decals - {-1} else 'REJECT'):>7}"
        )

    # A pole's mesh is a flat card; anything with real height is the failure
    # mode the investigation predicted, and this is where that shows up.
    print("\n== per-class detail ==")
    for (vtable, type_value), entries in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        record, context = entries[0]
        print(f"\n  class vtable {vtable}  type {type_value:#010x} ({describe_type(type_value)})")
        print(f"    model key      {context.get('key', '-')}")
        caller = as_int(context.get("caller"), 0)
        print(f"    caller         {context.get('caller', '-')} {KNOWN_CALLERS.get(caller, '')}")
        print(f"    zoom/rot       {context.get('zoom', '-')}/{context.get('rot', '-')}")
        print(f"    box source     {record.get('box', '-')}  values {record.get('boxv', '-')}")
        print(f"    positions      {record.get('pos', '-')}")
        print(f"    uvs            {record.get('uv', '-')}")
        print(f"    transform      m={record.get('xf.m', '-')}")
        print(f"                   t={record.get('xf.t', '-')} s={record.get('xf.s', '-')} "
              f"flags={record.get('xf.flags', '-')}")
        print(f"    texture bind   {record.get('bind', '-')}")
        print(f"    decals         {sorted({as_int(r.get('decal'), -1) for r, _ in entries})}")
        if show_all:
            for sample, _ in entries:
                print(f"      {sample['raw']}")

    # ---- Path B: prebuilt network pieces ---------------------------------
    print("\n== UpdateShadow (prebuilt network pieces) ==")
    if not net_shadows:
        print("  none. No cSC4NetworkOccupantWithPreBuiltModel drew in this run,")
        print("  or ShadowQuality was 2 or lower.")
    else:
        outcomes = collections.Counter()
        for record in net_shadows:
            first = record.get("decal0", "?")
            outcome = "created" if "->-" not in first.replace("->-1", "X").replace("->-2", "X") else "cleared"
            after = first.split("->")[-1]
            if after == "-1":
                outcome = "disabled (-1)"
            elif after == "-2":
                outcome = "no texture (-2)"
            elif after.lstrip("-").isdigit():
                outcome = "decal created"
            outcomes[(record.get("vtbl", "?"), record.get("w13c", "?"), outcome)] += 1
        print(f"{'vtable':<12}{'class+13C':<12}{'outcome':<20}{'n':>5}")
        print("-" * 49)
        for (vtable, flags, outcome), count in outcomes.most_common():
            print(f"{vtable:<12}{flags:<12}{outcome:<20}{count:>5}")
        print("\n  sample:")
        for record in net_shadows[:5]:
            print(f"    {record['raw']}")

    print("\n== MapShadowTexture ==")
    if not net_maps:
        print("  never called. UpdateShadow bailed before the mapping, so the")
        print("  network flags, quality or terrain gate is what to look at first.")
    else:
        mapped = collections.Counter()
        for record in net_maps:
            mapped[(record.get("a1", "?"), record.get("a3", "?"), record.get("instance", "?"))] += 1
        print(f"{'a1 (piece)':<14}{'a3 (config)':<14}{'-> instance':<14}{'n':>5}")
        print("-" * 47)
        for (a1, a3, instance), count in mapped.most_common(30):
            note = "  ABORT" if as_int(instance) == 0 else ""
            print(f"{a1:<14}{a3:<14}{instance:<14}{count:>5}{note}")
        zeros = sum(c for (_, _, i), c in mapped.items() if as_int(i) == 0)
        print(f"\n  {zeros} of {sum(mapped.values())} mappings returned 0 (no shadow texture).")

    # ---- The Phase 1 verdict --------------------------------------------
    print("\n== reading ==")
    poles = [g for k, g in groups.items() if k[1] == 0x2890D4DE]
    others = [(k, g) for k, g in groups.items() if k[1] != 0x2890D4DE]
    if poles and others:
        pole_record = poles[0][0][0]
        _, pole_span = bounds_span(pole_record.get("pos"))
        pole_height = pole_span[1] if pole_span and len(pole_span) > 2 else None
        print(f"  power pole mesh height (y extent): {pole_height}")
        for key, entries in others:
            record = entries[0][0]
            _, span = bounds_span(record.get("pos"))
            height = span[1] if span and len(span) > 2 else None
            verdict = "flat like the pole" if (height is not None and pole_height is not None
                                               and height <= pole_height * 2) else "upright, will smear"
            print(f"  {describe_type(key[1]):<24} y extent {height} -> {verdict}")
    else:
        print("  need a power pole and at least one other class in the same trace")
        print("  to compare. Load a city that has both, or widen the camera sweep.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
