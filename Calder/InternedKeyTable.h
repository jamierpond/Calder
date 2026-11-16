#pragma once

#include "StringKeyedMap.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Calder
{

// Gives each distinct object key a small id, numbered in first-seen order.
// A struct's fields arrive in the same order every time it is written, so
// the id that followed the previous key last time is checked first. Small
// tables are then searched in order; a hash index is built only once the
// table outgrows that.
class InternedKeyTable
{
public:
    std::uint32_t getInternedKeyId(std::string_view key)
    {
        auto id = findKeyId(key);

        if (!id)
            id = addKey(key);

        recordKeyFollowingPreviousKey(*id);
        return *id;
    }

    // The key's id if the table has it; never adds one.
    std::optional<std::uint32_t> findInternedKeyId(std::string_view key)
    {
        auto id = findKeyId(key);

        if (id)
            recordKeyFollowingPreviousKey(*id);

        return id;
    }

    std::size_t getKeyCount() const { return keys.size(); }

    std::string_view getKeyName(std::size_t id) const
    {
        const auto& key = keys[id];
        return std::string_view {keyNameBytes}.substr(key.nameOffset,
                                                      key.nameLength);
    }

private:
    static constexpr auto keyCountSearchedInOrder = std::size_t {16};
    static constexpr auto typicalKeyNameByteCount = std::size_t {256};

    struct InternedKey
    {
        std::size_t nameOffset = 0;
        std::size_t nameLength = 0;
        std::optional<std::uint32_t> idOfKeyThatFollowedLastTime;
    };

    std::optional<std::uint32_t> findKeyId(std::string_view key) const
    {
        if (isPredictedKey(key))
            return getPredictedKeyId();

        if (!keyIdsByName.empty())
            return findKeyIdInIndex(key);

        return findKeyIdInOrder(key);
    }

    bool isPredictedKey(std::string_view key) const
    {
        auto predictedId = getPredictedKeyId();
        return predictedId && getKeyName(*predictedId) == key;
    }

    std::optional<std::uint32_t> findKeyIdInOrder(std::string_view key) const
    {
        for (auto id = std::size_t {0}; id < keys.size(); ++id)
        {
            if (getKeyName(id) == key)
                return static_cast<std::uint32_t>(id);
        }

        return std::nullopt;
    }

    std::optional<std::uint32_t> getPredictedKeyId() const
    {
        if (!previousKeyId)
            return std::nullopt;

        return keys[*previousKeyId].idOfKeyThatFollowedLastTime;
    }

    std::optional<std::uint32_t> findKeyIdInIndex(std::string_view key) const
    {
        if (auto it = keyIdsByName.find(key); it != keyIdsByName.end())
            return it->second;

        return std::nullopt;
    }

    std::uint32_t addKey(std::string_view key)
    {
        if (keys.empty())
            reserveForTypicalDocument();

        auto id = static_cast<std::uint32_t>(keys.size());
        keys.push_back({keyNameBytes.size(), key.size(), std::nullopt});
        keyNameBytes += key;

        if (!keyIdsByName.empty())
            keyIdsByName.emplace(std::string {key}, id);
        else if (keys.size() > keyCountSearchedInOrder)
            buildIndex();

        return id;
    }

    void reserveForTypicalDocument()
    {
        keys.reserve(keyCountSearchedInOrder);
        keyNameBytes.reserve(typicalKeyNameByteCount);
    }

    void buildIndex()
    {
        for (auto id = std::size_t {0}; id < keys.size(); ++id)
            keyIdsByName.emplace(std::string {getKeyName(id)},
                                 static_cast<std::uint32_t>(id));
    }

    void recordKeyFollowingPreviousKey(std::uint32_t id)
    {
        if (previousKeyId)
            keys[*previousKeyId].idOfKeyThatFollowedLastTime = id;

        previousKeyId = id;
    }

    std::vector<InternedKey> keys;
    std::string keyNameBytes;
    std::optional<std::uint32_t> previousKeyId;
    StringKeyedMap<std::uint32_t> keyIdsByName;
};

} // namespace Calder
