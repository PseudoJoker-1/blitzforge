"""Reader for DAVA .sc2 scene files and the KeyedArchive blocks inside them.

Enough to inspect and edit particle effects: an .sc2 is a small SFV2 header
followed by KeyedArchive blocks, and a KeyedArchive is a flat list of
VariantType key/value pairs where a value may itself be a nested archive.

    from sc2 import load_sc2, dump
    scene = load_sc2(path)          # -> Scene
    dump(scene.archives[0])         # pretty-print the tree
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Any

MAGIC = b"SFV2"
KA_MAGIC = b"KA"

# DAVA VariantType tags.
(NONE, BOOLEAN, INT32, FLOAT, STRING, WIDE_STRING, BYTE_ARRAY, UINT32,
 KEYED_ARCHIVE, INT64, UINT64, VECTOR2, VECTOR3, VECTOR4, MATRIX2, MATRIX3,
 MATRIX4, COLOR, FASTNAME, AABBOX3, FILEPATH, FLOAT64, INT8, UINT8, INT16,
 UINT16) = range(26)

TYPE_NAMES = {
    NONE: "none", BOOLEAN: "bool", INT32: "int32", FLOAT: "float",
    STRING: "string", WIDE_STRING: "wstring", BYTE_ARRAY: "bytes",
    UINT32: "uint32", KEYED_ARCHIVE: "archive", INT64: "int64",
    UINT64: "uint64", VECTOR2: "vec2", VECTOR3: "vec3", VECTOR4: "vec4",
    MATRIX2: "mat2", MATRIX3: "mat3", MATRIX4: "mat4", COLOR: "color",
    FASTNAME: "fastname", AABBOX3: "aabb", FILEPATH: "filepath",
    FLOAT64: "float64", INT8: "int8", UINT8: "uint8", INT16: "int16",
    UINT16: "uint16",
}

# Tags whose payload is a plain fixed-size struct.
FIXED = {
    BOOLEAN: ("<?", 1), INT8: ("<b", 1), UINT8: ("<B", 1),
    INT16: ("<h", 2), UINT16: ("<H", 2),
    INT32: ("<i", 4), UINT32: ("<I", 4), FLOAT: ("<f", 4),
    INT64: ("<q", 8), UINT64: ("<Q", 8), FLOAT64: ("<d", 8),
    VECTOR2: ("<2f", 8), VECTOR3: ("<3f", 12), VECTOR4: ("<4f", 16),
    COLOR: ("<4f", 16), MATRIX2: ("<4f", 16), MATRIX3: ("<9f", 36),
    MATRIX4: ("<16f", 64), AABBOX3: ("<6f", 24),
}

# Tags whose payload is uint32 length followed by that many bytes.
LENGTH_PREFIXED = {STRING, BYTE_ARRAY, FASTNAME, FILEPATH}


class Sc2Error(Exception):
    pass


@dataclass
class Value:
    tag: int
    data: Any

    @property
    def type_name(self) -> str:
        return TYPE_NAMES.get(self.tag, f"tag{self.tag}")

    def __repr__(self) -> str:
        return f"<{self.type_name} {self.data!r}>"


@dataclass
class Archive:
    version: int
    items: list[tuple[str, Value]] = field(default_factory=list)

    def get(self, key: str, default=None):
        for k, v in self.items:
            if k == key:
                return v.data
        return default

    def keys(self) -> list[str]:
        return [k for k, _ in self.items]


@dataclass
class Scene:
    version: int
    node_count: int
    archives: list[Archive]


def _read_value(buf: bytes, off: int) -> tuple[Value, int]:
    if off >= len(buf):
        raise Sc2Error("value read past end of buffer")
    tag = buf[off]
    off += 1

    if tag == NONE:
        return Value(tag, None), off

    if tag in FIXED:
        fmt, size = FIXED[tag]
        vals = struct.unpack_from(fmt, buf, off)
        data = vals[0] if len(vals) == 1 else list(vals)
        return Value(tag, data), off + size

    if tag in LENGTH_PREFIXED:
        (length,) = struct.unpack_from("<I", buf, off)
        off += 4
        payload = buf[off:off + length]
        off += length
        if tag == BYTE_ARRAY:
            return Value(tag, payload), off
        return Value(tag, payload.decode("utf-8", "replace")), off

    if tag == WIDE_STRING:
        (length,) = struct.unpack_from("<I", buf, off)
        off += 4
        payload = buf[off:off + length * 2]
        off += length * 2
        return Value(tag, payload.decode("utf-16-le", "replace")), off

    if tag == KEYED_ARCHIVE:
        (length,) = struct.unpack_from("<I", buf, off)
        off += 4
        nested = _read_archive(buf[off:off + length], 0)[0]
        return Value(tag, nested), off + length

    raise Sc2Error(f"unknown VariantType tag {tag} at offset {off - 1}")


def _read_archive(buf: bytes, off: int) -> tuple[Archive, int]:
    if buf[off:off + 2] != KA_MAGIC:
        raise Sc2Error(f"no KeyedArchive magic at offset {off}")
    version, count = struct.unpack_from("<HI", buf, off + 2)
    off += 8

    archive = Archive(version=version)
    for _ in range(count):
        key_val, off = _read_value(buf, off)
        value, off = _read_value(buf, off)
        key = key_val.data if isinstance(key_val.data, str) else repr(key_val.data)
        archive.items.append((key, value))
    return archive, off


def load_sc2(path) -> Scene:
    from pathlib import Path

    raw = Path(path).read_bytes()
    if raw[:5] == b"SFV2\x00" or raw[:4] != MAGIC:
        # allow a .dvpl to be handed straight in
        if raw[-4:] == b"DVPL":
            from dvpl import unpack as dvpl_unpack

            raw = dvpl_unpack(raw)
    if raw[:4] != MAGIC:
        raise Sc2Error(f"not an .sc2: magic={raw[:4]!r}")

    _, version, node_count = struct.unpack_from("<4sII", raw, 0)
    off = 12

    archives: list[Archive] = []
    while off < len(raw) - 8:
        idx = raw.find(KA_MAGIC, off)
        if idx < 0:
            break
        try:
            archive, off = _read_archive(raw, idx)
        except Sc2Error:
            off = idx + 2
            continue
        archives.append(archive)

    return Scene(version=version, node_count=node_count, archives=archives)


def dump(node, indent: int = 0, max_list: int = 6) -> None:
    pad = "  " * indent
    if isinstance(node, Archive):
        print(f"{pad}Archive(v{node.version}, {len(node.items)} items)")
        for key, value in node.items:
            if value.tag == KEYED_ARCHIVE:
                print(f"{pad}  {key}:")
                dump(value.data, indent + 2, max_list)
            else:
                data = value.data
                if isinstance(data, list) and len(data) > max_list:
                    data = data[:max_list] + ["..."]
                if isinstance(data, bytes):
                    data = f"<{len(data)} bytes>"
                print(f"{pad}  {key} ({value.type_name}) = {data}")
    else:
        print(f"{pad}{node!r}")


if __name__ == "__main__":
    import sys

    scene = load_sc2(sys.argv[1])
    print(f"SFV2 version={scene.version} nodeCount={scene.node_count} "
          f"archives={len(scene.archives)}")
    for archive in scene.archives:
        dump(archive)
