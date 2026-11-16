#include "../Calder.h"

#include <Miro/Reflection/NumericConvert.h>

#include <bit>

namespace Calder
{
namespace Detail
{
namespace
{

const Document& getEmptyDocument()
{
    static const auto emptyDocument = Document {};
    return emptyDocument;
}

bool isPackedRecord(const ViewPosition& position)
{
    return position.elementEncoding == Encoding::PackedStruct;
}

// A stored UInt64 reads back as its int64 bit pattern.
bool isStoredAsUnsigned64(const ViewPosition& position)
{
    return position.elementEncoding == Encoding::UInt64;
}

std::optional<Number> readViewNumber(const Document& document,
                                     const ViewPosition& position)
{
    auto header = readViewHeader(document, position);

    if (!header.valid || header.category != ValueCategory::Number)
        return std::nullopt;

    auto reader = ByteReader {document.bytes(), header.payloadOffset};
    auto number = readNumber(reader, header.encoding);

    if (!reader.hasReadEverythingInBounds)
        return std::nullopt;

    return number;
}

std::optional<StoredPackedLayout> readLayoutOfArrayAt(const Document& document,
                                                      std::size_t valueOffset)
{
    auto layout = StoredPackedLayout {};
    auto header = parseValueHeader(document.bytes(), valueOffset);

    if (!readStoredPackedLayout(document, header, layout))
        return std::nullopt;

    return layout;
}

View findPackedRecordField(const Document& document,
                           const ViewPosition& record,
                           std::string_view key)
{
    auto layout = readLayoutOfArrayAt(document, record.valueOffset);

    if (!layout)
        return {};

    auto fieldIndex = findPackedFieldAtDepth(
        *layout, record.packedPrefixField, record.packedPathDepth, key);

    if (!fieldIndex)
        return {};

    const auto& field = layout->fields[*fieldIndex];
    auto childDepth = record.packedPathDepth + 1;

    if (layout->getPath(field).size() > childDepth)
    {
        auto child = record;
        child.packedPathDepth = childDepth;
        child.packedPrefixField = *fieldIndex;
        return ViewAccess::makeView(document, child);
    }

    auto recordOffset =
        layout->recordsOffset + record.packedRecordIndex * layout->recordSize;
    return ViewAccess::makeView(document,
                                {.valueOffset = recordOffset + field.offsetInRecord,
                                 .elementEncoding = field.encoding});
}

std::vector<std::string_view> listPackedRecordKeys(const Document& document,
                                                   const ViewPosition& record)
{
    auto layout = readLayoutOfArrayAt(document, record.valueOffset);

    if (!layout)
        return {};

    return listPackedKeysAtDepth(
        *layout, record.packedPrefixField, record.packedPathDepth);
}

View findPackedRecord(const Document& document,
                      const ValueHeader& array,
                      std::size_t index)
{
    auto layout = readLayoutOfArrayAt(document, array.valueOffset);

    if (!layout || index >= layout->recordCount)
        return {};

    return ViewAccess::makeView(document,
                                {.valueOffset = array.valueOffset,
                                 .elementEncoding = Encoding::PackedStruct,
                                 .packedRecordIndex = index});
}

std::vector<std::string_view> listObjectKeys(const Document& document,
                                             const ValueHeader& header)
{
    auto keys = std::vector<std::string_view> {};
    auto cursor = ObjectEntryCursor {
        document, header, header.payloadOffset, header.payloadEndOffset};

    while (auto entry = cursor.readNextEntry())
        keys.push_back(entry->key);

    return keys;
}

} // namespace

View ViewAccess::makeView(const Document& document, const ViewPosition& position)
{
    auto view = View {};
    view.document = document;
    view.position = position;
    return view;
}

const Document& ViewAccess::getDocument(const View& view)
{
    return view.document.get();
}

const ViewPosition& ViewAccess::getPosition(const View& view)
{
    return view.position;
}

ValueHeader readViewHeader(const Document& document, const ViewPosition& position)
{
    if (position.elementEncoding == Encoding::Tagged)
        return parseValueHeader(document.bytes(), position.valueOffset);

    if (isPackedRecord(position))
        return makePackedRecordHeader(position.valueOffset);

    return makePackedElementHeader(position.elementEncoding, position.valueOffset);
}

} // namespace Detail

using Detail::Encoding;
using Detail::ValueCategory;

View::View()
    : document(Detail::getEmptyDocument())
{
}

bool View::isValid() const
{
    return Detail::readViewHeader(document, position).valid;
}

bool View::isNull() const
{
    auto header = Detail::readViewHeader(document, position);
    return header.valid && header.category == ValueCategory::Null;
}

bool View::isBool() const
{
    auto header = Detail::readViewHeader(document, position);
    return header.valid && header.category == ValueCategory::Bool;
}

bool View::isNumber() const
{
    auto header = Detail::readViewHeader(document, position);
    return header.valid && header.category == ValueCategory::Number;
}

bool View::isInteger() const
{
    auto header = Detail::readViewHeader(document, position);
    return header.valid && header.category == ValueCategory::Number
           && Detail::isIntegerEncoding(header.encoding);
}

bool View::isString() const
{
    auto header = Detail::readViewHeader(document, position);
    return header.valid && header.category == ValueCategory::String;
}

bool View::isObject() const
{
    auto header = Detail::readViewHeader(document, position);
    return header.valid && header.category == ValueCategory::Object;
}

bool View::isArray() const
{
    auto header = Detail::readViewHeader(document, position);
    return header.valid && header.category == ValueCategory::Array;
}

View View::operator[](std::string_view key) const
{
    if (Detail::isPackedRecord(position))
        return Detail::findPackedRecordField(document, position, key);

    if (!isObject())
        return {};

    auto header = Detail::readViewHeader(document, position);
    auto cursor = Detail::ObjectEntryCursor {
        document, header, header.payloadOffset, header.payloadEndOffset};
    auto entry = cursor.findEntryWithKey(key);

    if (!entry)
        return {};

    return Detail::ViewAccess::makeView(document,
                                        {.valueOffset = entry->valueOffset});
}

View View::operator[](std::size_t index) const
{
    auto header = Detail::readViewHeader(document, position);

    if (!isArray() || index >= header.elementCount)
        return {};

    if (header.encoding == Encoding::PackedStruct)
        return Detail::findPackedRecord(document, header, index);

    auto reader = Detail::ByteReader {document.get().bytes(), header.payloadOffset};

    if (!Detail::skipElements(reader, header.encoding, index)
        || reader.readOffsetInBytes >= header.payloadEndOffset)
        return {};

    return Detail::ViewAccess::makeView(document,
                                        {.valueOffset = reader.readOffsetInBytes,
                                         .elementEncoding = header.encoding});
}

std::size_t View::getSize() const
{
    if (isArray())
        return Detail::readViewHeader(document, position).elementCount;

    return getKeys().size();
}

std::vector<std::string_view> View::getKeys() const
{
    if (Detail::isPackedRecord(position))
        return Detail::listPackedRecordKeys(document, position);

    if (!isObject())
        return {};

    return Detail::listObjectKeys(document,
                                  Detail::readViewHeader(document, position));
}

bool View::asBool(bool fallback) const
{
    auto header = Detail::readViewHeader(document, position);

    if (!header.valid || header.category != ValueCategory::Bool)
        return fallback;

    return header.tag == Detail::Tag::True;
}

std::int64_t View::asInteger(std::int64_t fallback) const
{
    auto number = Detail::readViewNumber(document, position);

    if (!number)
        return fallback;

    if (Detail::isStoredAsUnsigned64(position))
    {
        auto storedBits = std::get<std::int64_t>(*number);
        return storedBits < 0 ? fallback : storedBits;
    }

    auto integer = fallback;
    std::visit([&integer](auto value)
               { Miro::Detail::convertNumber(value, integer); },
               *number);
    return integer;
}

double View::asNumber(double fallback) const
{
    auto number = Detail::readViewNumber(document, position);

    if (!number)
        return fallback;

    if (Detail::isStoredAsUnsigned64(position))
        return static_cast<double>(
            std::bit_cast<std::uint64_t>(std::get<std::int64_t>(*number)));

    return std::visit([](auto value) { return static_cast<double>(value); },
                      *number);
}

std::string_view View::asString(std::string_view fallback) const
{
    auto header = Detail::readViewHeader(document, position);

    if (!header.valid || header.category != ValueCategory::String)
        return fallback;

    auto reader = Detail::ByteReader {document.get().bytes(), header.payloadOffset};
    auto text = reader.readString(reader.readVarint());

    if (!reader.hasReadEverythingInBounds)
        return fallback;

    return text;
}

} // namespace Calder
