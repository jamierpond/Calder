#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Let clang warn when a View outlives the File it points into.
#if defined(__clang__)
    #define CALDER_LIFETIMEBOUND [[clang::lifetimebound]]
    #define CALDER_POINTER_TYPE [[gsl::Pointer]]
#else
    #define CALDER_LIFETIMEBOUND
    #define CALDER_POINTER_TYPE
#endif

namespace Calder::Detail
{

class Document;
class FileState;
class JournalWriterState;
struct ViewAccess;
enum class Encoding : std::uint8_t;

template <typename Element>
concept IsByte =
    std::same_as<Element, char> || std::same_as<Element, unsigned char>
    || std::same_as<Element, std::uint8_t> || std::same_as<Element, std::byte>;

// A string literal is a char array too; it is refused so that a file name
// passed where bytes belong does not compile.
template <typename Range>
concept IsByteRange =
    std::ranges::contiguous_range<Range> && std::ranges::sized_range<Range>
    && IsByte<std::remove_cv_t<std::ranges::range_value_t<Range>>>
    && !std::is_array_v<std::remove_cvref_t<Range>>;

// A place inside one record of a packed struct array: the first `depth`
// segments of the path of the layout's field `fieldIndex`.
struct PackedPathPrefix
{
    std::size_t fieldIndex = 0;
    std::size_t depth = 0;
};

// Where a View points. An element of a packed number array has no tag, so
// its encoding comes from the array. An element of a packed struct array is
// a record: valueOffset is then the array's, and packedPath the place inside
// record packedRecordIndex.
struct ViewPosition
{
    std::size_t valueOffset = 0;
    Encoding elementEncoding {};
    std::size_t packedRecordIndex = 0;
    PackedPathPrefix packedPath;
};

} // namespace Calder::Detail
