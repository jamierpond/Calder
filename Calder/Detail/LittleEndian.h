#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

// Every multi-byte field of the Miro binary format is little-endian. The
// shifts below produce that order on any host; whole arrays are copied
// in one go only when the host is little-endian too.

namespace Calder::Detail
{

template <typename T>
concept FixedWidthNumber = std::integral<T> || std::floating_point<T>;

template <std::size_t ByteCount>
struct UnsignedIntegerOfSize;

template <>
struct UnsignedIntegerOfSize<1>
{
    using Type = std::uint8_t;
};

template <>
struct UnsignedIntegerOfSize<2>
{
    using Type = std::uint16_t;
};

template <>
struct UnsignedIntegerOfSize<4>
{
    using Type = std::uint32_t;
};

template <>
struct UnsignedIntegerOfSize<8>
{
    using Type = std::uint64_t;
};

template <typename T>
using UnsignedBitsOf = typename UnsignedIntegerOfSize<sizeof(T)>::Type;

inline constexpr auto bitsPerByte = 8;
inline constexpr auto lowByteMask = 0xFFU;

template <FixedWidthNumber T>
constexpr std::array<std::uint8_t, sizeof(T)> encodeLittleEndian(T value)
{
    auto bits = std::bit_cast<UnsignedBitsOf<T>>(value);
    auto bytes = std::array<std::uint8_t, sizeof(T)> {};

    for (auto& byte: bytes)
    {
        byte = static_cast<std::uint8_t>(bits & lowByteMask);
        auto widenedBits = static_cast<std::uint64_t>(bits);
        bits = static_cast<UnsignedBitsOf<T>>(widenedBits >> bitsPerByte);
    }

    return bytes;
}

template <FixedWidthNumber T>
constexpr T decodeLittleEndian(std::span<const std::uint8_t, sizeof(T)> bytes)
{
    using Bits = UnsignedBitsOf<T>;
    auto bits = Bits {0};

    for (auto i = sizeof(T); i > 0; --i)
    {
        auto shiftedBits = static_cast<Bits>(bits << bitsPerByte);
        bits = static_cast<Bits>(shiftedBits | bytes[i - 1]);
    }

    return std::bit_cast<T>(bits);
}

template <FixedWidthNumber T>
void appendLittleEndianElementsOneByOne(std::vector<std::uint8_t>& out,
                                        std::span<const T> values)
{
    for (auto value: values)
    {
        auto bytes = encodeLittleEndian(value);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
}

template <FixedWidthNumber T>
void appendLittleEndianElements(std::vector<std::uint8_t>& out,
                                std::span<const T> values)
{
    if constexpr (std::endian::native == std::endian::little)
    {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(values.data());
        out.insert(out.end(), bytes, bytes + values.size_bytes());
    }
    else
    {
        appendLittleEndianElementsOneByOne(out, values);
    }
}

// `bytes` holds exactly dest.size() encoded elements.
template <FixedWidthNumber T>
void decodeLittleEndianElementsOneByOne(std::span<const std::uint8_t> bytes,
                                        std::span<T> dest)
{
    for (auto i = std::size_t {0}; i < dest.size(); ++i)
        dest[i] = decodeLittleEndian<T>(
            bytes.subspan(i * sizeof(T)).template first<sizeof(T)>());
}

template <FixedWidthNumber T>
void decodeLittleEndianElements(std::span<const std::uint8_t> bytes,
                                std::span<T> dest)
{
    if constexpr (std::endian::native == std::endian::little)
        std::memcpy(dest.data(), bytes.data(), dest.size_bytes());
    else
        decodeLittleEndianElementsOneByOne(bytes, dest);
}

} // namespace Calder::Detail
