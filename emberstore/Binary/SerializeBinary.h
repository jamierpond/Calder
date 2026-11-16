#pragma once

#include "BinaryReflector.h"
#include <Miro/Reflection/ReflectContainers.h>
#include <Miro/Reflection/ReflectDispatch.h>

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// Binary counterparts of toJSON / fromJSON. Loads never throw: missing
// keys and mismatched types keep prior values. fromBinary also reads
// journals ("MIBJ"), replaying their frames into std::map<std::string, V>.

namespace emberstore
{
namespace Detail
{

template <typename T>
struct IsReplayableJournalMap : std::false_type
{
};

template <typename V>
struct IsReplayableJournalMap<std::map<std::string, V>> : std::true_type
{
    using Value = V;
};

template <typename T>
void loadValidDocument(T& value,
                       const Binary::Document& document,
                       const CustomOptions& custom,
                       BinaryLoadReport& report)
{
    auto ref = BinaryReaderReflector {
        document,
        Binary::Detail::parseValueHeader(document.bytes(),
                                         document.getRootValueOffset()),
        Miro::Detail::topLevelOptions<T>(Mode::Load, /*schema=*/false, custom),
        report};
    Miro::Detail::reflectValue(ref, value);
}

template <typename T>
void loadBinaryDocument(T& value,
                        const Binary::Document& document,
                        const CustomOptions& custom,
                        BinaryLoadReport& report)
{
    if (document.isValid())
        loadValidDocument(value, document, custom, report);
}

template <typename Map>
void replayJournalFrame(Map& value,
                        const Binary::JournalFrame& frame,
                        const Binary::Document& baseDocument,
                        const CustomOptions& custom,
                        BinaryLoadReport& report)
{
    auto key = std::string {frame.key};

    if (frame.kind == Binary::FrameKind::Remove)
    {
        value.erase(key);
        return;
    }

    auto frameDocument = Binary::Document {frame.value, baseDocument};

    if (!frameDocument.isValid())
    {
        ++report.skippedJournalFrameCount;
        return;
    }

    auto element = typename IsReplayableJournalMap<Map>::Value {};
    loadValidDocument(element, frameDocument, custom, report);
    value[key] = std::move(element);
}

// Types other than std::map<std::string, V> load the base alone.
template <typename T>
void loadBinaryJournal(T& value,
                       std::span<const std::uint8_t> data,
                       const CustomOptions& custom,
                       BinaryLoadReport& report)
{
    const auto journalInfo = Binary::inspectJournal(data);

    if (!journalInfo.valid)
        return;

    report.skippedJournalFrameCount = journalInfo.skippedFrameCount;
    report.isJournalTruncated = journalInfo.truncated;
    auto baseDocument =
        Binary::Document {data.subspan(Binary::headerSize, journalInfo.baseSize)};
    loadBinaryDocument(value, baseDocument, custom, report);

    if constexpr (IsReplayableJournalMap<T>::value)
    {
        for (const auto& frame: Binary::readJournalFrames(data))
            replayJournalFrame(value, frame, baseDocument, custom, report);
    }
}

} // namespace Detail

namespace Detail
{

template <typename T>
void writeReflectedValue(Binary::Writer& writer,
                         const T& value,
                         CustomOptions custom)
{
    auto ref =
        BinaryWriterReflector {writer,
                               Miro::Detail::topLevelOptions<T>(
                                   Mode::Save, /*schema=*/false, std::move(custom))};
    Miro::Detail::reflectValue(ref, const_cast<T&>(value));
}

} // namespace Detail

template <typename T>
Binary::Buffer toBinary(const T& value, CustomOptions custom = {})
{
    auto writer = Binary::Writer {};
    Detail::writeReflectedValue(writer, value, std::move(custom));
    return writer.finishWithKeyTable();
}

// Never throws: on any fault `value` keeps what was loaded before it.
// The report lists what could not be loaded as written.
template <typename T>
BinaryLoadReport fromBinary(T& value,
                            std::span<const std::uint8_t> data,
                            CustomOptions custom = {})
{
    auto report = BinaryLoadReport {};

    try
    {
        if (Binary::isJournal(data))
            Detail::loadBinaryJournal(value, data, custom, report);
        else
            Detail::loadBinaryDocument(
                value, Binary::Document {data}, custom, report);
    }
    catch (...)
    {
    }

    return report;
}

template <typename T>
T createFromBinary(std::span<const std::uint8_t> data, CustomOptions custom = {})
{
    auto value = T {};
    fromBinary(value, data, std::move(custom));
    return value;
}

// A base document with no frames yet; append binaryJournalSet /
// binaryJournalRemove frames to it. Binary::readJournalKeyTable gives the
// key table binaryJournalSet needs.
template <typename T>
Binary::Buffer toBinaryJournal(const T& value, CustomOptions custom = {})
{
    return Binary::makeJournal(toBinary(value, std::move(custom)));
}

// nullopt if `value` uses a field name the journal's key table lacks: the
// journal then needs a new base that has it (toBinaryJournal of the whole map).
template <typename V>
std::optional<Binary::Buffer>
    binaryJournalSet(std::string_view key,
                     const V& value,
                     Binary::InternedKeyTable& journalKeyTable,
                     CustomOptions custom = {})
{
    auto writer = Binary::Writer {journalKeyTable};
    Detail::writeReflectedValue(writer, value, std::move(custom));
    auto frameValue = writer.finishJournalFrameValue();

    if (!frameValue)
        return std::nullopt;

    return Binary::makeFrame(Binary::FrameKind::Set, key, *frameValue);
}

inline Binary::Buffer binaryJournalRemove(std::string_view key)
{
    return Binary::makeFrame(Binary::FrameKind::Remove, key, {});
}

} // namespace emberstore
