#pragma once

#include <Calder/Format.h>

#include <initializer_list>
#include <string_view>

namespace CalderTestHelpers
{

using Calder::Detail::Buffer;

// The value whatever the load found: for tests of damaged bytes, which
// check that loading returns and what it leaves behind.
template <typename T>
T loadIgnoringProblems(const Calder::ByteRange auto& bytes)
{
    auto value = T {};
    auto result = Calder::fromBinary(value, bytes);
    static_cast<void>(result);
    return value;
}

template <typename T>
Buffer makeJournalOf(const T& value)
{
    return Calder::Detail::makeJournal(Calder::toBinary(value));
}

template <typename V>
Buffer makeSetFrame(std::string_view key,
                    const V& value,
                    Calder::Detail::InternedKeyTable& keyTable)
{
    return Calder::Detail::makeJournalSetFrame(key, value, keyTable).frame;
}

inline Buffer makeRemoveFrame(std::string_view key)
{
    return Calder::Detail::makeJournalRemoveFrame(key);
}

inline Buffer concatenateBuffers(std::initializer_list<Buffer> parts)
{
    auto out = Buffer {};

    for (const auto& part: parts)
        out.insert(out.end(), part.begin(), part.end());

    return out;
}

} // namespace CalderTestHelpers
