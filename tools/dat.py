#!/usr/bin/env python3
"""dat.py - read NOLF's .dat worlds, and check that the reading is right.

Stage 2 of docs/PLAN-FILES-NOT-HEAP.md. The container is fully mapped and
validated here; the leaf records (polygons and surfaces) are NOT, and the open
questions are written down at the bottom of this file rather than guessed at.

Same discipline as rez.py and dtx.py: parse, then prove the parse landed. FOUR
identities are enforced, and all four hold on all 103 worlds - 20198 world
models and 1265645 polygons:

  1. THE MODEL CHAIN LANDS EXACTLY ON objectDataPos. Each WorldModel record
     begins with a u32 pointing at the next one, and the last points at the
     header's own objectDataPos. A wrong stride anywhere derails it.
  2. THE DECLARED MODEL COUNT EQUALS THE NUMBER OF RECORDS WALKED. The count
     sits immediately before the first record and is written independently of
     the chain, so agreeing is not automatic.
  3. The per-polygon vertex-count run sums to the model's declared vertex
     reference count.
  4. The texture-name blob splits into exactly the declared number of names.

THE SWEEP IS THE POINT, not the individual parse. Identity 3 passed on the two
models it was first tried against and failed on 98 of the 103 worlds, because
the per-polygon field is TWO BYTES - vertex count and something else - and the
something else is zero on both models that had been hand-picked. Two agreeing
samples proved nothing; running it over everything took one minute and said so.

CROSS-CHECKED AGAINST THE ENGINE'S OWN MEMORY, which had no part in producing
any of this. For WORLDS/M01S02.DAT - the level the desk harness loads - the
file declares 565 world models, and docs/GEOMETRY-FOUND.md counted 565 named
WorldModels in the heap. VisBSP's file counts are 10829 points and 5957
polygons; the heap walk read 10829 at +0xA8 and 5957 at +0xA0. Three numbers
from two entirely separate routes.

    python tools/dat.py info     WORLDS/M01S02.DAT
    python tools/dat.py models   WORLDS/M01S02.DAT
    python tools/dat.py textures WORLDS/M01S02.DAT
    python tools/dat.py surfaces WORLDS/M01S02.DAT
    python tools/dat.py polygons WORLDS/M01S02.DAT gate1
    python tools/dat.py sweep
"""

import collections
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rez import RezFile                                    # noqa: E402

MOUNT = ["NOLF.rez", "NOLF2.rez", "NOLFdll.rez", "NOLFl.rez",
         "Nolfu003.rez", "Nolfcres003.rez", "NolfGoty.rez", "Modernizer.rez"]


def mounted(game, suffix=".DAT"):
    have = {f.lower(): f for f in os.listdir(game)}
    out = {}
    for m in MOUNT:
        real = have.get(m.lower())
        if not real:
            continue
        r = RezFile(os.path.join(game, real))
        for n in r.files:
            if n.endswith(suffix):
                out[n] = r
    return out


class World:
    """The container. Everything in here satisfies an identity."""

    def __init__(self, blob, name=""):
        self.name = name
        self.b = blob
        (self.version, self.object_data_pos,
         self.render_data_pos) = struct.unpack_from("<3I", blob, 0)
        if self.version != 66:
            raise ValueError(f"{name}: version {self.version}, expected 66")

        # The header is 3 offsets plus 8 reserved dwords, then a length-
        # prefixed properties string - "AmbientLight 32 32 53" and the like.
        o = 44
        n, = struct.unpack_from("<I", blob, o)
        self.info = blob[o + 4:o + 4 + n].decode("latin-1", "replace")
        o += 4 + n

        # The world tree: one scalar, then two NESTED bounding boxes (the
        # inner one is the root node's, the outer the world's), then the node
        # count and ONE LAYOUT BIT PER NODE. That last is what makes the model
        # list findable at all - get the bit packing wrong and everything
        # after it is off by a few bytes and nothing parses.
        self.tree_scalar, = struct.unpack_from("<f", blob, o)
        o += 4
        self.box = struct.unpack_from("<12f", blob, o)
        o += 48
        self.tree_nodes, = struct.unpack_from("<I", blob, o)
        o += 8                                  # node count + one reserved
        o += (self.tree_nodes + 7) // 8         # the packed layout

        self.model_count, = struct.unpack_from("<I", blob, o)
        o += 4
        self.first_model = o

        self.models = []
        p = o
        while p != self.object_data_pos:
            if p + 46 > len(blob):
                raise ValueError(f"{name}: the model chain ran off the file")
            nxt, = struct.unpack_from("<I", blob, p)
            ln, = struct.unpack_from("<H", blob, p + 44)
            if ln == 0 or ln > 64 or p + 46 + ln > len(blob):
                raise ValueError(f"{name}: bad model name length at {p}")
            nm = blob[p + 46:p + 46 + ln]
            if not all(32 <= c < 127 for c in nm):
                raise ValueError(f"{name}: non-printable model name at {p}")
            self.models.append(Model(self, p, nxt, nm.decode()))
            if nxt <= p:
                raise ValueError(f"{name}: chain does not advance at {p}")
            p = nxt

        # IDENTITY 2. Written independently of the chain, so agreement is a
        # real check and not a tautology.
        if self.model_count != len(self.models):
            raise ValueError(f"{name}: declares {self.model_count} world "
                             f"models, the chain walks {len(self.models)}")

    def __getitem__(self, nm):
        for m in self.models:
            if m.name == nm:
                return m
        raise KeyError(nm)


def _newell(pts):
    """A polygon's normal from its own vertices. Newell's method rather than a
    cross product of the first three: three nearly-collinear points give a
    meaningless normal, and world geometry has plenty of those."""
    n = [0.0, 0.0, 0.0]
    for i in range(len(pts)):
        a, c = pts[i], pts[(i + 1) % len(pts)]
        n[0] += (a[1] - c[1]) * (a[2] + c[2])
        n[1] += (a[2] - c[2]) * (a[0] + c[0])
        n[2] += (a[0] - c[0]) * (a[1] + c[1])
    L = math.sqrt(sum(x * x for x in n))
    return [x / L for x in n] if L > 1e-9 else None


class Model:
    """One WorldModel. The counts and the texture list are validated; the
    polygon and surface bytes are located but NOT decoded - see the notes."""

    def __init__(self, world, start, nxt, name):
        b = world.b
        self.world, self.start, self.next, self.name = world, start, nxt, name

        # Two fields sit just before the name. The first is 2 on every model
        # in M01S02 except VisBSP (32) and PhysicsBSP (20), so it marks those
        # two as special - but it does NOT separate an AIVolume from a real
        # door, and neither does anything else at model level. Whatever says
        # "do not draw" is per surface, not per model.
        self.field36, self.field40 = struct.unpack_from("<2I", b, start + 36)

        e = start + 46 + len(name)
        c = struct.unpack_from("<12I", b, e)
        (self.n_points, self.n_planes, self.n_surfaces, self.c3,
         self.n_polygons, self.n_nodes, self.n_vertex_refs,
         self.c7, self.c8, self.c9, self.c10, self.c11) = c
        self.counts = c
        e += 48

        # Bounding box, then a world translation - zero on the BSPs, non-zero
        # on the models the engine moves.
        self.box_min = struct.unpack_from("<3f", b, e)
        self.box_max = struct.unpack_from("<3f", b, e + 12)
        self.translation = struct.unpack_from("<3f", b, e + 24)
        e += 36

        # The texture list: total bytes, count, then NUL-terminated names.
        # THIS IS THE BRIDGE TO STAGE 1 - these are exactly the names the
        # Dtx cache is keyed by.
        blob_len, n_tex = struct.unpack_from("<2I", b, e)
        raw = b[e + 8:e + 8 + blob_len]
        # Split on NUL and drop only the TRAILING empty. A blob with an empty
        # name in the middle is legal - Chevelle_Body has one - and filtering
        # every empty loses it and miscounts by one.
        parts = raw.split(bytes([0]))
        if parts and parts[-1] == b"":
            parts.pop()
        self.textures = [t.decode("latin-1") for t in parts]
        if len(self.textures) != n_tex:
            raise ValueError(f"{name}: declares {n_tex} textures, the blob "
                             f"splits into {len(self.textures)}")
        e += 8 + blob_len

        # TWO BYTES PER POLYGON, not one u16: the low byte is the vertex
        # count, the high byte is something else. On VisBSP and on every
        # single-texture model that high byte is zero, so reading the pair
        # as a u16 gives the right answer and looks correct - it matched on
        # both models this was first checked against, then failed on 98 of
        # the 103 worlds. Hinge is the counter-example: its 25 quads read
        # 4, 1540, 1796, 1284, ... as u16 and 4 with high bytes 0, 6, 7, 5,
        # ... as pairs. THE SWEEP IS WHAT CAUGHT IT; two hand-picked models
        # agreeing proved nothing.
        _pairs = struct.unpack_from("<%dB" % (self.n_polygons * 2), b, e)
        self.poly_sizes = _pairs[0::2]
        self.poly_flags = _pairs[1::2]
        if sum(self.poly_sizes) != self.n_vertex_refs:
            raise ValueError(f"{name}: polygon sizes sum to "
                             f"{sum(self.poly_sizes)}, declared "
                             f"{self.n_vertex_refs} vertex references")
        e += self.n_polygons * 2
        self.geometry_start = e

        # ---- ONE displacement, before the planes, and only VisBSP has it ----
        #
        # The story this file used to tell was "VisBSP's surfaces are not after
        # its planes". That was the SYMPTOM. VisBSP's PLANES are not where the
        # counts say either - 0 of M01S02's 1688 are unit normals at the
        # computed offset, against 100% for every other model - and everything
        # downstream is measured from the planes, so the surfaces, the points
        # and the polygons were all wrong for the one reason.
        #
        # Locate the planes and the whole model falls out: the surfaces sit
        # EXACTLY at the planes end, and all 5957 polygons parse with the
        # ordinary 10-byte header. The displacement on M01S02 is 273505 bytes
        # for the planes and 273505 for the surfaces - the same number, which
        # is what says it is one inserted block and not two problems.
        #
        # No formula for it. Solved exactly over all twelve counts plus a
        # constant on 12 worlds, the answer fails out of sample on 78 of the
        # other 90, and the leftover is not a multiple of any count - there is
        # variable-length data in that block. So the array is LOCATED, the same
        # retreat find_points already makes.
        self.plane_assumed = e
        self._plane_at = None

    @property
    def plane_start(self):
        """Where the plane array really is.

        The computed position first, because it is right on every model but
        VisBSP and costs one check; a scan only when that fails.
        """
        if self._plane_at is None:
            if self._planes_unit(self.plane_assumed):
                self._plane_at = self.plane_assumed
            else:
                self._plane_at = self.find_planes()
        return self._plane_at

    @plane_start.setter
    def plane_start(self, v):
        self._plane_at = v

    @property
    def surface_start(self):
        """Immediately after the planes, on every model including VisBSP.

        That was never in doubt; what was wrong was where the planes are.
        """
        return self.plane_start + self.n_planes * 16

    @surface_start.setter
    def surface_start(self, v):
        self._surface_override = v

    def _planes_unit(self, o, sample=None):
        """Are the plane normals at `o` unit length? The whole array unless a
        sample is named - the cheap probe the scan rejects offsets with."""
        b, n = self.world.b, self.n_planes
        if not n or o < 0 or o + 16 * n > len(b):
            return False
        idx = range(n) if sample is None else sample
        for i in idx:
            if i >= n:
                continue
            x, y, z = struct.unpack_from("<3f", b, o + i * 16)
            if not 0.99 < x * x + y * y + z * z < 1.01:
                return False
        return True

    def find_planes(self):
        """Scan for the plane array, or keep the computed offset if not found.

        A plane is {normal, dist} and the NORMAL IS UNIT LENGTH, so n of them
        in a row at stride 16 is not something other data does by accident -
        the same criterion, and the same strength, as find_points. On M01S02's
        VisBSP exactly one offset in the whole model satisfies it.

        AND THE ANSWER CHECKS ITSELF against things the scan never looked at:
        the surfaces then sit exactly at the planes end, surface_check passes,
        the flags collapse from 1177 distinct values to 14 with Sky.dtx alone
        on 110, and all 5957 polygons parse with their normals matching the
        planes their surfaces name. Nothing was fitted to make any of that
        happen.
        """
        b, n = self.world.b, self.n_planes
        if not n:
            return self.plane_assumed
        probe = (1, 2, n - 1)
        for o in range(self.plane_assumed, self.next - 16 * n + 1):
            if not self._planes_unit(o, (0,)):
                continue
            if not self._planes_unit(o, probe):
                continue
            if self._planes_unit(o):
                return o
        return self.plane_assumed

    # A plane is {LTVector normal, float dist}. Confirmed on a model that is a
    # box: six axis normals and six face distances.
    def planes(self):
        for i in range(self.n_planes):
            yield struct.unpack_from("<4f", self.world.b, self.plane_start + i * 16)

    # THE SURFACE RECORD, 53 bytes. Everything named here is checked, and the
    # checks are in surface_check() below.
    #
    #   +0   float3   texture-space origin
    #   +12  float3   texture-space U
    #   +24  float3   texture-space V
    #   +36  u16      TEXTURE INDEX into self.textures
    #   +38  u32      PLANE INDEX into self.planes()
    #   +42  u32      undecoded
    #   +46  u32      undecoded
    #   +50  u8       undecoded
    #   +51  u16      SURFACE FLAGS
    SURFACE_SIZE = 53

    def surfaces(self):
        b = self.world.b
        for i in range(self.n_surfaces):
            o = self.surface_start + i * self.SURFACE_SIZE
            yield {
                "origin": struct.unpack_from("<3f", b, o),
                "u": struct.unpack_from("<3f", b, o + 12),
                "v": struct.unpack_from("<3f", b, o + 24),
                "texture": struct.unpack_from("<H", b, o + 36)[0],
                "plane": struct.unpack_from("<I", b, o + 38)[0],
                "flags": struct.unpack_from("<H", b, o + 51)[0],
            }

    # ---- points, located rather than computed ----
    #
    # The points array is n records of {LTVector position, LTVector normal},
    # and the normal is UNIT LENGTH. A run of n consecutive unit vectors at
    # stride 24 is not something other data does by accident, so this locates
    # the array without needing to know the size of anything between it and
    # the polygons. Cached; it is the one linear scan in this file.
    def find_points(self):
        if getattr(self, "_points_at", None) is not None:
            return self._points_at
        b = self.world.b
        n = self.n_points
        unpack = struct.unpack_from

        def unit(o, i):
            x, y, z = unpack("<3f", b, o + i * 24 + 12)
            return 0.99 < x * x + y * y + z * z < 1.01

        # Cascade the test, cheapest first. Checking a sample of n/6 normals at
        # every candidate offset is n/6 unpacks per byte scanned, which on a
        # million-byte model is minutes; checking ONE and only then widening is
        # one unpack per byte and rejects essentially everything immediately.
        for o in range(self.surface_start + self.n_surfaces * self.SURFACE_SIZE,
                       self.next - 24 * n + 1):
            if not unit(o, 0):
                continue
            if not all(unit(o, i) for i in (1, 2, n - 1) if i < n):
                continue
            if all(unit(o, i) for i in range(n)):
                self._points_at = o
                return o
        self._points_at = -1
        return -1

    def points(self):
        o = self.find_points()
        if o < 0:
            return []
        b = self.world.b
        return [(struct.unpack_from("<3f", b, o + i * 24),
                 struct.unpack_from("<3f", b, o + i * 24 + 12))
                for i in range(self.n_points)]

    # ---- polygons: a CHECKED parse, not a guessed stride ----
    #
    #   float3   centre
    #   ...      a prefix whose length VARIES - 6, 10, 14, 18 and 26 bytes all
    #            occur, and what selects it is not known
    #   u16      SURFACE INDEX, always the last 4 bytes of the header minus 2
    #   u16      undecoded
    #   per vertex: u16 point index, u8 r, u8 g, u8 b
    #
    # Because the prefix length is unknown, the length is MEASURED per record
    # against three things a wrong length cannot satisfy at once: every vertex
    # index in range, the next polygon's centre inside the model's own bounding
    # box, and - the one that actually decides it - THE POLYGON'S VERTICES
    # COPLANAR WITH THE PLANE ITS SURFACE NAMES. That last is a two-hop
    # geometric chain, polygon -> surface -> plane, checked against a normal
    # computed from the polygon's own points. It came out at 100.0% on every
    # header length: 7882 of 7882 at 10 bytes, 13309 of 13309 at 14, 102 of 102
    # at 18, 84 of 84 at 22.
    #
    # Reading the surface index at a FIXED offset is what does not work: +10 is
    # right for the 14-byte header and does not exist in the 10-byte one, and
    # searching fixed offsets across both scored 50.5% at best.
    def polygons(self):
        b = self.world.b
        po = self.find_points()
        if po < 0:
            return None
        V = [struct.unpack_from("<3f", b, po + i * 24) for i in range(self.n_points)]
        P = [struct.unpack_from("<4f", b, self.plane_start + i * 16)
             for i in range(self.n_planes)]
        S = [struct.unpack_from("<I", b, self.surface_start + j * self.SURFACE_SIZE + 38)[0]
             for j in range(self.n_surfaces)]
        o = self.surface_start + self.n_surfaces * self.SURFACE_SIZE
        out = []
        for i, nv in enumerate(self.poly_sizes):
            centre = struct.unpack_from("<3f", b, o)
            if not self._in_box(centre):
                return None
            hit = None
            # THE HEADER LENGTH IS 10 + 4k, and k is the number of
            # lightmap references. k == 0 exactly when the lightmap width and
            # height are both zero - that holds on 23758 of 23758 polygons, so
            # the family is 10, 14, 18, ... and nothing else. Searching from 6
            # instead produced 346 six-byte "headers", which cannot exist.
            for h in range(10, 64, 4):
                q = o + 12 + h
                if q + 5 * nv > po:
                    break
                idx = [struct.unpack_from("<H", b, q + k * 5)[0] for k in range(nv)]
                if any(v >= self.n_points for v in idx):
                    continue
                si, = struct.unpack_from("<H", b, o + 12 + h - 4)
                if si >= self.n_surfaces or S[si] >= self.n_planes:
                    continue
                nrm = _newell([V[v] for v in idx])
                if nrm is None:
                    continue
                pl = P[S[si]]
                if abs(abs(sum(nrm[k] * pl[k] for k in range(3))) - 1.0) > 0.02:
                    continue
                if i + 1 < len(self.poly_sizes) and                    not self._in_box(struct.unpack_from("<3f", b, q + 5 * nv)):
                    continue
                hit = (h, si, idx, q)
                break
            if hit is None:
                return None
            h, si, idx, q = hit
            lw, lh = struct.unpack_from("<2H", b, o + 12)
            out.append({"centre": centre, "header": h, "surface": si,
                        "lightmap": (lw, lh), "lm_refs": (h - 10) // 4,
                        "points": idx,
                        "colours": [tuple(b[q + k * 5 + 2:q + k * 5 + 5])
                                    for k in range(nv)]})
            o = q + 5 * nv
        self._polys_end = o
        return out

    def _in_box(self, v, pad=64.0):
        return all(self.box_min[k] - pad <= v[k] <= self.box_max[k] + pad
                   for k in range(3))

    def surface_check(self, base=None):
        """Is the surface array where we think it is? Two conditions that a
        wrong offset cannot satisfy: every texture index in range AND every
        declared texture actually referenced, and every plane index in range.

        VisBSP FAILS THIS IN EVERY WORLD and that is a known open problem -
        something sits between its planes and its surfaces. See the notes."""
        ti, pi = [], []
        b = self.world.b
        # Resolves through plane_start, so a model whose planes had to be
        # located is checked where they actually are.
        if base is None:
            base = self.surface_start
        for i in range(self.n_surfaces):
            o = base + i * self.SURFACE_SIZE
            if o + self.SURFACE_SIZE > self.next:
                return False
            ti.append(struct.unpack_from("<H", b, o + 36)[0])
            pi.append(struct.unpack_from("<I", b, o + 38)[0])
        if not ti:
            return False
        if set(ti) != set(range(len(self.textures))):
            return False
        return max(pi) < self.n_planes

    @property
    def size(self):
        return self.next - self.start

    def __str__(self):
        return (f"{self.name}: {self.n_points} points, {self.n_planes} planes, "
                f"{self.n_surfaces} surfaces, {self.n_polygons} polygons, "
                f"{self.n_vertex_refs} vertex refs, {len(self.textures)} "
                f"textures, {self.size} bytes")


def load(game, path):
    files = mounted(game)
    key = path.replace("\\", "/").upper().lstrip("/")
    if key not in files:
        raise SystemExit(f"{path}: not in any mounted archive")
    return World(files[key].read(key), key)


def main():
    args = sys.argv[1:]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    game = os.path.join(root, "game")
    if not args:
        raise SystemExit(__doc__)
    cmd = args[0]

    if cmd == "info":
        w = load(game, args[1])
        print(f"{w.name}: version {w.version}, {len(w.b)} bytes")
        print(f"  info string   : {w.info!r}")
        print(f"  objectDataPos : {w.object_data_pos}")
        print(f"  renderDataPos : {w.render_data_pos}")
        print(f"  world tree    : {w.tree_nodes} nodes, "
              f"{(w.tree_nodes + 7) // 8} bytes of layout")
        print(f"  world models  : {w.model_count} (chain walked "
              f"{len(w.models)}, ending exactly on objectDataPos)")
        big = sorted(w.models, key=lambda m: -m.n_polygons)[:5]
        for m in big:
            print(f"    {m}")

    elif cmd == "models":
        w = load(game, args[1])
        for m in w.models:
            print(f"  {m}")

    elif cmd == "textures":
        w = load(game, args[1])
        # Which textures does each CLASS of model use? Evidence for stage 2,
        # not a rule to ship: the classes we currently skip by NAME turn out
        # to be textured with the editor's own marker textures.
        by = collections.defaultdict(collections.Counter)
        for m in w.models:
            k = ("VisBSP" if m.name == "VisBSP" else
                 "PhysicsBSP" if m.name == "PhysicsBSP" else
                 "AIVolume" if m.name.startswith("AIVolume") else
                 "blocker" if m.name.startswith("blocker") else
                 "Translucent" if m.name.startswith("TranslucentWorldModel")
                 else "(other)")
            for t in m.textures:
                by[k][t.rsplit("\\", 1)[-1]] += 1
        for k in sorted(by):
            top = by[k].most_common(4)
            print(f"  {k:<12} {sum(by[k].values()):5d} texture refs  "
                  f"{len(by[k]):3d} distinct   top: {top}")

    elif cmd == "surfaces":
        w = load(game, args[1])
        # The whole point of the surface record: it names a texture and it
        # carries flags. Group by texture so the marker surfaces are visible.
        by = collections.defaultdict(collections.Counter)
        okm = badm = 0
        for m in w.models:
            if not m.n_surfaces:
                continue
            if not m.surface_check():
                badm += 1
                continue
            okm += 1
            for s_ in m.surfaces():
                by[m.textures[s_["texture"]].rsplit("\\", 1)[-1]][s_["flags"]] += 1
        print(f"  {okm} models validated, {badm} did not (VisBSP is always "
              f"among them - see the notes)")
        rows = sorted(by.items(), key=lambda kv: -sum(kv[1].values()))
        for t, fl in rows[:20]:
            print("   %-24s %6d surfaces  flags %s"
                  % (t, sum(fl.values()), fl.most_common(3)))

    elif cmd == "polygons":
        w = load(game, args[1])
        m = w[args[2]] if len(args) > 2 else max(w.models, key=lambda x: x.n_polygons)
        print(f"  {m}")
        if not m.surface_check():
            raise SystemExit("  its surface array is not where we can find it")
        pl = m.polygons()
        if pl is None:
            raise SystemExit("  the polygon parse did not validate")
        sf = list(m.surfaces())
        for i, p_ in enumerate(pl[:20]):
            s_ = sf[p_["surface"]]
            print("   poly %-4d %d verts  surface %-5d %-28s flags %-5d "
                  "lightmap %dx%d (%d refs)"
                  % (i, len(p_["points"]), p_["surface"],
                     m.textures[s_["texture"]].rsplit("\\", 1)[-1], s_["flags"],
                     p_["lightmap"][0], p_["lightmap"][1], p_["lm_refs"]))
        if len(pl) > 20:
            print(f"   ... {len(pl) - 20} more")

    elif cmd == "sweep":
        files = mounted(game)
        ok, bad = 0, []
        models = polys = 0
        surf_ok = surf_bad = surf_n = 0
        poly_ok = poly_bad = poly_n = 0
        for n in sorted(files):
            try:
                w = World(files[n].read(n), n)
                for m in w.models:
                    models += 1
                    polys += m.n_polygons
                    if not m.n_surfaces:
                        continue
                    if m.surface_check():
                        surf_ok += 1
                        surf_n += m.n_surfaces
                        pl = m.polygons()
                        if pl is None:
                            poly_bad += 1
                        else:
                            poly_ok += 1
                            poly_n += len(pl)
                    else:
                        surf_bad += 1
                ok += 1
            except (ValueError, struct.error) as e:
                bad.append(str(e))
        print(f"{ok} of {len(files)} worlds parse with every identity holding")
        print(f"  {models} world models, {polys} polygons")
        print(f"  {surf_ok} models' surface arrays validate "
              f"({surf_n} surfaces); {surf_bad} do not")
        print(f"  {poly_ok} models' polygons parse and validate geometrically "
              f"({poly_n} polygons); {poly_bad} do not")
        for e in bad:
            print(f"  FAILED {e}")

    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()


# --------------------------------------------------------------------------
# WHAT IS DECODED, WHAT IS NOT, AND HOW EACH WAS PROVED
#
# THE POLYGON RECORD IS DONE TOO, and it is self-checking rather than a stride:
#
#   float3   centre
#   u16      lightmap width      \  both zero exactly when the polygon has no
#   u16      lightmap height     /  lightmap, on 23758 of 23758 checked
#   u32      lightmap reference x k       k = 0 when unlit
#   u16      lightmap index (incrementing across the model)
#   u16      SURFACE INDEX
#   u16      undecoded
#   per vertex: u16 point index, u8 r, u8 g, u8 b
#
# So the header is 10 + 4k bytes and nothing else - 10, 14, 18, 22, 26, 30.
# Searching from 6 instead produced 346 "six-byte headers", which cannot exist
# under that rule and were false matches; restricting the family removed them.
#
# HOW THE SURFACE INDEX WAS FOUND, and why a fixed offset does not work. It
# sits at the END of the header minus 4, so it moves with k. Read at a fixed
# +10 it is right for the 14-byte header and does not exist in the 10-byte one,
# and the best any fixed offset managed across both was 50.5%. Read relative to
# the header end it is 100.0% on every length: 7882 of 7882 at h=10, 13309 of
# 13309 at h=14, 102 of 102 at h=18, 84 of 84 at h=22.
#
# AND THE TEST THAT SAYS SO IS GEOMETRIC, not a range check. For each candidate
# the polygon's own normal is computed from its vertices by Newell's method,
# and compared with the plane that its surface names - a two-hop chain,
# polygon -> surface -> plane, against a number derived from a completely
# different array. A wrong header length gives wrong vertex indices, which give
# a wrong normal, which does not match. That is why polygons() measures the
# length per record instead of assuming one: every polygon it returns has
# passed that check.
#
# Over every world: 18672 models and 257720 polygons parse, 1046 models do not,
# and 480 more have no locatable surface array (VisBSP is 103 of those).
#
# ---------------------------------------------------------------------------
# THE SURFACE RECORD IS DONE, 53 bytes, and it is the record stage 2 needs.
# It sits immediately after the planes, at a known offset, so it can be read
# without decoding the polygons at all.
#
#   +0   float3   texture-space origin
#   +12  float3   texture-space U
#   +24  float3   texture-space V
#   +36  u16      TEXTURE INDEX into the model's own texture list
#   +38  u32      PLANE INDEX
#   +42  u32      undecoded
#   +46  u32      undecoded
#   +50  u8       undecoded
#   +51  u16      SURFACE FLAGS
#
# HOW THE TEXTURE INDEX WAS FOUND, and why it is not a guess. Every offset and
# every width (u8/u16/u32) in the 53 bytes was tested against one criterion a
# wrong field cannot satisfy: the set of values must be EXACTLY
# {0 .. nTextures-1} - every declared texture referenced by some surface, and
# none out of range. Over models with three or more textures, +36 as a u16 hit
# 1773 of 1773. The next best was 1.6%. An earlier candidate at +46 was "always
# in range" on 99.5% and looked convincing, and it fails this test on 99.6% of
# the same models - being in range is what small numbers do.
#
# HOW THE FLAGS WERE CONFIRMED, ACROSS TWO FILE FORMATS. A surface's flags
# default to the DTX user-flags word of the texture it uses - a value in a
# completely different file that this parser had no hand in producing. Reading
# the texture index at +36, looking its name up in the model's list, opening
# that .dtx and comparing its +0x14 against the u16 at +51:
#
#     620561 of 622960 surfaces agree - 99.6%, over 19718 models.
#
# The 0.4% that differ are per-surface overrides, which is exactly why the
# value is stored per surface instead of being looked up. The plane index at
# +38 is in range on all 19718 models, zero exceptions.
#
# WHAT THIS SAYS ABOUT THE MARKER GEOMETRY, game-wide and file-derived:
#
#     AI.dtx          49435 surfaces, flags 202 on every one
#     Invisible.dtx   31980 surfaces, 202 on 31940 (32 are 0, 8 are 40)
#     Occluder.dtx       58 surfaces, 202 on every one
#     Sky.dtx          2292 surfaces, flags 110
#     Hullmaker.dtx       4 surfaces, flags 40
#
#   and by model class: 100.0% of all 49290 AIVolume surfaces and 100.0% of all
#   2028 blocker surfaces carry flags 202, against 0.7% of PhysicsBSP.
#   Ordinary models carry them too - 13.1% of "other" surfaces and 14.9% of
#   translucent ones - which is the case a per-model name list can never
#   handle, and the reason the decision has to be taken here.
#
# WHICH BIT MEANS "DO NOT DRAW" IS NOT SETTLED, and must not be guessed. 202 is
# 0xCA and 110 is 0x6E; they share bits 1, 3 and 6, and ordinary surfaces carry
# 20, 30, 10, 11 and 71. The experiment that settles it is in the renderer, not
# in this file: skip polygons whose surface flags match a candidate rule, run
# the desk harness, and look at whether the "sky" and "INVISIBLE" lettering in
# logs/shot-file/drive-009.png goes away while WORLD DRAWN stays where it is.
# One desk run per candidate, and the counter says which.
#
# ---------------------------------------------------------------------------
# STILL OPEN
#
# 1. VISBSP. Its surface array is NOT immediately after its planes - the check
#    above fails on VisBSP in all 103 worlds and on 377 other models. Searching
#    for the offset where the flags become legal finds it 273494 bytes later in
#    M01S02 and 148049 in M01S04, and those do not divide by any count in the
#    header. VisBSP is the only model with a non-zero node count (c5) and with
#    c7/c8/c10 populated, so something - the BSP tree, or lightmap data - sits
#    in between. THIS IS THE ONE THAT MATTERS: VisBSP is the visible world.
#
# 2. WHAT k IS WHEN IT IS MORE THAN ONE. The polygon record is now structured
#    (see above) and k - the number of lightmap references - is 0 or 1 on
#    99.1% of polygons. Where it is 2, 3 or 5 the cause is unidentified; the
#    likely answer is one lightmap per light animation, because the world's
#    render-data section opens with a list whose first entry is named
#    "LightAnim_BASE". The parse handles it by measuring, so this is a gap in
#    understanding rather than in capability.
#
# 3. The 70-ish bytes between the polygons and the points, which is 14 * c9 on
#    most models and not on all.
#
# DO NOT GUESS A STRIDE. Two counts agreeing is not a parse that works, and two
# hand-picked models agreeing is not a sweep - the per-polygon vertex-count
# field looked correct on both models it was first tried against and was wrong
# on 98 of the 103 worlds.
