#!/usr/bin/env python3
"""Synthetic checks, plus optional original-asset regression checks.

python3 tests/tunnel_patch.py
python3 tests/tunnel_patch.py <original TRACKS> <patched TRACKS>
"""

import collections
import contextlib
import io
import importlib.util
from pathlib import Path
import struct
import sys
import unittest
import tempfile
from unittest import mock

spec = importlib.util.spec_from_file_location(
    "open_tunnel", Path(__file__).resolve().parents[1] / "tools/open_tunnel.py"
)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def collider(groups, x=0):
    h = bytearray(48)
    struct.pack_into("<IHHff", h, 0, 1428, 1, 1, x, 0)
    polygons = b"".join(bytes([g, i]) + bytes(30) for i, g in enumerate(groups))
    refs = b"".join(struct.pack("<HH", i, 100 + i) for i in range(len(groups)))
    return b"".join(
        m.pack(k, v)
        for k, v in [
            (0x34131, h),
            (0x34134, polygons),
            (0x34136, refs),
            (0x34137, struct.pack("<HH", 0, len(groups))),
        ]
    )


class TunnelPatch(unittest.TestCase):
    def test_failed_write_leaves_no_output_and_retry_succeeds(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / "output"
            files = {
                "L4RA.BUN": b"city",
                "STREAML4RA.BUN": b"stream",
                "ROUTESL4RA/PathsFreeRoam.bin": b"paths",
            }
            write = Path.write_bytes

            def fail(path, data):
                if path.name == "PathsFreeRoam.bin":
                    raise OSError("simulated disk failure")
                return write(path, data)

            with mock.patch.object(Path, "write_bytes", fail):
                with self.assertRaisesRegex(OSError, "disk failure"):
                    m.publish_patch(out, files)
            self.assertEqual(list(Path(folder).iterdir()), [])
            m.publish_patch(out, files)
            for name, contents in files.items():
                self.assertEqual((out / name).read_bytes(), contents)
            with self.assertRaisesRegex(ValueError, "already exist"):
                m.publish_patch(out, files)

    def test_bad_navigation_is_validated_before_creating_output(self):
        with tempfile.TemporaryDirectory() as folder:
            root, out = Path(folder) / "input", Path(folder) / "output"
            path = root / "ROUTESL4RA/PathsFreeRoam.bin"
            path.parent.mkdir(parents=True)
            path.write_bytes(b"")
            with self.assertRaisesRegex(ValueError, "navigation gates"):
                m.build_patch(root, out)
            self.assertFalse(out.exists())

    def test_city_walls_survive_and_drag_race_walls_are_excluded(self):
        out = m.leaf_map(m.merge_collision(collider([0, 9]), collider([0, 1], 32))[8:])
        self.assertEqual(
            [out[0x34134][i] for i in range(0, len(out[0x34134]), 32)], [0, 9]
        )
        self.assertEqual(struct.unpack_from("<IHHff", out[0x34131]), (1428, 2, 1, 0, 0))
        # Left cell still addresses the original career wall, right cell the new surface.
        self.assertEqual(struct.unpack("<4H", out[0x34136]), (1, 101, 0, 100))
        self.assertEqual(struct.unpack("<4H", out[0x34137]), (0, 1, 1, 1))

    def test_roadwork_filter_does_not_remove_event_barriers(self):
        def scene(names):
            types = b"".join(name.ljust(68, b"\0") for name in names)
            rows = []
            for i in range(len(names)):
                row = bytearray(64)
                struct.pack_into("<H", row, 24, i)
                rows.append(row)
            return {0x34102: types, 0x34103: b"".join(rows)}

        city = scene(
            [
                b"XW_YellowBarrierA_1b_00",
                b"XO_TrackBarrier_Player_00",
                b"XO_ConstLight_1a_00",
            ]
        )
        drag = scene([b"XO_ConstLight_1a_00"])
        self.assertEqual(m.removed_roadwork(706, city, drag), [0])
        self.assertEqual(m.removed_roadwork(1502, city, drag), [])

    def test_gate_bounds_accept_iterators_without_consuming_them(self):
        self.assertFalse(m.in_gate(iter([(0, 0), (1, 1)])))
        self.assertFalse(m.in_gate(iter([])))
        self.assertTrue(m.in_gate(iter([(205, -1060), (240, -1055)])))

    def test_truncated_chunks_fail(self):
        for data in [b"abc", struct.pack("<II", 1, 20) + b"x"]:
            with self.assertRaises(ValueError):
                list(m.chunks(data))

    def test_invalid_polygon_reference_fails(self):
        data = bytearray(collider([0]))
        p = next(p for k, p, n in m.chunks(data) if k == 0x34136)
        struct.pack_into("<H", data, p, 4)
        with self.assertRaisesRegex(ValueError, "polygon reference"):
            m.merge_collision(bytes(data), collider([0]))


def verify_failure_retry(original, patched):
    with tempfile.TemporaryDirectory() as folder:
        root, out = Path(folder) / "input", Path(folder) / "output"
        root.mkdir()
        for name in ("L4RA.BUN", "L4RD.BUN", "STREAML4RA.BUN", "STREAML4RD.BUN"):
            (root / name).symlink_to((original / name).resolve())
        nav = root / "ROUTESL4RA/PathsFreeRoam.bin"
        nav.parent.mkdir()
        nav.write_bytes(b"")
        try:
            m.build_patch(root, out)
        except ValueError as error:
            assert "navigation gates" in str(error)
        else:
            raise AssertionError("invalid navigation was accepted")
        assert not out.exists()
        nav.write_bytes((original / "ROUTESL4RA/PathsFreeRoam.bin").read_bytes())
        with contextlib.redirect_stdout(io.StringIO()):
            m.build_patch(root, out)
        for name in (
            "L4RA.BUN",
            "STREAML4RA.BUN",
            "ROUTESL4RA/PathsFreeRoam.bin",
            "tunnel-patch.json",
        ):
            assert (out / name).read_bytes() == (patched / name).read_bytes()
    print(
        "PASS: correcting invalid navigation can retry into the same output directory"
    )


def verify_assets(original, patched):
    a = (original / "L4RA.BUN").read_bytes()
    b = (patched / "L4RA.BUN").read_bytes()
    sa = (original / "STREAML4RA.BUN").read_bytes()
    sb = (patched / "STREAML4RA.BUN").read_bytes()
    _, ra = m.stream_registry(a)
    _, rb = m.stream_registry(b)
    # Every city definition outside the patched stream offsets is untouched.
    restored = bytearray(b)
    for section in m.TARGET | m.STANDALONE_ARROW_SECTIONS | {m.TEXTURE_SECTION}:
        rec = ra[section][0]
        restored[rec + 20 : rec + 32] = a[rec + 20 : rec + 32]
    vp, vn = next((p, n) for k, p, n in m.leaves(a) if k == 0x34153)
    for o in range(vp, vp + vn, 164):
        if m.u32(a, o + 8) not in (
            {s - 20 for s in m.TARGET} | {1311, 1313, 1404, 1419, 1514, 1515}
        ):
            continue

        def neighbors(blob):
            count = m.u32(blob, o + 16)
            assert count <= 72
            return set(struct.unpack_from(f"<{count}H", blob, o + 20))

        assert neighbors(a) <= neighbors(b)
        if m.u32(a, o + 8) + 20 in m.TUNNEL_TERRAIN:
            assert m.TUNNEL_TERRAIN <= neighbors(b)
        restored[o + 16 : o + 164] = a[o + 16 : o + 164]
    assert restored == a
    path = "ROUTESL4RA/PathsFreeRoam.bin"
    pa, pb = (original / path).read_bytes(), (patched / path).read_bytes()
    la, lb = list(m.leaves(pa)), list(m.leaves(pb))
    assert len(la) == len(lb)
    for (ka, oa, na), (kb, ob, nb) in zip(la, lb):
        assert ka == kb
        aa, bb = pa[oa : oa + na], pb[ob : ob + nb]
        if ka == 0x3414D:
            rows = [aa[i : i + 36] for i in range(0, na, 36)]
            expected = []
            for row in rows:
                x, y, xx, yy = struct.unpack_from("<4f", row)
                if not m.in_gate([(x, y), (xx, yy)]):
                    expected.append(row)
            assert len(rows) - len(expected) == 2
            assert bb == b"".join(expected)
        else:
            assert aa == bb
    groups = next(a[p : p + n] for k, p, n in m.leaves(a) if k == 0x3412F)
    # Only career gates own these scenery slots; race groups must not be affected.
    fields = {k: a[p : p + n] for k, p, n in m.leaves(a)}
    overrides, groups_data = fields[0x34107], fields[0x34108]
    o = 0
    referenced = set()
    while o < len(groups_data):
        name = groups_data[o + 8 : o + 40].split(b"\0")[0]
        count = m.u32(groups_data, o + 48)
        for index in struct.unpack_from(f"<{count}H", groups_data, o + 52):
            sid, row = struct.unpack_from("<HH", overrides, index * 8)
            if row in m.GATE_ROWS.get(sid, set()):
                assert name.startswith(b"BARRIERS_CAREER"), (name, sid, row)
                referenced.add((sid, row))
        o += (52 + count * 2 + 3) & ~3
    assert referenced == {
        (sid, row) for sid, rows in m.GATE_ROWS.items() for row in rows
    }
    assert sb[: len(sa)] == sa
    list(m.chunks(sb))  # appended sections and padding must form a valid stream
    dragsc = m.scenes((original / "STREAML4RD.BUN").read_bytes())
    # Audit the whole construction corridor, including neighboring chunks that
    # do not contain changed tunnel meshes. This catches leftover approach props.
    original_scenes = m.scenes(sa)
    patched_scenes = m.scenes(sb)
    for sid, indices in m.TUNNEL_ARROWS.items():
        scene = patched_scenes[sid]
        for i in indices:
            row = m.instances(scene)[i]
            t = m.u16(row, 24)
            assert scene[0x34102][t * 68 : (t + 1) * 68] == bytes(68), (sid, i)
    audited_props = 0
    for sid, scene in original_scenes.items():
        present = {
            (m.type_name(dragsc[sid], r), bytes(r[:24]))
            for r in m.instances(dragsc[sid])
        }
        for i, row in enumerate(m.instances(scene)):
            x, y, _, xx, yy, _ = struct.unpack_from("<6f", row)
            name = m.type_name(scene, row)
            if not (-220 < x < xx < 200 and -950 < y < yy < 100):
                continue
            if name not in m.ROADWORK_MODELS or (name, bytes(row[:24])) in present:
                continue
            target = m.instances(patched_scenes[sid])[i]
            t = m.u16(target, 24)
            assert patched_scenes[sid][0x34102][t * 68 : (t + 1) * 68] == bytes(68), (
                sid,
                i,
                name,
            )
            audited_props += 1
    assert audited_props == 159, audited_props
    barrier_count = 0
    for section, (rec, off, size) in ra.items():
        _, newoff, newsize = rb[section]
        if section == m.TEXTURE_SECTION:
            old_info, old_header, old_pixels = m.texture_parts(sa[off : off + size])
            new_info, new_header, new_pixels = m.texture_parts(
                sb[newoff : newoff + newsize]
            )
            assert old_header == new_header
            assert new_pixels[: len(old_pixels)] == old_pixels
            rows = new_info[0x33310004]

            def textures(info):
                records, metadata = info[0x33310004], info[0x33310005]
                return {
                    m.u32(records, i + 36): (
                        records[i : i + 124],
                        metadata[i // 124 * 32 : (i // 124 + 1) * 32],
                    )
                    for i in range(0, len(records), 124)
                }

            old_textures, new_textures = textures(old_info), textures(new_info)
            assert new_textures.keys() - old_textures.keys() == {m.WALL_TEXTURE}
            assert all(new_textures[h] == value for h, value in old_textures.items())
            wall = new_textures[m.WALL_TEXTURE][0]
            assert new_info[0x33310001] == old_info[0x33310001]
            hashes = new_info[0x33310002]
            ids = [m.u32(hashes, i) for i in range(0, len(hashes), 8)]
            assert ids == sorted(set(ids))
            assert m.WALL_TEXTURE in ids
            assert ids == [m.u32(rows, i + 36) for i in range(0, len(rows), 124)]
            _, dr = m.stream_registry((original / "L4RD.BUN").read_bytes())
            _, do, dn = dr[m.TEXTURE_SECTION]
            ds = (original / "STREAML4RD.BUN").read_bytes()
            di, _, dp = m.texture_parts(ds[do : do + dn])
            drows = di[0x33310004]
            pos = next(
                i
                for i in range(0, len(drows), 124)
                if m.u32(drows, i + 36) == m.WALL_TEXTURE
            )
            src, count = m.u32(drows, pos + 48), m.u32(drows, pos + 56)
            dst = m.u32(wall, 48)
            assert new_pixels[dst : dst + count] == dp[src : src + count]
            continue
        if section in m.STANDALONE_ARROW_SECTIONS:
            old, new = sa[off : off + size], sb[newoff : newoff + newsize]
            p = next(p for k, p, n in m.chunks(old) if k == 0x80034100)
            assert old[: p - 8] == new[: p - 8]
            os, ns = m.scenes(old)[section], m.scenes(new)[section]
            for i, (r1, r2) in enumerate(zip(m.instances(os), m.instances(ns))):
                if i in m.TUNNEL_ARROWS[section]:
                    assert r1[:24] == r2[:24] and r1[26:] == r2[26:]
                else:
                    assert r1 == r2
            for k in (0x34104, 0x34105, 0x34106):
                assert os.get(k) == ns.get(k)
            continue
        if section not in m.TARGET:
            assert (newoff, newsize) == (off, size)
            continue
        old = sa[off : off + size]
        new = sb[newoff : newoff + newsize]
        oldsc, newsc = m.scenes(old), m.scenes(new)
        if section in m.GATE_SECTIONS:
            assert oldsc[section] == newsc[section]
        else:
            assert any(
                tag in newsc[section][0x34102] for tag in (b"TRN_CS_", b"TRN_CN_")
            )
        for sid in oldsc.keys() - {section}:
            oi, ni = m.instances(oldsc[sid]), m.instances(newsc[sid])
            assert len(oi) == len(ni)
            drag = dragsc[sid]
            removed = set(m.removed_roadwork(sid, oldsc[sid], drag)) | m.GATE_ROWS.get(
                sid, set()
            )
            removed |= m.TUNNEL_ARROWS.get(sid, set())
            if sid in m.ROADWORK_SCENES:
                assert removed, "entrance blocker regression: no roadwork removed"
            for i, (first, second) in enumerate(zip(oi, ni)):
                if i in removed:
                    assert first[:24] == second[:24] and first[26:] == second[26:]
                    t = m.u16(second, 24)
                    assert newsc[sid][0x34102][t * 68 : (t + 1) * 68] == bytes(68)
                    continue
                if not m.is_tunnel_terrain(m.type_name(oldsc[sid], first)):
                    assert first == second
                    t = m.u16(first, 24)
                    assert (
                        oldsc[sid][0x34102][t * 68 : (t + 1) * 68]
                        == newsc[sid][0x34102][t * 68 : (t + 1) * 68]
                    )
            for k in (0x34104, 0x34105, 0x34106):
                assert oldsc[sid].get(k) == newsc[sid].get(k)

        def walls(blob):
            v = next(blob[p : p + n] for k, p, n in m.leaves(blob) if k == 0x34134)
            return collections.Counter(
                v[i : i + 32] for i in range(0, len(v), 32) if v[i]
            )

        expected = collections.Counter(
            {
                p: count
                for p, count in walls(old).items()
                if m.keep_gate_collision(p, groups)
            }
        )
        assert expected == walls(new), section
        barrier_count += sum(walls(new).values())
    print(
        f"PASS assets: {len(m.TARGET | m.STANDALONE_ARROW_SECTIONS | {m.TEXTURE_SECTION})} sections patched; {barrier_count} event collision polygons and unrelated city definitions retained"
    )


if __name__ == "__main__":
    args = sys.argv[1:]
    result = unittest.main(argv=[sys.argv[0]], exit=False)
    if not result.result.wasSuccessful():
        sys.exit(1)
    if args:
        if len(args) != 2:
            sys.exit("expected original TRACKS and patched TRACKS")
        verify_assets(*(Path(a) for a in args))
        verify_failure_retry(*(Path(a) for a in args))
