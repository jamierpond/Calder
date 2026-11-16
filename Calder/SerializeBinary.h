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
// journals ("CLDJ"), replaying their frames into std::map<std::string, V>.

namespace Calder
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
                       const Calder::Document& document,
                       const CustomOptions& custom,
                       BinaryLoadReport& report)
{
    auto ref = Reader {
        document,
        Calder::Detail::parseValueHeader(document.bytes(),
                                         document.getRootValueOffset()),
        Miro::Detail::topLevelOptions<T>(Mode::Load, /*schema=*/false, custom),
        report};
    Miro::Detail::reflectValue(ref, value);
}

template <typename T>
void loadBinaryDocument(T& value,
                        const Calder::Document& document,
                        const CustomOptions& custom,
                        BinaryLoadReport& report)
{
    if (document.isValid())
        loadValidDocument(value, document, custom, report);
}

template <typename Map>
void replayJournalFrame(Map& value,
                        const Calder::JournalFrame& frame,
                        const Calder::Document& baseDocument,
                        const CustomOptions& custom,
                        BinaryLoadReport& report)
{
    auto key = std::string {frame.key};

    if (frame.kind == Calder::FrameKind::Remove)
    {
        value.erase(key);
        return;
    }

    auto frameDocument = Calder::Document {frame.value, baseDocument};

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
    const auto journalInfo = Calder::inspectJournal(data);

    if (!journalInfo.valid)
        return;

    report.skippedJournalFrameCount = journalInfo.skippedFrameCount;
    report.isJournalTruncated = journalInfo.truncated;
    auto baseDocument =
        Calder::Document {data.subspan(Calder::headerSize, journalInfo.baseSize)};
    loadBinaryDocument(value, baseDocument, custom, report);

    if constexpr (IsReplayableJournalMap<T>::value)
    {
        for (const auto& frame: Calder::readJournalFrames(data))
            replayJournalFrame(value, frame, baseDocument, custom, report);
    }
}

} // namespace Detail

namespace Detail
{

template <typename T>
void writeReflectedValue(Detail::ByteWriter& writer,
                         const T& value,
                         CustomOptions custom)
{
    auto ref = Writer {writer,
                       Miro::Detail::topLevelOptions<T>(
                           Mode::Save, /*schema=*/false, std::move(custom))};
    Miro::Detail::reflectValue(ref, const_cast<T&>(value));
}

} // namespace Detail

template <typename T>
Calder::Buffer toBinary(const T& value, CustomOptions custom = {})
{
    auto writer = Detail::ByteWriter {};
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
        if (Calder::isJournal(data))
            Detail::loadBinaryJournal(value, data, custom, report);
        else
            Detail::loadBinaryDocument(
                value, Calder::Document {data}, custom, report);
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
// binaryJournalRemove frames to it. Calder::readJournalKeyTable gives the
// key table binaryJournalSet needs.
template <typename T>
Calder::Buffer toBinaryJournal(const T& value, CustomOptions custom = {})
{
    return Calder::makeJournal(toBinary(value, std::move(custom)));
}

// nullopt if `value` uses a field name the journal's key table lacks: the
// journal then needs a new base that has it (toBinaryJournal of the whole map).
template <typename V>
std::optional<Calder::Buffer>
    binaryJournalSet(std::string_view key,
                     const V& value,
                     Calder::InternedKeyTable& journalKeyTable,
                     CustomOptions custom = {})
{
    auto writer = Detail::ByteWriter {journalKeyTable};
    Detail::writeReflectedValue(writer, value, std::move(custom));
    auto frameValue = writer.finishJournalFrameValue();

    if (!frameValue)
        return std::nullopt;

    return Calder::makeFrame(Calder::FrameKind::Set, key, *frameValue);
}

inline Calder::Buffer binaryJournalRemove(std::string_view key)
{
    return Calder::makeFrame(Calder::FrameKind::Remove, key, {});
}

} // namespace Calder
