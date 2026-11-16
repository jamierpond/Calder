#include "Wire.h"

#include <algorithm>
#include <bit>

namespace Calder::Detail
{
namespace
{

constexpr auto frameOverhead =
    sizeof(FrameKind) + sizeof(std::uint32_t) + sizeof(std::uint32_t);

constexpr std::array frameKinds {FrameKind::Set, FrameKind::Remove};

struct ValueLayout
{
    Tag tag;
    ValueCategory category;
    Encoding encoding;
};

// Ordered by wire byte, so the byte read from the buffer finds its row.
constexpr std::array valueLayouts {
    ValueLayout {Tag::Null, ValueCategory::Null, Encoding::Tagged},
    ValueLayout {Tag::False, ValueCategory::Bool, Encoding::Tagged},
    ValueLayout {Tag::True, ValueCategory::Bool, Encoding::Tagged},
    ValueLayout {Tag::Int, ValueCategory::Number, Encoding::Zigzag},
    ValueLayout {Tag::Double, ValueCategory::Number, Encoding::Float64},
    ValueLayout {Tag::Float32, ValueCategory::Number, Encoding::Float32},
    ValueLayout {Tag::String, ValueCategory::String, Encoding::Tagged},
    ValueLayout {Tag::Object, ValueCategory::Object, Encoding::Tagged},
    ValueLayout {Tag::Map, ValueCategory::Object, Encoding::Tagged},
    ValueLayout {Tag::Array, ValueCategory::Array, Encoding::Tagged},
    ValueLayout {Tag::PackedFloat, ValueCategory::Array, Encoding::Float32},
    ValueLayout {Tag::PackedDouble, ValueCategory::Array, Encoding::Float64},
    ValueLayout {Tag::PackedInt32, ValueCategory::Array, Encoding::Zigzag},
    ValueLayout {Tag::PackedInt64, ValueCategory::Array, Encoding::Zigzag},
    ValueLayout {Tag::PackedInt8, ValueCategory::Array, Encoding::Int8},
    ValueLayout {Tag::PackedUInt8, ValueCategory::Array, Encoding::UInt8},
    ValueLayout {Tag::PackedInt16, ValueCategory::Array, Encoding::Int16},
    ValueLayout {Tag::PackedUInt16, ValueCategory::Array, Encoding::UInt16},
    ValueLayout {
        Tag::PackedStructArray, ValueCategory::Array, Encoding::PackedStruct},
};

struct PackedFieldLayout
{
    PackedFieldType type;
    Encoding encoding;
};

// Ordered by wire byte, like valueLayouts.
constexpr std::array packedFieldLayouts {
    PackedFieldLayout {PackedFieldType::Int8, Encoding::Int8},
    PackedFieldLayout {PackedFieldType::UInt8, Encoding::UInt8},
    PackedFieldLayout {PackedFieldType::Int16, Encoding::Int16},
    PackedFieldLayout {PackedFieldType::UInt16, Encoding::UInt16},
    PackedFieldLayout {PackedFieldType::Int32, Encoding::Int32},
    PackedFieldLayout {PackedFieldType::UInt32, Encoding::UInt32},
    PackedFieldLayout {PackedFieldType::Int64, Encoding::Int64},
    PackedFieldLayout {PackedFieldType::UInt64, Encoding::UInt64},
    PackedFieldLayout {PackedFieldType::Float32, Encoding::Float32},
    PackedFieldLayout {PackedFieldType::Float64, Encoding::Float64},
};

constexpr bool arePackedFieldLayoutsInWireOrder()
{
    for (auto i = std::size_t {0}; i < packedFieldLayouts.size(); ++i)
    {
        if (static_cast<std::size_t>(packedFieldLayouts[i].type) != i)
            return false;
    }

    return true;
}

static_assert(arePackedFieldLayoutsInWireOrder());

constexpr bool areValueLayoutsInWireOrder()
{
    for (auto i = std::size_t {0}; i < valueLayouts.size(); ++i)
    {
        if (static_cast<std::size_t>(valueLayouts[i].tag) != i)
            return false;
    }

    return true;
}

static_assert(areValueLayoutsInWireOrder());

std::optional<ValueLayout> findValueLayout(std::uint8_t wireByte)
{
    if (wireByte >= valueLayouts.size())
        return std::nullopt;

    return valueLayouts[wireByte];
}

std::uint32_t calculateFnv1aChecksum(std::span<const std::uint8_t> data)
{
    constexpr auto offsetBasis = std::uint32_t {2166136261U};
    constexpr auto prime = std::uint32_t {16777619U};
    auto hash = offsetBasis;

    for (auto byte: data)
    {
        hash ^= byte;
        hash *= prime;
    }

    return hash;
}

void appendBytes(Buffer& out, std::span<const std::uint8_t> bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void appendU32(Buffer& out, std::uint32_t value)
{
    appendBytes(out, encodeLittleEndian(value));
}

void appendVarint(Buffer& out, std::uint64_t value)
{
    while (value >= varintContinuationBit)
    {
        out.push_back(static_cast<std::uint8_t>(value) | varintContinuationBit);
        value >>= varintBitsPerByte;
    }

    out.push_back(static_cast<std::uint8_t>(value));
}

std::span<const std::uint8_t> viewAsBytes(std::string_view text)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    return {bytes, text.size()};
}

bool isFrameKind(std::uint8_t byte)
{
    return std::ranges::any_of(frameKinds,
                               [byte](FrameKind kind)
                               { return static_cast<std::uint8_t>(kind) == byte; });
}

// The frame's size from its length field, if its header is complete and
// the declared payload lies inside the journal.
std::optional<std::size_t> readFrameSize(std::span<const std::uint8_t> data,
                                         std::size_t frameOffset)
{
    if (data.size() - frameOffset < frameOverhead)
        return std::nullopt;

    auto reader = ByteReader {data, frameOffset + sizeof(FrameKind)};
    auto payloadSize = std::size_t {reader.readU32()};

    if (payloadSize > data.size() - frameOffset - frameOverhead)
        return std::nullopt;

    return frameOverhead + payloadSize;
}

// Reads a frame already known to fit; nullopt if its kind, checksum or
// key is corrupt.
std::optional<JournalFrame> decodeFrameAt(std::span<const std::uint8_t> data,
                                          std::size_t frameOffset)
{
    auto reader = ByteReader {data, frameOffset};
    auto kindByte = reader.readByte();
    auto payload = reader.readBytes(reader.readU32());

    if (!isFrameKind(kindByte)
        || reader.readU32() != calculateFnv1aChecksum(payload))
        return std::nullopt;

    auto payloadReader = ByteReader {payload};
    auto key = payloadReader.readString(payloadReader.readVarint());

    if (!payloadReader.hasReadEverythingInBounds)
        return std::nullopt;

    auto kind = static_cast<FrameKind>(kindByte);
    auto value = payload.subspan(payloadReader.readOffsetInBytes);
    return JournalFrame {kind, key, value};
}

std::optional<std::size_t> readJournalBaseSize(std::span<const std::uint8_t> data)
{
    if (!isJournal(data))
        return std::nullopt;

    auto reader = ByteReader {data, headerFieldOffset};
    auto baseSize = std::size_t {reader.readU32()};

    if (!reader.hasReadEverythingInBounds || baseSize > data.size() - headerSize)
        return std::nullopt;

    return baseSize;
}

template <typename FrameVisitor>
JournalInfo walkJournal(std::span<const std::uint8_t> data,
                        FrameVisitor&& visitFrame)
{
    auto journalInfo = JournalInfo {};
    auto baseSize = readJournalBaseSize(data);

    if (!baseSize)
        return journalInfo;

    journalInfo.valid = true;
    journalInfo.baseSize = *baseSize;

    for (auto frameOffset = headerSize + *baseSize; frameOffset < data.size();)
    {
        auto frameSize = readFrameSize(data, frameOffset);

        if (!frameSize)
        {
            journalInfo.truncated = true;
            break;
        }

        if (auto frame = decodeFrameAt(data, frameOffset))
            visitFrame(*frame);
        else
            ++journalInfo.skippedFrameCount;

        frameOffset += *frameSize;
        journalInfo.frameBytes += *frameSize;
    }

    return journalInfo;
}

bool doesValueEndAt(std::span<const std::uint8_t> data,
                    std::size_t valueOffset,
                    std::size_t expectedEndOffset)
{
    auto reader = ByteReader {data, valueOffset};
    return skipValue(reader) && reader.readOffsetInBytes == expectedEndOffset;
}

// A count or size that runs past the end of the bytes marks the header
// invalid, rather than shrinking the container to what is there.
void markInvalidIfPastEnd(ByteReader& reader, std::size_t endOffset)
{
    if (endOffset > reader.bytesToRead.size())
        reader.hasReadEverythingInBounds = false;
}

void readFixedWidthArrayHeader(ByteReader& reader,
                               ValueHeader& header,
                               std::size_t width)
{
    header.elementCount = reader.readU32();
    header.payloadOffset = reader.readOffsetInBytes;
    header.payloadEndOffset = header.payloadOffset + header.elementCount * width;
    markInvalidIfPastEnd(reader, header.payloadEndOffset);
}

void readArrayHeader(ByteReader& reader, ValueHeader& header)
{
    if (auto width = getFixedByteWidth(header.encoding); width != 0)
    {
        readFixedWidthArrayHeader(reader, header, width);
        return;
    }

    auto size = std::size_t {reader.readU32()};
    header.payloadEndOffset = reader.readOffsetInBytes + size;
    header.elementCount = reader.readU32();
    header.payloadOffset = reader.readOffsetInBytes;
    markInvalidIfPastEnd(reader, header.payloadEndOffset);

    // Every element takes at least one byte, which bounds a corrupt count.
    if (header.payloadOffset + header.elementCount > header.payloadEndOffset)
        reader.hasReadEverythingInBounds = false;
}

} // namespace

JournalInfo inspectJournal(std::span<const std::uint8_t> data)
{
    return walkJournal(data, [](const JournalFrame&) {});
}

std::vector<JournalFrame> readJournalFrames(std::span<const std::uint8_t> data)
{
    auto frames = std::vector<JournalFrame> {};
    walkJournal(data,
                [&frames](const JournalFrame& frame) { frames.push_back(frame); });
    return frames;
}

Buffer makeJournal(std::span<const std::uint8_t> baseDoc)
{
    auto out = Buffer {};
    out.reserve(headerSize + baseDoc.size());
    appendBytes(out, journalMagic);
    appendU32(out, convertToStoredSize(baseDoc.size()));
    appendBytes(out, baseDoc);
    return out;
}

InternedKeyTable readJournalKeyTable(std::span<const std::uint8_t> journal)
{
    auto baseSize = readJournalBaseSize(journal);

    if (!baseSize)
        return {};

    auto base = Document {journal.subspan(headerSize, *baseSize)};
    auto keyTable = InternedKeyTable {};

    for (auto name: base.getKeyNames())
        keyTable.getInternedKeyId(name);

    // A base naming one key twice would shift every later id. With an empty
    // table every write that names a field compacts instead.
    if (keyTable.getKeyCount() != base.getKeyCount())
        return {};

    return keyTable;
}

Buffer replaceKeyTable(std::span<const std::uint8_t> document,
                       const InternedKeyTable& keyTable)
{
    auto tableOffset =
        std::size_t {ByteReader {document, headerFieldOffset}.readU32()};
    auto out = Buffer(document.begin(),
                      document.begin() + static_cast<std::ptrdiff_t>(tableOffset));
    appendVarint(out, keyTable.getKeyCount());

    for (auto id = std::size_t {0}; id < keyTable.getKeyCount(); ++id)
    {
        auto name = keyTable.getKeyName(id);
        appendVarint(out, name.size());
        appendBytes(out, viewAsBytes(name));
    }

    return out;
}

Buffer makeFrame(FrameKind kind,
                 std::string_view key,
                 std::span<const std::uint8_t> frameValue)
{
    constexpr auto keyLengthReserve = std::size_t {4};
    auto payload = Buffer {};
    payload.reserve(key.size() + frameValue.size() + keyLengthReserve);
    appendVarint(payload, key.size());
    appendBytes(payload, viewAsBytes(key));
    appendBytes(payload, frameValue);

    auto out = Buffer {};
    out.reserve(frameOverhead + payload.size());
    out.push_back(static_cast<std::uint8_t>(kind));
    appendU32(out, convertToStoredSize(payload.size()));
    appendBytes(out, payload);
    appendU32(out, calculateFnv1aChecksum(payload));
    return out;
}

std::size_t getFixedByteWidth(Encoding encoding)
{
    auto width = std::size_t {0};
    visitFixedWidthEncoding(encoding,
                            [&width](auto row)
                            { width = sizeof(typename decltype(row)::NumberType); });
    return width;
}

bool isIntegerEncoding(Encoding encoding)
{
    return encoding != Encoding::Tagged && encoding != Encoding::Float32
           && encoding != Encoding::Float64 && encoding != Encoding::PackedStruct;
}

namespace
{

// A uint64 travels as its int64 bit pattern, the way Miro passes it on.
template <FixedWidthNumber T>
Number widenToNumber(T value)
{
    if constexpr (std::floating_point<T>)
        return static_cast<double>(value);
    else if constexpr (std::same_as<T, std::uint64_t>)
        return std::bit_cast<std::int64_t>(value);
    else
        return static_cast<std::int64_t>(value);
}

} // namespace

Number readNumber(ByteReader& reader, Encoding encoding)
{
    if (encoding == Encoding::Zigzag)
        return reader.readZigzag();

    auto number = Number {std::int64_t {0}};
    auto readFixedWidthNumber = [&reader, &number](auto row)
    {
        number = widenToNumber(
            reader.readFixedWidth<typename decltype(row)::NumberType>());
    };

    if (!visitFixedWidthEncoding(encoding, readFixedWidthNumber))
        reader.hasReadEverythingInBounds = false;

    return number;
}

bool skipElements(ByteReader& reader, Encoding encoding, std::size_t count)
{
    // Records have no fixed width the encoding knows; callers that walk
    // them read the stored layout instead.
    if (encoding == Encoding::PackedStruct)
    {
        reader.hasReadEverythingInBounds = count == 0;
        return reader.hasReadEverythingInBounds;
    }

    if (auto width = getFixedByteWidth(encoding); width != 0)
    {
        reader.skip(count * width);
        return reader.hasReadEverythingInBounds;
    }

    for (auto i = std::size_t {0}; i < count && reader.hasReadEverythingInBounds;
         ++i)
    {
        if (encoding == Encoding::Tagged)
            skipValue(reader);
        else
            reader.readVarint();
    }

    return reader.hasReadEverythingInBounds;
}

namespace
{

// Packed float arrays store an element count; every other array stores
// its payload size.
void skipArrayPayload(ByteReader& reader, Encoding encoding)
{
    auto width = std::max(getFixedByteWidth(encoding), std::size_t {1});
    reader.skip(std::size_t {reader.readU32()} * width);
}

} // namespace

bool skipValue(ByteReader& reader)
{
    auto layout = findValueLayout(reader.readByte());

    if (!layout)
    {
        reader.hasReadEverythingInBounds = false;
        return false;
    }

    switch (layout->category)
    {
        case ValueCategory::Number:
            return skipElements(reader, layout->encoding, 1);

        case ValueCategory::String:
            reader.skip(reader.readVarint());
            break;

        case ValueCategory::Object:
            reader.skip(reader.readU32());
            break;

        case ValueCategory::Array:
            skipArrayPayload(reader, layout->encoding);
            break;

        case ValueCategory::Null:
        case ValueCategory::Bool:
            break;
    }

    return reader.hasReadEverythingInBounds;
}

ValueHeader parseValueHeader(std::span<const std::uint8_t> data,
                             std::size_t valueOffset)
{
    auto header = ValueHeader {};
    auto reader = ByteReader {data, valueOffset};
    auto layout = findValueLayout(reader.readByte());

    if (!reader.hasReadEverythingInBounds || !layout)
        return header;

    header.tag = layout->tag;
    header.category = layout->category;
    header.encoding = layout->encoding;
    header.valueOffset = valueOffset;
    header.payloadOffset = reader.readOffsetInBytes;
    header.payloadEndOffset = data.size();

    if (header.category == ValueCategory::Object)
    {
        auto size = std::size_t {reader.readU32()};
        header.payloadOffset = reader.readOffsetInBytes;
        header.payloadEndOffset = reader.readOffsetInBytes + size;
        markInvalidIfPastEnd(reader, header.payloadEndOffset);
    }
    else if (header.category == ValueCategory::Array)
    {
        readArrayHeader(reader, header);
    }

    header.valid = reader.hasReadEverythingInBounds;
    return header;
}

namespace
{

// The tag a packed element would carry if it were stored on its own.
Tag getNumberTag(Encoding encoding)
{
    if (encoding == Encoding::Float32)
        return Tag::Float32;

    if (encoding == Encoding::Float64)
        return Tag::Double;

    return Tag::Int;
}

} // namespace

ValueHeader makePackedElementHeader(Encoding encoding, std::size_t elementOffset)
{
    auto header = ValueHeader {};
    header.tag = getNumberTag(encoding);
    header.category = ValueCategory::Number;
    header.encoding = encoding;
    header.valid = true;
    header.valueOffset = elementOffset;
    header.payloadOffset = elementOffset;
    return header;
}

ByteReader makeElementReader(const Document& document,
                             const ValueHeader& array,
                             std::size_t fromOffset)
{
    auto payloadEnd = std::min(array.payloadEndOffset, document.bytes().size());
    return ByteReader {document.bytes().first(payloadEnd), fromOffset};
}

ValueHeader makePackedRecordHeader(std::size_t recordOffset)
{
    auto header = ValueHeader {};
    header.tag = Tag::Object;
    header.category = ValueCategory::Object;
    header.valid = true;
    header.valueOffset = recordOffset;
    header.payloadOffset = recordOffset;
    header.payloadEndOffset = recordOffset;
    return header;
}

std::optional<PackedFieldType> findPackedFieldType(std::uint8_t wireByte)
{
    if (wireByte >= packedFieldLayouts.size())
        return std::nullopt;

    return packedFieldLayouts[wireByte].type;
}

std::optional<PackedFieldType> findPackedFieldType(Encoding encoding)
{
    auto row = std::ranges::find(
        packedFieldLayouts, encoding, &PackedFieldLayout::encoding);

    if (row == packedFieldLayouts.end())
        return std::nullopt;

    return row->type;
}

Encoding getEncoding(PackedFieldType type)
{
    auto row = std::ranges::find(packedFieldLayouts, type, &PackedFieldLayout::type);
    return row->encoding;
}

namespace
{

std::size_t countBytesLeft(const ByteReader& reader)
{
    auto size = reader.bytesToRead.size();
    return size - std::min(reader.readOffsetInBytes, size);
}

bool readStoredPackedField(const Document& document,
                           ByteReader& reader,
                           StoredPackedLayout& layout)
{
    auto pathLength = reader.readVarint();

    // Each segment takes at least one byte, which bounds a corrupt length.
    if (pathLength == 0 || pathLength > countBytesLeft(reader))
        return false;

    auto field = StoredPackedField {.firstPathSegment = layout.pathSegments.size(),
                                    .pathLength = pathLength};

    for (auto i = std::uint64_t {0}; i < pathLength; ++i)
    {
        auto name = document.findKeyName(reader.readVarint());

        if (!name)
            return false;

        layout.pathSegments.push_back(*name);
    }

    auto type = findPackedFieldType(reader.readByte());

    if (!type || !reader.hasReadEverythingInBounds)
        return false;

    field.encoding = getEncoding(*type);
    field.offsetInRecord = layout.recordSize;
    layout.recordSize += getFixedByteWidth(field.encoding);
    layout.fields.push_back(field);
    return true;
}

} // namespace

bool readStoredPackedLayout(const Document& document,
                            const ValueHeader& header,
                            StoredPackedLayout& layout)
{
    layout.fields.clear();
    layout.pathSegments.clear();
    layout.recordSize = 0;
    layout.recordCount = 0;

    auto reader = ByteReader {document.bytes().first(header.payloadEndOffset),
                              header.payloadOffset};
    auto fieldCount = reader.readVarint();

    if (fieldCount == 0 || fieldCount > countBytesLeft(reader))
        return false;

    for (auto i = std::uint64_t {0}; i < fieldCount; ++i)
    {
        if (!readStoredPackedField(document, reader, layout))
            return false;
    }

    layout.recordsOffset = reader.readOffsetInBytes;
    auto recordBytes = header.payloadEndOffset - layout.recordsOffset;
    layout.recordCount =
        std::min(header.elementCount, recordBytes / layout.recordSize);
    return true;
}

namespace
{

// True when `path` continues `prefix` for its first `depth` segments.
bool startsWithPrefix(std::span<const std::string_view> path,
                      std::span<const std::string_view> prefix,
                      std::size_t depth)
{
    return path.size() > depth && prefix.size() >= depth
           && std::ranges::equal(path.first(depth), prefix.first(depth));
}

bool isLastKey(const std::vector<std::string_view>& keys, std::string_view key)
{
    return !keys.empty() && keys.back() == key;
}

std::span<const std::string_view> getPathPrefix(const StoredPackedLayout& layout,
                                                const PackedPathPrefix& prefix)
{
    if (prefix.depth == 0)
        return {};

    return layout.getPath(layout.fields[prefix.fieldIndex]);
}

} // namespace

std::optional<std::size_t> findPackedFieldAtDepth(const StoredPackedLayout& layout,
                                                  const PackedPathPrefix& place,
                                                  std::string_view key,
                                                  std::size_t firstFieldToTry)
{
    auto prefix = getPathPrefix(layout, place);
    auto fieldCount = layout.fields.size();

    for (auto step = std::size_t {0}; step < fieldCount; ++step)
    {
        auto fieldIndex = (firstFieldToTry + step) % fieldCount;
        auto path = layout.getPath(layout.fields[fieldIndex]);

        if (startsWithPrefix(path, prefix, place.depth) && path[place.depth] == key)
            return fieldIndex;
    }

    return std::nullopt;
}

std::vector<std::string_view> listPackedKeysAtDepth(const StoredPackedLayout& layout,
                                                    const PackedPathPrefix& place)
{
    auto prefix = getPathPrefix(layout, place);
    auto depth = place.depth;
    auto keys = std::vector<std::string_view> {};

    // The writer stores the fields of one nested struct next to each other,
    // so comparing with the previous key is enough to list each once.
    for (const auto& field: layout.fields)
    {
        auto path = layout.getPath(field);

        if (startsWithPrefix(path, prefix, depth) && !isLastKey(keys, path[depth]))
            keys.push_back(path[depth]);
    }

    return keys;
}

ObjectEntryCursor::ObjectEntryCursor(const Document& documentToUse,
                                     const ValueHeader& container,
                                     std::size_t fromOffset,
                                     std::size_t untilOffset)
    : document(documentToUse)
    , reader(documentToUse.bytes(), fromOffset)
    , endOffset(untilOffset)
    , hasInlineKeys(container.tag == Tag::Map)
{
}

std::string_view ObjectEntryCursor::readKey()
{
    if (hasInlineKeys)
        return reader.readString(reader.readVarint());

    auto name = document.findKeyName(reader.readVarint());

    if (!name)
        reader.hasReadEverythingInBounds = false;

    return name.value_or(std::string_view {});
}

bool ObjectEntryCursor::hasStoppedAtDamage() const
{
    return !reader.hasReadEverythingInBounds;
}

std::optional<ObjectEntry> ObjectEntryCursor::readNextEntry()
{
    if (!reader.hasReadEverythingInBounds || reader.readOffsetInBytes >= endOffset)
        return std::nullopt;

    auto key = readKey();
    auto valueOffset = reader.readOffsetInBytes;

    if (!skipValue(reader))
        return std::nullopt;

    return ObjectEntry {key, valueOffset, reader.readOffsetInBytes};
}

std::optional<ObjectEntry> ObjectEntryCursor::findEntryWithKey(std::string_view key)
{
    while (auto entry = readNextEntry())
    {
        if (entry->key == key)
            return entry;
    }

    return std::nullopt;
}

Document::Document(std::span<const std::uint8_t> dataToUse)
    : data(dataToUse)
{
    if (!isDocument(data) || data.size() < headerSize)
        return;

    auto tableOffset = std::size_t {ByteReader {data, headerFieldOffset}.readU32()};

    if (tableOffset < headerSize || tableOffset > data.size())
        return;

    valid =
        readKeyTable(tableOffset) && doesValueEndAt(data, headerSize, tableOffset);
}

Document::Document(std::span<const std::uint8_t> frameValueToUse,
                   const Document& baseDocumentToUse)
    : data(frameValueToUse)
    , rootValueOffset(0)
    , baseDocumentKeyNames(baseDocumentToUse.getKeyNames())
    , valid(baseDocumentToUse.isValid()
            && doesValueEndAt(frameValueToUse, 0, frameValueToUse.size()))
{
}

bool Document::readKeyTable(std::size_t tableOffset)
{
    auto reader = ByteReader {data, tableOffset};
    auto count = reader.readVarint();

    // Each key takes at least its length byte, which bounds a corrupt count.
    if (count > data.size() - std::min(reader.readOffsetInBytes, data.size()))
        return false;

    keyNamesById.reserve(count);

    for (auto id = std::uint32_t {0}; id < count; ++id)
    {
        auto name = reader.readString(reader.readVarint());

        if (!reader.hasReadEverythingInBounds)
            return false;

        keyNamesById.push_back(name);
    }

    return reader.readOffsetInBytes == data.size();
}

} // namespace Calder::Detail
