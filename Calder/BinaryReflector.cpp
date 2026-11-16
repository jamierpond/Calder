#include "BinaryReflector.h"
#include <Miro/Reflection/NumericConvert.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace Calder
{

using Calder::Tag;
using Calder::Detail::ByteReader;
using Calder::Detail::Encoding;
using Calder::Detail::ValueCategory;
using Calder::Detail::ValueHeader;

namespace
{

// NaN compares false, so it stays a full double.
bool isExactlyRepresentableAsFloat(double value)
{
    return static_cast<double>(static_cast<float>(value)) == value;
}

bool areAllExactlyRepresentableAsFloat(std::span<double> values)
{
    return std::ranges::all_of(values, isExactlyRepresentableAsFloat);
}

void patchPayloadSize(Detail::ByteWriter& writer, std::size_t sizeFieldOffset)
{
    auto payloadSize =
        writer.getNextWriteOffset() - (sizeFieldOffset + sizeof(std::uint32_t));
    writer.patchU32(sizeFieldOffset, static_cast<std::uint32_t>(payloadSize));
}

template <typename Element>
void writePackedFixedWidth(Detail::ByteWriter& writer,
                           Tag tag,
                           std::span<Element> values)
{
    writer.writeTag(tag);
    writer.writeU32(static_cast<std::uint32_t>(values.size()));
    writer.writeFixedWidthElements(std::span<const Element> {values});
}

// A uint64 travels as its int64 bit pattern, the way Miro passes it to
// visit(); convertPackedElement reinterprets it back.
template <typename Integer>
std::int64_t getWireInteger(Integer value)
{
    if constexpr (std::same_as<Integer, std::uint64_t>)
        return std::bit_cast<std::int64_t>(value);
    else
        return static_cast<std::int64_t>(value);
}

template <typename Integer>
void writePackedIntegers(Detail::ByteWriter& writer,
                         Tag tag,
                         std::span<Integer> values)
{
    writer.writeTag(tag);
    auto sizeFieldOffset = writer.getNextWriteOffset();
    writer.writeU32(0);
    writer.writeU32(static_cast<std::uint32_t>(values.size()));

    for (auto value: values)
        writer.writeZigzag(getWireInteger(value));

    patchPayloadSize(writer, sizeFieldOffset);
}

void writePacked(Detail::ByteWriter& writer, std::span<float> values)
{
    writePackedFixedWidth(writer, Tag::PackedFloat, values);
}

void writePacked(Detail::ByteWriter& writer, std::span<double> values)
{
    if (!areAllExactlyRepresentableAsFloat(values))
    {
        writePackedFixedWidth(writer, Tag::PackedDouble, values);
        return;
    }

    auto narrowed = std::vector<float>(values.begin(), values.end());
    writePacked(writer, std::span<float> {narrowed});
}

void writePacked(Detail::ByteWriter& writer, std::span<std::int32_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt32, values);
}

void writePacked(Detail::ByteWriter& writer, std::span<std::int64_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt64, values);
}

void writePacked(Detail::ByteWriter& writer, std::span<std::uint32_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt64, values);
}

void writePacked(Detail::ByteWriter& writer, std::span<std::uint64_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt64, values);
}

void writePacked(Detail::ByteWriter& writer, std::span<std::int8_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedInt8, values);
}

void writePacked(Detail::ByteWriter& writer, std::span<std::uint8_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedUInt8, values);
}

void writePacked(Detail::ByteWriter& writer, std::span<std::int16_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedInt16, values);
}

void writePacked(Detail::ByteWriter& writer, std::span<std::uint16_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedUInt16, values);
}

void writeDouble(Detail::ByteWriter& writer, double value)
{
    if (!isExactlyRepresentableAsFloat(value))
    {
        writer.writeTag(Tag::Double);
        writer.writeFixedWidth(value);
        return;
    }

    auto narrow = static_cast<float>(value);
    writer.writeTag(Tag::Float32);
    writer.writeFixedWidth(narrow);
}

void writePrimitive(Detail::ByteWriter& writer, bool value)
{
    writer.writeTag(value ? Tag::True : Tag::False);
}

void writePrimitive(Detail::ByteWriter& writer, const std::string& value)
{
    writer.writeTag(Tag::String);
    writer.writeString(value);
}

void writePrimitive(Detail::ByteWriter& writer, double value)
{
    writeDouble(writer, value);
}

template <std::integral Integer>
void writePrimitive(Detail::ByteWriter& writer, Integer value)
{
    writer.writeTag(Tag::Int);
    writer.writeZigzag(static_cast<std::int64_t>(value));
}

void readPrimitive(const Calder::Document&, const ValueHeader& header, bool& out)
{
    if (header.tag == Tag::True)
        out = true;
    else if (header.tag == Tag::False)
        out = false;
}

void readPrimitive(const Calder::Document& document,
                   const ValueHeader& header,
                   std::string& out)
{
    auto reader = ByteReader {document.bytes(), header.payloadOffset};
    auto text = reader.readString(reader.readVarint());

    if (reader.hasReadEverythingInBounds)
        out = std::string {text};
}

template <typename Out>
void convertDecodedNumber(const Calder::Detail::Number& number, Out& out)
{
    std::visit([&out](auto value) { Miro::Detail::convertNumber(value, out); },
               number);
}

template <typename Out>
void readPrimitive(const Calder::Document& document,
                   const ValueHeader& header,
                   Out& out)
{
    auto reader = ByteReader {document.bytes(), header.payloadOffset};
    auto number = Calder::Detail::readNumber(reader, header.encoding);

    if (reader.hasReadEverythingInBounds)
        convertDecodedNumber(number, out);
}

template <typename T>
ValueKind getExpectedValueKind(const T&)
{
    if constexpr (std::same_as<T, bool>)
        return ValueKind::Bool;
    else if constexpr (std::same_as<T, std::string>)
        return ValueKind::String;
    else
        return ValueKind::Number;
}

void appendFieldPathStep(std::string& path, std::string_view key)
{
    if (!path.empty())
        path += '.';

    path += key;
}

void appendFieldPathStep(std::string& path, std::size_t index)
{
    path += '[';
    path += std::to_string(index);
    path += ']';
}

void appendFieldPathStep(std::string&, std::monostate) {}

template <typename T>
void convertPackedElement(const Calder::Detail::Number& number, T& element)
{
    if constexpr (std::same_as<T, std::uint64_t>)
    {
        if (const auto* integer = std::get_if<std::int64_t>(&number))
        {
            element = std::bit_cast<std::uint64_t>(*integer);
            return;
        }
    }

    convertDecodedNumber(number, element);
}

// The encoding whose bytes are exactly this C++ number type's,
// little-endian.
template <typename T>
constexpr Encoding getFixedWidthEncoding()
{
    if constexpr (std::same_as<T, float>)
        return Encoding::Float32;
    else if constexpr (std::same_as<T, double>)
        return Encoding::Float64;
    else if constexpr (std::same_as<T, std::int8_t>)
        return Encoding::Int8;
    else if constexpr (std::same_as<T, std::uint8_t>)
        return Encoding::UInt8;
    else if constexpr (std::same_as<T, std::int16_t>)
        return Encoding::Int16;
    else if constexpr (std::same_as<T, std::uint16_t>)
        return Encoding::UInt16;
    else if constexpr (std::same_as<T, std::int32_t>)
        return Encoding::Int32;
    else if constexpr (std::same_as<T, std::uint32_t>)
        return Encoding::UInt32;
    else if constexpr (std::same_as<T, std::int64_t>)
        return Encoding::Int64;
    else
        return Encoding::UInt64;
}

template <typename T>
bool canCopyRawBytes(Encoding encoding)
{
    return encoding == getFixedWidthEncoding<T>();
}

using NumberTypeTag = Miro::PackedNumberType;

Encoding getFixedWidthEncoding(const NumberTypeTag& type)
{
    return std::visit(
        [](auto typeTag)
        { return getFixedWidthEncoding<typename decltype(typeTag)::type>(); },
        type);
}

std::size_t getByteWidth(const NumberTypeTag& type)
{
    return std::visit(
        [](auto typeTag) { return sizeof(typename decltype(typeTag)::type); }, type);
}

constexpr auto isHostLittleEndian = std::endian::native == std::endian::little;

// True when each element's bytes are its fields in reflect order with no
// padding, so the whole array is already the records the wire holds.
bool areElementsLaidOutAsRecords(const PackedStructArrayRef& ref)
{
    if (!isHostLittleEndian)
        return false;

    auto expectedOffset = std::size_t {0};

    for (const auto& field: ref.fields)
    {
        if (field.offset != expectedOffset)
            return false;

        expectedOffset += getByteWidth(field.type);
    }

    return expectedOffset == ref.stride;
}

void writePackedStructLayout(Detail::ByteWriter& writer,
                             const PackedStructArrayRef& ref)
{
    writer.writeVarint(ref.fields.size());

    for (const auto& field: ref.fields)
    {
        writer.writeVarint(field.path.size());

        for (auto segment: field.path)
            writer.writeVarint(writer.getInternedKeyId(segment));

        auto type =
            Calder::Detail::findPackedFieldType(getFixedWidthEncoding(field.type));
        writer.writeByte(static_cast<std::uint8_t>(*type));
    }
}

void writePackedStructRecordsFieldByField(Detail::ByteWriter& writer,
                                          const PackedStructArrayRef& ref)
{
    for (auto i = std::size_t {0}; i < ref.getElementCount(); ++i)
    {
        const auto* element = ref.elementBytes.data() + i * ref.stride;

        for (const auto& field: ref.fields)
        {
            std::visit(
                [&writer, element, &field](auto typeTag)
                {
                    using Number = typename decltype(typeTag)::type;
                    auto value = Number {};
                    std::memcpy(&value, element + field.offset, sizeof(value));
                    writer.writeFixedWidth(value);
                },
                field.type);
        }
    }
}

void writePackedStructArray(Detail::ByteWriter& writer,
                            const PackedStructArrayRef& ref)
{
    writer.writeTag(Tag::PackedStructArray);
    auto sizeFieldOffset = writer.getNextWriteOffset();
    writer.writeU32(0);
    writer.writeU32(static_cast<std::uint32_t>(ref.getElementCount()));
    writePackedStructLayout(writer, ref);

    if (areElementsLaidOutAsRecords(ref))
        writer.writeBytes(ref.elementBytes.data(), ref.elementBytes.size());
    else
        writePackedStructRecordsFieldByField(writer, ref);

    patchPayloadSize(writer, sizeFieldOffset);
}

bool arePathsEqual(std::span<const std::string_view> first,
                   std::span<const std::string_view> second)
{
    return std::ranges::equal(first, second);
}

// True when the stored records are byte for byte the C++ elements.
bool areRecordsLaidOutAsElements(const Calder::Detail::StoredPackedLayout& layout,
                                 const PackedStructArrayRef& ref)
{
    if (!isHostLittleEndian || layout.recordSize != ref.stride
        || layout.fields.size() != ref.fields.size())
        return false;

    for (auto i = std::size_t {0}; i < ref.fields.size(); ++i)
    {
        const auto& stored = layout.fields[i];
        const auto& wanted = ref.fields[i];

        if (stored.offsetInRecord != wanted.offset
            || stored.encoding != getFixedWidthEncoding(wanted.type)
            || !arePathsEqual(layout.getPath(stored), wanted.path))
            return false;
    }

    return true;
}

// One C++ field filled from one stored field, matched by path.
struct PackedFieldCopy
{
    std::size_t sourceOffset = 0;
    Encoding sourceEncoding = Encoding::Float32;
    std::size_t destinationOffset = 0;
    NumberTypeTag destinationType;
    bool isRawByteCopy = false;
};

std::optional<PackedFieldCopy>
    planPackedFieldCopy(const Calder::Detail::StoredPackedLayout& layout,
                        const Miro::PackedStructField& wanted)
{
    for (const auto& stored: layout.fields)
    {
        if (!arePathsEqual(layout.getPath(stored), wanted.path))
            continue;

        auto isSameType = stored.encoding == getFixedWidthEncoding(wanted.type);
        return PackedFieldCopy {.sourceOffset = stored.offsetInRecord,
                                .sourceEncoding = stored.encoding,
                                .destinationOffset = wanted.offset,
                                .destinationType = wanted.type,
                                .isRawByteCopy = isSameType && isHostLittleEndian};
    }

    return std::nullopt;
}

void copyPackedField(std::span<const std::uint8_t> record,
                     std::byte* element,
                     const PackedFieldCopy& copy)
{
    if (copy.isRawByteCopy)
    {
        std::memcpy(element + copy.destinationOffset,
                    record.data() + copy.sourceOffset,
                    getByteWidth(copy.destinationType));
        return;
    }

    auto reader = ByteReader {record, copy.sourceOffset};
    auto number = Calder::Detail::readNumber(reader, copy.sourceEncoding);

    std::visit(
        [element, &copy, &number](auto typeTag)
        {
            using Number = typename decltype(typeTag)::type;
            auto* destination = element + copy.destinationOffset;
            auto value = Number {};
            std::memcpy(&value, destination, sizeof(value));
            convertPackedElement(number, value);
            std::memcpy(destination, &value, sizeof(value));
        },
        copy.destinationType);
}

void readPackedRecordsFieldByField(const Calder::Detail::StoredPackedLayout& layout,
                                   std::span<const std::uint8_t> records,
                                   const PackedStructArrayRef& ref,
                                   std::size_t count)
{
    auto copies = std::vector<PackedFieldCopy> {};

    for (const auto& wanted: ref.fields)
    {
        if (auto copy = planPackedFieldCopy(layout, wanted))
            copies.push_back(*copy);
    }

    for (auto i = std::size_t {0}; i < count; ++i)
    {
        auto record = records.subspan(i * layout.recordSize, layout.recordSize);
        auto* element = ref.elementBytes.data() + i * ref.stride;

        for (const auto& copy: copies)
            copyPackedField(record, element, copy);
    }
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

// True when `path` continues `prefix` for its first `depth` segments.
bool startsWithPrefix(std::span<const std::string_view> path,
                      std::span<const std::string_view> prefix,
                      std::size_t depth)
{
    return path.size() > depth && prefix.size() >= depth
           && std::ranges::equal(path.first(depth), prefix.first(depth));
}

template <typename T>
void readPackedElements(ByteReader& reader, Encoding encoding, std::span<T> dest)
{
    if (canCopyRawBytes<T>(encoding))
    {
        auto bytes = reader.readBytes(dest.size_bytes());

        if (reader.hasReadEverythingInBounds)
            Calder::Detail::decodeLittleEndianElements(bytes, dest);

        return;
    }

    for (auto& element: dest)
        convertPackedElement(Calder::Detail::readNumber(reader, encoding), element);
}

ValueKind getValueKind(ValueCategory category)
{
    switch (category)
    {
        case ValueCategory::Null:
            return ValueKind::Null;
        case ValueCategory::Bool:
            return ValueKind::Bool;
        case ValueCategory::Number:
            return ValueKind::Number;
        case ValueCategory::String:
            return ValueKind::String;
        case ValueCategory::Object:
            return ValueKind::Object;
        case ValueCategory::Array:
            return ValueKind::Array;
    }

    return ValueKind::Absent;
}

} // namespace

Writer::Writer(Detail::ByteWriter& writerToUse, Options optsToUse)
    : Reflector(std::move(optsToUse))
    , writer(writerToUse)
{
    startSlot();
}

Writer::~Writer() { finishSlot(); }

void Writer::startSlot()
{
    slot =
        SlotState {.isOpen = true, .slotStartOffset = writer.getNextWriteOffset()};

    // A Raw value commits to an object, as the JSON layer does, until
    // visit() / writeNull() / resizeArray() learn otherwise.
    if (opts.shape == Shape::Map)
        writeSizedHeader(Tag::Map);
    else if (opts.shape == Shape::Array)
        writeArrayHeader();
    else if (opts.shape != Shape::Primitive)
        writeSizedHeader(Tag::Object);
}

void Writer::restartSlot(Options&& optsToUse)
{
    opts = std::move(optsToUse);
    startSlot();
}

void Writer::finishSlot()
{
    if (!slot.isOpen)
        return;

    slot.isOpen = false;
    finishOpenChild();

    if (slot.omittableSlotStartOffset != Calder::npos)
        discardPendingSlot();
    else if (!slot.isSlotReplacedByPackedOrNull)
        patchSizes();
}

void Writer::finishOpenChild()
{
    if (reusableChild)
        reusableChild->finishSlot();
}

void Writer::discardPendingSlot()
{
    writer.truncateTo(slot.omittableSlotStartOffset);

    if (slot.isOmittableSlotAnArrayElement)
        writer.writeTag(Tag::Null);
}

void Writer::patchSizes()
{
    if (slot.countFieldOffset != Calder::npos)
        writer.patchU32(slot.countFieldOffset,
                        static_cast<std::uint32_t>(slot.elementCount));

    if (slot.sizeFieldOffset != Calder::npos)
        patchPayloadSize(writer, slot.sizeFieldOffset);
}

void Writer::writeSizedHeader(Tag tag)
{
    writer.writeTag(tag);
    slot.sizeFieldOffset = writer.getNextWriteOffset();
    writer.writeU32(0);
}

void Writer::writeArrayHeader()
{
    writeSizedHeader(Tag::Array);
    slot.countFieldOffset = writer.getNextWriteOffset();
    writer.writeU32(0);
}

void Writer::visit(PrimitiveRef ref)
{
    if (opts.shape == Shape::Raw)
    {
        writer.truncateTo(slot.slotStartOffset);
        slot.sizeFieldOffset = Calder::npos;
    }

    std::visit([this](auto* value) { writePrimitive(writer, *value); }, ref.data);
}

bool Writer::visitPacked(PackedArrayRef ref)
{
    if (opts.shape != Shape::Array)
        return false;

    writer.truncateTo(slot.slotStartOffset);
    slot.isSlotReplacedByPackedOrNull = true;
    std::visit([this](auto values) { writePacked(writer, values); }, ref.data);
    return true;
}

bool Writer::visitPackedStructs(PackedStructArrayRef ref)
{
    if (opts.shape != Shape::Array)
        return false;

    writer.truncateTo(slot.slotStartOffset);
    slot.isSlotReplacedByPackedOrNull = true;
    writePackedStructArray(writer, ref);
    return true;
}

void Writer::writeNull()
{
    // An empty optional of a container type arrives after its header.
    writer.truncateTo(slot.slotStartOffset);
    slot.isSlotReplacedByPackedOrNull = true;
    writer.writeTag(Tag::Null);
}

Reflector& Writer::spawnChild(std::size_t childStartOffset,
                              Options&& childOpts,
                              bool isArrayElement)
{
    auto isOmittable = childOpts.omittable;

    if (reusableChild)
        reusableChild->restartSlot(std::move(childOpts));
    else
        reusableChild = new Writer(writer, std::move(childOpts));

    reusableChild->slot.isOmittableSlotAnArrayElement = isArrayElement;

    if (isOmittable)
        reusableChild->slot.omittableSlotStartOffset = childStartOffset;

    return *reusableChild;
}

Reflector& Writer::atKey(std::string_view key, Options childOpts)
{
    // The previous child patches its size from writer.getNextWriteOffset(), so it
    // must finish before the key is appended.
    finishOpenChild();

    auto keyStart = writer.getNextWriteOffset();

    if (opts.shape == Shape::Map)
        writer.writeString(key);
    else
        writer.writeVarint(writer.getInternedKeyId(key));

    return spawnChild(keyStart, std::move(childOpts), false);
}

Reflector& Writer::atIndex(std::size_t index, Options childOpts)
{
    finishOpenChild();
    slot.elementCount = std::max(slot.elementCount, index + 1);
    return spawnChild(writer.getNextWriteOffset(), std::move(childOpts), true);
}

void Writer::resizeArray(std::size_t newSize)
{
    if (opts.shape == Shape::Raw && slot.countFieldOffset == Calder::npos)
    {
        writer.truncateTo(slot.slotStartOffset);
        writeArrayHeader();
    }

    slot.elementCount = std::max(slot.elementCount, newSize);
}

Reader::Reader(const Calder::Document& documentToUse,
               const ValueHeader& headerToUse,
               Options optsToUse,
               BinaryLoadReport& reportToUse)
    : Reflector(std::move(optsToUse))
    , document(documentToUse)
    , header(headerToUse)
    , report(reportToUse)
    , resumeOffset(headerToUse.payloadOffset)
{
}

Reader::Reader(const Reader& parentToUse,
               StepFromParent stepToUse,
               const ValueHeader& headerToUse,
               Options optsToUse)
    : Reflector(std::move(optsToUse))
    , document(parentToUse.document)
    , header(headerToUse)
    , report(parentToUse.report)
    , parent(parentToUse)
    , stepFromParent(stepToUse)
    , resumeOffset(headerToUse.payloadOffset)
{
}

bool Reader::isStoredAs(ValueKind expectedKind) const
{
    return !header.valid || kind() == expectedKind;
}

void Reader::reportTypeMismatch(ValueKind expectedKind) const
{
    report.typeMismatches.push_back({buildFieldPath(), kind(), expectedKind});
}

std::string Reader::buildFieldPath() const
{
    auto path = std::string {};

    if (parent)
        path = parent->get().buildFieldPath();

    std::visit([&path](auto step) { appendFieldPathStep(path, step); },
               stepFromParent);
    return path;
}

Reader::~Reader() = default;

void Reader::restartSlot(StepFromParent stepToUse,
                         const ValueHeader& headerToUse,
                         Options&& optsToUse,
                         std::optional<PackedRecordSlot> packedRecordToUse)
{
    opts = std::move(optsToUse);
    header = headerToUse;
    stepFromParent = stepToUse;
    hasReportedObjectMismatch = false;
    resumeOffset = headerToUse.payloadOffset;
    resumeElementIndex = 0;
    packedRecord = packedRecordToUse;
    hasReadStoredPackedLayout = false;
}

bool Reader::hasCategory(ValueCategory category) const
{
    return header.valid && header.category == category;
}

ValueKind Reader::kind() const
{
    if (!header.valid)
        return ValueKind::Absent;

    return getValueKind(header.category);
}

bool Reader::isIntegerNumber() const
{
    return hasCategory(ValueCategory::Number)
           && Calder::Detail::isIntegerEncoding(header.encoding);
}

void Reader::visit(PrimitiveRef ref)
{
    if (!header.valid)
        return;

    std::visit(
        [this](auto* value)
        {
            auto expectedKind = getExpectedValueKind(*value);

            if (isStoredAs(expectedKind))
                readPrimitive(document, header, *value);
            else
                reportTypeMismatch(expectedKind);
        },
        ref.data);
}

bool Reader::visitPacked(PackedArrayRef ref)
{
    if (!hasCategory(ValueCategory::Array) || header.encoding == Encoding::Tagged)
        return false;

    std::visit(
        [this](auto dest)
        {
            auto reader = ByteReader {document.bytes(), header.payloadOffset};
            auto count = std::min(dest.size(), header.elementCount);
            readPackedElements(reader, header.encoding, dest.first(count));
        },
        ref.data);

    return true;
}

bool Reader::readStoredPackedLayoutOnce()
{
    if (!hasReadStoredPackedLayout)
    {
        hasReadStoredPackedLayout = true;
        isStoredPackedLayoutValid = Calder::Detail::readStoredPackedLayout(
            document, header, storedPackedLayout);
    }

    return isStoredPackedLayoutValid;
}

bool Reader::visitPackedStructs(PackedStructArrayRef ref)
{
    if (!header.valid || header.tag != Tag::PackedStructArray)
        return false;

    if (!readStoredPackedLayoutOnce())
        return true;

    const auto& layout = storedPackedLayout;
    auto count = std::min(ref.getElementCount(), layout.recordCount);
    auto records =
        document.bytes().subspan(layout.recordsOffset, count * layout.recordSize);

    if (areRecordsLaidOutAsElements(layout, ref))
        std::memcpy(ref.elementBytes.data(), records.data(), records.size());
    else
        readPackedRecordsFieldByField(layout, records, ref, count);

    return true;
}

std::size_t Reader::arraySize() const
{
    if (!isStoredAs(ValueKind::Array))
        reportTypeMismatch(ValueKind::Array);

    if (!hasCategory(ValueCategory::Array))
        return 0;

    return header.elementCount;
}

Vector<std::string> Reader::mapKeys() const
{
    auto keys = Vector<std::string> {};

    if (packedRecord)
    {
        appendPackedRecordKeys(keys);
        return keys;
    }

    if (!isStoredAs(ValueKind::Object))
        reportTypeMismatch(ValueKind::Object);

    if (!hasCategory(ValueCategory::Object))
        return keys;

    auto cursor = Calder::Detail::ObjectEntryCursor {
        document, header, header.payloadOffset, header.payloadEndOffset};

    while (auto entry = cursor.readNextEntry())
        keys.add(std::string {entry->key});

    return keys;
}

std::optional<Calder::Detail::ObjectEntry> Reader::findEntry(std::string_view key)
{
    // Fields usually arrive in written order: resume from the cursor,
    // then wrap around once for reordered or missing keys.
    auto resumeOffsetBeforeSearch = resumeOffset;
    auto entry =
        Calder::Detail::ObjectEntryCursor {
            document, header, resumeOffsetBeforeSearch, header.payloadEndOffset}
            .findEntryWithKey(key);

    if (!entry)
        entry =
            Calder::Detail::ObjectEntryCursor {
                document, header, header.payloadOffset, resumeOffsetBeforeSearch}
                .findEntryWithKey(key);

    if (entry)
        resumeOffset = entry->valueEndOffset;

    return entry;
}

Reflector& Reader::spawnChild(StepFromParent step,
                              const ValueHeader& childHeader,
                              Options&& childOpts,
                              std::optional<PackedRecordSlot> childPackedRecord)
{
    if (!reusableChild)
        reusableChild = new Reader(*this, step, childHeader, Options {});

    reusableChild->restartSlot(
        step, childHeader, std::move(childOpts), childPackedRecord);
    return *reusableChild;
}

Reflector& Reader::spawnPackedRecordChild(std::size_t index, Options&& childOpts)
{
    if (!readStoredPackedLayoutOnce() || index >= storedPackedLayout.recordCount)
        return spawnMissingChild(index, std::move(childOpts));

    auto recordOffset =
        storedPackedLayout.recordsOffset + index * storedPackedLayout.recordSize;
    return spawnChild(index,
                      makePackedRecordHeader(recordOffset),
                      std::move(childOpts),
                      PackedRecordSlot {.layout = storedPackedLayout,
                                        .recordOffset = recordOffset});
}

std::optional<std::size_t>
    Reader::findPackedFieldAtThisDepth(std::string_view key) const
{
    const auto& layout = packedRecord->layout.get();
    auto prefix = getPackedRecordPrefix();

    for (auto i = std::size_t {0}; i < layout.fields.size(); ++i)
    {
        auto path = layout.getPath(layout.fields[i]);

        if (startsWithPrefix(path, prefix, packedRecord->pathDepth)
            && path[packedRecord->pathDepth] == key)
            return i;
    }

    return std::nullopt;
}

std::span<const std::string_view> Reader::getPackedRecordPrefix() const
{
    const auto& layout = packedRecord->layout.get();

    if (packedRecord->pathDepth == 0)
        return {};

    return layout.getPath(layout.fields[packedRecord->prefixField]);
}

void Reader::appendPackedRecordKeys(Vector<std::string>& keys) const
{
    const auto& layout = packedRecord->layout.get();
    auto prefix = getPackedRecordPrefix();
    auto depth = packedRecord->pathDepth;
    auto previousKey = std::optional<std::string_view> {};

    for (const auto& field: layout.fields)
    {
        auto path = layout.getPath(field);

        if (!startsWithPrefix(path, prefix, depth) || previousKey == path[depth])
            continue;

        previousKey = path[depth];
        keys.add(std::string {path[depth]});
    }
}

Reflector& Reader::spawnPackedFieldChild(std::string_view key, Options&& childOpts)
{
    auto fieldIndex = findPackedFieldAtThisDepth(key);

    if (!fieldIndex)
        return spawnMissingChild(key, std::move(childOpts));

    const auto& layout = packedRecord->layout.get();
    const auto& field = layout.fields[*fieldIndex];
    auto childDepth = packedRecord->pathDepth + 1;

    if (layout.getPath(field).size() == childDepth)
        return spawnChild(
            key,
            Calder::Detail::makePackedElementHeader(
                field.encoding, packedRecord->recordOffset + field.offsetInRecord),
            std::move(childOpts));

    return spawnChild(key,
                      makePackedRecordHeader(packedRecord->recordOffset),
                      std::move(childOpts),
                      PackedRecordSlot {.layout = layout,
                                        .recordOffset = packedRecord->recordOffset,
                                        .pathDepth = childDepth,
                                        .prefixField = *fieldIndex});
}

Reflector& Reader::spawnMissingChild(StepFromParent step, Options&& childOpts)
{
    return spawnChild(step, ValueHeader {}, std::move(childOpts));
}

Reflector& Reader::spawnElementChild(std::size_t index,
                                     std::size_t elementOffset,
                                     Options&& childOpts)
{
    if (header.encoding == Encoding::Tagged)
        return spawnChild(
            index,
            Calder::Detail::parseValueHeader(document.bytes(), elementOffset),
            std::move(childOpts));

    return spawnChild(
        index,
        Calder::Detail::makePackedElementHeader(header.encoding, elementOffset),
        std::move(childOpts));
}

Reflector& Reader::atKey(std::string_view key, Options childOpts)
{
    if (packedRecord)
        return spawnPackedFieldChild(key, std::move(childOpts));

    if (!isStoredAs(ValueKind::Object) && !hasReportedObjectMismatch)
    {
        reportTypeMismatch(ValueKind::Object);
        hasReportedObjectMismatch = true;
    }

    auto entry = hasCategory(ValueCategory::Object) ? findEntry(key) : std::nullopt;

    if (!entry)
        return spawnMissingChild(key, std::move(childOpts));

    return spawnChild(
        key,
        Calder::Detail::parseValueHeader(document.bytes(), entry->valueOffset),
        std::move(childOpts));
}

void Reader::rewindResumeOffsetIfPast(std::size_t index)
{
    if (index >= resumeElementIndex)
        return;

    resumeOffset = header.payloadOffset;
    resumeElementIndex = 0;
}

Reflector& Reader::atIndex(std::size_t index, Options childOpts)
{
    if (!hasCategory(ValueCategory::Array) || index >= header.elementCount)
        return spawnMissingChild(index, std::move(childOpts));

    if (header.encoding == Encoding::PackedStruct)
        return spawnPackedRecordChild(index, std::move(childOpts));

    rewindResumeOffsetIfPast(index);

    auto reader = ByteReader {document.bytes(), resumeOffset};
    auto skipped = Calder::Detail::skipElements(
        reader, header.encoding, index - resumeElementIndex);

    if (!skipped || reader.readOffsetInBytes >= header.payloadEndOffset)
        return spawnMissingChild(index, std::move(childOpts));

    auto elementOffset = reader.readOffsetInBytes;
    resumeOffset = elementOffset;
    resumeElementIndex = index;

    if (Calder::Detail::skipElements(reader, header.encoding, 1))
    {
        resumeOffset = reader.readOffsetInBytes;
        resumeElementIndex = index + 1;
    }

    return spawnElementChild(index, elementOffset, std::move(childOpts));
}

} // namespace Calder
