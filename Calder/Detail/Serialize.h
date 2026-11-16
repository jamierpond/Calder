#pragma once

#include "Reflectors.h"
#include "ViewAccess.h"
#include <Miro/Reflection/ReflectContainers.h>
#include <Miro/Reflection/ReflectDispatch.h>

#include <map>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The reflection walks behind toBinary / fromBinary. Loads never throw:
// missing keys and mismatched types keep prior values. A journal ("CLDJ")
// replays its frames into a std::map<std::string, V>.

namespace Calder::Detail
{

template <typename T>
struct IsReplayableJournalMap : std::false_type
{
};

template <typename V, typename Compare, typename Allocator>
struct IsReplayableJournalMap<std::map<std::string, V, Compare, Allocator>>
    : std::true_type
{
    using Value = V;
};

std::span<const std::uint8_t> viewAsBytes(const IsByteRange auto& bytes)
{
    const auto* first =
        reinterpret_cast<const std::uint8_t*>(std::ranges::data(bytes));
    return {first, std::ranges::size(bytes)};
}

template <typename Load>
LoadResult runGuardedLoad(Load&& load)
{
    auto result = LoadResult {};

    try
    {
        load(result);
    }
    catch (const std::bad_alloc&)
    {
        result.status = LoadStatus::OutOfMemory;
    }
    catch (...)
    {
        result.status = LoadStatus::Damaged;
    }

    return result;
}

template <typename T>
void reflectFrom(T& value, const ReadStart& start, LoadResult& result)
{
    auto ref = Reader {start, Miro::Detail::topLevelOptions<T>(Mode::Load), result};
    Miro::Detail::reflectValue(ref, value);
}

template <typename T>
void loadDocument(T& value, const Document& document, LoadResult& result)
{
    if (!document.isValid())
    {
        result.status = LoadStatus::Damaged;
        return;
    }

    auto rootHeader =
        parseValueHeader(document.bytes(), document.getRootValueOffset());
    reflectFrom(value, {.document = document, .header = rootHeader}, result);
}

template <typename Map>
void replayJournalFrame(Map& value,
                        const JournalFrame& frame,
                        const Document& baseDocument,
                        LoadResult& result)
{
    auto key = std::string {frame.key};

    if (frame.kind == FrameKind::Remove)
    {
        value.erase(key);
        return;
    }

    auto frameDocument = Document {frame.value, baseDocument};

    if (!frameDocument.isValid())
    {
        ++result.lostJournalFrameCount;
        return;
    }

    auto element = typename IsReplayableJournalMap<Map>::Value {};
    loadDocument(element, frameDocument, result);
    value[key] = std::move(element);
}

// A type other than a map with string keys loads the base alone, and its
// frames count as lost.
template <typename T>
void loadJournal(T& value, std::span<const std::uint8_t> data, LoadResult& result)
{
    const auto journalInfo = inspectJournal(data);

    if (!journalInfo.valid)
    {
        result.status = LoadStatus::Damaged;
        return;
    }

    auto baseDocument = Document {data.subspan(headerSize, journalInfo.baseSize)};

    if (!baseDocument.isValid())
    {
        result.status = LoadStatus::Damaged;
        return;
    }

    auto tornFrameCount = std::size_t {journalInfo.truncated ? 1U : 0U};
    result.lostJournalFrameCount = journalInfo.skippedFrameCount + tornFrameCount;
    loadDocument(value, baseDocument, result);
    auto frames = readJournalFrames(data);

    if constexpr (IsReplayableJournalMap<T>::value)
    {
        for (const auto& frame: frames)
            replayJournalFrame(value, frame, baseDocument, result);
    }
    else
    {
        result.lostJournalFrameCount += frames.size();
    }
}

template <typename T>
void loadBytes(T& value, std::span<const std::uint8_t> data, LoadResult& result)
{
    if (isJournal(data))
        loadJournal(value, data, result);
    else if (isDocument(data))
        loadDocument(value, Document {data}, result);
    else
        result.status = LoadStatus::NotCalderData;
}

template <typename T>
void loadPackedRecordView(T& value,
                          const Document& document,
                          const ViewPosition& position,
                          LoadResult& result)
{
    auto layout = StoredPackedLayout {};
    auto arrayHeader = parseValueHeader(document.bytes(), position.valueOffset);

    if (!readStoredPackedLayout(document, arrayHeader, layout))
    {
        result.status = LoadStatus::Damaged;
        return;
    }

    auto recordOffset =
        layout.recordsOffset + position.packedRecordIndex * layout.recordSize;
    auto slot = PackedRecordSlot {
        .layout = layout, .recordOffset = recordOffset, .path = position.packedPath};
    reflectFrom(value,
                {.document = document,
                 .header = makePackedRecordHeader(recordOffset),
                 .packedRecord = slot},
                result);
}

template <typename T>
void loadView(T& value, const View& view, LoadResult& result)
{
    if (!view.isValid())
    {
        result.status = LoadStatus::ValueMissing;
        return;
    }

    const auto& document = ViewAccess::getDocument(view);
    const auto& position = ViewAccess::getPosition(view);

    if (position.elementEncoding == Encoding::PackedStruct)
        loadPackedRecordView(value, document, position, result);
    else
        reflectFrom(
            value,
            {.document = document, .header = readViewHeader(document, position)},
            result);
}

template <typename T>
void writeReflectedValue(ByteWriter& writer, const T& value)
{
    auto ref = Writer {writer, Miro::Detail::topLevelOptions<T>(Mode::Save)};
    Miro::Detail::reflectValue(ref, const_cast<T&>(value));
}

template <typename T>
Buffer writeDocument(const T& value)
{
    auto writer = ByteWriter {};
    writeReflectedValue(writer, value);
    return writer.finishWithKeyTable();
}

// A set frame, or the names `value` uses that the journal's key table lacks:
// a frame never adds a name, so the base has to gain them first.
struct JournalSetFrame
{
    bool hasFrame() const { return keysMissingFromJournal.empty(); }

    Buffer frame;
    std::vector<std::string> keysMissingFromJournal;
};

template <typename V>
JournalSetFrame makeJournalSetFrame(std::string_view key,
                                    const V& value,
                                    InternedKeyTable& journalKeyTable)
{
    auto writer = ByteWriter {journalKeyTable};
    writeReflectedValue(writer, value);
    auto frameValue = writer.finishJournalFrameValue();

    if (!frameValue)
        return {.keysMissingFromJournal = writer.getKeysMissingFromJournal()};

    return {.frame = makeFrame(FrameKind::Set, key, *frameValue)};
}

inline Buffer makeJournalRemoveFrame(std::string_view key)
{
    return makeFrame(FrameKind::Remove, key, {});
}

} // namespace Calder::Detail
