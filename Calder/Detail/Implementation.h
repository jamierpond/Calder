#pragma once

#include "File.h"
#include "JournalWriter.h"
#include "Serialize.h"
#include <Miro/Reflection/ReflectEnum.h>
#include <Miro/Reflection/ReflectMacro.h>
#include <Miro/Reflection/ReflectPolymorphic.h>

namespace Calder
{

inline bool LoadResult::hasLoaded() const { return status == LoadStatus::Loaded; }

inline bool LoadResult::hasProblems() const
{
    return !hasLoaded() || fieldProblemCount != 0 || missingFieldCount != 0
           || lostJournalFrameCount != 0;
}

template <typename T>
std::vector<std::uint8_t> toBinary(const T& value)
{
    return Detail::writeDocument(value);
}

template <typename T>
std::optional<T> createFromBinary(const ByteRange auto& bytes,
                                  const LoadOptions& options)
{
    auto value = T {};

    if (!fromBinary(value, bytes, options).hasLoaded())
        return std::nullopt;

    return value;
}

template <typename T>
LoadResult
    fromBinary(T& value, const ByteRange auto& bytes, const LoadOptions& options)
{
    auto data = Detail::viewAsBytes(bytes);
    return Detail::runGuardedLoad(options,
                                  data.size(),
                                  [&](const Detail::LoadContext& load)
                                  { Detail::loadBytes(value, data, load); });
}

template <typename T>
bool saveToFile(const T& value, const std::filesystem::path& file)
{
    try
    {
        return Detail::writeFileAtomically(file, toBinary(value));
    }
    catch (...)
    {
        return false;
    }
}

template <typename T>
std::optional<T> createFromFile(const std::filesystem::path& file,
                                const LoadOptions& options)
{
    auto value = T {};

    if (!loadFromFile(value, file, options).hasLoaded())
        return std::nullopt;

    return value;
}

template <typename T>
LoadResult loadFromFile(T& value,
                        const std::filesystem::path& file,
                        const LoadOptions& options)
{
    auto bytes = Detail::FileBytes::read(file);

    if (!bytes)
        return {.status = LoadStatus::FileMissing};

    return fromBinary(value, bytes->getBytes(), options);
}

template <typename T>
std::optional<T> createFromBinary(const View& view, const LoadOptions& options)
{
    auto value = T {};

    if (!fromBinary(value, view, options).hasLoaded())
        return std::nullopt;

    return value;
}

template <typename T>
LoadResult fromBinary(T& value, const View& view, const LoadOptions& options)
{
    auto inputSizeInBytes = Detail::ViewAccess::getDocument(view).bytes().size();
    return Detail::runGuardedLoad(options,
                                  inputSizeInBytes,
                                  [&](const Detail::LoadContext& load)
                                  { Detail::loadView(value, view, load); });
}

template <typename T>
bool JournalWriter::appendSet(std::string_view key, const T& value)
{
    if (getStatus() != LoadStatus::Loaded)
        return false;

    try
    {
        auto setFrame =
            Detail::makeJournalSetFrame(key, value, state->getKeyTable());

        if (!setFrame.hasFrame())
        {
            if (!state->addKeysToBase(setFrame.keysMissingFromJournal))
                return false;

            setFrame = Detail::makeJournalSetFrame(key, value, state->getKeyTable());
        }

        return state->appendFrame(setFrame.frame);
    }
    catch (...)
    {
        return false;
    }
}

template <typename Map>
bool JournalWriter::rewrite(const Map& entries)
{
    if (!state)
        return false;

    try
    {
        return state->replaceJournal(Detail::makeJournal(toBinary(entries)));
    }
    catch (...)
    {
        return false;
    }
}

} // namespace Calder
