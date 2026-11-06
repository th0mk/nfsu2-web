#!/usr/bin/env python3
"""Open the Construction Tunnel using the owner's original PC game assets.

python3 tools/open_tunnel.py /path/to/original/TRACKS /path/to/game/MODPACKS/open-tunnel/TRACKS

Writes L4RA.BUN, STREAML4RA.BUN and ROUTESL4RA/PathsFreeRoam.bin to a NEW
output directory. Keep the original files in game/TRACKS: never overwrite them.
Restart the server after installing the pack, then enable Open Construction Tunnel
in the Mods tab (open_tunnel = on in settings.ini) and launch the game. It is off
by default; missing pack files fall back to the original map. To migrate a prior
installation, move its three patched files into MODPACKS/open-tunnel/TRACKS and
restore all three original TRACKS files from the backup first.
No downloaded mod or redistributed game assets are required.
"""

from pathlib import Path
import argparse
import hashlib
import json
import struct
import tempfile


def chunks(b, s=0, e=None):
    if e is None:
        e = len(b)
    while s + 8 <= e:
        k, n = struct.unpack_from("<II", b, s)
        p = s + 8
        if p + n > e:
            raise ValueError("chunk extends beyond its container")
        yield k, p, n
        s = p + n
    if s != e:
        raise ValueError("truncated chunk header")


def leaves(b, s=0, e=None):
    for k, p, n in chunks(b, s, e):
        if k >> 28 == 8:
            yield from leaves(b, p, p + n)
        else:
            yield k, p, n


def scenes(b):
    out = {}

    def walk(s, e):
        for k, p, n in chunks(b, s, e):
            if k == 0x80034100:
                ls = {k: b[p : p + n] for k, p, n in chunks(b, p, p + n)}
                sec = struct.unpack_from("<I", ls[0x34101], 12)[0]
                out[sec] = ls
            elif k >> 28 == 8:
                walk(p, p + n)

    walk(0, len(b))
    return out


def u32(b, o=0):
    return struct.unpack_from("<I", b, o)[0]


def u16(b, o=0):
    return struct.unpack_from("<H", b, o)[0]


def pack(k, b):
    return struct.pack("<II", k, len(b)) + b


# Construction North/South terrain changed by the original drag layout, including
# the approaches. Event scenes retain their indices for the city's group overrides.
TARGET = {726, 729, 732, 733, 734, 1428, 1433, 1435, 1438, 1522, 1524, 1526, 1528, 1536}
# Career gates across the two ends of this route (race gates use other rows).
GATE_ROWS = {1810: {24, 25, 26, 27}, 1416: {196, 197, 198, 199, 200}}
# Permanent side arrows in the closed tunnel, not members of any race group.
TUNNEL_ARROWS = {1408: set(range(13, 19)), 1506: set(range(35, 39)), 1516: {103, 104}}
GATE_SECTIONS = {s + 20 for s in GATE_ROWS}
TARGET |= GATE_SECTIONS
GATE_BOXES = [(-200, 100, -165, 112), (200, -1066, 245, -1050)]
STANDALONE_ARROW_SECTIONS = {1506}
TUNNEL_TERRAIN = {1428, 1433, 1438, 1526, 1528, 1536}
ROADWORK_SCENES = {706, 709, 712, 713, 714, 1415, 1504, 1508}
ROADWORK_MODELS = {
    b"XS_OrangeBumper_1a_00",
    b"XW_ConstrucWallA_1a_00",
    b"XW_YellowBarrierA_1b_00",
    b"XO_WorkZoneA_1a_00",
    b"XO_WorkZoneTextA_1a_00",
    b"XO_ConstLight_1a_00",
    b"XO_RubbleA_1a_00",
    b"XV_Digger_1a_00",
    b"XO_DumpTruckBody_1a_00",
    b"XO_SignHardhat_1a_00",
    b"XO_IP_Garbagecan_1a_00",
}


def removed_roadwork(section, city, drag):
    """Only construction props absent from the open layout, never event barriers."""
    if section not in ROADWORK_SCENES:
        return []
    present = {(type_name(drag, row), bytes(row[:24])) for row in instances(drag)}
    return [
        i
        for i, row in enumerate(instances(city))
        if type_name(city, row) in ROADWORK_MODELS
        and (type_name(city, row), bytes(row[:24])) not in present
    ]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def leaf_map(b):
    return {k: b[p : p + n] for k, p, n in chunks(b)}


def instances(s):
    b = s[0x34103]
    o = 0
    while b[o : o + 4] == b"\x11" * 4:
        o += 4
    require((len(b) - o) % 64 == 0, "invalid scenery instance array")
    return [bytearray(b[i : i + 64]) for i in range(o, len(b), 64)]


def type_name(s, a):
    t = u16(a, 24)
    require((t + 1) * 68 <= len(s[0x34102]), "invalid scenery model index")
    return s[0x34102][t * 68 : t * 68 + 32].split(b"\0")[0]


def is_tunnel_terrain(name):
    return name.startswith((b"TRN_CN_", b"TRN_CS_"))


def scene_blob(s):
    # All containers start at a multiple of 128. Native headers and instances align to 16.
    out = bytearray()
    for k, v in s.items():
        if k in (0x34101, 0x34103):
            if k == 0x34103:
                while v[:4] == b"\x11" * 4:
                    v = v[4:]
            v = b"\x11" * ((-(8 + len(out) + 8)) % 16) + v
        out += pack(k, v)
    return pack(0x80034100, out)


def merge_collision(a, d, keep_city=lambda p: True):
    """Use drag static surfaces plus city event walls, preserving cell metadata."""
    aa, dd = leaf_map(a), leaf_map(d)
    require(u32(aa[0x34131]) == u32(dd[0x34131]), "collision section mismatch")
    polys = []
    cells = {}
    bounds = []
    for s, want in (
        (dd, lambda p: p[0] == 0),
        (aa, lambda p: p[0] != 0 and keep_city(p)),
    ):
        h = s[0x34131]
        sec, w, he, x, y = struct.unpack_from("<IHHff", h)
        bounds.append((x, y, x + w * 32, y + he * 32))
        ps = s[0x34134]
        remap = {}
        for o in range(0, len(ps), 32):
            p = ps[o : o + 32]
            if want(p):
                remap[o // 32] = len(polys)
                polys.append(p)
        refs = s[0x34136]
        grid = s[0x34137]
        require(len(grid) == w * he * 4, "invalid collision grid")
        require(len(ps) % 32 == 0 and len(refs) % 4 == 0, "invalid collision arrays")
        require(
            w > 0 and he > 0 and x % 32 == 0 and y % 32 == 0, "invalid collision origin"
        )
        for j in range(w * he):
            start, count = struct.unpack_from("<HH", grid, j * 4)
            require(
                (start + count) * 4 <= len(refs), "invalid collision reference range"
            )
            dest = cells.setdefault((int(x / 32) + j % w, int(y / 32) + j // w), [])
            for ri in range(start, start + count):
                pi, meta = struct.unpack_from("<HH", refs, ri * 4)
                require(pi < len(ps) // 32, "invalid polygon reference")
                if pi in remap:
                    dest.append((remap[pi], meta))
    x = min(b[0] for b in bounds)
    y = min(b[1] for b in bounds)
    w = int((max(b[2] for b in bounds) - x) / 32)
    he = int((max(b[3] for b in bounds) - y) / 32)
    h = bytearray(dd[0x34131])
    struct.pack_into("<IHHff", h, 0, sec, w, he, x, y)
    for axis in range(3):
        struct.pack_into(
            "<f",
            h,
            16 + axis * 4,
            min(
                struct.unpack_from("<f", s[0x34131], 16 + axis * 4)[0] for s in (aa, dd)
            ),
        )
        struct.pack_into(
            "<f",
            h,
            32 + axis * 4,
            max(
                struct.unpack_from("<f", s[0x34131], 32 + axis * 4)[0] for s in (aa, dd)
            ),
        )
    refs = bytearray()
    grid = bytearray()
    for iy in range(he):
        for ix in range(w):
            rows = cells.get((int(x / 32) + ix, int(y / 32) + iy), [])
            grid += struct.pack("<HH", len(refs) // 4, len(rows))
            for row in rows:
                refs += struct.pack("<HH", *row)
    require(len(polys) < 65536 and len(refs) // 4 < 65536, "collision index overflow")
    return pack(
        0x80034130,
        b"".join(
            pack(k, v)
            for k, v in [
                (0x34131, h),
                (0x34134, b"".join(polys)),
                (0x34136, refs),
                (0x34137, grid),
            ]
        ),
    )


def stream_registry(b):
    for k, p, n in leaves(b):
        if k == 0x34110:
            require(n % 80 == 0, "invalid stream directory")
            return p, {
                u16(b, o + 8): (o, u32(b, o + 20), u32(b, o + 24))
                for o in range(p, p + n, 80)
            }

    raise ValueError("missing stream directory")


def in_gate(points):
    points = tuple(points)
    if not points:
        return False
    return any(
        all(x0 <= x <= x1 and y0 <= y <= y1 for x, y in points)
        for x0, y0, x1, y1 in GATE_BOXES
    )


def keep_gate_collision(p, groups):
    name = groups[p[0] * 32 + 9 : p[0] * 32 + 32].split(b"\0")[0]
    if not name.startswith(b"BARRIERS_CAREER"):
        return True
    xs = [v / 8 for v in struct.unpack_from("<4h", p, 8)]
    ys = [v / 8 for v in struct.unpack_from("<4h", p, 16)]
    return not in_gate(zip(xs, ys))


def patch_visibility(city, drag):
    cp, cn = next((p, n) for k, p, n in leaves(city) if k == 0x34153)
    dv = next(drag[p : p + n] for k, p, n in leaves(drag) if k == 0x34153)
    changed = []
    route = {s - 20 for s in TARGET}
    for o in range(cp, cp + cn, 164):
        sec = u32(city, o + 8)
        dr = next(
            dv[j : j + 164] for j in range(0, len(dv), 164) if u32(dv, j + 8) == sec
        )
        existing = set(
            struct.unpack_from("<" + str(u32(city, o + 16)) + "H", city, o + 20)
        )
        desired = set(struct.unpack_from("<" + str(u32(dr, 16)) + "H", dr, 20))
        # Nearby city cells also need the tunnel sections that the drag layout
        # requests, even when their own scenery did not need replacement.
        if sec not in route and not (desired & TUNNEL_TERRAIN) - existing:
            continue
        combined = existing | (desired if sec in route else desired & TUNNEL_TERRAIN)
        if len(combined) > 72:
            combined = existing | {
                i for i in desired if i in route or i in TARGET or i >= 2400
            }
        if sec + 20 in TUNNEL_TERRAIN:
            combined |= TUNNEL_TERRAIN
        require(len(combined) <= 72, "streaming neighbor capacity exceeded")
        ids = sorted(combined)
        struct.pack_into("<I", city, o + 16, len(ids))
        city[o + 20 : o + 164] = struct.pack("<72H", *(ids + [0] * (72 - len(ids))))
        changed.append(sec)
    return changed


def patch_paths(data):
    removed = []

    def rewrite(b):
        result = bytearray()
        for k, p, n in chunks(b):
            body = b[p : p + n]
            if k == 0x3414D:
                kept = []
                for i in range(0, n, 36):
                    row = body[i : i + 36]
                    x, y, xx, yy = struct.unpack_from("<4f", row)
                    if in_gate([(x, y), (xx, yy)]):
                        removed.append(row)
                    else:
                        kept.append(row)
                body = b"".join(kept)
            elif k >> 28 == 8:
                body = rewrite(body)
            result += pack(k, body)
        return result

    result = rewrite(data)
    require(len(removed) == 2, "expected the two tunnel navigation gates")
    return result


TEXTURE_SECTION = 2500
WALL_TEXTURE = 0xE382F747


def texture_parts(section):
    p, n = next((p, n) for k, p, n in chunks(section) if k == 0xB3300000)
    children = {k: (q, z) for k, q, z in chunks(section, p, p + n)}
    info_p, info_n = children[0xB3310000]
    data_p, data_n = children[0xB3320000]
    info = {
        k: section[q : q + z] for k, q, z in chunks(section, info_p, info_p + info_n)
    }
    data = {k: (q, z) for k, q, z in chunks(section, data_p, data_p + data_n)}
    raw_p, raw_n = data[0x33320002]
    aligned = (raw_p + 127) & ~127
    header_p, header_n = data[0x33320001]
    return (
        info,
        section[header_p : header_p + header_n],
        section[aligned : raw_p + raw_n],
    )


def add_wall_texture(city, drag):
    ci, ch, pixels = texture_parts(city)
    di, _, dp = texture_parts(drag)
    descriptors = di[0x33310004]
    index = next(
        i // 124
        for i in range(0, len(descriptors), 124)
        if u32(descriptors, i + 36) == WALL_TEXTURE
    )
    row = bytearray(descriptors[index * 124 : (index + 1) * 124])
    require(
        row[12:36].split(b"\0")[0] == b"ARC_REFINERY_WALL", "unexpected wall texture"
    )
    require(
        not any(
            u32(ci[0x33310004], i + 36) == WALL_TEXTURE
            for i in range(0, len(ci[0x33310004]), 124)
        ),
        "wall texture already installed",
    )
    offset, size = u32(row, 48), u32(row, 56)
    require(offset + size <= len(dp), "invalid texture data span")
    # DXT1 has no palette; retain every existing texture's data and offsets.
    meta = di[0x33310005][index * 32 : (index + 1) * 32]
    require(
        meta[20:24] == b"DXT1" and u32(row, 60) == 0, "unexpected wall texture format"
    )
    padded = pixels + bytes((-len(pixels)) % 128)
    struct.pack_into("<II", row, 48, len(padded), 0)
    pixels = padded + dp[offset : offset + size]
    rows = [
        (
            ci[0x33310004][i : i + 124],
            ci[0x33310005][i // 124 * 32 : (i // 124 + 1) * 32],
        )
        for i in range(0, len(ci[0x33310004]), 124)
    ]
    rows.append((bytes(row), meta))
    rows.sort(key=lambda pair: u32(pair[0], 36))
    ci[0x33310004] = b"".join(r for r, _ in rows)
    ci[0x33310005] = b"".join(meta for _, meta in rows)
    ci[0x33310002] = b"".join(struct.pack("<II", u32(r, 36), 0) for r, _ in rows)
    info = pack(0xB3310000, b"".join(pack(k, v) for k, v in ci.items()))
    data = pack(0x33320001, ch)
    # Outer header + info + data container header + data header + padding chunk
    # + pixel chunk header must put the pixel bytes on a 128-byte boundary.
    pad = (-(8 + len(info) + 8 + len(data) + 16)) % 128
    data += pack(0, bytes(pad)) + pack(0x33320002, pixels)
    texture = pack(0xB3300000, info + pack(0xB3320000, data))
    result = bytearray(texture)
    for k, p, n in chunks(city):
        if k == 0xB3300000:
            continue
        result += pack(0, bytes(((p - 8) - len(result) - 8) % 128))
        result += city[p - 8 : p + n]
    return result


def publish_patch(out, files):
    """Expose a complete directory only after every staged write succeeds."""
    require(not out.exists(), "output directory must not already exist")
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=f".{out.name}-", dir=out.parent
    ) as temporary:
        staged = Path(temporary) / "patch"
        staged.mkdir()
        for name, contents in files.items():
            target = staged / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents)
        require(not out.exists(), "output directory must not already exist")
        staged.rename(out)


def build_patch(root, out):
    if out.exists():
        raise ValueError("output directory must not already exist")
    original_paths = (root / "ROUTESL4RA/PathsFreeRoam.bin").read_bytes()
    paths = patch_paths(original_paths)
    ca = bytearray((root / "L4RA.BUN").read_bytes())
    cd = (root / "L4RD.BUN").read_bytes()
    a = (root / "STREAML4RA.BUN").read_bytes()
    d = (root / "STREAML4RD.BUN").read_bytes()
    _, ar = stream_registry(ca)
    _, dr = stream_registry(cd)
    stream = bytearray(a)
    city_groups = next(ca[p : p + n] for k, p, n in leaves(ca) if k == 0x3412F)
    drag_groups = next(cd[p : p + n] for k, p, n in leaves(cd) if k == 0x3412F)
    require(
        city_groups[:4] == drag_groups[:4] and city_groups[8:32] == drag_groups[8:32],
        "static collision group differs",
    )
    overrides = next(ca[p : p + n] for k, p, n in leaves(ca) if k == 0x34107)
    require(
        not any(u16(overrides, o) in TARGET for o in range(0, len(overrides), 8)),
        "terrain has event overrides",
    )
    visibility = patch_visibility(ca, cd)
    for sec in sorted(TARGET):
        _, ao, an = ar[sec]
        _, do, dn = dr[sec]
        ac = list(chunks(a, ao, ao + an))
        dc = list(chunks(d, do, do + dn))
        asc = scenes(a[ao : ao + an])
        dsc = scenes(d[do : do + dn])
        primary = dict(asc[sec] if sec in GATE_SECTIONS else dsc[sec])
        require(b"TRN_CS_TunnelA" not in asc[sec][0x34102], "city already patched")
        require(
            0x34105 not in primary and 0x34105 not in asc[sec],
            "unexpected event terrain",
        )
        # Keep the original visibility tree and per-instance culling data. The city
        # event scene keeps its instance indices, so group overrides still address it.
        parts = []

        def add(blob, mod=0):
            length = sum(map(len, parts))
            pad = (mod - length - 8) % 128
            parts.append(pack(0, bytes(pad)))
            parts.append(blob)

        # Replace only local terrain model collections. Shared city props remain in the original global model pack.
        model_chunks, model_data, model_start = (
            (ac, a, ao) if sec in GATE_SECTIONS else (dc, d, do)
        )
        for k, p, n in model_chunks:
            if k == 0x80134000:
                add(model_data[p - 8 : p + n], (p - 8 - model_start) % 128)
        add(scene_blob(primary))
        for s, sc in asc.items():
            if s == sec:
                continue
            removed = (
                removed_roadwork(s, sc, dsc[s])
                + sorted(GATE_ROWS.get(s, set()))
                + sorted(TUNNEL_ARROWS.get(s, set()))
            )
            sc = dict(sc)
            types = bytearray(sc[0x34102])
            rows = instances(sc)
            for i in TUNNEL_ARROWS.get(s, set()):
                require(
                    i < len(rows)
                    and type_name(sc, rows[i]) == b"XO_TrackBarrier_Player_00",
                    "unexpected permanent tunnel arrow",
                )
                require(
                    not any(
                        u16(overrides, o) == s and u16(overrides, o + 2) == i
                        for o in range(0, len(overrides), 8)
                    ),
                    "permanent tunnel arrow has an event override",
                )
            for i in GATE_ROWS.get(s, set()):
                require(i < len(rows), "missing tunnel gate instance")
                row = rows[i]
                x, y, _, xx, yy, _ = struct.unpack_from("<6f", row)
                require(
                    type_name(sc, row) == b"XO_TrackBarrierB_B_1a_00"
                    and in_gate([(x, y), (xx, yy)]),
                    "unexpected tunnel gate instance",
                )
            replacement = [
                row
                for row in instances(dsc[s])
                if is_tunnel_terrain(type_name(dsc[s], row))
            ]
            existing = [
                i for i, row in enumerate(rows) if is_tunnel_terrain(type_name(sc, row))
            ]
            require(len(replacement) == len(existing), "terrain instance count differs")
            for i, new in zip(existing, replacement):
                old = rows[i]
                require(old[:24] == new[:24], "terrain bounds changed")
                oldtype = u16(old, 24)
                newtype = u16(new, 24)
                types[oldtype * 68 : (oldtype + 1) * 68] = dsc[s][0x34102][
                    newtype * 68 : (newtype + 1) * 68
                ]
                struct.pack_into("<H", new, 24, oldtype)
                struct.pack_into("<I", new, 28, 0)
                rows[i] = new
            if removed:
                # Leave slots and bounds intact: scenery trees and SMOKEABLE overrides
                # still refer to the same rows. A type with no model hashes is inert.
                empty_type = len(types) // 68
                types += bytes(68)
                for i in removed:
                    struct.pack_into("<H", rows[i], 24, empty_type)
                print(s, "blocking instances removed:", len(removed))
            sc[0x34102] = types
            sc[0x34103] = b"".join(rows)
            add(scene_blob(sc))
        ab = next(a[p : p + n] for k, p, n in ac if k == 0x80034130)
        db = next(d[p : p + n] for k, p, n in dc if k == 0x80034130)
        add(
            merge_collision(
                ab,
                ab if sec in GATE_SECTIONS else db,
                lambda p: keep_gate_collision(p, city_groups),
            )
        )
        for k, p, n in ac:
            if k not in (0, 0x80134000, 0x80034100, 0x80034130):
                add(a[p - 8 : p + n], (p - 8 - ao) % 128)
        blob = b"".join(parts)
        stream += pack(0, bytes((-len(stream) - 8) % 2048))
        offset = len(stream)
        stream += blob
        rec = ar[sec][0]
        struct.pack_into("<III", ca, rec + 20, offset, len(blob), len(blob))
        print(sec, "bytes", an, "->", len(blob), "unrelated city barriers retained")
    # O6's event scenery is stored in its own section, not with terrain 1526.
    # Preserve the model prefix byte-for-byte and replace only its final scene.
    for sec in STANDALONE_ARROW_SECTIONS:
        rec, offset, size = ar[sec]
        old = a[offset : offset + size]
        scene_chunks = [(p, n) for k, p, n in chunks(old) if k == 0x80034100]
        require(len(scene_chunks) == 1, "unexpected standalone scenery")
        p, n = scene_chunks[0]
        require(
            p + n == len(old) and (p - 8) % 128 == 0, "unexpected standalone alignment"
        )
        sc = dict(scenes(old)[sec])
        rows = instances(sc)
        types = bytearray(sc[0x34102])
        empty_type = len(types) // 68
        types += bytes(68)
        for i in TUNNEL_ARROWS[sec]:
            require(
                type_name(sc, rows[i]) == b"XO_TrackBarrier_Player_00",
                "unexpected standalone tunnel arrow",
            )
            require(
                not any(
                    u16(overrides, o) == sec and u16(overrides, o + 2) == i
                    for o in range(0, len(overrides), 8)
                ),
                "arrow has event override",
            )
            struct.pack_into("<H", rows[i], 24, empty_type)
        sc[0x34102], sc[0x34103] = types, b"".join(rows)
        blob = old[: p - 8] + scene_blob(sc)
        stream += pack(0, bytes((-len(stream) - 8) % 2048))
        offset = len(stream)
        stream += blob
        struct.pack_into("<III", ca, rec + 20, offset, len(blob), len(blob))
    rec, ao, an = ar[TEXTURE_SECTION]
    _, do, dn = dr[TEXTURE_SECTION]
    blob = add_wall_texture(a[ao : ao + an], d[do : do + dn])
    stream += pack(0, bytes((-len(stream) - 8) % 2048))
    offset = len(stream)
    stream += blob
    struct.pack_into("<III", ca, rec + 20, offset, len(blob), len(blob))
    manifest = {
        "sections": sorted(TARGET | STANDALONE_ARROW_SECTIONS | {TEXTURE_SECTION}),
        "visibility_sections": visibility,
        "inputs": {
            name: hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in (
                "L4RA.BUN",
                "L4RD.BUN",
                "STREAML4RA.BUN",
                "STREAML4RD.BUN",
                "ROUTESL4RA/PathsFreeRoam.bin",
            )
        },
    }
    publish_patch(
        out,
        {
            "L4RA.BUN": ca,
            "STREAML4RA.BUN": stream,
            "ROUTESL4RA/PathsFreeRoam.bin": paths,
            "tunnel-patch.json": (json.dumps(manifest, indent=2) + "\n").encode(),
        },
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tracks", type=Path, help="original game TRACKS directory")
    parser.add_argument(
        "output", type=Path, help="new output directory for patched assets"
    )
    args = parser.parse_args()
    try:
        build_patch(args.tracks, args.output)
    except (OSError, ValueError, KeyError, StopIteration, struct.error) as e:
        parser.exit(1, f"open_tunnel: {e}\n")
