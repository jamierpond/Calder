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
std::optional<T> createFromBinary(const ByteRange auto& bytes)
{
    auto value = T {};

    if (!fromBinary(value, bytes).hasLoaded())
        return std::nullopt;

    return value;
}

template <typename T>
LoadResult fromBinary(T& value, const ByteRange auto& bytes)
{
    return Detail::runGuardedLoad(
        [&](LoadResult& result)
        { Detail::loadBytes(value, Detail::viewAsBytes(bytes), result); });
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
std::optional<T> createFromFile(const std::filesystem::path& file)
{
    auto value = T {};

    if (!loadFromFile(value, file).hasLoaded())
        return std::nullopt;

    return value;
}

template <typename T>
LoadResult loadFromFile(T& value, const std::filesystem::path& file)
{
    auto bytes = Detail::FileBytes::read(file);

    if (!bytes)
        return {.status = LoadStatus::FileMissing};

    return fromBinary(value, bytes->getBytes());
}

template <typename T>
std::optional<T> createFromBinary(const View& view)
{
    auto value = T {};

    if (!fromBinary(value, view).hasLoaded())
        return std::nullopt;

    return value;
}

template <typename T>
LoadResult fromBinary(T& value, const View& view)
{
    return Detail::runGuardedLoad([&](LoadResult& result)
                                  { Detail::loadView(value, view, result); });
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
