#pragma once

#include "InternedKeyTable.h"
#include "LittleEndian.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Miro binary wire format ("MIB1"). Fixed-width fields are little-endian.
//
//   file      := "MIB1" | u32 keyTableOffset | value | keyTable
//   keyTable  := varint keyCount | { varint length | bytes }*
//   value     := tag byte | payload
//
//   Null / False / True   (no payload)
//   Int          zigzag varint
//   Double       8 raw IEEE-754 bytes
//   Float32      4 raw bytes, used when the double round-trips through float
//   String       varint byteLength | utf8 bytes
//   Object       u32 payloadSize | { varint keyId | value }*
//   Map          u32 payloadSize | { varint keyLength | bytes | value }*
//   Array        u32 payloadSize | u32 count | value*
//   PackedFloat  u32 count | count * 4 raw bytes
//   PackedDouble u32 count | count * 8 raw bytes
//   PackedInt32  u32 payloadSize | u32 count | zigzag varints
//   PackedInt64  u32 payloadSize | u32 count | zigzag varints
//   PackedInt8, PackedUInt8, PackedInt16, PackedUInt16
//                u32 count | count * 1 or 2 raw bytes
//
// payloadSize counts the bytes after the size field itself.
//
// Journal ("MIBJ"), for map-shaped values that grow one entry at a time:
//
//   journal := "MIBJ" | u32 baseSize | baseDoc (a "MIB1" file) | frame*
//   frame   := u8 kind (1 set, 2 remove) | u32 payloadSize | payload
//              | u32 calculateFnv1aChecksum(payload)
//   payload := varint keyLength | key | [value, for set]
//
// A set frame's value has no magic and no key table: its object key ids
// index the base document's key table. A frame never adds a name; a write
// that needs one the base lacks rewrites the journal with a new base. So
// every frame depends on the base alone. Readers replay frames in order,
// skip a corrupt frame whose length still fits, and stop at a torn one
// that runs past the end of the file.

namespace Calder
{

using Buffer = std::vector<std::uint8_t>;

namespace Detail
{

// Sizes and offsets on the wire are 32-bit, so one document holds at most
// 4 GiB; larger data has to be split into several documents.
inline constexpr auto largestStoredSize =
    std::size_t {std::numeric_limits<std::uint32_t>::max()};

inline std::uint32_t convertToStoredSize(std::size_t sizeInBytes)
{
    if (sizeInBytes > largestStoredSize)
    {
        throw std::length_error(
            "Calder: a binary document can be at most 4 GiB because its "
            "sizes are 32-bit; this one is "
            + std::to_string(sizeInBytes)
            + " bytes. Split the data into several documents.");
    }

    return static_cast<std::uint32_t>(sizeInBytes);
}

} // namespace Detail

enum class Tag : std::uint8_t
{
    Null = 0,
    False = 1,
    True = 2,
    Int = 3,
    Double = 4,
    Float32 = 5,
    String = 6,
    Object = 7,
    Map = 8,
    Array = 9,
    PackedFloat = 10,
    PackedDouble = 11,
    PackedInt32 = 12,
    PackedInt64 = 13,
    PackedInt8 = 14,
    PackedUInt8 = 15,
    PackedInt16 = 16,
    PackedUInt16 = 17,
};

inline constexpr auto npos = static_cast<std::size_t>(-1);
inline constexpr std::array<std::uint8_t, 4> magic {'M', 'I', 'B', '1'};
inline constexpr std::array<std::uint8_t, 4> journalMagic {'M', 'I', 'B', 'J'};
inline constexpr std::size_t headerFieldOffset = magic.size();
inline constexpr std::size_t headerSize = headerFieldOffset + sizeof(std::uint32_t);

inline bool startsWithMagic(std::span<const std::uint8_t> data,
                            const std::array<std::uint8_t, 4>& expected)
{
    return data.size() >= expected.size()
           && std::memcmp(data.data(), expected.data(), expected.size()) == 0;
}

inline bool isDocument(std::span<const std::uint8_t> data)
{
    return startsWithMagic(data, magic);
}

inline bool isJournal(std::span<const std::uint8_t> data)
{
    return startsWithMagic(data, journalMagic);
}

inline bool isEncoded(std::span<const std::uint8_t> data)
{
    return isDocument(data) || isJournal(data);
}

enum class FrameKind : std::uint8_t
{
    Set = 1,
    Remove = 2,
};

// `key` and `value` view the journal's bytes; `value` is empty for Remove.
struct JournalFrame
{
    FrameKind kind = FrameKind::Set;
    std::string_view key;
    std::span<const std::uint8_t> value;
};

// A frame whose length still fits the file but whose kind, checksum or
// key is corrupt is skipped and counted. A frame that runs past the end
// of the file is a torn write: reading stops there and `truncated` is
// set. frameBytes covers every frame before that point, skipped ones too.
struct JournalInfo
{
    bool valid = false;
    bool truncated = false;
    std::size_t baseSize = 0;
    std::size_t frameBytes = 0;
    std::size_t skippedFrameCount = 0;
};

JournalInfo inspectJournal(std::span<const std::uint8_t> data);
std::vector<JournalFrame> readJournalFrames(std::span<const std::uint8_t> data);

Buffer makeJournal(std::span<const std::uint8_t> baseDoc);
Buffer makeFrame(FrameKind kind,
                 std::string_view key,
                 std::span<const std::uint8_t> frameValue);

// The base document's key table, which every set frame's ids index. Empty
// if `journal` is not a readable journal.
InternedKeyTable readJournalKeyTable(std::span<const std::uint8_t> journal);

inline std::uint64_t encodeZigzag(std::int64_t value)
{
    return (static_cast<std::uint64_t>(value) << 1)
           ^ static_cast<std::uint64_t>(value >> 63);
}

inline std::int64_t decodeZigzag(std::uint64_t value)
{
    return static_cast<std::int64_t>(value >> 1)
           ^ -static_cast<std::int64_t>(value & 1);
}

inline constexpr std::uint8_t varintContinuationBit = 0x80;
inline constexpr std::uint8_t varintPayloadBits = 0x7F;
inline constexpr int varintBitsPerByte = 7;
inline constexpr int varintMaxShift = 64;

// Append-only output with back-patched container sizes and interned
// object keys. finishWithKeyTable() appends the key table and spends the
// writer. Given a journal's key table, it writes a set frame's value
// instead, and finishJournalFrameValue() spends it.
namespace Detail
{

class ByteWriter
{
public:
    ByteWriter()
    {
        constexpr auto initialCapacity = std::size_t {1024};
        buffer.reserve(initialCapacity);
        buffer.insert(buffer.end(), magic.begin(), magic.end());
        writeU32(0);
    }

    explicit ByteWriter(InternedKeyTable& journalKeyTableToUse)
        : journalKeyTable(journalKeyTableToUse)
    {
        constexpr auto initialCapacity = std::size_t {256};
        buffer.reserve(initialCapacity);
    }

    std::size_t getNextWriteOffset() const { return buffer.size(); }

    void writeByte(std::uint8_t byte) { buffer.push_back(byte); }
    void writeTag(Tag tag) { writeByte(static_cast<std::uint8_t>(tag)); }

    // resize + memcpy rather than insert: insert's general range path
    // costs more than the copy for the few bytes a field writes.
    void writeBytes(const void* data, std::size_t size)
    {
        auto offset = buffer.size();
        buffer.resize(offset + size);
        std::memcpy(buffer.data() + offset, data, size);
    }

    template <Detail::FixedWidthNumber T>
    void writeFixedWidth(T value)
    {
        auto bytes = Detail::encodeLittleEndian(value);
        writeBytes(bytes.data(), bytes.size());
    }

    template <Detail::FixedWidthNumber T>
    void writeFixedWidthElements(std::span<const T> values)
    {
        Detail::appendLittleEndianElements(buffer, values);
    }

    void writeU32(std::uint32_t value) { writeFixedWidth(value); }

    void patchU32(std::size_t fieldOffset, std::uint32_t value)
    {
        auto bytes = Detail::encodeLittleEndian(value);
        std::ranges::copy(bytes,
                          buffer.begin() + static_cast<std::ptrdiff_t>(fieldOffset));
    }

    void writeVarint(std::uint64_t value)
    {
        while (value >= varintContinuationBit)
        {
            writeByte(static_cast<std::uint8_t>(value) | varintContinuationBit);
            value >>= varintBitsPerByte;
        }

        writeByte(static_cast<std::uint8_t>(value));
    }

    void writeZigzag(std::int64_t value) { writeVarint(encodeZigzag(value)); }

    void writeString(std::string_view text)
    {
        writeVarint(text.size());
        writeBytes(text.data(), text.size());
    }

    std::uint32_t getInternedKeyId(std::string_view key)
    {
        if (!journalKeyTable)
            return keyTable.getInternedKeyId(key);

        return getJournalKeyId(key);
    }

    void truncateTo(std::size_t newSizeInBytes) { buffer.resize(newSizeInBytes); }

    Buffer finishWithKeyTable()
    {
        auto tableOffset = buffer.size();
        writeVarint(keyTable.getKeyCount());

        for (auto id = std::size_t {0}; id < keyTable.getKeyCount(); ++id)
            writeString(keyTable.getKeyName(id));

        // Every size and offset inside the document is at most its length,
        // so checking the whole document covers all of them.
        Detail::convertToStoredSize(buffer.size());
        patchU32(headerFieldOffset, static_cast<std::uint32_t>(tableOffset));
        return std::move(buffer);
    }

    // nullopt if the value used a key name the journal's table lacks.
    std::optional<Buffer> finishJournalFrameValue()
    {
        if (hasMetKeyMissingFromJournal)
            return std::nullopt;

        return std::move(buffer);
    }

private:
    std::uint32_t getJournalKeyId(std::string_view key)
    {
        auto id = journalKeyTable->get().findInternedKeyId(key);

        if (!id)
        {
            hasMetKeyMissingFromJournal = true;
            return 0;
        }

        return *id;
    }

    Buffer buffer;
    InternedKeyTable keyTable;
    std::optional<std::reference_wrapper<InternedKeyTable>> journalKeyTable;
    bool hasMetKeyMissingFromJournal = false;
};

} // namespace Detail

class Document;

namespace Detail
{

// An out-of-range read clears `ok` and yields zeros, so callers check
// once at the end.
struct ByteReader
{
    explicit ByteReader(std::span<const std::uint8_t> bytesToReadToUse,
                        std::size_t startOffsetInBytes = 0)
        : bytesToRead(bytesToReadToUse)
        , readOffsetInBytes(startOffsetInBytes)
        , hasReadEverythingInBounds(startOffsetInBytes <= bytesToReadToUse.size())
    {
    }

    std::uint8_t readByte()
    {
        if (readOffsetInBytes >= bytesToRead.size())
        {
            hasReadEverythingInBounds = false;
            return 0;
        }

        return bytesToRead[readOffsetInBytes++];
    }

    std::uint64_t readVarint()
    {
        auto result = std::uint64_t {0};

        for (auto shift = 0; shift < varintMaxShift; shift += varintBitsPerByte)
        {
            auto byte = readByte();
            result |= static_cast<std::uint64_t>(byte & varintPayloadBits) << shift;

            if ((byte & varintContinuationBit) == 0)
                return result;
        }

        hasReadEverythingInBounds = false;
        return 0;
    }

    std::int64_t readZigzag() { return decodeZigzag(readVarint()); }

    std::span<const std::uint8_t> readBytes(std::size_t size)
    {
        if (readOffsetInBytes > bytesToRead.size()
            || size > bytesToRead.size() - readOffsetInBytes)
        {
            hasReadEverythingInBounds = false;
            return {};
        }

        auto bytes = bytesToRead.subspan(readOffsetInBytes, size);
        readOffsetInBytes += size;
        return bytes;
    }

    template <FixedWidthNumber T>
    T readFixedWidth()
    {
        auto bytes = readBytes(sizeof(T));

        if (!hasReadEverythingInBounds)
            return T {};

        return Detail::decodeLittleEndian<T>(bytes.template first<sizeof(T)>());
    }

    std::uint32_t readU32() { return readFixedWidth<std::uint32_t>(); }

    std::string_view readString(std::size_t size)
    {
        auto bytes = readBytes(size);
        const auto* text = reinterpret_cast<const char*>(bytes.data());
        return {text, bytes.size()};
    }

    void skip(std::size_t size) { readBytes(size); }

    std::span<const std::uint8_t> bytesToRead;
    std::size_t readOffsetInBytes = 0;
    bool hasReadEverythingInBounds = true;
};

enum class ValueCategory : std::uint8_t
{
    Null,
    Bool,
    Number,
    String,
    Object,
    Array,
};

// How a number is stored, or for an array how each element is stored.
enum class Encoding : std::uint8_t
{
    Tagged,
    Zigzag,
    Float32,
    Float64,
    Int8,
    UInt8,
    Int16,
    UInt16,
};

using Number = std::variant<std::int64_t, double>;

// Zero for Tagged and varint encodings.
std::size_t getFixedByteWidth(Encoding encoding);

bool isIntegerEncoding(Encoding encoding);

Number readNumber(ByteReader& reader, Encoding encoding);

// Advances past `count` elements of an array stored with `encoding`.
bool skipElements(ByteReader& reader, Encoding encoding, std::size_t count);

// Advances past one whole value, tag included.
bool skipValue(ByteReader& reader);

// One value's header. Containers get their payload range and element
// count, clamped to the buffer; numbers get where their bytes start.
struct ValueHeader
{
    Tag tag = Tag::Null;
    ValueCategory category = ValueCategory::Null;
    Encoding encoding = Encoding::Tagged;
    bool valid = false;
    std::size_t valueOffset = npos;
    std::size_t payloadOffset = 0;
    std::size_t payloadEndOffset = 0;
    std::size_t elementCount = 0;
};

ValueHeader parseValueHeader(std::span<const std::uint8_t> data,
                             std::size_t valueOffset);

// A packed array element has no tag; it reads as a number at `elementOffset`.
ValueHeader makePackedElementHeader(Encoding encoding, std::size_t elementOffset);

struct ObjectEntry
{
    std::string_view key;
    std::size_t valueOffset = 0;
    std::size_t valueEndOffset = 0;
};

// Walks the entries of an Object or Map between two payload offsets.
class ObjectEntryCursor
{
public:
    ObjectEntryCursor(const Document& documentToUse,
                      const ValueHeader& container,
                      std::size_t fromOffset,
                      std::size_t untilOffset);

    std::optional<ObjectEntry> readNextEntry();
    std::optional<ObjectEntry> findEntryWithKey(std::string_view key);

private:
    std::string_view readKey();

    const Document& document;
    ByteReader reader;
    std::size_t endOffset;
    bool hasInlineKeys;
};

} // namespace Detail

class View;

// Non-owning handle on an encoded buffer: validates the header and
// indexes the key table. The bytes must outlive the Document and its Views.
class Document
{
public:
    Document() = default;

    explicit Document(std::span<const std::uint8_t> dataToUse);

    // A journal set frame's value, its key ids naming the base document's
    // keys. The base must outlive this Document.
    Document(std::span<const std::uint8_t> frameValueToUse,
             const Document& baseDocumentToUse);

    bool isValid() const { return valid; }
    std::span<const std::uint8_t> bytes() const { return data; }
    std::size_t getRootValueOffset() const { return rootValueOffset; }

    View root() const;

    std::size_t getKeyCount() const { return getKeyNames().size(); }

    std::string_view getKeyName(std::uint64_t id) const
    {
        auto keyNames = getKeyNames();
        return id < keyNames.size() ? keyNames[id] : std::string_view {};
    }

private:
    bool readKeyTable(std::size_t tableOffset);

    std::span<const std::string_view> getKeyNames() const
    {
        if (baseDocumentKeyNames)
            return *baseDocumentKeyNames;

        return keyNamesById;
    }

    std::span<const std::uint8_t> data {};
    std::size_t rootValueOffset = headerSize;
    std::vector<std::string_view> keyNamesById;
    std::optional<std::span<const std::string_view>> baseDocumentKeyNames;
    bool valid = false;
};

// Cursor into an encoded buffer that touches only the bytes on the path
// to the requested value, skipping siblings by their size prefixes.
// operator[](i) on a non-packed array costs O(i) skips.
class View
{
public:
    View() = default;

    View(const Document& documentToUse, std::size_t valueOffsetToUse);

    bool isValid() const { return document != nullptr && header.valid; }
    bool isObject() const { return isValid() && header.tag == Tag::Object; }
    bool isMap() const { return isValid() && header.tag == Tag::Map; }
    bool isArray() const { return hasCategory(Detail::ValueCategory::Array); }

    View operator[](std::string_view key) const;
    View operator[](std::size_t index) const;

    // Array element count, or object/map entry count (scans entries).
    std::size_t size() const;

    std::int64_t asInt(std::int64_t fallback = 0) const;
    double asNumber(double fallback = 0.0) const;
    std::string_view asString() const;

    // Resume from here later with View {document, offset}.
    std::size_t getValueOffset() const { return header.valueOffset; }

private:
    View(const Document& documentToUse, const Detail::ValueHeader& headerToUse);

    bool hasCategory(Detail::ValueCategory category) const
    {
        return isValid() && header.category == category;
    }

    std::optional<Detail::Number> readNumber() const;

    const Document* document = nullptr;
    Detail::ValueHeader header {};
};

} // namespace Calder
