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

void patchPayloadSize(Calder::Writer& writer, std::size_t sizeFieldOffset)
{
    auto payloadSize =
        writer.getNextWriteOffset() - (sizeFieldOffset + sizeof(std::uint32_t));
    writer.patchU32(sizeFieldOffset, static_cast<std::uint32_t>(payloadSize));
}

template <typename Element>
void writePackedFixedWidth(Calder::Writer& writer,
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
void writePackedIntegers(Calder::Writer& writer, Tag tag, std::span<Integer> values)
{
    writer.writeTag(tag);
    auto sizeFieldOffset = writer.getNextWriteOffset();
    writer.writeU32(0);
    writer.writeU32(static_cast<std::uint32_t>(values.size()));

    for (auto value: values)
        writer.writeZigzag(getWireInteger(value));

    patchPayloadSize(writer, sizeFieldOffset);
}

void writePacked(Calder::Writer& writer, std::span<float> values)
{
    writePackedFixedWidth(writer, Tag::PackedFloat, values);
}

void writePacked(Calder::Writer& writer, std::span<double> values)
{
    if (!areAllExactlyRepresentableAsFloat(values))
    {
        writePackedFixedWidth(writer, Tag::PackedDouble, values);
        return;
    }

    auto narrowed = std::vector<float>(values.begin(), values.end());
    writePacked(writer, std::span<float> {narrowed});
}

void writePacked(Calder::Writer& writer, std::span<std::int32_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt32, values);
}

void writePacked(Calder::Writer& writer, std::span<std::int64_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt64, values);
}

void writePacked(Calder::Writer& writer, std::span<std::uint32_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt64, values);
}

void writePacked(Calder::Writer& writer, std::span<std::uint64_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt64, values);
}

void writePacked(Calder::Writer& writer, std::span<std::int8_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedInt8, values);
}

void writePacked(Calder::Writer& writer, std::span<std::uint8_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedUInt8, values);
}

void writePacked(Calder::Writer& writer, std::span<std::int16_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedInt16, values);
}

void writePacked(Calder::Writer& writer, std::span<std::uint16_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedUInt16, values);
}

void writeDouble(Calder::Writer& writer, double value)
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

void writePrimitive(Calder::Writer& writer, bool value)
{
    writer.writeTag(value ? Tag::True : Tag::False);
}

void writePrimitive(Calder::Writer& writer, const std::string& value)
{
    writer.writeTag(Tag::String);
    writer.writeString(value);
}

void writePrimitive(Calder::Writer& writer, double value)
{
    writeDouble(writer, value);
}

template <std::integral Integer>
void writePrimitive(Calder::Writer& writer, Integer value)
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
bool canCopyRawBytes(Encoding encoding)
{
    if constexpr (std::same_as<T, float>)
        return encoding == Encoding::Float32;
    else if constexpr (std::same_as<T, double>)
        return encoding == Encoding::Float64;
    else if constexpr (std::same_as<T, std::int8_t>)
        return encoding == Encoding::Int8;
    else if constexpr (std::same_as<T, std::uint8_t>)
        return encoding == Encoding::UInt8;
    else if constexpr (std::same_as<T, std::int16_t>)
        return encoding == Encoding::Int16;
    else if constexpr (std::same_as<T, std::uint16_t>)
        return encoding == Encoding::UInt16;
    else
        return false;
}

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

BinaryWriterReflector::BinaryWriterReflector(Calder::Writer& writerToUse,
                                             Options optsToUse)
    : Reflector(std::move(optsToUse))
    , writer(writerToUse)
{
    startSlot();
}

BinaryWriterReflector::~BinaryWriterReflector() { finishSlot(); }

void BinaryWriterReflector::startSlot()
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

void BinaryWriterReflector::restartSlot(Options&& optsToUse)
{
    opts = std::move(optsToUse);
    startSlot();
}

void BinaryWriterReflector::finishSlot()
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

void BinaryWriterReflector::finishOpenChild()
{
    if (reusableChild)
        reusableChild->finishSlot();
}

void BinaryWriterReflector::discardPendingSlot()
{
    writer.truncateTo(slot.omittableSlotStartOffset);

    if (slot.isOmittableSlotAnArrayElement)
        writer.writeTag(Tag::Null);
}

void BinaryWriterReflector::patchSizes()
{
    if (slot.countFieldOffset != Calder::npos)
        writer.patchU32(slot.countFieldOffset,
                        static_cast<std::uint32_t>(slot.elementCount));

    if (slot.sizeFieldOffset != Calder::npos)
        patchPayloadSize(writer, slot.sizeFieldOffset);
}

void BinaryWriterReflector::writeSizedHeader(Tag tag)
{
    writer.writeTag(tag);
    slot.sizeFieldOffset = writer.getNextWriteOffset();
    writer.writeU32(0);
}

void BinaryWriterReflector::writeArrayHeader()
{
    writeSizedHeader(Tag::Array);
    slot.countFieldOffset = writer.getNextWriteOffset();
    writer.writeU32(0);
}

void BinaryWriterReflector::visit(PrimitiveRef ref)
{
    if (opts.shape == Shape::Raw)
    {
        writer.truncateTo(slot.slotStartOffset);
        slot.sizeFieldOffset = Calder::npos;
    }

    std::visit([this](auto* value) { writePrimitive(writer, *value); }, ref.data);
}

bool BinaryWriterReflector::visitPacked(PackedArrayRef ref)
{
    if (opts.shape != Shape::Array)
        return false;

    writer.truncateTo(slot.slotStartOffset);
    slot.isSlotReplacedByPackedOrNull = true;
    std::visit([this](auto values) { writePacked(writer, values); }, ref.data);
    return true;
}

void BinaryWriterReflector::writeNull()
{
    // An empty optional of a container type arrives after its header.
    writer.truncateTo(slot.slotStartOffset);
    slot.isSlotReplacedByPackedOrNull = true;
    writer.writeTag(Tag::Null);
}

Reflector& BinaryWriterReflector::spawnChild(std::size_t childStartOffset,
                                             Options&& childOpts,
                                             bool isArrayElement)
{
    auto isOmittable = childOpts.omittable;

    if (reusableChild)
        reusableChild->restartSlot(std::move(childOpts));
    else
        reusableChild = new BinaryWriterReflector(writer, std::move(childOpts));

    reusableChild->slot.isOmittableSlotAnArrayElement = isArrayElement;

    if (isOmittable)
        reusableChild->slot.omittableSlotStartOffset = childStartOffset;

    return *reusableChild;
}

Reflector& BinaryWriterReflector::atKey(std::string_view key, Options childOpts)
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

Reflector& BinaryWriterReflector::atIndex(std::size_t index, Options childOpts)
{
    finishOpenChild();
    slot.elementCount = std::max(slot.elementCount, index + 1);
    return spawnChild(writer.getNextWriteOffset(), std::move(childOpts), true);
}

void BinaryWriterReflector::resizeArray(std::size_t newSize)
{
    if (opts.shape == Shape::Raw && slot.countFieldOffset == Calder::npos)
    {
        writer.truncateTo(slot.slotStartOffset);
        writeArrayHeader();
    }

    slot.elementCount = std::max(slot.elementCount, newSize);
}

BinaryReaderReflector::BinaryReaderReflector(const Calder::Document& documentToUse,
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

BinaryReaderReflector::BinaryReaderReflector(
    const BinaryReaderReflector& parentToUse,
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

bool BinaryReaderReflector::isStoredAs(ValueKind expectedKind) const
{
    return !header.valid || kind() == expectedKind;
}

void BinaryReaderReflector::reportTypeMismatch(ValueKind expectedKind) const
{
    report.typeMismatches.push_back({buildFieldPath(), kind(), expectedKind});
}

std::string BinaryReaderReflector::buildFieldPath() const
{
    auto path = std::string {};

    if (parent)
        path = parent->get().buildFieldPath();

    std::visit([&path](auto step) { appendFieldPathStep(path, step); },
               stepFromParent);
    return path;
}

BinaryReaderReflector::~BinaryReaderReflector() = default;

void BinaryReaderReflector::restartSlot(StepFromParent stepToUse,
                                        const ValueHeader& headerToUse,
                                        Options&& optsToUse)
{
    opts = std::move(optsToUse);
    header = headerToUse;
    stepFromParent = stepToUse;
    hasReportedObjectMismatch = false;
    resumeOffset = headerToUse.payloadOffset;
    resumeElementIndex = 0;
}

bool BinaryReaderReflector::hasCategory(ValueCategory category) const
{
    return header.valid && header.category == category;
}

ValueKind BinaryReaderReflector::kind() const
{
    if (!header.valid)
        return ValueKind::Absent;

    return getValueKind(header.category);
}

bool BinaryReaderReflector::isIntegerNumber() const
{
    return hasCategory(ValueCategory::Number)
           && Calder::Detail::isIntegerEncoding(header.encoding);
}

void BinaryReaderReflector::visit(PrimitiveRef ref)
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

bool BinaryReaderReflector::visitPacked(PackedArrayRef ref)
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

std::size_t BinaryReaderReflector::arraySize() const
{
    if (!isStoredAs(ValueKind::Array))
        reportTypeMismatch(ValueKind::Array);

    if (!hasCategory(ValueCategory::Array))
        return 0;

    return header.elementCount;
}

Vector<std::string> BinaryReaderReflector::mapKeys() const
{
    auto keys = Vector<std::string> {};

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

std::optional<Calder::Detail::ObjectEntry>
    BinaryReaderReflector::findEntry(std::string_view key)
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

Reflector& BinaryReaderReflector::spawnChild(StepFromParent step,
                                             const ValueHeader& childHeader,
                                             Options&& childOpts)
{
    if (reusableChild)
        reusableChild->restartSlot(step, childHeader, std::move(childOpts));
    else
        reusableChild = new BinaryReaderReflector(
            *this, step, childHeader, std::move(childOpts));

    return *reusableChild;
}

Reflector& BinaryReaderReflector::spawnMissingChild(StepFromParent step,
                                                    Options&& childOpts)
{
    return spawnChild(step, ValueHeader {}, std::move(childOpts));
}

Reflector& BinaryReaderReflector::spawnElementChild(std::size_t index,
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

Reflector& BinaryReaderReflector::atKey(std::string_view key, Options childOpts)
{
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

void BinaryReaderReflector::rewindResumeOffsetIfPast(std::size_t index)
{
    if (index >= resumeElementIndex)
        return;

    resumeOffset = header.payloadOffset;
    resumeElementIndex = 0;
}

Reflector& BinaryReaderReflector::atIndex(std::size_t index, Options childOpts)
{
    if (!hasCategory(ValueCategory::Array) || index >= header.elementCount)
        return spawnMissingChild(index, std::move(childOpts));

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
