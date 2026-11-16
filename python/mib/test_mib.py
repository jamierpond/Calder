import math
import struct
import unittest

from mib import MibError, decode, decode_document, fnv1a32, read_journal, to_json_text


def from_hex(text):
    return bytes.fromhex(text)


def u32(value):
    return struct.pack("<I", value)


def document(root_bytes, names=()):
    table = bytes([len(names)]) + b"".join(bytes([len(name)]) + name for name in names)
    return b"MIB1" + u32(8 + len(root_bytes)) + root_bytes + table


def sized(tag, payload):
    return bytes([tag]) + u32(len(payload)) + payload


def frame(kind, payload, checksum=None):
    if checksum is None:
        checksum = fnv1a32(payload)
    return bytes([kind]) + u32(len(payload)) + payload + u32(checksum)


def journal(base, frames=b""):
    return b"MIBJ" + u32(len(base)) + base + frames


EXAMPLE_NESTED = from_hex(
    "4D 49 42 31 37 00 00 00 07 2A 00 00 00 00 06 03"
    "4D 69 78 01 09 1E 00 00 00 02 00 00 00 07 08 00"
    "00 00 02 06 01 41 03 03 F0 01 07 08 00 00 00 02"
    "06 01 42 03 03 B4 01 04 04 6E 61 6D 65 06 74 72"
    "61 63 6B 73 05 74 69 74 6C 65 03 62 70 6D"
)
EXAMPLE_PACKED_FLOATS = from_hex(
    "4D 49 42 31 41 00 00 00 07 34 00 00 00 00 0A 02"
    "00 00 00 00 00 00 3F 00 00 80 3F 01 04 9A 99 99"
    "99 99 99 B9 3F 02 05 00 00 80 3E 03 0B 02 00 00"
    "00 9A 99 99 99 99 99 B9 3F 00 00 00 00 00 00 00"
    "40 04 05 67 61 69 6E 73 06 63 75 74 6F 66 66 05"
    "6C 65 76 65 6C 05 63 75 72 76 65"
)
EXAMPLE_OPTIONALS = from_hex(
    "4D 49 42 31 14 00 00 00 07 07 00 00 00 00 00 01"
    "03 0A 03 00 04 01 61 01 62 01 63 01 64"
)
EXAMPLE_MAP = from_hex(
    "4D 49 42 31 1B 00 00 00 07 0E 00 00 00 00 08 08"
    "00 00 00 01 78 03 02 01 79 03 03 01 06 63 6F 75"
    "6E 74 73"
)
EXAMPLE_JOURNAL = from_hex(
    "4D 49 42 4A 1A 00 00 00 4D 49 42 31 17 00 00 00"
    "08 0A 00 00 00 01 61 07 03 00 00 00 00 03 02 01"
    "01 78 01 0A 00 00 00 01 62 07 03 00 00 00 00 03"
    "04 4B 32 C9 8E 02 02 00 00 00 01 61 17 B6 74 4C"
)
EXAMPLE_LEAKS = from_hex(
    "4D 49 42 31 2F 00 00 00 07 22 00 00 00 00 06 02"
    "6F 6E 01 03 0E 02 03 01 03 07 05 00 00 00 04 06"
    "02 68 69 05 0D 06 00 00 00 02 00 00 00 02 01 06"
    "04 6D 6F 64 65 05 73 74 72 61 79 02 69 64 06 65"
    "69 74 68 65 72 12 62 61 73 69 63 5F 73 74 72 69"
    "6E 67 3C 63 68 61 72 3E 03 62 69 67"
)

EXAMPLE_PACKED_STRUCTS = from_hex(
    "4D 49 42 31 37 00 00 00 07 2A 00 00 00 00 12 24"
    "00 00 00 02 00 00 00 03 01 01 02 02 02 03 08 02"
    "02 04 08 01 00 00 00 00 3F 00 00 80 3F FE FF 00"
    "00 00 40 00 00 80 3E 05 07 6D 61 72 6B 65 72 73"
    "02 69 64 02 61 74 01 78 01 79"
)


def packed_structs(count, layout, records):
    return sized(18, u32(count) + layout + records)


def decode_root(root_bytes, names=()):
    return decode_document(document(root_bytes, names))


class WorkedExamples(unittest.TestCase):
    def test_nested_object_with_list_of_objects(self):
        expected = {"name": "Mix", "tracks": [{"title": "A", "bpm": 120}, {"title": "B", "bpm": 90}]}
        self.assertEqual(decode_document(EXAMPLE_NESTED), expected)

    def test_packed_float_list_keeps_floats(self):
        tree = decode_document(EXAMPLE_PACKED_FLOATS)
        self.assertEqual(tree, {"gains": [0.5, 1.0], "cutoff": 0.1, "level": 0.25, "curve": [0.1, 2.0]})
        self.assertTrue(all(isinstance(number, float) for number in tree["gains"] + tree["curve"]))

    def test_empty_optional_and_absent_field(self):
        self.assertEqual(decode_document(EXAMPLE_OPTIONALS), {"a": None, "b": 5, "d": None})

    def test_map_with_inline_keys(self):
        self.assertEqual(decode_document(EXAMPLE_MAP), {"counts": {"x": 1, "y": -2}})

    def test_journal_with_two_frames(self):
        report = read_journal(EXAMPLE_JOURNAL)
        self.assertEqual(report.tree, {"b": {"x": 2}})
        self.assertEqual(report.frames_applied, 2)
        self.assertIsNone(report.truncated_at)
        self.assertEqual(report.skipped_frames, [])

    def test_journal_cut_inside_frame_two_is_torn(self):
        for length in range(54, 64):
            report = read_journal(EXAMPLE_JOURNAL[:length])
            self.assertEqual(report.tree, {"a": {"x": 1}, "b": {"x": 2}}, length)
            self.assertEqual(report.truncated_at, 53)

    def test_journal_corrupt_frame_one_is_skipped(self):
        damaged = bytearray(EXAMPLE_JOURNAL)
        damaged[40] ^= 0xFF
        report = read_journal(bytes(damaged))
        self.assertEqual(report.tree, {})
        self.assertEqual(len(report.skipped_frames), 1)

    def test_leaks_on_the_wire(self):
        expected = {"mode": "on", "stray": 7, "id": -1, "either": {"basic_string<char>": "hi"}, "big": [1, -1]}
        self.assertEqual(decode_document(EXAMPLE_LEAKS), expected)

    def test_packed_struct_array_is_a_list_of_objects(self):
        self.assertEqual(
            decode_document(EXAMPLE_PACKED_STRUCTS),
            {"markers": [{"id": 1, "at": {"x": 0.5, "y": 1.0}}, {"id": -2, "at": {"x": 2.0, "y": 0.25}}]},
        )

    def test_smallest_document(self):
        self.assertIsNone(decode_document(from_hex("4D 49 42 31 09 00 00 00 00 00")))


class PrimitiveExamples(unittest.TestCase):
    def test_varint_and_zigzag(self):
        self.assertEqual(decode_root(from_hex("03 F0 01")), 120)
        self.assertEqual(decode_root(from_hex("03 80 00")), 0)

    def test_extreme_integers(self):
        self.assertEqual(decode_root(from_hex("03 FE FF FF FF FF FF FF FF FF 01")), 2**63 - 1)
        self.assertEqual(decode_root(from_hex("03 FF FF FF FF FF FF FF FF FF 01")), -(2**63))

    def test_floats(self):
        self.assertEqual(decode_root(from_hex("05 00 00 C0 3F")), 1.5)
        self.assertEqual(decode_root(from_hex("05 CD CC CC 3D")), 0.10000000149011612)
        self.assertEqual(decode_root(from_hex("04 9A 99 99 99 99 99 B9 3F")), 0.1)
        self.assertIsInstance(decode_root(from_hex("05 00 00 40 40")), float)

    def test_non_finite_floats_print_as_null(self):
        nan = decode_root(from_hex("04 00 00 00 00 00 00 F8 7F"))
        infinity = decode_root(from_hex("05 00 00 80 7F"))
        self.assertTrue(math.isnan(nan))
        self.assertEqual(infinity, math.inf)
        self.assertEqual(to_json_text([nan, infinity, -infinity]), "[null, null, null]")

    def test_negative_zero(self):
        value = decode_root(from_hex("05 00 00 00 80"))
        self.assertEqual(value, 0.0)
        self.assertEqual(math.copysign(1.0, value), -1.0)

    def test_string_with_zero_byte(self):
        self.assertEqual(decode_root(from_hex("06 03 61 00 62")), "a\x00b")

    def test_empty_containers(self):
        self.assertEqual(decode_root(from_hex("07 00 00 00 00")), {})
        self.assertEqual(decode_root(from_hex("09 04 00 00 00 00 00 00 00")), [])
        self.assertEqual(decode_root(from_hex("0A 00 00 00 00")), [])

    def test_packed_int16(self):
        self.assertEqual(decode_root(from_hex("10 02 00 00 00 FF FF 2C 01")), [-1, 300])

    def test_every_packed_kind(self):
        self.assertEqual(decode_root(from_hex("0E 02 00 00 00 FF 7F")), [-1, 127])
        self.assertEqual(decode_root(from_hex("0F 02 00 00 00 FF 7F")), [255, 127])
        self.assertEqual(decode_root(from_hex("11 01 00 00 00 FF FF")), [65535])
        self.assertEqual(decode_root(from_hex("0C 07 00 00 00 02 00 00 00 01 F0 01")), [-1, 120])

    def test_duplicate_keys_last_wins(self):
        root = sized(8, from_hex("01 6B 03 02 01 6B 03 04"))
        self.assertEqual(decode_root(root), {"k": 2})

    def test_mixed_array(self):
        root = sized(9, u32(3) + from_hex("00 02 06 01 7A"))
        self.assertEqual(decode_root(root), [None, True, "z"])

    def test_unused_key_table_names(self):
        self.assertEqual(decode_root(from_hex("00"), (b"unused", b"names")), None)

    def test_fnv1a32(self):
        self.assertEqual(fnv1a32(from_hex("01 61")), 0x4C74B617)


class ValidationRules(unittest.TestCase):
    def assertRejected(self, data, fragment):
        with self.assertRaises(MibError) as caught:
            decode_document(data)
        self.assertIn(fragment, str(caught.exception))

    def test_rule1_too_short_or_wrong_magic(self):
        self.assertRejected(from_hex("4D 49 42 31 09 00 00 00 00"), "shorter than 10")
        self.assertRejected(b"MIB2" + EXAMPLE_MAP[4:], "MIB1")

    def test_rule2_key_table_offset_range(self):
        self.assertRejected(b"MIB1" + u32(8) + from_hex("00 00"), "key table offset")
        self.assertRejected(b"MIB1" + u32(11) + from_hex("00 00"), "key table offset")

    def test_rule3_key_table(self):
        self.assertRejected(b"MIB1" + u32(9) + from_hex("00 05 00"), "key table count")
        self.assertRejected(b"MIB1" + u32(9) + from_hex("00 01 05 61"), "key table name")
        self.assertRejected(b"MIB1" + u32(9) + from_hex("00 00 00"), "after the key table")
        self.assertRejected(b"MIB1" + u32(9) + from_hex("00 80"), "key table count")

    def test_rule4_root_ends_at_key_table(self):
        self.assertRejected(b"MIB1" + u32(10) + from_hex("00 00 00"), "root value ends")
        self.assertRejected(b"MIB1" + u32(9) + from_hex("03 00"), "Int")

    def test_rule5_unknown_tag(self):
        self.assertRejected(document(from_hex("13")), "unknown tag 19")

    def test_rule15_packed_struct_layout(self):
        names = (b"a", b"b")
        one_u8_field = from_hex("01 01 00 01")
        self.assertEqual(decode_root(packed_structs(2, one_u8_field, from_hex("07 FF")), names), [{"a": 7}, {"a": 255}])
        self.assertRejected(document(packed_structs(0, from_hex("00"), b""), names), "field count 0")
        self.assertRejected(document(packed_structs(0, from_hex("01 00 00 01"), b""), names), "path length 0")
        self.assertRejected(document(packed_structs(0, from_hex("01 01 05 01"), b""), names), "key id 5")
        self.assertRejected(document(packed_structs(0, from_hex("01 01 00 0A"), b""), names), "field type 10")
        self.assertRejected(document(packed_structs(2, one_u8_field, from_hex("07")), names), "do not fill")
        self.assertRejected(document(packed_structs(0, from_hex("02 01 00 01 01 00 01"), b""), names), "twice")
        self.assertRejected(document(packed_structs(0, from_hex("02 01 00 01 02 00 01 01"), b""), names), "inside a number")

    def test_rule6_varints(self):
        self.assertRejected(document(from_hex("03 FF FF FF FF FF FF FF FF FF 02")), "overflows 64 bits")
        self.assertRejected(document(from_hex("03" + "80" * 10 + "00")), "longer than 10 bytes")
        self.assertRejected(document(sized(7, from_hex("80")), (b"a",)), "key id")

    def test_rule7_string_length(self):
        self.assertRejected(document(from_hex("06 05 61 62")), "String")
        self.assertRejected(document(sized(8, from_hex("05 61 03 00"))), "map key")

    def test_rule8_payload_size_inside_container(self):
        self.assertRejected(document(from_hex("07 10 00 00 00 00")), "Object payload")
        inner_too_big = sized(9, u32(1) + from_hex("09 FF 00 00 00"))
        self.assertRejected(document(inner_too_big), "Array payload")

    def test_rule9_entries_fill_object(self):
        entry_overruns = bytes([7]) + u32(2) + from_hex("00 03 02")
        self.assertRejected(document(entry_overruns, (b"a",)), "Int")
        missing_value = sized(7, from_hex("00"))
        self.assertRejected(document(missing_value, (b"a",)), "tag")

    def test_rule10_counted_arrays(self):
        self.assertRejected(document(sized(9, u32(0xFFFFFFFF))), "count")
        self.assertRejected(document(sized(9, u32(1) + from_hex("00 00"))), "do not fill")
        self.assertRejected(document(sized(13, u32(3) + from_hex("02 04"))), "count")
        self.assertRejected(document(sized(12, u32(1) + from_hex("02 04"))), "do not fill")
        self.assertRejected(document(sized(12, u32(1) + from_hex("80"))), "packed integer")

    def test_rule11_fixed_width_packed_fits(self):
        self.assertRejected(document(from_hex("0B FF FF FF FF 00")), "packed array elements")
        self.assertRejected(document(from_hex("10 02 00 00 00 FF FF 2C")), "packed array elements")

    def test_rule12_key_id_in_table(self):
        self.assertRejected(document(sized(7, from_hex("01 00")), (b"a",)), "key id 1")

    def test_rule13_utf8(self):
        self.assertRejected(document(from_hex("06 02 C3 28")), "String is not valid UTF-8")
        self.assertRejected(document(sized(8, from_hex("01 FF 00"))), "map key is not valid UTF-8")
        self.assertRejected(document(from_hex("00"), (b"\xff",)), "key table name is not valid UTF-8")

    def test_rule14_depth(self):
        depth = 300
        nested = from_hex("00")
        for _ in range(depth):
            nested = sized(9, u32(1) + nested)
        self.assertRejected(document(nested), "nesting deeper")


class JournalRules(unittest.TestCase):
    base_with_a = document(sized(8, from_hex("01 61 03 02")))
    base_with_x = document(sized(8, from_hex("01 61") + sized(7, from_hex("00 03 02"))), [b"x"])

    def set_frame(self, key, frame_value):
        return frame(1, bytes([len(key)]) + key + frame_value)

    def test_bad_header_is_an_error(self):
        with self.assertRaises(MibError):
            read_journal(b"MIBJ" + u32(5))
        with self.assertRaises(MibError):
            read_journal(b"MIBJ" + u32(100) + self.base_with_a)

    def test_replay_set_remove_set(self):
        frames = (
            self.set_frame(b"b", from_hex("03 04"))
            + frame(2, from_hex("01 61"))
            + self.set_frame(b"a", from_hex("03 06"))
        )
        report = read_journal(journal(self.base_with_a, frames))
        self.assertEqual(report.tree, {"b": 2, "a": 3})
        self.assertEqual(report.frames_applied, 3)

    def test_unknown_kind_wrong_checksum_and_bad_key_are_skipped(self):
        frames = (
            frame(3, from_hex("01 61"))
            + frame(2, from_hex("01 61"), checksum=0)
            + frame(2, from_hex("05 61"))
            + self.set_frame(b"b", from_hex("00"))
        )
        report = read_journal(journal(self.base_with_a, frames))
        self.assertEqual(report.tree, {"a": 1, "b": None})
        self.assertEqual(len(report.skipped_frames), 3)

    def test_set_frame_with_bad_value_is_skipped(self):
        report = read_journal(journal(self.base_with_a, self.set_frame(b"a", b"MIB1")))
        self.assertEqual(report.tree, {"a": 1})
        self.assertEqual(len(report.skipped_frames), 1)

    def test_frame_value_names_fields_from_the_base_key_table(self):
        frames = self.set_frame(b"b", sized(7, from_hex("00 03 04")))
        report = read_journal(journal(self.base_with_x, frames))
        self.assertEqual(report.tree, {"a": {"x": 1}, "b": {"x": 2}})

    def test_frame_key_id_outside_the_base_table_is_skipped(self):
        frames = self.set_frame(b"b", sized(7, from_hex("01 03 04"))) + self.set_frame(b"c", from_hex("03 06"))
        report = read_journal(journal(self.base_with_x, frames))
        self.assertEqual(report.tree, {"a": {"x": 1}, "c": 3})
        self.assertEqual(len(report.skipped_frames), 1)

    def test_frame_value_must_fill_its_payload(self):
        report = read_journal(journal(self.base_with_a, self.set_frame(b"b", from_hex("03 04 00"))))
        self.assertEqual(report.tree, {"a": 1})
        self.assertEqual(len(report.skipped_frames), 1)

    def test_whole_document_as_frame_value_is_skipped(self):
        report = read_journal(journal(self.base_with_a, self.set_frame(b"b", document(from_hex("03 04")))))
        self.assertEqual(report.tree, {"a": 1})
        self.assertEqual(len(report.skipped_frames), 1)

    def test_remove_frame_ignores_trailing_bytes(self):
        report = read_journal(journal(self.base_with_a, frame(2, from_hex("01 61 FF FF"))))
        self.assertEqual(report.tree, {})

    def test_header_shorter_than_nine_is_torn(self):
        report = read_journal(journal(self.base_with_a, from_hex("01 00 00 00 00 00 00 00")))
        self.assertEqual(report.truncated_at, 8 + len(self.base_with_a))

    def test_bad_base_root_replays_onto_empty_object(self):
        base_with_bad_root = document(from_hex("13"), [b"x"])
        report = read_journal(journal(base_with_bad_root, self.set_frame(b"b", from_hex("02"))))
        self.assertEqual(report.tree, {"b": True})
        self.assertIsNotNone(report.base_problem)

    def test_unreadable_base_key_table_skips_set_frames(self):
        frames = self.set_frame(b"b", from_hex("02")) + frame(2, from_hex("01 62"))
        report = read_journal(journal(b"junk", frames))
        self.assertEqual(report.tree, {})
        self.assertEqual(report.frames_applied, 1)
        self.assertEqual(len(report.skipped_frames), 1)
        self.assertIsNotNone(report.base_problem)

    def test_non_object_base_ignores_frames(self):
        report = read_journal(journal(document(from_hex("03 02")), frame(2, from_hex("01 61"))))
        self.assertEqual(report.tree, 1)
        self.assertEqual(report.frames_applied, 0)
        self.assertIsNotNone(report.base_problem)

    def test_decode_dispatches_on_magic(self):
        self.assertEqual(decode(EXAMPLE_JOURNAL).tree, {"b": {"x": 2}})
        self.assertEqual(decode(EXAMPLE_MAP), {"counts": {"x": 1, "y": -2}})


if __name__ == "__main__":
    unittest.main()
