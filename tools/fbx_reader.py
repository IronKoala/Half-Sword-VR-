"""Autodesk .fbx reader in plain Python (numpy + zlib), binary and text files, for the weapon converter.

Why not a library: the converter is a one-file exe built with whatever Python is on the build PC, and the
FBX readers on PyPI either crash (ufbx 0.0.5's bindings segfault when their objects are freed) or have no
build for new Pythons (assimp-py). What a weapon model needs from an FBX file is small:
  - the node tree (binary: "Kaydara FBX Binary" header, then records of end offset, property count,
    property bytes, name, typed properties, child records; offsets are 64-bit from version 7500;
    text: "Name: prop, prop { ... }" with arrays written "*N { a: ... }");
  - Objects: Geometry (Vertices, PolygonVertexIndex with the last corner of each polygon stored as
    -index-1, LayerElementNormal / LayerElementUV / LayerElementMaterial with their Mapping- and
    ReferenceInformationType), Model (Properties70 transform values), Material, Texture, Video
    (embedded image bytes in Content);
  - Connections: "OO" child -> parent, "OP" child -> parent's property (a texture on a material's
    DiffuseColor / NormalMap ...);
  - GlobalSettings UnitScaleFactor (cm per file unit).
A model's world transform is the FBX SDK's:
    T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1, times its parent's,
with the geometry's own offset (GeometricTranslation/Rotation/Scaling) applied to its mesh only.
Rotations are Euler angles in degrees in the model's RotationOrder (0 = XYZ, x applied first);
Pre/PostRotation always XYZ.
"""
import base64
import math
import re
import struct
import zlib

import numpy as np


class Node:
    __slots__ = ("name", "props", "children")

    def __init__(self, name, props, children):
        self.name, self.props, self.children = name, props, children

    def find(self, name):
        for c in self.children:
            if c.name == name:
                return c
        return None

    def all(self, name):
        return [c for c in self.children if c.name == name]

    def value(self, name, default=None):
        c = self.find(name)
        return c.props[0] if c is not None and c.props else default


# ---- binary ---------------------------------------------------------------------------------------------
_SCALARS = {b"Y": "<h", b"C": "<?", b"I": "<i", b"F": "<f", b"D": "<d", b"L": "<q"}
_ARRAYS = {b"f": np.float32, b"d": np.float64, b"l": np.int64, b"i": np.int32, b"b": np.uint8}


def _parse_binary(data):
    version = struct.unpack_from("<I", data, 23)[0]
    wide = version >= 7500
    head = struct.Struct("<QQQ" if wide else "<III")

    def props(pos, count):
        out = []
        for _ in range(count):
            t = data[pos:pos + 1]
            pos += 1
            if t in _SCALARS:
                fmt = _SCALARS[t]
                out.append(struct.unpack_from(fmt, data, pos)[0])
                pos += struct.calcsize(fmt)
            elif t in _ARRAYS:
                n, enc, clen = struct.unpack_from("<III", data, pos)
                pos += 12
                raw = data[pos:pos + clen]
                pos += clen
                if enc == 1:
                    raw = zlib.decompress(raw)
                out.append(np.frombuffer(raw, _ARRAYS[t], n))
            elif t in (b"S", b"R"):
                n = struct.unpack_from("<I", data, pos)[0]
                pos += 4
                raw = data[pos:pos + n]
                pos += n
                out.append(raw.decode("utf-8", "replace") if t == b"S" else raw)
            else:
                raise ValueError(f"unknown FBX property type {t!r}")
        return out

    def nodes(pos, end):
        out = []
        while pos + head.size <= end:
            stop, count, plen = head.unpack_from(data, pos)
            nlen = data[pos + head.size]
            if stop == 0:
                break
            name = data[pos + head.size + 1:pos + head.size + 1 + nlen].decode("ascii", "replace")
            ppos = pos + head.size + 1 + nlen
            children = nodes(ppos + plen, stop) if ppos + plen < stop else []
            out.append(Node(name, props(ppos, count), children))
            pos = stop
        return out

    return Node("", [], nodes(27, len(data)))


# ---- text -----------------------------------------------------------------------------------------------
_TOKEN = re.compile(rb'\s*(?:;[^\n]*\n\s*)*(?:("(?:[^"]|"")*")|([A-Za-z_][\w|]*:)|([{}])|(\*\d+)|(,)|([^\s,{}"]+))')


def _parse_text(data):
    """Text FBX: 'Name: p1, p2 {' children '}'; arrays 'Name: *N { a: 1,2,3 }'."""
    pos, n = 0, len(data)
    tokens = []
    while pos < n:
        m = _TOKEN.match(data, pos)
        if not m or m.end() == pos:
            break
        pos = m.end()
        if m.group(1) is not None:
            tokens.append(("s", m.group(1)[1:-1].decode("utf-8", "replace")))
        elif m.group(2) is not None:
            tokens.append(("k", m.group(2)[:-1].decode("ascii", "replace")))
        elif m.group(3) is not None:
            tokens.append((m.group(3).decode(), None))
        elif m.group(4) is not None:
            tokens.append(("n", int(m.group(4)[1:])))
        elif m.group(5) is not None:
            tokens.append((",", None))
        else:
            tokens.append(("v", m.group(6).decode("ascii", "replace")))

    def number(text):
        try:
            return int(text)
        except ValueError:
            try:
                return float(text)
            except ValueError:
                return text

    i = 0

    def block():
        nonlocal i
        out = []
        while i < len(tokens):
            kind, val = tokens[i]
            if kind == "}":
                i += 1
                return out
            if kind != "k":
                i += 1
                continue
            name = val
            i += 1
            props, children = [], []
            while i < len(tokens) and tokens[i][0] in ("s", "v", ",", "n"):
                k, v = tokens[i]
                if k == "s":
                    props.append(v)
                elif k == "v":
                    props.append(number(v))
                i += 1
                # a new "Key:" on the next line ends the properties (no braces)
            if i < len(tokens) and tokens[i][0] == "{":
                i += 1
                children = block()
            # an array: "Name: *N { a: v,v,... }" -> one numpy property
            if len(children) == 1 and children[0].name == "a" and not props:
                vals = children[0].props
                arr = np.array([v for v in vals if not isinstance(v, str)], dtype=np.float64)
                if all(isinstance(v, int) for v in vals):
                    arr = arr.astype(np.int64)
                props, children = [arr], []
            out.append(Node(name, props, children))
        return out

    return Node("", [], block())


def parse(path):
    data = open(path, "rb").read()
    if data.startswith(b"Kaydara FBX Binary"):
        return _parse_binary(data)
    return _parse_text(data)


# ---- the scene ------------------------------------------------------------------------------------------
def _props70(node):
    """A node's property table: FBX 7 "Properties70" of P records (name, type, label, flags, values...)
    or FBX 6 "Properties60" of Property records (name, type, flags, values...)."""
    out = {}
    if node is None:
        return out
    p70 = node.find("Properties70")
    for p in (p70.all("P") if p70 is not None else []):
        if p.props:
            out[p.props[0]] = p.props[4:] if len(p.props) > 4 else []
    p60 = node.find("Properties60")
    for p in (p60.all("Property") if p60 is not None else []):
        if p.props:
            out[p.props[0]] = p.props[3:] if len(p.props) > 3 else []
    return out


def _array(node, dtype):
    """An array value: one numpy property (binary, and text FBX 7's "*N { a: }"), or FBX 6 text's plain
    comma list (many number properties)."""
    if node is None or not node.props:
        return np.zeros(0, dtype)
    if len(node.props) == 1 and isinstance(node.props[0], np.ndarray):
        return np.asarray(node.props[0], dtype)
    return np.asarray([v for v in node.props if isinstance(v, (int, float, np.integer, np.floating))], dtype)


def _find_prop(node, name):
    """The first Properties70/60 value called name anywhere in the tree (FBX 6 keeps the unit in
    different places)."""
    t = _props70(node)
    if t.get(name):
        return t[name]
    for c in node.children:
        v = _find_prop(c, name)
        if v:
            return v
    return None


def _vec(props, key, default):
    v = props.get(key)
    if not v or len(v) < 3:
        return np.array(default, np.float64)
    return np.array([float(x) for x in v[:3]], np.float64)


def _euler(deg, order=0):
    """Rotation matrix for Euler angles (degrees); order 0..5 = XYZ XZY YZX YXZ ZXY ZYX, the first
    letter applied first."""
    x, y, z = (math.radians(a) for a in deg)
    rx = np.array([[1, 0, 0], [0, math.cos(x), -math.sin(x)], [0, math.sin(x), math.cos(x)]])
    ry = np.array([[math.cos(y), 0, math.sin(y)], [0, 1, 0], [-math.sin(y), 0, math.cos(y)]])
    rz = np.array([[math.cos(z), -math.sin(z), 0], [math.sin(z), math.cos(z), 0], [0, 0, 1]])
    seq = {0: (rx, ry, rz), 1: (rx, rz, ry), 2: (ry, rz, rx), 3: (ry, rx, rz), 4: (rz, rx, ry), 5: (rz, ry, rx)}
    a, b, c = seq.get(int(order), seq[0])
    return c @ b @ a


def _m4(r=None, t=None, s=None):
    m = np.eye(4)
    if r is not None:
        m[:3, :3] = r
    if s is not None:
        m[:3, :3] = m[:3, :3] @ np.diag(s)
    if t is not None:
        m[:3, 3] = t
    return m


def _local(props):
    t = _vec(props, "Lcl Translation", (0, 0, 0))
    r = _vec(props, "Lcl Rotation", (0, 0, 0))
    s = _vec(props, "Lcl Scaling", (1, 1, 1))
    order = int(props.get("RotationOrder", [0])[0]) if props.get("RotationOrder") else 0
    pre = _euler(_vec(props, "PreRotation", (0, 0, 0)))
    post = _euler(_vec(props, "PostRotation", (0, 0, 0)))
    roff, rp = _vec(props, "RotationOffset", (0, 0, 0)), _vec(props, "RotationPivot", (0, 0, 0))
    soff, sp = _vec(props, "ScalingOffset", (0, 0, 0)), _vec(props, "ScalingPivot", (0, 0, 0))
    return (_m4(t=t) @ _m4(t=roff) @ _m4(t=rp) @ _m4(r=pre) @ _m4(r=_euler(r, order)) @ _m4(r=post.T)
            @ _m4(t=-rp) @ _m4(t=soff) @ _m4(t=sp) @ _m4(s=s) @ _m4(t=-sp))


def _geometric(props):
    return _m4(t=_vec(props, "GeometricTranslation", (0, 0, 0))) @ _m4(r=_euler(_vec(props, "GeometricRotation", (0, 0, 0)))) \
        @ _m4(s=_vec(props, "GeometricScaling", (1, 1, 1)))


def _layer(geom, name, values_key, index_key, n_ctrl, poly_of_corner, ctrl_of_corner):
    """Per-corner values of a LayerElement (normals, UVs), or None."""
    le = geom.find(name)
    if le is None:
        return None
    vals = le.find(values_key)
    if vals is None or not vals.props:
        return None
    width = 3 if values_key == "Normals" else 2
    v = _array(vals, np.float64)
    v = v[: len(v) // width * width].reshape(-1, width)
    mapping = str(le.value("MappingInformationType", "ByPolygonVertex"))
    ref = str(le.value("ReferenceInformationType", "Direct"))
    n_corner = len(ctrl_of_corner)
    if mapping in ("ByVertice", "ByVertex", "ByControlPoint"):
        key = ctrl_of_corner
    elif mapping == "ByPolygon":
        key = poly_of_corner
    elif mapping == "AllSame":
        key = np.zeros(n_corner, np.int64)
    else:  # ByPolygonVertex
        key = np.arange(n_corner)
    if ref in ("IndexToDirect", "Index"):
        idx = le.find(index_key)
        ia = _array(idx, np.int64)
        if len(ia):
            key = ia[np.clip(key, 0, len(ia) - 1)]
    key = np.clip(key, 0, len(v) - 1)
    return v[key]


def load(path):
    """[(names, positions (n,3) in metres, normals or None, uvs (n,2), triangles (m,3), material)] with one
    entry per mesh and material; material = dict(name, diffuse=(r,g,b) or None, textures={slot: (file
    names, embedded bytes or None)}). Unit: GlobalSettings UnitScaleFactor (cm per unit)."""
    root = parse(path)
    us = _find_prop(root, "UnitScaleFactor")
    unit_cm = float(us[0]) if us else 1.0
    objects = root.find("Objects")
    if objects is None:
        return [], unit_cm
    # Objects by id: FBX 7 numbers; FBX 6 the "Type::name" string.
    by_id = {}
    for o in objects.children:
        if o.props and isinstance(o.props[0], (int, np.integer)):
            by_id[int(o.props[0])] = o
        elif o.props and isinstance(o.props[0], str):
            by_id[o.props[0]] = o

    def key(v):
        return int(v) if isinstance(v, (int, np.integer)) else str(v)
    parent_of, children_of, prop_links = {}, {}, []
    conns = root.find("Connections")
    for c in ((conns.all("C") + conns.all("Connect")) if conns is not None else []):
        if len(c.props) < 3:
            continue
        kind, child, parent = c.props[0], key(c.props[1]), key(c.props[2])
        children_of.setdefault(parent, []).append(child)
        if kind == "OO":
            parent_of.setdefault(child, parent)
        elif kind == "OP" and len(c.props) > 3:
            prop_links.append((child, parent, str(c.props[3])))

    def short(n):
        s = str(n.props[1]) if len(n.props) > 1 and not isinstance(n.props[0], str) else str(n.props[0])
        return s.split("\x00\x01")[0].split("::")[-1]

    world_cache = {}

    def world(mid):
        if mid in world_cache:
            return world_cache[mid]
        node = by_id.get(mid)
        m = _local(_props70(node)) if node is not None else np.eye(4)
        p = parent_of.get(mid)
        if p is not None and p in by_id and by_id[p].name == "Model":
            m = world(p) @ m
        world_cache[mid] = m
        return m

    # textures on materials: OP texture -> material property; Video (embedded) -> texture (OO)
    def texture_info(tid):
        t = by_id.get(tid)
        names = [str(t.value(k)) for k in ("RelativeFilename", "FileName", "Filename") if t is not None and t.value(k)]
        content = None
        for v in children_of.get(tid, []):
            vn = by_id.get(v)
            if vn is not None and vn.name == "Video":
                c = vn.find("Content")
                if c is not None and c.props:
                    raw = c.props[0]
                    if isinstance(raw, str):  # text FBX: base64, possibly in several chunks
                        try:
                            raw = b"".join(base64.b64decode(x) for x in c.props if isinstance(x, str) and x)
                        except ValueError:
                            raw = b""
                    if isinstance(raw, (bytes, bytearray)) and len(raw) > 4:
                        content = bytes(raw)
                names += [str(vn.value(k)) for k in ("RelativeFilename", "Filename") if vn.value(k)]
        return names, content

    # FBX 6: an embedded image may sit in a Video that is not linked to its texture: matched by file name.
    video_by_name = {}
    for v in by_id.values():
        if v.name == "Video" and v.find("Content") is not None and v.find("Content").props:
            raw = v.find("Content").props[0]
            if isinstance(raw, (bytes, bytearray)) and len(raw) > 4:
                for k in ("RelativeFilename", "Filename", "FileName"):
                    if v.value(k):
                        video_by_name[str(v.value(k)).replace("\\", "/").rsplit("/", 1)[-1].lower()] = bytes(raw)

    def texture_info_any(tid):
        names, content = texture_info(tid)
        if content is None:
            for n in names:
                content = video_by_name.get(n.replace("\\", "/").rsplit("/", 1)[-1].lower())
                if content:
                    break
        return names, content

    mat_textures = {}
    for child, parent, prop in prop_links:
        if child in by_id and by_id[child].name == "Texture" and parent in by_id and by_id[parent].name == "Material":
            mat_textures.setdefault(parent, {})[prop] = texture_info_any(child)

    def material(mid):
        if mid is None or mid not in by_id:
            return None
        n = by_id[mid]
        p = _props70(n)
        diffuse = None
        for key in ("DiffuseColor", "Diffuse", "base_color"):
            if p.get(key) and len(p[key]) >= 3:
                diffuse = tuple(float(x) for x in p[key][:3])
                break
        return dict(name=short(n), diffuse=diffuse, textures=mat_textures.get(mid, {}))

    out = []
    for gid, g in by_id.items():
        if g.name not in ("Geometry", "Model") or g.find("Vertices") is None or g.find("PolygonVertexIndex") is None:
            continue
        # FBX 7: a Geometry linked to its Model; FBX 6: the Model holds its own mesh.
        model = gid if g.name == "Model" else parent_of.get(gid)
        if model is None or model not in by_id or by_id[model].name != "Model":
            continue
        mnode = by_id[model]
        ctrl = _array(g.find("Vertices"), np.float64)
        ctrl = ctrl[: len(ctrl) // 3 * 3].reshape(-1, 3)
        pvi = _array(g.find("PolygonVertexIndex"), np.int64)
        if len(pvi) == 0 or len(ctrl) == 0:
            continue
        last = pvi < 0
        ctrl_of_corner = np.where(last, -pvi - 1, pvi)
        poly_of_corner = np.concatenate([[0], np.cumsum(last)[:-1]])
        starts = np.concatenate([[0], np.nonzero(last)[0][:-1] + 1])
        ends = np.nonzero(last)[0]
        nrm = _layer(g, "LayerElementNormal", "Normals", "NormalsIndex", len(ctrl), poly_of_corner, ctrl_of_corner)
        uv = _layer(g, "LayerElementUV", "UV", "UVIndex", len(ctrl), poly_of_corner, ctrl_of_corner)
        mats_of_model = [c for c in children_of.get(model, []) if c in by_id and by_id[c].name == "Material"]
        # FBX 6: textures hang on the model, not on its materials: material i gets texture i (or the only one).
        # Which: LayerElementTexture's TextureId per polygon, matched to the polygons' materials; without
        # it, texture i to material i (one texture and one material: that one).
        tex_of_model = [c for c in children_of.get(model, []) if c in by_id and by_id[c].name == "Texture"]
        let = g.find("LayerElementTexture")
        tex_ids = _array(let.find("TextureId"), np.int64) if let is not None else np.zeros(0, np.int64)
        lem0 = g.find("LayerElementMaterial")
        mat_ids = _array(lem0.find("Materials"), np.int64) if lem0 is not None else np.zeros(0, np.int64)
        for i, mid in enumerate(mats_of_model):
            if not tex_of_model or mat_textures.get(mid):
                continue
            pick = None
            if len(tex_ids):
                if len(tex_ids) == 1 or str(let.value("MappingInformationType", "")) == "AllSame":
                    pick = int(tex_ids[0])
                elif len(mat_ids) == len(tex_ids):
                    ids = tex_ids[mat_ids == i]
                    pick = int(np.bincount(ids[ids >= 0]).argmax()) if (ids >= 0).any() else None
                elif len(mat_ids) <= 1:
                    pick = int(np.bincount(tex_ids[tex_ids >= 0]).argmax()) if (tex_ids >= 0).any() else None
            elif i < len(tex_of_model) and (len(tex_of_model) == len(mats_of_model) or len(mats_of_model) == 1):
                pick = i
            if pick is not None and 0 <= pick < len(tex_of_model):
                mat_textures[mid] = {"DiffuseColor": texture_info_any(tex_of_model[pick])}
        lem = g.find("LayerElementMaterial")
        n_poly = len(ends)
        if lem is not None and len(_array(lem.find("Materials"), np.int64)):
            mi = _array(lem.find("Materials"), np.int64)
            poly_mat = (np.full(n_poly, mi[0] if len(mi) else 0) if str(lem.value("MappingInformationType", "")) == "AllSame"
                        or len(mi) < n_poly else mi[:n_poly])
        else:
            poly_mat = np.zeros(n_poly, np.int64)
        # fan triangles per polygon
        tri_a, tri_mat = [], []
        for p, (s0, e0) in enumerate(zip(starts, ends)):
            for k in range(s0 + 1, e0):
                tri_a.append((s0, k, k + 1))
                tri_mat.append(poly_mat[p])
        if not tri_a:
            continue
        tri_a = np.asarray(tri_a, np.int64)
        tri_mat = np.asarray(tri_mat, np.int64)
        mprops = _props70(mnode)
        m = world(model) @ _geometric(mprops)
        pos = ctrl[ctrl_of_corner] @ m[:3, :3].T + m[:3, 3]
        if nrm is not None:
            nm = np.linalg.inv(m[:3, :3]).T
            nrm = nrm @ nm.T
            nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
        if np.linalg.det(m[:3, :3]) < 0:  # mirrored: keep the faces facing out
            tri_a = tri_a[:, ::-1]
        for mi_ in np.unique(tri_mat):
            sel = tri_a[tri_mat == mi_]
            used = np.unique(sel)
            remap = np.full(len(pos), -1, np.int64)
            remap[used] = np.arange(len(used))
            mid = mats_of_model[mi_] if 0 <= mi_ < len(mats_of_model) else None
            mat = material(mid)
            names = [short(mnode) or "mesh", short(g)] + ([mat["name"]] if mat else [])
            out.append((names, pos[used] * unit_cm / 100.0, None if nrm is None else nrm[used],
                        np.zeros((len(used), 2)) if uv is None else uv[used], remap[sel], mat))
    return out, unit_cm
