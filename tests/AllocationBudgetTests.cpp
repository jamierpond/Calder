// Every load has a memory budget. A file can store an element in one byte that
// becomes kilobytes in memory; past the budget a load stops before allocating.
// This executable replaces the global operator new to measure peak memory.

#include <Calder/Calder.h>
#include <Miro/Reflect.h>

#include <NanoTest/NanoTest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace
{

std::atomic<std::size_t> liveAllocatedBytes {0};
std::atomic<std::size_t> peakAllocatedBytes {0};

constexpr auto allocationHeaderSize = alignof(std::max_align_t);

void* allocateCounted(std::size_t size)
{
    auto* block =
        static_cast<unsigned char*>(std::malloc(size + allocationHeaderSize));

    if (block == nullptr)
        throw std::bad_alloc {};

    *reinterpret_cast<std::size_t*>(block) = size;
    auto live = liveAllocatedBytes.fetch_add(size) + size;
    auto peak = peakAllocatedBytes.load();

    while (live > peak && !peakAllocatedBytes.compare_exchange_weak(peak, live))
    {
    }

    return block + allocationHeaderSize;
}

void freeCounted(void* pointer) noexcept
{
    if (pointer == nullptr)
        return;

    auto* block = static_cast<unsigned char*>(pointer) - allocationHeaderSize;
    liveAllocatedBytes.fetch_sub(*reinterpret_cast<std::size_t*>(block));
    std::free(block);
}

} // namespace

void* operator new(std::size_t size) { return allocateCounted(size); }

void* operator new[](std::size_t size) { return allocateCounted(size); }

void operator delete(void* pointer) noexcept { freeCounted(pointer); }

void operator delete[](void* pointer) noexcept { freeCounted(pointer); }

void operator delete(void* pointer, std::size_t) noexcept { freeCounted(pointer); }

void operator delete[](void* pointer, std::size_t) noexcept { freeCounted(pointer); }

using namespace nano;

namespace
{

constexpr auto largeElementSampleCount = std::size_t {512};

struct LargeElement
{
    MIRO_REFLECT(samples)

    std::array<double, largeElementSampleCount> samples {};
};

struct LargeElementList
{
    MIRO_REFLECT(items)

    std::vector<LargeElement> items;
};

struct LargeElementMap
{
    MIRO_REFLECT(entries)

    std::map<std::string, LargeElement> entries;
};

struct NullMap
{
    MIRO_REFLECT(entries)

    std::map<std::string, std::optional<int>> entries;
};

struct NullList
{
    MIRO_REFLECT(items)

    std::vector<std::optional<int>> items;
};

constexpr auto attackElementCount = std::size_t {100'000};
constexpr auto peakBytesPerInputByte = std::size_t {4};

// Miro lists every key before it asks to allocate the map's entries.
constexpr auto peakBytesPerInputByteWithKeyList = std::size_t {16};

struct MeasuredLoad
{
    Calder::LoadResult result;
    std::size_t peakBytesAboveStart = 0;
};

template <typename T>
MeasuredLoad loadMeasuringPeak(T& value, const std::vector<std::uint8_t>& bytes)
{
    auto start = liveAllocatedBytes.load();
    peakAllocatedBytes = start;
    auto result = Calder::fromBinary(value, bytes);
    return {std::move(result), peakAllocatedBytes.load() - start};
}

std::vector<std::uint8_t> makeNullListBytes(std::size_t elementCount)
{
    auto nulls = NullList {};
    nulls.items.resize(elementCount);
    return Calder::toBinary(nulls);
}

} // namespace

auto nullsIntoLargeElementsAreRefused =
    test("Allocation budget: 100,000 one-byte nulls into 4 KB elements stop "
         "the load") = []
{
    auto bytes = makeNullListBytes(attackElementCount);
    auto list = LargeElementList {};
    list.items.resize(1);
    auto measured = loadMeasuringPeak(list, bytes);

    check(measured.result.status == Calder::LoadStatus::OverBudget);
    check(measured.peakBytesAboveStart < bytes.size() * peakBytesPerInputByte);
    check(list.items.size() == 1);
};

auto nullEntriesIntoLargeMapValuesAreRefused =
    test("Allocation budget: 100,000 null map entries into 4 KB values stop "
         "the load") = []
{
    auto nulls = NullMap {};

    for (std::size_t i = 0; i < attackElementCount; ++i)
        nulls.entries[std::to_string(i)] = std::nullopt;

    auto bytes = Calder::toBinary(nulls);
    auto map = LargeElementMap {};
    auto measured = loadMeasuringPeak(map, bytes);

    check(measured.result.status == Calder::LoadStatus::OverBudget);
    check(measured.peakBytesAboveStart
          < bytes.size() * peakBytesPerInputByteWithKeyList);
};

auto journalFramesIntoLargeMapValuesAreRefused =
    test("Allocation budget: journal frames of nulls into 4 KB values stop "
         "the load") = []
{
    auto keyTable = Calder::Detail::InternedKeyTable {};
    auto journal = Calder::Detail::makeJournal(
        Calder::toBinary(std::map<std::string, std::optional<int>> {}));

    for (std::size_t i = 0; i < attackElementCount; ++i)
    {
        auto frame = Calder::Detail::makeJournalSetFrame(
            std::to_string(i), std::optional<int> {}, keyTable);
        journal.insert(journal.end(), frame.frame.begin(), frame.frame.end());
    }

    auto map = std::map<std::string, LargeElement> {};
    auto measured = loadMeasuringPeak(map, journal);

    check(measured.result.status == Calder::LoadStatus::OverBudget);
};

auto largeRealArrayLoadsWithTheDefault =
    test("Allocation budget: a real 20 MB array of 4 KB elements loads with "
         "the default") = []
{
    constexpr auto realElementCount = std::size_t {5'000};
    auto written = LargeElementList {};
    written.items.resize(realElementCount);

    for (auto& item: written.items)
        item.samples.fill(1.5);

    auto list = LargeElementList {};
    check(Calder::fromBinary(list, Calder::toBinary(written)).hasLoaded());
    check(list.items.size() == realElementCount);
};

auto noLimitLoadsPastTheDefault =
    test("Allocation budget: noAllocationLimit loads what the default refuses") = []
{
    constexpr auto overDefaultElementCount = std::size_t {20'000};
    auto bytes = makeNullListBytes(overDefaultElementCount);
    auto list = LargeElementList {};

    check(Calder::fromBinary(list, bytes).status == Calder::LoadStatus::OverBudget);

    auto unlimited = Calder::LoadOptions {
        .maximumAllocationInBytes = Calder::LoadOptions::noAllocationLimit};
    check(Calder::fromBinary(list, bytes, unlimited).hasLoaded());
    check(list.items.size() == overDefaultElementCount);
};

auto smallLimitRefusesASmallArray =
    test("Allocation budget: a lower limit refuses what the default allows") = []
{
    constexpr auto elementCount = std::size_t {10};
    constexpr auto limitBelowTenElements = sizeof(LargeElement) * elementCount / 2;
    auto bytes = makeNullListBytes(elementCount);
    auto list = LargeElementList {};

    check(Calder::fromBinary(list, bytes).hasLoaded());

    auto tight =
        Calder::LoadOptions {.maximumAllocationInBytes = limitBelowTenElements};
    check(Calder::fromBinary(list, bytes, tight).status
          == Calder::LoadStatus::OverBudget);
};
