"""Reads the triangle meshes and texture paths of a Skyrim Special Edition .nif (Gamebryo 20.2.0.7,
Bethesda stream version 100, BSTriShape geometry). Layouts follow niftools' nif.xml
(https://github.com/niftools/nifxml): Header, NiObjectNET, NiAVObject, NiNode, BSTriShape,
BSVertexDesc / BSVertexDataSSE, BSLightingShaderProperty, BSShaderTextureSet.

Every block's size is in the header (since 20.2.0.5), so blocks that are not read are skipped.
"""
import struct

import numpy as np


class NifError(Exception):
    pass


class _R:
    def __init__(self, b, p=0):
        self.b, self.p = b, p

    def u8(self):
        self.p += 1
        return self.b[self.p - 1]

    def u16(self):
        self.p += 2
        return struct.unpack_from("<H", self.b, self.p - 2)[0]

    def u32(self):
        self.p += 4
        return struct.unpack_from("<I", self.b, self.p - 4)[0]

    def i32(self):
        self.p += 4
        return struct.unpack_from("<i", self.b, self.p - 4)[0]

    def u64(self):
        self.p += 8
        return struct.unpack_from("<Q", self.b, self.p - 8)[0]

    def f32(self, n=1):
        v = struct.unpack_from("<%df" % n, self.b, self.p)
        self.p += 4 * n
        return v if n > 1 else v[0]

    def export_string(self):  # byte length, includes the trailing zero
        n = self.u8()
        self.p += n
        return self.b[self.p - n:self.p].rstrip(b"\0").decode("latin-1")

    def sized_string(self):  # uint length
        n = self.u32()
        self.p += n
        return self.b[self.p - n:self.p].decode("latin-1")


def read_nif(path):
    """[(name, positions (n,3), normals (n,3), uvs (n,2) v-up, faces (m,3), textures [diffuse, normal,
    ..., env mask], shader)], positions in the file's units with node transforms applied.
    shader is "lighting" or "effect" (BSEffectShaderProperty: glow/overlay layers)."""
    b = open(path, "rb").read()
    line = b[:b.index(b"\n")]
    if not line.startswith(b"Gamebryo File Format"):
        raise NifError("not a Gamebryo .nif file")
    r = _R(b, len(line) + 1)
    version, endian, user_version, n_blocks = r.u32(), r.u8(), r.u32(), r.u32()
    if version != 0x14020007 or endian != 1:
        raise NifError(f"nif version {version:#x} is not supported (only Skyrim SE / Fallout 4 era 20.2.0.7)")
    bsver = r.u32()
    if bsver != 100:
        raise NifError(f"Bethesda stream version {bsver} is not supported yet (Skyrim SE = 100; "
                       "Skyrim LE = 83 needs converting to SE first, e.g. with Cathedral Assets Optimizer)")
    r.export_string()  # author
    r.export_string()  # process script
    r.export_string()  # export script
    types = [r.sized_string() for _ in range(r.u16())]
    block_type = [types[r.u16() & 0x7FFF] for _ in range(n_blocks)]
    sizes = [r.u32() for _ in range(n_blocks)]
    n_strings = r.u32()
    r.u32()  # max string length
    strings = [r.sized_string() for _ in range(n_strings)]
    n_groups = r.u32()
    r.p += 4 * n_groups
    starts = []
    p = r.p
    for s in sizes:
        starts.append(p)
        p += s

    def string(i):
        return strings[i] if 0 <= i < len(strings) else ""

    def net(r, lighting=False):
        if lighting:
            r.u32()  # shader type
        name = string(r.i32())
        n_extra = r.u32()
        r.p += 4 * n_extra  # extra data refs
        r.i32()  # controller
        return name

    def av(r):
        r.u32()  # flags (BS version > 26)
        t = np.array(r.f32(3))
        m = np.array(r.f32(9)).reshape(3, 3)  # column-major: row vectors map as v @ m
        s = r.f32()
        r.i32()  # collision object
        return t, m, s

    nodes, shapes = {}, {}
    for i, kind in enumerate(block_type):
        rr = _R(b, starts[i])
        if kind in ("NiNode", "BSFadeNode", "BSLeafAnimNode", "BSMultiBoundNode", "BSOrderedNode", "BSValueNode"):
            name = net(rr)
            t, m, s = av(rr)
            children = [rr.i32() for _ in range(rr.u32())]
            nodes[i] = (name, t, m, s, children)
        elif kind in ("BSTriShape", "BSDynamicTriShape", "BSSubIndexTriShape", "BSMeshLODTriShape"):
            name = net(rr)
            t, m, s = av(rr)
            rr.f32(4)  # bounding sphere
            skin, shader, _alpha = rr.i32(), rr.i32(), rr.i32()
            desc = rr.u64()
            n_tris, n_verts = rr.u16(), rr.u16()
            data_size = rr.u32()
            stride = (desc & 0xF) * 4
            flags = (desc >> 44) & 0xFFFF
            if data_size == 0 or n_verts == 0:
                continue
            raw = np.frombuffer(b, np.uint8, n_verts * stride, rr.p).reshape(n_verts, stride)
            rr.p += n_verts * stride
            o = 0
            pos = raw[:, o:o + 12].copy().view("<f4").reshape(n_verts, 3).astype(np.float64)
            o += 16  # position + bitangent X or unused W
            uv = np.zeros((n_verts, 2))
            if flags & 0x2:
                uv = raw[:, o:o + 4].copy().view("<f2").reshape(n_verts, 2).astype(np.float64)
                o += 4
            nrm = np.zeros((n_verts, 3))
            if flags & 0x8:
                nrm = raw[:, o:o + 3].astype(np.float64) / 255.0 * 2.0 - 1.0
                o += 4
            faces = np.frombuffer(b, "<u2", n_tris * 3, rr.p).reshape(n_tris, 3).astype(np.int64)
            shapes[i] = (name, t, m, s, pos, nrm, uv, faces, shader, skin)

    def textures(shader):
        if shader < 0 or block_type[shader] != "BSLightingShaderProperty":
            return []
        rr = _R(b, starts[shader])
        net(rr, lighting=True)
        rr.u32(), rr.u32()  # shader flags 1, 2
        rr.f32(4)  # uv offset, uv scale
        ts = rr.i32()
        if ts < 0 or block_type[ts] != "BSShaderTextureSet":
            return []
        rt = _R(b, starts[ts])
        return [rt.sized_string() for _ in range(rt.u32())]

    # World transforms down the node tree from the roots.
    child_of = {c: k for k, (_, _, _, _, ch) in nodes.items() for c in ch}
    world = {}

    def node_world(k):
        if k in world:
            return world[k]
        _, t, m, s, _ = nodes[k]
        if k in child_of:
            pt, pm, ps = node_world(child_of[k])
            res = (pt + ps * (t @ pm), m @ pm, s * ps)
        else:
            res = (t, m, s)
        world[k] = res
        return res

    def node_path(k):
        names = []
        while k is not None:
            names.append(nodes[k][0] if k in nodes else "")
            k = child_of.get(k)
        return names

    out = []
    for i, (name, t, m, s, pos, nrm, uv, faces, shader, skin) in shapes.items():
        wt, wm, ws = (t, m, s)
        if i in child_of:
            pt, pm, ps = node_world(child_of[i])
            wt, wm, ws = pt + ps * (t @ pm), m @ pm, s * ps
        p = ws * (pos @ wm) + wt
        n = nrm @ wm
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
        kind = "effect" if shader >= 0 and block_type[shader] == "BSEffectShaderProperty" else "lighting"
        parents = [x for x in node_path(child_of.get(i)) if x] if i in child_of else []
        # DirectX UVs (v down) -> OBJ convention (v up)
        out.append((name, parents, p, n, np.column_stack([uv[:, 0], 1.0 - uv[:, 1]]), faces,
                    textures(shader), kind))
    return out
