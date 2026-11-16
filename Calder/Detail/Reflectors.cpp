#include "../Calder.h"

#include <Miro/Reflection/NumericConvert.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace Calder::Detail
{

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

void patchPayloadSize(ByteWriter& writer, std::size_t sizeFieldOffset)
{
    auto payloadSize =
        writer.getNextWriteOffset() - (sizeFieldOffset + sizeof(std::uint32_t));
    writer.patchU32(sizeFieldOffset, static_cast<std::uint32_t>(payloadSize));
}

template <typename Element>
void writePackedFixedWidth(ByteWriter& writer, Tag tag, std::span<Element> values)
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
void writePackedIntegers(ByteWriter& writer, Tag tag, std::span<Integer> values)
{
    writer.writeTag(tag);
    auto sizeFieldOffset = writer.getNextWriteOffset();
    writer.writeU32(0);
    writer.writeU32(static_cast<std::uint32_t>(values.size()));

    for (auto value: values)
        writer.writeZigzag(getWireInteger(value));

    patchPayloadSize(writer, sizeFieldOffset);
}

void writePacked(ByteWriter& writer, std::span<float> values)
{
    writePackedFixedWidth(writer, Tag::PackedFloat, values);
}

void writePacked(ByteWriter& writer, std::span<double> values)
{
    if (!areAllExactlyRepresentableAsFloat(values))
    {
        writePackedFixedWidth(writer, Tag::PackedDouble, values);
        return;
    }

    auto narrowed = std::vector<float>(values.size());
    std::ranges::transform(values,
                           narrowed.begin(),
                           [](double value) { return static_cast<float>(value); });
    writePacked(writer, std::span<float> {narrowed});
}

void writePacked(ByteWriter& writer, std::span<std::int32_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt32, values);
}

void writePacked(ByteWriter& writer, std::span<std::int64_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt64, values);
}

void writePacked(ByteWriter& writer, std::span<std::uint32_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt64, values);
}

void writePacked(ByteWriter& writer, std::span<std::uint64_t> values)
{
    writePackedIntegers(writer, Tag::PackedInt64, values);
}

void writePacked(ByteWriter& writer, std::span<std::int8_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedInt8, values);
}

void writePacked(ByteWriter& writer, std::span<std::uint8_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedUInt8, values);
}

void writePacked(ByteWriter& writer, std::span<std::int16_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedInt16, values);
}

void writePacked(ByteWriter& writer, std::span<std::uint16_t> values)
{
    writePackedFixedWidth(writer, Tag::PackedUInt16, values);
}

void writeDouble(ByteWriter& writer, double value)
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

void writePrimitive(ByteWriter& writer, bool value)
{
    writer.writeTag(value ? Tag::True : Tag::False);
}

void writePrimitive(ByteWriter& writer, const std::string& value)
{
    writer.writeTag(Tag::String);
    writer.writeString(value);
}

void writePrimitive(ByteWriter& writer, double value) { writeDouble(writer, value); }

template <std::integral Integer>
void writePrimitive(ByteWriter& writer, Integer value)
{
    writer.writeTag(Tag::Int);
    writer.writeZigzag(static_cast<std::int64_t>(value));
}

ReadOutcome readPrimitive(const Document&, const ValueHeader& header, bool& out)
{
    out = header.tag == Tag::True;
    return ReadOutcome::Read;
}

ReadOutcome readPrimitive(const Document& document,
                          const ValueHeader& header,
                          std::string& out)
{
    auto reader = ByteReader {document.bytes(), header.payloadOffset};
    auto text = reader.readString(reader.readVarint());

    if (!reader.hasReadEverythingInBounds)
        return ReadOutcome::OutOfBounds;

    out = std::string {text};
    return ReadOutcome::Read;
}

template <typename Out>
bool convertDecodedNumber(const Number& number, Out& out)
{
    return std::visit([&out](auto value)
                      { return Miro::Detail::convertNumber(value, out); },
                      number);
}

template <typename Out>
ReadOutcome
    readPrimitive(const Document& document, const ValueHeader& header, Out& out)
{
    auto reader = ByteReader {document.bytes(), header.payloadOffset};
    auto number = readNumber(reader, header.encoding);

    if (!reader.hasReadEverythingInBounds)
        return ReadOutcome::OutOfBounds;

    if (!convertDecodedNumber(number, out))
        return ReadOutcome::NumberDidNotFit;

    return ReadOutcome::Read;
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
bool convertPackedElement(const Number& number, T& element)
{
    if constexpr (std::same_as<T, std::uint64_t>)
    {
        if (const auto* integer = std::get_if<std::int64_t>(&number))
        {
            element = std::bit_cast<std::uint64_t>(*integer);
            return true;
        }
    }

    return convertDecodedNumber(number, element);
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
        {
            return Detail::getFixedWidthEncoding<typename decltype(typeTag)::type>();
        },
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

void writePackedStructLayout(ByteWriter& writer, const PackedStructArrayRef& ref)
{
    writer.writeVarint(ref.fields.size());

    for (const auto& field: ref.fields)
    {
        writer.writeVarint(field.path.size());

        for (auto segment: field.path)
            writer.writeVarint(writer.getInternedKeyId(segment));

        auto type = findPackedFieldType(getFixedWidthEncoding(field.type));
        writer.writeByte(static_cast<std::uint8_t>(*type));
    }
}

void writePackedStructRecordsFieldByField(ByteWriter& writer,
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
                    using FieldNumber = typename decltype(typeTag)::type;
                    auto value = FieldNumber {};
                    std::memcpy(&value, element + field.offset, sizeof(value));
                    writer.writeFixedWidth(value);
                },
                field.type);
        }
    }
}

void writePackedStructArray(ByteWriter& writer, const PackedStructArrayRef& ref)
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

bool doesStoredFieldMatchElementField(const StoredPackedLayout& layout,
                                      const StoredPackedField& stored,
                                      const Miro::PackedStructField& wanted)
{
    return stored.offsetInRecord == wanted.offset
           && stored.encoding == getFixedWidthEncoding(wanted.type)
           && arePathsEqual(layout.getPath(stored), wanted.path);
}

bool doRecordsHaveElementShape(const StoredPackedLayout& layout,
                               const PackedStructArrayRef& ref)
{
    return isHostLittleEndian && layout.recordSize == ref.stride
           && layout.fields.size() == ref.fields.size();
}

// True when the stored records are byte for byte the C++ elements.
bool areRecordsLaidOutAsElements(const StoredPackedLayout& layout,
                                 const PackedStructArrayRef& ref)
{
    if (!doRecordsHaveElementShape(layout, ref))
        return false;

    for (auto i = std::size_t {0}; i < ref.fields.size(); ++i)
    {
        if (!doesStoredFieldMatchElementField(
                layout, layout.fields[i], ref.fields[i]))
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
    planPackedFieldCopy(const StoredPackedLayout& layout,
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

// False when the stored number does not fit the C++ field.
bool copyPackedField(std::span<const std::uint8_t> record,
                     std::byte* element,
                     const PackedFieldCopy& copy)
{
    if (copy.isRawByteCopy)
    {
        std::memcpy(element + copy.destinationOffset,
                    record.data() + copy.sourceOffset,
                    getByteWidth(copy.destinationType));
        return true;
    }

    auto reader = ByteReader {record, copy.sourceOffset};
    auto number = readNumber(reader, copy.sourceEncoding);

    return std::visit(
        [element, &copy, &number](auto typeTag)
        {
            using FieldNumber = typename decltype(typeTag)::type;
            auto* destination = element + copy.destinationOffset;
            auto value = FieldNumber {};
            std::memcpy(&value, destination, sizeof(value));
            auto hasFit = convertPackedElement(number, value);
            std::memcpy(destination, &value, sizeof(value));
            return hasFit;
        },
        copy.destinationType);
}

std::vector<PackedFieldCopy> planPackedFieldCopies(const StoredPackedLayout& layout,
                                                   const PackedStructArrayRef& ref)
{
    auto copies = std::vector<PackedFieldCopy> {};

    for (const auto& wanted: ref.fields)
    {
        if (auto copy = planPackedFieldCopy(layout, wanted))
            copies.push_back(*copy);
    }

    return copies;
}

void readPackedRecordsFieldByField(const StoredPackedLayout& layout,
                                   std::span<const std::uint8_t> records,
                                   const PackedStructArrayRef& ref,
                                   const RefusedElementReporter& reportRefused)
{
    auto copies = planPackedFieldCopies(layout, ref);
    auto count = records.size() / layout.recordSize;

    for (auto i = std::size_t {0}; i < count; ++i)
    {
        auto record = records.subspan(i * layout.recordSize, layout.recordSize);
        auto* element = ref.elementBytes.data() + i * ref.stride;
        auto haveAllFit =
            std::ranges::all_of(copies,
                                [&](const PackedFieldCopy& copy)
                                { return copyPackedField(record, element, copy); });

        if (!haveAllFit)
            reportRefused(i);
    }
}

template <typename T>
void convertPackedElements(ByteReader& reader,
                           Encoding encoding,
                           std::span<T> dest,
                           const RefusedElementReporter& reportRefused)
{
    for (auto i = std::size_t {0}; i < dest.size(); ++i)
    {
        if (!convertPackedElement(readNumber(reader, encoding), dest[i]))
            reportRefused(i);
    }
}

template <typename T>
void readPackedElements(ByteReader& reader,
                        Encoding encoding,
                        std::span<T> dest,
                        const RefusedElementReporter& reportRefused)
{
    if (!canCopyRawBytes<T>(encoding))
    {
        convertPackedElements(reader, encoding, dest, reportRefused);
        return;
    }

    auto bytes = reader.readBytes(dest.size_bytes());

    if (reader.hasReadEverythingInBounds)
        decodeLittleEndianElements(bytes, dest);
}

constexpr auto maxKeptFieldProblems = std::size_t {64};

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

Writer::Writer(ByteWriter& writerToUse, Options optsToUse)
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

    if (slot.omittableSlotStartOffset != npos)
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
    if (slot.countFieldOffset != npos)
        writer.patchU32(slot.countFieldOffset,
                        static_cast<std::uint32_t>(slot.elementCount));

    if (slot.sizeFieldOffset != npos)
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
        slot.sizeFieldOffset = npos;
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
    if (opts.shape == Shape::Raw && slot.countFieldOffset == npos)
    {
        writer.truncateTo(slot.slotStartOffset);
        writeArrayHeader();
    }

    slot.elementCount = std::max(slot.elementCount, newSize);
}

Reader::Reader(const ReadStart& start, Options optsToUse, LoadResult& resultToUse)
    : Reflector(std::move(optsToUse))
    , document(start.document)
    , header(start.header)
    , result(resultToUse)
    , resumeOffset(start.header.payloadOffset)
    , packedRecord(start.packedRecord)
{
}

Reader::Reader(const Reader& parentToUse,
               StepFromParent stepToUse,
               const ValueHeader& headerToUse,
               Options optsToUse)
    : Reflector(std::move(optsToUse))
    , document(parentToUse.document)
    , header(headerToUse)
    , result(parentToUse.result)
    , parent(parentToUse)
    , depth(parentToUse.depth + 1)
    , stepFromParent(stepToUse)
    , resumeOffset(headerToUse.payloadOffset)
{
    if (depth > maxNestingDepth)
        throw NestingTooDeep {};
}

bool Reader::isStoredAs(ValueKind expectedKind) const
{
    return !header.valid || kind() == expectedKind;
}

void Reader::reportTypeMismatch() const
{
    reportFieldProblem(FieldProblem::Kind::WrongType);
}

void Reader::reportFieldProblem(FieldProblem::Kind problemKind) const
{
    ++result.fieldProblemCount;

    if (result.fieldProblems.size() < maxKeptFieldProblems)
        result.fieldProblems.push_back({buildFieldPath(), problemKind});
}

void Reader::reportFieldProblemAtIndex(FieldProblem::Kind problemKind,
                                       std::size_t index) const
{
    ++result.fieldProblemCount;

    if (result.fieldProblems.size() >= maxKeptFieldProblems)
        return;

    auto path = buildFieldPath();
    appendFieldPathStep(path, index);
    result.fieldProblems.push_back({std::move(path), problemKind});
}

void Reader::reportDamage() const
{
    if (result.status == LoadStatus::Loaded)
        result.status = LoadStatus::Damaged;
}

void Reader::reportDamageIfStopped(const ObjectEntryCursor& cursor) const
{
    if (cursor.hasStoppedAtDamage())
        reportDamage();
}

void Reader::countMissingField(const Options& childOpts) const
{
    if (!childOpts.omittable)
        ++result.missingFieldCount;
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
    packedResumeField = 0;
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
    return hasCategory(ValueCategory::Number) && isIntegerEncoding(header.encoding);
}

void Reader::reportReadOutcome(ReadOutcome outcome) const
{
    if (outcome == ReadOutcome::OutOfBounds)
        reportDamage();
    else if (outcome == ReadOutcome::NumberDidNotFit)
        reportFieldProblem(FieldProblem::Kind::NumberDidNotFit);
}

void Reader::visit(PrimitiveRef ref)
{
    if (!header.valid)
        return;

    std::visit(
        [this](auto* value)
        {
            if (isStoredAs(getExpectedValueKind(*value)))
                reportReadOutcome(readPrimitive(document, header, *value));
            else
                reportTypeMismatch();
        },
        ref.data);
}

RefusedElementReporter Reader::makeRefusedElementReporter() const
{
    return [this](std::size_t index)
    { reportFieldProblemAtIndex(FieldProblem::Kind::NumberDidNotFit, index); };
}

void Reader::countMissingElements(std::size_t wantedCount,
                                  std::size_t storedCount) const
{
    if (storedCount < wantedCount)
        result.missingFieldCount += wantedCount - storedCount;
}

bool Reader::visitPacked(PackedArrayRef ref)
{
    if (!hasCategory(ValueCategory::Array) || header.encoding == Encoding::Tagged)
        return false;

    std::visit(
        [this](auto dest)
        {
            auto reader = makeElementReader(document, header, header.payloadOffset);
            auto count = std::min(dest.size(), header.elementCount);
            countMissingElements(dest.size(), count);
            readPackedElements(reader,
                               header.encoding,
                               dest.first(count),
                               makeRefusedElementReporter());

            if (!reader.hasReadEverythingInBounds)
                reportDamage();
        },
        ref.data);

    return true;
}

bool Reader::readStoredPackedLayoutOnce()
{
    if (!hasReadStoredPackedLayout)
    {
        hasReadStoredPackedLayout = true;
        isStoredPackedLayoutValid =
            readStoredPackedLayout(document, header, storedPackedLayout);

        if (!isStoredPackedLayoutValid)
            reportDamage();
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
    countMissingElements(ref.getElementCount(), count);

    if (count == 0)
        return true;

    auto records =
        document.bytes().subspan(layout.recordsOffset, count * layout.recordSize);

    if (areRecordsLaidOutAsElements(layout, ref))
        std::memcpy(ref.elementBytes.data(), records.data(), records.size());
    else
        readPackedRecordsFieldByField(
            layout, records, ref, makeRefusedElementReporter());

    return true;
}

std::size_t Reader::arraySize() const
{
    if (!isStoredAs(ValueKind::Array))
        reportTypeMismatch();

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
        reportTypeMismatch();

    if (!hasCategory(ValueCategory::Object))
        return keys;

    auto cursor = ObjectEntryCursor {
        document, header, header.payloadOffset, header.payloadEndOffset};

    while (auto entry = cursor.readNextEntry())
        keys.add(std::string {entry->key});

    reportDamageIfStopped(cursor);
    return keys;
}

std::optional<ObjectEntry> Reader::findEntry(std::string_view key)
{
    // Fields usually arrive in written order: resume from the cursor,
    // then wrap around once for reordered or missing keys.
    auto resumeOffsetBeforeSearch = resumeOffset;
    auto fromResume = ObjectEntryCursor {
        document, header, resumeOffsetBeforeSearch, header.payloadEndOffset};
    auto entry = fromResume.findEntryWithKey(key);
    reportDamageIfStopped(fromResume);

    if (!entry)
    {
        auto fromStart = ObjectEntryCursor {
            document, header, header.payloadOffset, resumeOffsetBeforeSearch};
        entry = fromStart.findEntryWithKey(key);
        reportDamageIfStopped(fromStart);
    }

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

Reflector& Reader::spawnStoredChild(StepFromParent step,
                                    const ValueHeader& childHeader,
                                    Options&& childOpts)
{
    if (!childHeader.valid)
        reportDamage();

    return spawnChild(step, childHeader, std::move(childOpts));
}

Reflector& Reader::spawnPackedRecordChild(std::size_t index, Options&& childOpts)
{
    if (!readStoredPackedLayoutOnce())
        return spawnMissingChild(index, std::move(childOpts));

    if (index >= storedPackedLayout.recordCount)
    {
        countMissingField(childOpts);
        return spawnMissingChild(index, std::move(childOpts));
    }

    auto recordOffset =
        storedPackedLayout.recordsOffset + index * storedPackedLayout.recordSize;
    return spawnChild(index,
                      makePackedRecordHeader(recordOffset),
                      std::move(childOpts),
                      PackedRecordSlot {.layout = storedPackedLayout,
                                        .recordOffset = recordOffset});
}

std::optional<std::size_t> Reader::findPackedFieldAtThisDepth(std::string_view key)
{
    auto fieldIndex = findPackedFieldAtDepth(
        packedRecord->layout.get(), packedRecord->path, key, packedResumeField);

    if (fieldIndex)
        packedResumeField = *fieldIndex + 1;

    return fieldIndex;
}

void Reader::appendPackedRecordKeys(Vector<std::string>& keys) const
{
    for (auto key:
         listPackedKeysAtDepth(packedRecord->layout.get(), packedRecord->path))
        keys.add(std::string {key});
}

Reflector& Reader::spawnPackedFieldChild(std::string_view key, Options&& childOpts)
{
    auto fieldIndex = findPackedFieldAtThisDepth(key);

    if (!fieldIndex)
    {
        countMissingField(childOpts);
        return spawnMissingChild(key, std::move(childOpts));
    }

    const auto& layout = packedRecord->layout.get();
    const auto& field = layout.fields[*fieldIndex];
    auto childDepth = packedRecord->path.depth + 1;

    if (layout.getPath(field).size() == childDepth)
        return spawnChild(key,
                          makePackedElementHeader(field.encoding,
                                                  packedRecord->recordOffset
                                                      + field.offsetInRecord),
                          std::move(childOpts));

    return spawnChild(
        key,
        makePackedRecordHeader(packedRecord->recordOffset),
        std::move(childOpts),
        PackedRecordSlot {.layout = layout,
                          .recordOffset = packedRecord->recordOffset,
                          .path = {.fieldIndex = *fieldIndex, .depth = childDepth}});
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
        return spawnStoredChild(index,
                                parseValueHeader(document.bytes(), elementOffset),
                                std::move(childOpts));

    return spawnChild(index,
                      makePackedElementHeader(header.encoding, elementOffset),
                      std::move(childOpts));
}

Reflector& Reader::atKey(std::string_view key, Options childOpts)
{
    if (packedRecord)
        return spawnPackedFieldChild(key, std::move(childOpts));

    if (!isStoredAs(ValueKind::Object) && !hasReportedObjectMismatch)
    {
        reportTypeMismatch();
        hasReportedObjectMismatch = true;
    }

    if (!hasCategory(ValueCategory::Object))
        return spawnMissingChild(key, std::move(childOpts));

    auto entry = findEntry(key);

    if (!entry)
    {
        countMissingField(childOpts);
        return spawnMissingChild(key, std::move(childOpts));
    }

    return spawnStoredChild(key,
                            parseValueHeader(document.bytes(), entry->valueOffset),
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
    if (!hasCategory(ValueCategory::Array))
        return spawnMissingChild(index, std::move(childOpts));

    if (index >= header.elementCount)
    {
        countMissingField(childOpts);
        return spawnMissingChild(index, std::move(childOpts));
    }

    if (header.encoding == Encoding::PackedStruct)
        return spawnPackedRecordChild(index, std::move(childOpts));

    rewindResumeOffsetIfPast(index);

    auto reader = makeElementReader(document, header, resumeOffset);
    auto skipped = skipElements(reader, header.encoding, index - resumeElementIndex);

    if (!skipped || reader.readOffsetInBytes >= header.payloadEndOffset)
    {
        reportDamage();
        return spawnMissingChild(index, std::move(childOpts));
    }

    auto elementOffset = reader.readOffsetInBytes;
    resumeOffset = elementOffset;
    resumeElementIndex = index;

    if (skipElements(reader, header.encoding, 1))
    {
        resumeOffset = reader.readOffsetInBytes;
        resumeElementIndex = index + 1;
    }

    return spawnElementChild(index, elementOffset, std::move(childOpts));
}

} // namespace Calder::Detail
