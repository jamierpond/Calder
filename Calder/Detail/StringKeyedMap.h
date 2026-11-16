#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Calder::Detail
{

// A std::unordered_map<std::string, T> turns the key passed to find() into a
// std::string first, which allocates. Marking the hash `is_transparent` (with
// std::equal_to<> as the comparison) lets find() take a string_view as is.
struct StringLookupHash
{
    using is_transparent = void;

    std::size_t operator()(std::string_view text) const
    {
        return std::hash<std::string_view> {}(text);
    }
};

template <typename T>
using StringKeyedMap =
    std::unordered_map<std::string, T, StringLookupHash, std::equal_to<>>;

} // namespace Calder::Detail
