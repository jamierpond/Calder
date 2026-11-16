#include "TestTypes.h"

#include <Calder/Calder.h>

#include <NanoTest/NanoTest.h>

#include <cstdint>
#include <map>
#include <stdexcept>
#include <variant>

using namespace nano;
using namespace Miro;
using namespace Calder;

namespace
{

using Bytes = std::vector<std::uint8_t>;

template <typename T>
Bytes encodeToBytes(T value)
{
    auto encoded = Calder::Detail::encodeLittleEndian(value);
    return {encoded.begin(), encoded.end()};
}

template <typename T>
T decodeFromBytes(const Bytes& bytes)
{
    return Calder::Detail::decodeLittleEndian<T>(
        std::span<const std::uint8_t> {bytes}.first<sizeof(T)>());
}

template <typename T>
bool encodesAs(T value, const Bytes& expected)
{
    return encodeToBytes(value) == expected && decodeFromBytes<T>(expected) == value;
}

template <typename T>
Bytes encodeElementsOneByOne(std::span<const T> values)
{
    auto out = Bytes {};
    Calder::Detail::appendLittleEndianElementsOneByOne(out, values);
    return out;
}

template <typename T>
Bytes encodeElementsInBulk(std::span<const T> values)
{
    auto out = Bytes {};
    Calder::Detail::appendLittleEndianElements(out, values);
    return out;
}

struct CountAsString
{
    void reflect(Miro::Reflector& ref) { ref["count"](count); }

    std::string count = "unchanged";
};

struct CountAsList
{
    void reflect(Miro::Reflector& ref) { ref["count"](count); }

    std::vector<int> count;
};

struct InnerWithText
{
    void reflect(Miro::Reflector& ref) { ref["x"](x); }

    std::string x = "unchanged";
};

struct ItemsWithText
{
    void reflect(Miro::Reflector& ref) { ref["items"](items); }

    std::vector<InnerWithText> items;
};

struct EveryShape
{
    int number = 3;
    double ratio = 0.1;
    std::string name = "n";
    std::vector<float> samples {1.5F, 2.5F};
    std::vector<Inner> inners {{1}, {2}};
    std::map<std::string, int> counts {{"a", 1}};
    std::optional<int> maybe;
    std::variant<int, std::string> either = std::string {"x"};
    Omittable<int> omitted;

    MIRO_REFLECT(
        number, ratio, name, samples, inners, counts, maybe, either, omitted)
};

} // namespace

auto littleEndianIntegers = test("Binary: integers are written little-endian") = []
{
    check(encodesAs(std::uint16_t {0x0102}, {0x02, 0x01}));
    check(encodesAs(std::uint32_t {0x01020304}, {0x04, 0x03, 0x02, 0x01}));
    check(encodesAs(std::uint64_t {0x0102030405060708},
                    {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}));
    check(encodesAs(std::int32_t {-2}, {0xFE, 0xFF, 0xFF, 0xFF}));
    check(encodesAs(std::int64_t {-2},
                    {0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
};

auto littleEndianFloats = test("Binary: floats are written little-endian") = []
{
    check(encodesAs(1.5F, {0x00, 0x00, 0xC0, 0x3F}));
    check(encodesAs(0.1, {0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9, 0x3F}));
};

auto littleEndianDocuments =
    test("Binary: documents pin every fixed-width field") = []
{
    check(toBinary(0.1)
          == Bytes {0x4D,
                    0x49,
                    0x42,
                    0x31,
                    0x11,
                    0x00,
                    0x00,
                    0x00,
                    0x04,
                    0x9A,
                    0x99,
                    0x99,
                    0x99,
                    0x99,
                    0x99,
                    0xB9,
                    0x3F,
                    0x00});
    check(toBinary(0.1F)
          == Bytes {0x4D,
                    0x49,
                    0x42,
                    0x31,
                    0x0D,
                    0x00,
                    0x00,
                    0x00,
                    0x05,
                    0xCD,
                    0xCC,
                    0xCC,
                    0x3D,
                    0x00});
    check(toBinary(std::vector<float> {1.5F, 2.5F})
          == Bytes {0x4D, 0x49, 0x42, 0x31, 0x15, 0x00, 0x00, 0x00,
                    0x0A, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0,
                    0x3F, 0x00, 0x00, 0x20, 0x40, 0x00});
    check(toBinary(std::vector<double> {0.1, 0.2})
          == Bytes {0x4D, 0x49, 0x42, 0x31, 0x1D, 0x00, 0x00, 0x00, 0x0B, 0x02,
                    0x00, 0x00, 0x00, 0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9,
                    0x3F, 0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xC9, 0x3F, 0x00});
    check(toBinary(std::vector<int> {1, 2, 3})
          == Bytes {0x4D, 0x49, 0x42, 0x31, 0x14, 0x00, 0x00, 0x00, 0x0C, 0x07, 0x00,
                    0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x04, 0x06, 0x00});

    auto emptyKeyTable = Calder::InternedKeyTable {};
    check(binaryJournalSet("b", 2, emptyKeyTable)
          == Bytes {0x01,
                    0x04,
                    0x00,
                    0x00,
                    0x00,
                    0x01,
                    0x62,
                    0x03,
                    0x04,
                    0x9D,
                    0xE4,
                    0xB7,
                    0xED});
};

auto journalFrameUsesBaseKeyIds =
    test("Binary: a journal set frame's value is bare, its key ids the base's") = []
{
    auto journal = toBinaryJournal(std::map<std::string, Inner> {{"a", {1}}});
    auto keyTable = Calder::readJournalKeyTable(journal);

    check(keyTable.getKeyCount() == 1);
    check(binaryJournalSet("b", Inner {2}, keyTable)
          == Bytes {0x01,
                    0x0A,
                    0x00,
                    0x00,
                    0x00,
                    0x01,
                    0x62,
                    0x07,
                    0x03,
                    0x00,
                    0x00,
                    0x00,
                    0x00,
                    0x03,
                    0x04,
                    0x4B,
                    0x32,
                    0xC9,
                    0x8E});
};

auto littleEndianBulkMatchesOneByOne =
    test("Binary: the big-endian element path round-trips with the bulk path") = []
{
    const auto floats = std::vector<float> {1.5F, -0.1F, 3.0e38F};
    const auto doubles = std::vector<double> {0.1, -2.5, 1.0e300};

    auto floatsOneByOne = encodeElementsOneByOne(std::span {floats});
    auto doublesOneByOne = encodeElementsOneByOne(std::span {doubles});
    check(floatsOneByOne == encodeElementsInBulk(std::span {floats}));
    check(doublesOneByOne == encodeElementsInBulk(std::span {doubles}));

    auto floatsBack = std::vector<float>(floats.size());
    auto doublesBack = std::vector<double>(doubles.size());
    Calder::Detail::decodeLittleEndianElements(floatsOneByOne,
                                               std::span {floatsBack});
    Calder::Detail::decodeLittleEndianElementsOneByOne(doublesOneByOne,
                                                       std::span {doublesBack});
    check(floatsBack == floats);
    check(doublesBack == doubles);
};

auto mismatchPrimitive =
    test("Binary: a field stored as another type is reported") = []
{
    auto loaded = CountAsString {};
    auto report = fromBinary(loaded, toBinary(ClassWithInt {}));

    check(loaded.count == "unchanged");
    check(report.typeMismatches.size() == 1);
    check(report.typeMismatches[0].fieldPath == "count");
    check(report.typeMismatches[0].storedKind == ValueKind::Number);
    check(report.typeMismatches[0].expectedKind == ValueKind::String);
};

auto mismatchNested = test("Binary: a nested mismatch reports its full path") = []
{
    auto loaded = ItemsWithText {};
    auto report = fromBinary(loaded, toBinary(ClassWithVectorOfObjects {}));

    check(report.typeMismatches.size() == 3);
    check(report.typeMismatches[1].fieldPath == "items[1].x");
};

auto mismatchContainer =
    test("Binary: a scalar stored where a list is expected is reported") = []
{
    auto loaded = CountAsList {};
    auto report = fromBinary(loaded, toBinary(ClassWithInt {}));

    check(report.typeMismatches.size() == 1);
    check(report.typeMismatches[0].expectedKind == ValueKind::Array);
};

auto cleanLoadHasNoReport =
    test("Binary: a same-type round trip reports nothing") = []
{
    auto loaded = EveryShape {};
    loaded.number = 0;
    auto report = fromBinary(loaded, toBinary(EveryShape {}));

    check(!report.hasProblems());
    check(loaded.number == 3);
};

auto documentSizeLimit =
    test("Binary: a document over 4 GiB is refused, not written") = []
{
    using Calder::Detail::convertToStoredSize;
    using Calder::Detail::largestStoredSize;

    check(convertToStoredSize(largestStoredSize) == largestStoredSize);

    auto wasRefused = false;

    try
    {
        convertToStoredSize(largestStoredSize + 1);
    }
    catch (const std::length_error&)
    {
        wasRefused = true;
    }

    check(wasRefused);
};
