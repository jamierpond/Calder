# Calder binary format: specification

Calder was developed inside emberstore, whose encoder (`emberstore/Binary`) is
the reference implementation; the text below still names it where it describes
that encoder.

This document describes emberstore's binary files well enough to write a
decoder for them in any language, without reading the C++ code. A decoder
written from it turns any file into a generic tree with JSON's shapes:
objects with named fields, arrays, strings, numbers, booleans and null.

There are two kinds of file:

- A **document** (starts with the four ASCII bytes `MIB1`) holds one value.
- A **journal** (starts with `MIBJ`) holds a document plus a list of later
  changes to it, appended one at a time. The changes name their fields
  through the document's key table.

emberstore stores these in files ending `.ember`. A stored file that starts
with neither `MIB1` nor `MIBJ` is JSON text, not this format.

The rules below are what the encoder in `emberstore/Binary` writes today
(branch `jp/binary`, November 2026). Where the C++ reader accepts more than
this spec allows, section 9 says so.

Contents:

1. Terms and primitive encodings
2. Document layout
3. Values: every tag
4. The key table
5. Decoding a document into a JSON-shaped tree
6. Journals
7. Limits
8. Validation rules for untrusted input
9. Where the C++ reader is more lenient than this spec
10. Known leaks of C++ into the format
11. Worked examples
12. Checking a decoder against the C++ code

---

## 1. Terms and primitive encodings

**Byte offsets** count from 0 at the first byte of the document (or journal)
being read.

**u8** is one unsigned byte.

**u32** is an unsigned 32-bit integer stored in 4 bytes, **little-endian**
(least significant byte first). `2A 00 00 00` is 42. Every fixed-width field
in the format is little-endian, on every machine.

**f32** is an IEEE 754 binary32 floating-point number in 4 bytes,
little-endian. `00 00 C0 3F` is 1.5.

**f64** is an IEEE 754 binary64 floating-point number in 8 bytes,
little-endian. `9A 99 99 99 99 99 B9 3F` is 0.1.

**i8, u8, i16, u16** are fixed-width integers of 1 or 2 bytes, signed
(two's complement) or unsigned, little-endian. They appear only inside
packed arrays.

**varint** is an unsigned integer of up to 64 bits in 1 to 10 bytes, the
encoding Protocol Buffers and LEB128 use. Each byte carries 7 bits of the
number, least significant group first. The top bit of each byte (0x80) is
set when another byte follows and clear on the last byte.

```
read_varint():
    result = 0
    for i in 0 .. 9:                    # at most 10 bytes
        byte = read_u8()                # error if past the end
        result |= (byte & 0x7F) << (7 * i)
        if byte & 0x80 == 0:
            if i == 9 and byte > 0x01:  # the 10th byte holds only bit 63
                error "varint overflows 64 bits"
            return result
    error "varint longer than 10 bytes"

write_varint(n):                        # n is unsigned 64-bit
    while n >= 0x80:
        write_u8((n & 0x7F) | 0x80)
        n >>= 7
    write_u8(n)
```

`F0 01` is 0x70 + (0x01 << 7) = 240. The encoder always writes the shortest
form. A decoder accepts longer forms too (`80 00` reads as 0).

**zigzag** maps a signed 64-bit integer onto an unsigned one so that numbers
near zero, negative or positive, become small varints: 0 → 0, −1 → 1,
1 → 2, −2 → 3, 2 → 4, and so on. A **zigzag varint** is a signed integer
stored as the varint of its zigzag form.

```
zigzag_encode(n):  # n signed 64-bit; ">>" is an arithmetic (sign-filling) shift
    return ((n << 1) ^ (n >> 63)) as unsigned 64-bit

zigzag_decode(u):  # u unsigned 64-bit
    return ((u >> 1) as signed 64-bit) ^ -((u & 1) as signed 64-bit)

# In a language with unbounded integers (Python):
zigzag_decode(u) = (u >> 1) if u & 1 == 0 else -((u >> 1) + 1)
```

Every zigzag varint decodes to a value in the signed 64-bit range
−2^63 … 2^63 − 1. `F0 01` is varint 240, zigzag-decoded to 120.

**string bytes** are a varint byte length followed by that many bytes. The
format does not promise or check UTF-8 (see section 5.4).

**tag** is the u8 at the start of every value that says what kind of value
it is (section 3).

**payload** is the bytes of a value after its tag.

**payload size** is a u32 that some containers write right after their tag.
It counts the bytes that follow the payload size field itself, up to the end
of the container. It never counts the tag or the 4 bytes of the size field.

**count** is a u32 giving the number of elements in an array.

**key id** is a varint index into the document's key table, used instead of
writing a field name inline.

---

## 2. Document layout

```
offset 0   4 bytes   magic: ASCII "MIB1" = 4D 49 42 31
offset 4   u32       key table offset: where the key table starts
offset 8   value     the root value (any kind, section 3)
...        key table (section 4), starting at the key table offset,
                     running to the end of the document
```

- The root value starts at offset 8, always. It can be any kind of value: an
  object, an array, a bare number, a string, null.
- The root value ends exactly at the key table offset.
- The key table ends exactly at the end of the document.
- A document with no object fields still has a key table: the single byte
  `00` (zero names).

The key table sits at the end because the encoder only knows every field
name once it has written the whole value. A decoder reads the key table
first (it needs the names to decode objects), then decodes the root value.

The smallest document is 10 bytes, for a root of null:
`4D 49 42 31  09 00 00 00  00  00`.

---

## 3. Values: every tag

Every value is one tag byte followed by its payload. There are 18 tags,
numbered 0 to 17. Any other tag byte is an error.

| Tag | Name | Payload | JSON shape |
|----:|------|---------|-----------|
| 0 | Null | nothing | null |
| 1 | False | nothing | false |
| 2 | True | nothing | true |
| 3 | Int | zigzag varint | integer |
| 4 | Double | f64 | floating-point number |
| 5 | Float32 | f32 | floating-point number |
| 6 | String | varint byte length, then the bytes | string |
| 7 | Object | u32 payload size, then entries `{ varint key id, value }` | object |
| 8 | Map | u32 payload size, then entries `{ varint key length, key bytes, value }` | object |
| 9 | Array | u32 payload size, u32 count, then `count` values | array |
| 10 | PackedFloat | u32 count, then `count` × f32 | array of floating-point numbers |
| 11 | PackedDouble | u32 count, then `count` × f64 | array of floating-point numbers |
| 12 | PackedInt32 | u32 payload size, u32 count, then `count` zigzag varints | array of integers |
| 13 | PackedInt64 | u32 payload size, u32 count, then `count` zigzag varints | array of integers |
| 14 | PackedInt8 | u32 count, then `count` × i8 | array of integers |
| 15 | PackedUInt8 | u32 count, then `count` × u8 | array of integers |
| 16 | PackedInt16 | u32 count, then `count` × i16 | array of integers |
| 17 | PackedUInt16 | u32 count, then `count` × u16 | array of integers |

Note which containers carry a payload size and which don't:

- **Object, Map, Array, PackedInt32, PackedInt64** start with a u32 payload
  size, so a reader can skip them without parsing their contents. For Array,
  PackedInt32 and PackedInt64 the payload size *includes* the 4-byte count
  that follows it.
- **The six fixed-width packed arrays** (PackedFloat, PackedDouble,
  PackedInt8, PackedUInt8, PackedInt16, PackedUInt16) have no payload size.
  Their payload is the count followed by exactly `count × width` bytes,
  where width is 4, 8, 1, 1, 2 and 2 bytes respectively.

### 3.1 Null, False, True

The tag is the whole value. Null is used for an empty optional field, and for
an element of an array that the encoder left out (section 5.5).

### 3.2 Int

A signed 64-bit integer as a zigzag varint. All whole-number fields use this
tag, whatever their width in the program that wrote them: 8, 16, 32 or
64 bits, signed or unsigned. An unsigned 64-bit value of 2^63 or more is
written as the signed number with the same 64 bits, so it reads back
negative (section 10.1).

`03 F0 01` is the integer 120.

### 3.3 Double and Float32

Both hold a floating-point number. The encoder uses Float32 when the number
converts to binary32 and back to the identical binary64 value, and Double
otherwise. So Float32 never loses anything: the decoder widens the f32 to
binary64 and gets exactly the number that was written.

- 0.25 is exactly representable in binary32, so it is stored as Float32:
  `05 00 00 80 3E`.
- 0.1 is not, so it is stored as Double: `04 9A 99 99 99 99 99 B9 3F`.
- A field that held binary32 0.1 in the program (the nearest binary32 value
  to 0.1) is stored as Float32 `05 CD CC CC 3D` and decodes to
  0.10000000149011612, the exact value of that binary32 number.
- NaN is always stored as Double (the round-trip check fails for NaN).
  Infinity and −0.0 survive the round trip, so they are stored as Float32.

The choice is made per value, not per field: the same field can be Float32
in one record and Double in the next.

### 3.4 String

```
06 | varint byteLength | byteLength bytes
```

`06 02 68 69` is "hi". The bytes are whatever the program held; the encoder
does not check that they are UTF-8 (section 5.4).

### 3.5 Object

An object whose field names are stored in the key table.

```
07 | u32 payloadSize | entry*
entry := varint keyId | value
```

The entries fill the payload exactly: decode entries until you reach
`(offset after the size field) + payloadSize`. There is no entry count.
`keyId` is an index into the key table (section 4). Entries appear in the
order the program declared its fields; the order carries no meaning.

An empty object is `07 00 00 00 00`.

### 3.6 Map

An object whose keys are data (for example user-chosen names), so they are
written inline rather than in the key table.

```
08 | u32 payloadSize | entry*
entry := varint keyLength | keyLength bytes | value
```

Same rules as Object otherwise. The map key is *not* a tagged String value:
there is no `06` byte before it. The encoder writes maps in the order the
program held them (usually sorted by key bytes, but a decoder must not rely
on that).

`08 08 00 00 00  01 78 03 02  01 79 03 03` is the map `{"x": 1, "y": -2}`.

### 3.7 Array

An array of arbitrary values, each with its own tag.

```
09 | u32 payloadSize | u32 count | value × count
```

`payloadSize` covers the count field and all the elements. The elements may
be of different kinds. An empty array is `09 04 00 00 00 00 00 00 00`.

### 3.8 Packed arrays

A packed array holds numbers without a tag per element.

```
PackedFloat   0A | u32 count | count × f32
PackedDouble  0B | u32 count | count × f64
PackedInt8    0E | u32 count | count × i8
PackedUInt8   0F | u32 count | count × u8
PackedInt16   10 | u32 count | count × i16
PackedUInt16  11 | u32 count | count × u16
PackedInt32   0C | u32 payloadSize | u32 count | count × zigzag varint
PackedInt64   0D | u32 payloadSize | u32 count | count × zigzag varint
```

Rules for each:

- **PackedFloat / PackedDouble.** A list of floating-point numbers is stored
  as PackedFloat when *every* element survives the binary32 round trip
  (section 3.3), otherwise as PackedDouble. So one inexact element keeps the
  whole array at 8 bytes per element. Elements decode to binary64.
- **PackedInt32 / PackedInt64.** Both are zigzag varints, so they decode the
  same way. Each element decodes to a signed 64-bit integer. The tag only
  hints at the width the program used (section 10.5): PackedInt32 for
  signed 32-bit lists; PackedInt64 for signed 64-bit, unsigned 32-bit and
  unsigned 64-bit lists. An unsigned 64-bit element of 2^63 or more reads
  back negative, as for Int. A decoder does not range-check PackedInt32
  elements against 32 bits.
- **PackedInt8 / PackedUInt8 / PackedInt16 / PackedUInt16.** Raw
  fixed-width integers, little-endian; Int8 and Int16 are two's complement.
  `10 02 00 00 00 FF FF 2C 01` is the 16-bit signed list `[-1, 300]`.

Lists of other element kinds (strings, booleans, objects, lists) use Array.
An empty list of numbers is a packed array with count 0, for example
`0A 00 00 00 00`.

---

## 4. The key table

```
keyTable := varint keyCount | name × keyCount
name     := varint byteLength | byteLength bytes
```

The first name has key id 0, the next 1, and so on. Object entries refer to
these ids. Every object in the document shares the one table, so a field
name is stored once however many objects use it.

- The encoder adds names in the order it first meets them.
- The table never lists the same name twice, but a decoder does not need to
  reject duplicates: lookups go by id.
- The table may hold names that no entry uses. The encoder adds a name before
  it knows whether the field will be written, and a field that turns out to
  be absent (section 5.5) leaves its name behind. Unused names mean nothing.
- Names are bytes, like strings (section 5.4).

---

## 5. Decoding a document into a JSON-shaped tree

### 5.1 Procedure

```
decode_document(data):
    check len(data) >= 10 and data[0:4] == "MIB1"
    tableOffset = u32 at offset 4
    check 9 <= tableOffset <= len(data)
    names = read_key_table(data, tableOffset)        # must end exactly at len(data)
    root, end = decode_value(data, 8, limit = tableOffset, names, depth = 0)
    check end == tableOffset
    return root

read_key_table(data, offset):
    count, offset = read_varint(data, offset)
    check count <= len(data) - offset               # each name takes at least one byte
    names = []
    repeat count times:
        length, offset = read_varint(data, offset)
        check offset + length <= len(data)
        names.append(text(data[offset : offset + length]))
        offset += length
    check offset == len(data)
    return names

# Decodes one value starting at `offset`; no byte at or past `limit` may be read.
# Returns the value and the offset just after it.
decode_value(data, offset, limit, names, depth):
    check depth <= MAX_DEPTH
    tag = u8 at offset; offset += 1
    switch tag:
      0: return null, offset
      1: return false, offset
      2: return true, offset
      3: return zigzag_decode(read_varint()), offset
      4: return f64 at offset, offset + 8
      5: return widen(f32 at offset), offset + 4
      6: length = read_varint(); return text(next length bytes), offset after them
      7, 8:
         size = read_u32(); end = offset + size; check end <= limit
         result = empty object
         while offset < end:
             if tag == 7:
                 id = read_varint(); check id < len(names); key = names[id]
             else:
                 length = read_varint(); key = text(next length bytes)
             value, offset = decode_value(data, offset, end, names, depth + 1)
             result[key] = value                     # a later duplicate replaces an earlier one
         check offset == end
         return result, offset
      9:
         size = read_u32(); end = offset + size; check end <= limit
         count = read_u32()
         check count <= end - offset                 # every element takes at least one byte
         items = []
         repeat count times:
             value, offset = decode_value(data, offset, end, names, depth + 1)
             items.append(value)
         check offset == end
         return items, offset
      12, 13:
         size = read_u32(); end = offset + size; check end <= limit
         count = read_u32()
         check count <= end - offset
         items = [zigzag_decode(read_varint()) repeated count times]   # reads stay below end
         check offset == end
         return items, offset
      10, 11, 14, 15, 16, 17:
         width = {10: 4, 11: 8, 14: 1, 15: 1, 16: 2, 17: 2}[tag]
         count = read_u32()
         check count * width <= limit - offset       # compute in 64 bits or unbounded
         items = [read element of that type, count times]
         return items, offset
      otherwise: error "unknown tag"
```

Every `read_…` above fails if it would read at or past `limit`. Section 8
lists the same checks as rules.

### 5.2 What each tag becomes

| Tag | Tree value |
|-----|-----------|
| Null | null |
| False, True | false, true |
| Int | integer (signed 64-bit) |
| Float32, Double | floating-point number (binary64) |
| String | string |
| Object, Map | object |
| Array | array |
| PackedFloat, PackedDouble | array of floating-point numbers |
| PackedInt8 … PackedUInt16, PackedInt32, PackedInt64 | array of integers |

The tree keeps two kinds of number, **integer** and **floating-point**, as
JSON parsers in most languages do. An Int is always an integer, even when
small; a Float32 or Double is always floating-point, even when whole (3.0
stays a float).

### 5.3 Numbers

- **Integers are exact.** An Int holds any signed 64-bit value. A decoder
  keeps it as an integer type of at least 64 bits (Python `int`, Go
  `int64`), never as binary64. JSON text written from the tree prints all
  digits; a reader that parses JSON numbers as binary64 (JavaScript) will
  round integers beyond 2^53 = 9007199254740992, but that loss is the
  reader's, not the tree's.
- **Unsigned 64-bit values of 2^63 or more appear negative.** The tree shows
  the signed value with the same 64 bits (2^64 − 1 shows as −1). Without
  knowing the field was unsigned, a decoder cannot undo this
  (section 10.1).
- **Float32 is widened exactly.** Convert the binary32 to binary64 with no
  rounding step and no shortest-decimal trick. Binary32 0.1 is
  0.10000000149011612 in the tree, not 0.1.
- **NaN and infinity.** The tree keeps them as floating-point values. JSON
  has no spelling for them, so JSON text written from the tree prints `null`
  for NaN, +infinity and −infinity. That is a loss: they cannot be told
  apart from null in the text.
- **Negative zero.** The tree keeps −0.0. JSON text may print it as `-0.0`,
  `-0` or `0`; compare it as equal to 0.

### 5.4 Strings and keys

The format does not say which character encoding strings, map keys and
key-table names use. In practice they are UTF-8: the C++ program holds text
as UTF-8 bytes. The encoder does not check this, and neither does the C++
reader.

For the JSON-shaped tree, a decoder decodes these bytes as UTF-8. Bytes that
are not valid UTF-8 are an error for the JSON view, because JSON strings are
Unicode text. A decoder that also offers a raw view may return the bytes
unchanged instead. A string may contain a zero byte; it is an ordinary
character.

### 5.5 Objects, maps, null and absent fields

- **Object and Map both become a JSON object.** The tree drops which of the
  two it was. The difference is only that Map keys are written inline
  because they are data (section 3.6). A decoder may expose the distinction
  as extra information, but the JSON view has none.
- **Key order.** A decoder preserves the order in the file, but order carries
  no meaning. Two trees with the same keys and values in a different order
  are equal.
- **Duplicate keys.** The encoder never writes the same key twice in one
  object or map. If a file does, the **last** occurrence wins in the tree.
- **Null versus absent.** These are different and the tree keeps them
  different:
  - A field that is present with the value null (`00`) is an optional field
    that was empty. It becomes `"field": null`.
  - A field that is not in the object at all was left out on purpose (the
    program marked it as "may be absent", and it was absent), or the writing
    program did not have that field. It becomes a missing key.
- **Left-out array elements.** When an element of a list is "may be absent"
  and absent, the encoder writes Null in its place so the other elements keep
  their positions. The tree shows null.
- **Nested emptiness collapses.** An optional whose value is itself an empty
  optional is written as a single Null, the same as an empty outer optional.
  The tree can't tell them apart (neither can JSON).

### 5.6 Things the tree shows as plain data

Two program-level constructs have no tag of their own. They decode as
ordinary objects, strings and numbers, and a decoder doesn't need to treat
them specially; section 10 says what is lost.

- **A choice between alternatives** (a value that is one of several types)
  is an Object with exactly one entry. The key names the alternative and the
  value is the alternative's value: `{"Circle": {"radius": 2}}`.
- **A named constant from a fixed list** (an enumeration) is usually a
  String holding the constant's name, `"on"`. It is an Int when the program
  chose to store that list as numbers, or when the value has no name.

---

## 6. Journals

A journal stores a map (an object whose keys are data) that changes one
entry at a time. Each change is appended to the end of the file as a
**frame**, so a single change doesn't rewrite the whole file. emberstore
periodically rewrites the journal as a fresh base with no frames
("compaction"); that is a writer's choice, not part of the format.

Frames don't repeat field names. A set frame's value is a bare value whose
key ids index the **base document's key table**, the one at the end of the
base. A frame never adds a name: when emberstore needs to write a value with
a field name the base's table lacks, it compacts instead, writing a fresh
base whose table has it. So every frame depends on the base alone, and a
corrupt frame loses only itself.

### 6.1 Layout

```
offset 0   4 bytes          magic: ASCII "MIBJ" = 4D 49 42 4A
offset 4   u32              base size: the byte length of the base document
offset 8   base size bytes  base document: a complete "MIB1" document (section 2)
then       frame*           frames, back to back, to the end of the file

frame   := u8 kind | u32 payloadSize | payloadSize bytes of payload | u32 checksum
payload := varint keyLength | keyLength bytes of key | frameValue
```

- **kind** is 1 for **set** (put this key with this value) or 2 for
  **remove** (delete this key). Other values are corrupt.
- **payloadSize** counts the payload bytes only: not the kind, not the size
  field, not the checksum. A frame therefore takes `1 + 4 + payloadSize + 4`
  bytes.
- **key** is the map key, as bytes (section 5.4).
- **frameValue** is everything in the payload after the key. For a set
  frame it is one value (section 3) and nothing else: no `MIB1` magic, no
  key table offset, no key table. The value fills the rest of the payload
  exactly. Its Object key ids index the base document's key table (section
  4). Map keys are inline as always (section 3.6). For a remove frame
  frameValue is empty; if a remove frame has bytes there, ignore them.
- **checksum** is the 32-bit FNV-1a hash of the payload bytes (key length,
  key and frameValue), stored as a u32. The kind byte and the payload size
  are not covered.

FNV-1a, 32-bit:

```
fnv1a32(bytes):
    hash = 2166136261                 # 0x811C9DC5, the FNV-1a 32-bit offset basis
    for b in bytes:
        hash = hash XOR b
        hash = (hash * 16777619) mod 2^32   # 16777619 = 0x01000193, the FNV 32-bit prime
    return hash
```

`fnv1a32(01 61)` is 0x4C74B617, stored as `17 B6 74 4C`.

### 6.2 Reading a journal

```
read_journal(data):
    check len(data) >= 8 and data[0:4] == "MIBJ"
    baseSize = u32 at offset 4
    check baseSize <= len(data) - 8               # otherwise the journal is invalid
    base = data[8 : 8 + baseSize]
    names = the base's key names, read as decode_document does (rules 1-3)  # see "base" below
    tree = decode_document(base)                  # see "base" below
    offset = 8 + baseSize
    while offset < len(data):
        remaining = len(data) - offset
        if remaining < 9: TORN; stop              # 9 = kind + size + checksum
        payloadSize = u32 at offset + 1
        if payloadSize > remaining - 9: TORN; stop
        frameEnd = offset + 9 + payloadSize
        kind     = data[offset]
        payload  = data[offset + 5 : offset + 5 + payloadSize]
        checksum = u32 at offset + 5 + payloadSize
        if kind not in {1, 2} or checksum != fnv1a32(payload):
            CORRUPT; offset = frameEnd; continue
        keyLength, p = read_varint(payload, 0)    # fails if it runs past the payload
        if that failed or p + keyLength > payloadSize:
            CORRUPT; offset = frameEnd; continue
        key = text(payload[p : p + keyLength])
        if kind == 1:
            frameValue = payload[p + keyLength :]
            value, end = decode_value(frameValue, 0, len(frameValue), names, 0)
            if names is missing, that failed, or end != len(frameValue):
                CORRUPT; offset = frameEnd; continue
            tree[key] = value
        else:
            delete tree[key] if present
        offset = frameEnd
    return tree
```

The rules in words:

- **Replay in order.** Apply frames from first to last. A later set on the
  same key replaces the earlier value; a remove deletes it; a set after a
  remove adds it back.
- **Torn frame: stop.** A frame whose header is incomplete, or whose
  declared payload runs past the end of the file, is a write that was
  interrupted. Stop reading there. Keep everything applied before it. Report
  the journal as truncated.
- **Corrupt frame: skip it and go on.** A frame that fits inside the file but
  has an unknown kind, a wrong checksum, or a key that runs past its payload
  is skipped. Its size field still says where the next frame starts, so
  reading continues. Count and report skipped frames. (If the size field
  itself was damaged, the next "frame" starts at the wrong place and will
  most likely also fail its checksum or run past the end.)
- **Set frame whose value doesn't decode.** A frame value that breaks a
  rule of section 8 (rules 5 to 14, with the base's key table standing in
  for the document's), or doesn't fill the rest of the payload exactly, is
  corrupt: skip it. A key id the base's table doesn't have is such a break
  (rule 12).
- **The base.** The base document's root is a Map in every journal emberstore
  writes. If the root is not an object (Object or Map), the journal's tree
  is the base alone and frames are not applied: report this. If the base
  document doesn't decode, start from an empty object, replay the frames,
  and report it. If even its key table can't be read (rules 1 to 3), no set
  frame can be decoded: skip every set frame as corrupt; remove frames
  still apply.
- The journal's tree is the same kind of tree a document gives: a frame's
  value decodes exactly as a document's root value does in section 5, with
  the base's key table as its names.

---

## 7. Limits

- **4 GiB per container.** Payload sizes, counts and the key table offset are
  u32. So the root value must end before byte 2^32 of its document, an
  Object, Map, Array, PackedInt32 or PackedInt64 payload is under 4 GiB, and
  any array has at most 2^32 − 1 elements. The encoder does not check these:
  a larger value writes sizes that have wrapped around, and the file is
  corrupt.
- **Journals.** The base document is under 4 GiB (u32 base size), and so is
  each frame's payload. The journal file as a whole has no size field.
- **Key table.** The encoder numbers keys with 32-bit ids, so at most 2^32
  distinct field names per document.
- **Varints** carry at most 64 bits, in at most 10 bytes. String and key
  lengths are varints, but are also bounded by the bytes actually present.
- **Integers** are signed 64-bit. **Floating-point numbers** are binary64 at
  most.
- **Nesting depth** has no limit in the format. A decoder that recurses
  should set a maximum depth (`MAX_DEPTH` in section 5.1) and reject deeper
  input as an error, so a hostile file can't exhaust its stack. Files
  emberstore writes nest as deep as the program's data structures, rarely
  more than a few dozen levels.

---

## 8. Validation rules for untrusted input

A decoder applies all of these. Any failure makes the document invalid; the
decoder reports an error rather than returning part of a tree. (Journals are
the exception: section 6.2 says which frame failures skip a frame and which
stop reading.)

1. The document is at least 10 bytes and starts with `MIB1`.
2. The key table offset is at least 9 and at most the document length.
3. The key table: its count is no larger than the bytes left after the count
   varint (each name takes at least one byte), every name lies inside the
   document, and the table ends exactly at the end of the document.
4. The root value starts at offset 8 and ends exactly at the key table
   offset.
5. Every tag is 0 to 17.
6. Every varint ends within 10 bytes, its 10th byte (if any) is 0x00 or
   0x01, and it does not run past the end of the enclosing container.
7. Every String, map key and key-table name length fits in the bytes that
   remain in the enclosing container.
8. Every payload size ends inside the enclosing container (for the root,
   inside the value region that ends at the key table offset).
9. An Object or Map's entries fill its payload exactly; an entry may not
   start inside the payload and end past it.
10. An Array, PackedInt32 or PackedInt64 count is no larger than the payload
    bytes after the count field (each element takes at least one byte), the
    elements fill the payload exactly, and none runs past it.
11. A fixed-width packed array's `count × width` bytes fit in the enclosing
    container. Compute `count × width` without overflow (64-bit or
    unbounded arithmetic).
12. Every key id is less than the key table's count.
13. Strings, map keys and key names are valid UTF-8 for the JSON view
    (section 5.4).
14. Nesting stays within the decoder's maximum depth.

Never allocate memory from a count before checking it against the bytes
that remain (rules 3, 10, 11): a corrupt count of 4 billion must not make
the decoder try to allocate 4 billion elements.

---

## 9. Where the C++ reader is more lenient than this spec

The C++ reader (`Binary::Document`, `Binary::View` and `fromBinary`) never
throws on bad input. It reads what it can and treats the rest as missing.
A decoder that follows section 8 rejects these files instead. Files the
encoder writes pass both.

- It does not check that the root value ends at the key table offset, nor
  that the key table ends at the end of the document. Trailing bytes are
  ignored.
- It accepts a key table offset of 8 (no room for a root value).
- An unknown key id reads as the empty string `""`.
- An Array or packed count larger than the payload is cut down to what
  fits; elements that don't decode read as missing.
- An Object or Map entry that fails to decode ends the object there; later
  entries are ignored.
- An entry's value may run past its container's payload size, as long as
  it stays inside the document.
- A 10-byte varint's 10th byte may hold more than one bit; the extra bits
  are dropped.
- No UTF-8 check anywhere.
- Duplicate keys: the generic C++ tree (section 12) keeps the last
  occurrence, but looking up one field by name (`View::operator[]`) finds
  the first.
- A journal set frame is skipped only when its value's first tag is
  unknown or its header runs past the payload. Past that the value is read
  as leniently as a document: an unknown key id reads as `""` (so that
  field reads as missing), and bytes after the value are ignored. The
  frame still applies.
- A journal whose base document doesn't decode replays its frames onto an
  empty map, as section 6.2 says, but the C++ load report does not mention
  the bad base. If the base's header or key table is unreadable, every set
  frame is skipped and counted, as section 6.2 says.

---

## 10. Known leaks of C++ into the format

Each item is something a decoder that knows only this spec can't interpret
faithfully without knowing the C++ type that wrote it. Each ends with a
proposed fix in one sentence. None of these fixes are in the format today.

### 10.1 `uint64_t` above 2^63 reads as negative

A `std::uint64_t` field (or element of a `std::vector<std::uint64_t>`) is
written as the `int64_t` with the same bits, so 18446744073709551615
(2^64 − 1) is stored as zigzag(−1) = `01` and decodes to −1. Only a reader
that knows the field is unsigned can reinterpret it.
**Fix:** add a UInt tag (plain varint, no zigzag) and a PackedUInt64 tag,
and write unsigned 32- and 64-bit values with them.

### 10.2 Variant alternatives are named after compiler-specific C++ type names

A `std::variant<Ts...>` field is an Object with one entry whose key is the
active alternative's C++ type name, produced by `typeNameOf<T>()` from the
compiler's `__PRETTY_FUNCTION__`. For `std::variant<int, std::string>`
holding `"hi"`, GCC with libstdc++ writes the key `"basic_string<char>"`
(example 11.6); the older HTML explainer (`docs/miro-binary-format.html`,
generated on a Mac) shows `"string"`. A `std::vector<float>` alternative is
`"vector<float>"`. The same program built with another compiler or
standard library can write different keys, and a decoder can't tell this
object from an ordinary struct with one field. `Miro::Polymorphic<Base,
Derived...>` does the same with the derived class's unqualified name, which
is at least the programmer's own name, but still changes when the class is
renamed.
**Fix:** add a Union tag (alternative name as inline string bytes, then the
value) and require each alternative to have an explicit, portable name.

### 10.3 Enums switch between String and Int

An enum is written as the enumerator's C++ identifier (`"on"`), found by
probing the compiler's `__PRETTY_FUNCTION__` over the range −128…127 by
default. A value with no enumerator in that range (`static_cast<Mode>(7)`,
or any value outside the probed range) is written as an Int instead, and an
enum marked `MIRO_ENUM_AS_INTEGER` is always an Int. So one field can be a
String in one record and an Int in the next, and the strings are C++
identifiers that change when the code is renamed.
**Fix:** store each enum in one form, chosen per type and recorded in the
file (or always as integers with an optional name table), never by value.

### 10.4 Float vs double is decided per value

`float` and `double` fields both become Float32 or Double depending on
whether that particular value fits binary32 exactly (section 3.3), and
packed lists are narrowed only if every element fits. No value is lost, but
the tag says nothing about the field's type, so a decoder can't tell a
`float` field from a `double` one, and whole-valued floats like 3.0 must be
kept as floating-point by the tag, not by their digits.
**Fix:** none needed for the values; if the field's type matters, record it
in an optional schema section rather than in the per-value tag.

### 10.5 Integer widths are lost, and packed tags reflect C++ element types

Every scalar integer (`int8_t` … `uint64_t`, `char`, any integer type but
`bool`) is an Int; the width and signedness are gone. Packed tags follow the
C++ element type loosely: `int32_t` → PackedInt32, while `int64_t`,
`uint32_t` and `uint64_t` → PackedInt64, and `int8_t`/`uint8_t`/`int16_t`/
`uint16_t` get their own fixed-width tags. A `std::vector<char>` is not
packed at all (an Array of Ints) because `char` is not `int8_t`.
**Fix:** none needed for decoding; document the packed tags as storage
choices only, which this spec does.

### 10.6 Nullable and "may be absent" are not recorded

`std::optional<T>` and `OwningPointer<T>` write Null when empty;
`std::optional<std::optional<T>>` writes a single Null whether the outer or
the inner one is empty; `Miro::Omittable<T>` leaves the key out (or writes
Null as an array element). The file doesn't say whether a field *could* be
null or absent, only whether it is, and an absent field means "use the
program's default", which a decoder can't know.
**Fix:** an optional schema section listing each object type's fields with
their nullability and defaults would let a generic decoder fill them in.

### 10.7 Journal frames apply only for one C++ shape

The C++ reader replays journal frames only when the target type is
`std::map<std::string, V>`; for any other type it reads the base and
silently ignores the frames. So what a journal means depends on the
reader's C++ type. Section 6.2 pins it down for generic decoders (the base
must be an object for frames to apply).
**Fix:** have the journal writer refuse a base whose root is not a Map, and
state in the format that frames always apply to the root map.

### 10.8 Strings are bytes, not text

`std::string` holds any bytes, and neither the encoder nor the C++ reader
checks UTF-8, so a file can contain strings that no JSON decoder can
represent (section 5.4).
**Fix:** have the encoder reject invalid UTF-8 in String tags and keys, and
add a Bytes tag for binary data.

### 10.9 Raw JSON objects use Object, typed maps use Map

A `Miro::JSON` field's objects are written as Object (their keys go in the
key table), while a `std::map<std::string, V>` is a Map. So whether keys are
inline says more about the C++ container than about the data. Harmless for
the JSON view, which merges both (section 5.5).
**Fix:** none needed; a decoder should not read meaning into Object vs Map.

---

## 11. Worked examples

All bytes below were produced by the real encoder (`emberstore::toBinary`,
built from the `jp/binary` working tree with Miro at
`jamierpond/Miro@e1029443`) and labelled with the ByteLayout annotator
(`Apps/ByteLayout`). Offsets are decimal. "Tree" is the JSON-shaped tree a
decoder must produce; the C++ generic decode (section 12) gives the same.

### 11.1 Nested object with a list of objects

Caption: `struct Track { std::string title; int bpm; }` and
`struct Playlist { std::string name; std::vector<Track> tracks; }`, value
`{"Mix", {{"A", 120}, {"B", 90}}}`.

```
4D 49 42 31 37 00 00 00 07 2A 00 00 00 00 06 03
4D 69 78 01 09 1E 00 00 00 02 00 00 00 07 08 00
00 00 02 06 01 41 03 03 F0 01 07 08 00 00 00 02
06 01 42 03 03 B4 01 04 04 6E 61 6D 65 06 74 72
61 63 6B 73 05 74 69 74 6C 65 03 62 70 6D
```

```
@0   4D 49 42 31   magic "MIB1"
@4   37 00 00 00   key table offset = 55
@8   07            Object
@9   2A 00 00 00     payload size = 42 (offsets 13..54)
@13  00              key id 0 = "name"
@14  06 03 4D 69 78    String, length 3, "Mix"
@19  01              key id 1 = "tracks"
@20  09                Array
@21  1E 00 00 00         payload size = 30 (offsets 25..54, count included)
@25  02 00 00 00         count = 2
@29  07                  Object
@30  08 00 00 00           payload size = 8
@34  02                    key id 2 = "title"
@35  06 01 41                String "A"
@38  03                    key id 3 = "bpm"
@39  03 F0 01                Int, varint 240, zigzag → 120
@42  07                  Object
@43  08 00 00 00           payload size = 8
@47  02 06 01 42           "title": String "B"
@51  03 03 B4 01           "bpm": Int, varint 180, zigzag → 90
@55  04            key table: 4 names
@56  04 6E 61 6D 65            id 0 "name"
@61  06 74 72 61 63 6B 73      id 1 "tracks"
@68  05 74 69 74 6C 65         id 2 "title"
@74  03 62 70 6D               id 3 "bpm"
```

Tree:

```json
{"name": "Mix", "tracks": [{"title": "A", "bpm": 120}, {"title": "B", "bpm": 90}]}
```

"title" and "bpm" are stored once in the key table, though two objects use
them.

### 11.2 Packed float list, Float32 vs Double

Caption: `struct Synth { std::vector<float> gains; double cutoff; double
level; std::vector<double> curve; }`, value `{{0.5f, 1.0f}, 0.1, 0.25,
{0.1, 2.0}}`.

```
4D 49 42 31 41 00 00 00 07 34 00 00 00 00 0A 02
00 00 00 00 00 00 3F 00 00 80 3F 01 04 9A 99 99
99 99 99 B9 3F 02 05 00 00 80 3E 03 0B 02 00 00
00 9A 99 99 99 99 99 B9 3F 00 00 00 00 00 00 00
40 04 05 67 61 69 6E 73 06 63 75 74 6F 66 66 05
6C 65 76 65 6C 05 63 75 72 76 65
```

```
@0   4D 49 42 31   magic
@4   41 00 00 00   key table offset = 65
@8   07 34 00 00 00   Object, payload size 52
@13  00              "gains"
@14  0A                PackedFloat (no payload size)
@15  02 00 00 00         count = 2
@19  00 00 00 3F         f32 0.5
@23  00 00 80 3F         f32 1.0
@27  01              "cutoff"
@28  04 9A 99 99 99 99 99 B9 3F   Double 0.1 (does not fit binary32)
@37  02              "level"
@38  05 00 00 80 3E    Float32 0.25 (fits binary32 exactly)
@43  03              "curve"
@44  0B                PackedDouble: 0.1 doesn't fit binary32, so the whole list stays f64
@45  02 00 00 00         count = 2
@49  9A 99 99 99 99 99 B9 3F   f64 0.1
@57  00 00 00 00 00 00 00 40   f64 2.0
@65  04  05 "gains"  06 "cutoff"  05 "level"  05 "curve"   key table
```

Tree:

```json
{"gains": [0.5, 1.0], "cutoff": 0.1, "level": 0.25, "curve": [0.1, 2.0]}
```

All six numbers are floating-point in the tree, including the whole ones.

### 11.3 Empty optional, absent field, empty optional list

Caption: `struct Maybe { std::optional<int> a; std::optional<int> b;
Miro::Omittable<int> c; std::optional<std::vector<int>> d; }`, value with
`a` empty, `b = 5`, `c` absent, `d` empty.

```
4D 49 42 31 14 00 00 00 07 07 00 00 00 00 00 01
03 0A 03 00 04 01 61 01 62 01 63 01 64
```

```
@0   4D 49 42 31   magic
@4   14 00 00 00   key table offset = 20
@8   07 07 00 00 00   Object, payload size 7
@13  00 00         "a": Null (empty optional keeps its key)
@15  01 03 0A      "b": Int, zigzag 10 → 5
@18  03 00         "d": Null (key id 3; an empty optional list is Null, not an empty array)
@20  04            key table: 4 names
@21  01 61  01 62  01 63  01 64     "a" "b" "c" "d"
```

Tree:

```json
{"a": null, "b": 5, "d": null}
```

"c" is absent: no entry, so no key in the tree. Its name is still in the key
table (id 2) though no entry uses it (section 4).

### 11.4 Map with inline keys

Caption: `struct Counts { std::map<std::string, int> counts; }`, value
`{{"x", 1}, {"y", -2}}`.

```
4D 49 42 31 1B 00 00 00 07 0E 00 00 00 00 08 08
00 00 00 01 78 03 02 01 79 03 03 01 06 63 6F 75
6E 74 73
```

```
@0   4D 49 42 31   magic
@4   1B 00 00 00   key table offset = 27
@8   07 0E 00 00 00   Object, payload size 14
@13  00              key id 0 = "counts"
@14  08                Map
@15  08 00 00 00         payload size = 8
@19  01 78               key: length 1, "x" (inline, untagged)
@21  03 02               Int, zigzag 2 → 1
@23  01 79               key "y"
@25  03 03               Int, zigzag 3 → -2
@27  01  06 63 6F 75 6E 74 73     key table: 1 name, "counts"
```

Tree:

```json
{"counts": {"x": 1, "y": -2}}
```

The map's keys are not in the key table; only the field name "counts" is.

### 11.5 Journal with two frames

Caption: `std::map<std::string, Inner>` where `struct Inner { int x; }`. Base
`{"a": {x: 1}}`, then set `"b"` to `{x: 2}`, then remove `"a"`.

```
4D 49 42 4A 1A 00 00 00 4D 49 42 31 17 00 00 00
08 0A 00 00 00 01 61 07 03 00 00 00 00 03 02 01
01 78 01 0A 00 00 00 01 62 07 03 00 00 00 00 03
04 4B 32 C9 8E 02 02 00 00 00 01 61 17 B6 74 4C
```

```
@0   4D 49 42 4A        magic "MIBJ"
@4   1A 00 00 00        base size = 26
@8   base document (26 bytes, offsets below are within it):
       +0   4D 49 42 31   "MIB1"
       +4   17 00 00 00   key table offset = 23
       +8   08 0A 00 00 00     Map, payload size 10
       +13  01 61                key "a"
       +15  07 03 00 00 00       Object, payload size 3
       +20  00 03 02               "x": Int 1
       +23  01  01 78     key table: 1 name, "x" (id 0)
@34  frame 1
       01                 kind 1 = set
       0A 00 00 00        payload size = 10
       01 62              key "b"
       07 03 00 00 00 00 03 04
                          frame value (8 bytes): Object, payload size 3,
                          key id 0 = "x" from the base's table, Int 2
       4B 32 C9 8E        checksum = fnv1a32(the 10 payload bytes) = 0x8EC9324B
@53  frame 2
       02                 kind 2 = remove
       02 00 00 00        payload size = 2
       01 61              key "a"
       17 B6 74 4C        checksum = fnv1a32(01 61) = 0x4C74B617
@64  end of file
```

Replay: base `{"a": {"x": 1}}` → set b → `{"a": {"x": 1}, "b": {"x": 2}}` →
remove a. Tree:

```json
{"b": {"x": 2}}
```

Cut the file to any length from 54 to 63 bytes (inside frame 2) and the
reader stops at frame 2 as torn, giving `{"a": {"x": 1}, "b": {"x": 2}}`. Flip a byte
of frame 1's payload and its checksum fails: frame 1 is skipped, frame 2
still applies, giving `{}`.

### 11.6 What the leaks look like on the wire

Caption: `enum class Mode { off, on };` and `struct Leaky { Mode mode = on;
Mode stray = static_cast<Mode>(7); std::uint64_t id = UINT64_MAX;
std::variant<int, std::string> either = "hi"; std::vector<std::uint64_t>
big {1, UINT64_MAX}; }`, built with GCC and libstdc++.

```
4D 49 42 31 2F 00 00 00 07 22 00 00 00 00 06 02
6F 6E 01 03 0E 02 03 01 03 07 05 00 00 00 04 06
02 68 69 05 0D 06 00 00 00 02 00 00 00 02 01 06
04 6D 6F 64 65 05 73 74 72 61 79 02 69 64 06 65
69 74 68 65 72 12 62 61 73 69 63 5F 73 74 72 69
6E 67 3C 63 68 61 72 3E 03 62 69 67
```

```
@8   07 22 00 00 00   Object, payload size 34
@13  00 06 02 6F 6E       "mode": String "on"           (named enum value)
@18  01 03 0E             "stray": Int 7                 (same enum, unnamed value: 10.3)
@21  02 03 01             "id": Int, zigzag 1 → -1        (2^64 - 1: 10.1)
@24  03 07 05 00 00 00    "either": Object, payload size 5
@30    04 06 02 68 69       "basic_string<char>": String "hi"   (10.2)
@35  05 0D 06 00 00 00 02 00 00 00 02 01
                          "big": PackedInt64, size 6, count 2, [1, -1]   (10.1)
@47  key table: "mode" "stray" "id" "either" "basic_string<char>" "big"
```

Tree:

```json
{"mode": "on", "stray": 7, "id": -1, "either": {"basic_string<char>": "hi"}, "big": [1, -1]}
```

---

## 12. Checking a decoder against the C++ code

The C++ code already has a generic decoder: loading any document into a
`Miro::JSON` value (`emberstore::fromBinary(json, bytes)`) walks it without
knowing the type that wrote it, and `Miro::Json::print` turns that into JSON
text. When comparing another decoder's output with it, account for how that
text is printed:

- Object keys come out sorted, not in file order.
- Whole-valued floating-point numbers print without a fraction (`2`, not
  `2.0`), so compare numbers by value, not by whether the text has a
  decimal point.
- NaN and infinities print as `null`; −0.0 prints as `0`.
- Duplicate keys keep the last occurrence.
- For a journal, it loads the base document only and ignores the frames
  (section 10.7), so the journal tree of section 6.2 has to be checked
  against a typed `std::map<std::string, V>` load instead.
