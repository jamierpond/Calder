import json
import math
import struct
from dataclasses import dataclass, field

DOCUMENT_MAGIC = b"MIB1"
JOURNAL_MAGIC = b"MIBJ"
MINIMUM_DOCUMENT_SIZE = 10
MINIMUM_KEY_TABLE_OFFSET = 9
ROOT_VALUE_OFFSET = 8
JOURNAL_HEADER_SIZE = 8
FRAME_OVERHEAD = 9
FRAME_HEADER_SIZE = 5
FRAME_CHECKSUM_SIZE = 4
MAX_VARINT_BYTES = 10
MAX_DEPTH = 256

FNV_OFFSET_BASIS = 0x811C9DC5
FNV_PRIME = 0x01000193

TAG_NULL, TAG_FALSE, TAG_TRUE, TAG_INT, TAG_DOUBLE, TAG_FLOAT32, TAG_STRING = range(7)
TAG_OBJECT, TAG_MAP, TAG_ARRAY = 7, 8, 9
TAG_PACKED_INT32, TAG_PACKED_INT64 = 12, 13
TAG_PACKED_STRUCT_ARRAY = 18
FRAME_KIND_SET, FRAME_KIND_REMOVE = 1, 2

# Field type byte of a PackedStructArray field: struct format and width.
PACKED_FIELD_FORMATS = {
    0: ("b", 1),
    1: ("B", 1),
    2: ("h", 2),
    3: ("H", 2),
    4: ("i", 4),
    5: ("I", 4),
    6: ("q", 8),
    7: ("Q", 8),
    8: ("f", 4),
    9: ("d", 8),
}

FIXED_WIDTH_PACKED_FORMATS = {
    10: ("f", 4),
    11: ("d", 8),
    14: ("b", 1),
    15: ("B", 1),
    16: ("h", 2),
    17: ("H", 2),
}


class MibError(ValueError):
    def __init__(self, offset, message):
        super().__init__(f"offset {offset}: {message}")
        self.offset = offset
        self.reason = message


class Reader:
    def __init__(self, data, offset, limit):
        self.data = data
        self.offset = offset
        self.limit = limit

    def require(self, byte_count, what):
        if byte_count > self.limit - self.offset:
            raise MibError(self.offset, f"{what} runs past the end of its container")

    def read_bytes(self, byte_count, what):
        self.require(byte_count, what)
        start = self.offset
        self.offset += byte_count
        return self.data[start : self.offset]

    def read_u8(self, what):
        return self.read_bytes(1, what)[0]

    def read_u32(self, what):
        return struct.unpack("<I", self.read_bytes(4, what))[0]

    def read_varint(self, what):
        start = self.offset
        result = 0
        for index in range(MAX_VARINT_BYTES):
            byte = self.read_u8(what)
            result |= (byte & 0x7F) << (7 * index)
            if byte & 0x80 == 0:
                if index == MAX_VARINT_BYTES - 1 and byte > 0x01:
                    raise MibError(start, f"{what}: varint overflows 64 bits")
                return result
        raise MibError(start, f"{what}: varint longer than 10 bytes")

    def read_zigzag(self, what):
        unsigned = self.read_varint(what)
        if unsigned & 1 == 0:
            return unsigned >> 1
        return -((unsigned >> 1) + 1)

    def read_text(self, what):
        length = self.read_varint(f"{what} length")
        start = self.offset
        raw = self.read_bytes(length, what)
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise MibError(start + error.start, f"{what} is not valid UTF-8") from None


def read_document_key_names(data):
    if len(data) < MINIMUM_DOCUMENT_SIZE:
        raise MibError(0, f"document is {len(data)} bytes, shorter than 10")
    if data[:4] != DOCUMENT_MAGIC:
        raise MibError(0, "document does not start with MIB1")
    table_offset = struct.unpack_from("<I", data, 4)[0]
    if not MINIMUM_KEY_TABLE_OFFSET <= table_offset <= len(data):
        raise MibError(4, f"key table offset {table_offset} is outside 9..{len(data)}")
    return read_key_table(data, table_offset), table_offset


def decode_document(data):
    data = bytes(data)
    names, table_offset = read_document_key_names(data)
    reader = Reader(data, ROOT_VALUE_OFFSET, table_offset)
    root = decode_value(reader, names, 0)
    if reader.offset != table_offset:
        raise MibError(reader.offset, f"root value ends before the key table at {table_offset}")
    return root


def read_key_table(data, table_offset):
    reader = Reader(data, table_offset, len(data))
    count = reader.read_varint("key table count")
    if count > len(data) - reader.offset:
        raise MibError(table_offset, f"key table count {count} exceeds the bytes left")
    names = [reader.read_text("key table name") for _ in range(count)]
    if reader.offset != len(data):
        raise MibError(reader.offset, "bytes left over after the key table")
    return names


def decode_value(reader, names, depth):
    if depth > MAX_DEPTH:
        raise MibError(reader.offset, f"nesting deeper than {MAX_DEPTH}")
    tag_offset = reader.offset
    tag = reader.read_u8("tag")
    if tag == TAG_NULL:
        return None
    if tag == TAG_FALSE:
        return False
    if tag == TAG_TRUE:
        return True
    if tag == TAG_INT:
        return reader.read_zigzag("Int")
    if tag == TAG_DOUBLE:
        return struct.unpack("<d", reader.read_bytes(8, "Double"))[0]
    if tag == TAG_FLOAT32:
        return struct.unpack("<f", reader.read_bytes(4, "Float32"))[0]
    if tag == TAG_STRING:
        return reader.read_text("String")
    if tag in (TAG_OBJECT, TAG_MAP):
        return decode_object(reader, names, depth, tag)
    if tag == TAG_ARRAY:
        return decode_array(reader, names, depth)
    if tag in (TAG_PACKED_INT32, TAG_PACKED_INT64):
        return decode_packed_varints(reader)
    if tag in FIXED_WIDTH_PACKED_FORMATS:
        return decode_fixed_width_packed(reader, tag)
    if tag == TAG_PACKED_STRUCT_ARRAY:
        return decode_packed_struct_array(reader, names)
    raise MibError(tag_offset, f"unknown tag {tag}")


def open_sized_container(reader, what):
    size = reader.read_u32(f"{what} payload size")
    reader.require(size, f"{what} payload")
    return Reader(reader.data, reader.offset, reader.offset + size)


def read_counted_elements(payload, what):
    count = payload.read_u32(f"{what} count")
    if count > payload.limit - payload.offset:
        raise MibError(payload.offset - 4, f"{what} count {count} exceeds its payload")
    return count


def finish_container(reader, payload, what):
    if payload.offset != payload.limit:
        raise MibError(payload.offset, f"{what} elements do not fill its payload")
    reader.offset = payload.limit


def decode_object(reader, names, depth, tag):
    what = "Object" if tag == TAG_OBJECT else "Map"
    payload = open_sized_container(reader, what)
    result = {}
    while payload.offset < payload.limit:
        if tag == TAG_OBJECT:
            id_offset = payload.offset
            key_id = payload.read_varint("key id")
            if key_id >= len(names):
                raise MibError(id_offset, f"key id {key_id} not in key table of {len(names)}")
            key = names[key_id]
        else:
            key = payload.read_text("map key")
        result[key] = decode_value(payload, names, depth + 1)
    finish_container(reader, payload, what)
    return result


def decode_array(reader, names, depth):
    payload = open_sized_container(reader, "Array")
    count = read_counted_elements(payload, "Array")
    items = [decode_value(payload, names, depth + 1) for _ in range(count)]
    finish_container(reader, payload, "Array")
    return items


def decode_packed_varints(reader):
    payload = open_sized_container(reader, "packed integer array")
    count = read_counted_elements(payload, "packed integer array")
    items = [payload.read_zigzag("packed integer") for _ in range(count)]
    finish_container(reader, payload, "packed integer array")
    return items


def decode_fixed_width_packed(reader, tag):
    element_format, width = FIXED_WIDTH_PACKED_FORMATS[tag]
    count = reader.read_u32("packed array count")
    raw = reader.read_bytes(count * width, "packed array elements")
    return list(struct.unpack(f"<{count}{element_format}", raw))


def read_packed_struct_field(payload, names):
    path_offset = payload.offset
    path_length = payload.read_varint("packed struct field path length")
    if path_length == 0 or path_length > payload.limit - payload.offset:
        raise MibError(path_offset, f"packed struct field path length {path_length} is invalid")
    path = []
    for _ in range(path_length):
        id_offset = payload.offset
        key_id = payload.read_varint("packed struct field key id")
        if key_id >= len(names):
            raise MibError(id_offset, f"key id {key_id} not in key table of {len(names)}")
        path.append(names[key_id])
    type_offset = payload.offset
    field_type = payload.read_u8("packed struct field type")
    if field_type not in PACKED_FIELD_FORMATS:
        raise MibError(type_offset, f"unknown packed struct field type {field_type}")
    return tuple(path), PACKED_FIELD_FORMATS[field_type]


def check_packed_struct_paths(paths, offset):
    leaves = set(paths)
    if len(leaves) != len(paths):
        raise MibError(offset, "packed struct layout names a field twice")
    for path in paths:
        for prefix_length in range(1, len(path)):
            if path[:prefix_length] in leaves:
                raise MibError(offset, f"packed struct field {'.'.join(path)} is inside a number")


def store_at_path(record, path, value):
    for segment in path[:-1]:
        record = record.setdefault(segment, {})
    record[path[-1]] = value


def decode_packed_struct_array(reader, names):
    payload = open_sized_container(reader, "PackedStructArray")
    count = read_counted_elements(payload, "PackedStructArray")
    layout_offset = payload.offset
    field_count = payload.read_varint("packed struct field count")
    if field_count == 0 or field_count > payload.limit - payload.offset:
        raise MibError(layout_offset, f"packed struct field count {field_count} is invalid")
    fields = [read_packed_struct_field(payload, names) for _ in range(field_count)]
    check_packed_struct_paths([path for path, _ in fields], layout_offset)
    record_format = "<" + "".join(element_format for _, (element_format, _) in fields)
    record_size = sum(width for _, (_, width) in fields)
    if count * record_size != payload.limit - payload.offset:
        raise MibError(payload.offset, "PackedStructArray records do not fill its payload")
    records = []
    for values in struct.iter_unpack(record_format, payload.read_bytes(count * record_size, "records")):
        record = {}
        for (path, _), value in zip(fields, values):
            store_at_path(record, path, value)
        records.append(record)
    finish_container(reader, payload, "PackedStructArray")
    return records


def fnv1a32(data):
    hash_value = FNV_OFFSET_BASIS
    for byte in data:
        hash_value = ((hash_value ^ byte) * FNV_PRIME) & 0xFFFFFFFF
    return hash_value


@dataclass
class JournalReport:
    tree: object
    frames_applied: int = 0
    truncated_at: int | None = None
    skipped_frames: list = field(default_factory=list)
    base_problem: str | None = None


def read_journal(data):
    data = bytes(data)
    if len(data) < JOURNAL_HEADER_SIZE or data[:4] != JOURNAL_MAGIC:
        raise MibError(0, "journal does not start with MIBJ and an 8-byte header")
    base_size = struct.unpack_from("<I", data, 4)[0]
    if base_size > len(data) - JOURNAL_HEADER_SIZE:
        raise MibError(4, f"base size {base_size} runs past the end of the journal")
    base = data[JOURNAL_HEADER_SIZE : JOURNAL_HEADER_SIZE + base_size]
    report = JournalReport(tree={})
    base_names = read_base_key_names(base)
    try:
        report.tree = decode_document(base)
    except MibError as error:
        report.base_problem = f"base document does not decode ({error}); replayed onto {{}}"
    if not isinstance(report.tree, dict):
        report.base_problem = "base root is not an object; frames not applied"
        return report
    replay_frames(data, JOURNAL_HEADER_SIZE + base_size, base_names, report)
    return report


def read_base_key_names(base):
    try:
        names, _ = read_document_key_names(base)
    except MibError:
        return None
    return names


def replay_frames(data, offset, base_names, report):
    while offset < len(data):
        remaining = len(data) - offset
        if remaining < FRAME_OVERHEAD:
            report.truncated_at = offset
            return
        payload_size = struct.unpack_from("<I", data, offset + 1)[0]
        if payload_size > remaining - FRAME_OVERHEAD:
            report.truncated_at = offset
            return
        frame_end = offset + FRAME_OVERHEAD + payload_size
        problem = apply_frame(data[offset:frame_end], base_names, report.tree)
        if problem is None:
            report.frames_applied += 1
        else:
            report.skipped_frames.append((offset, problem))
        offset = frame_end


def decode_frame_value(frame_value, base_names):
    reader = Reader(frame_value, 0, len(frame_value))
    value = decode_value(reader, base_names, 0)
    if reader.offset != len(frame_value):
        raise MibError(reader.offset, "frame value ends before its payload does")
    return value


def apply_frame(frame_bytes, base_names, tree):
    kind = frame_bytes[0]
    checksum_offset = len(frame_bytes) - FRAME_CHECKSUM_SIZE
    payload = frame_bytes[FRAME_HEADER_SIZE:checksum_offset]
    checksum = struct.unpack_from("<I", frame_bytes, checksum_offset)[0]
    if kind not in (FRAME_KIND_SET, FRAME_KIND_REMOVE):
        return f"unknown frame kind {kind}"
    if checksum != fnv1a32(payload):
        return "checksum mismatch"
    reader = Reader(payload, 0, len(payload))
    try:
        key = reader.read_text("frame key")
    except MibError as error:
        return str(error)
    if kind == FRAME_KIND_REMOVE:
        tree.pop(key, None)
        return None
    if base_names is None:
        return "base key table is unreadable, so a set frame cannot be decoded"
    try:
        tree[key] = decode_frame_value(payload[reader.offset :], base_names)
    except MibError as error:
        return f"frame value does not decode ({error})"
    return None


def decode(data):
    data = bytes(data)
    if data[:4] == JOURNAL_MAGIC:
        return read_journal(data)
    return decode_document(data)


def replace_non_finite_floats(value):
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, list):
        return [replace_non_finite_floats(item) for item in value]
    if isinstance(value, dict):
        return {key: replace_non_finite_floats(item) for key, item in value.items()}
    return value


def to_json_text(tree, indent=None):
    return json.dumps(replace_non_finite_floats(tree), indent=indent, ensure_ascii=False, allow_nan=False)
