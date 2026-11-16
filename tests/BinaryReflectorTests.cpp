#include "TestTypes.h"

#include "CalderTestHelpers.h"
#include <Calder/Calder.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <cstdint>
#include <limits>

using namespace nano;
using namespace Miro;
using namespace Calder;
using namespace CalderTestHelpers;

namespace
{

template <typename T>
T roundTripThroughBinary(const T& value)
{
    return createFromBinary<T>(toBinary(value)).value();
}

} // namespace

// --- Primitive round trips ---

auto binaryBool = test("Binary: bool round trip") = []
{
    auto val = ClassWithBool {};
    val.active = false;

    check(roundTripThroughBinary(val).active == false);
};

auto binaryInt = test("Binary: int round trip") = []
{
    auto val = ClassWithInt {};
    val.count = -123456;

    check(roundTripThroughBinary(val).count == -123456);
};

auto binaryDouble = test("Binary: double stored losslessly") = []
{
    auto val = ClassWithDouble {};
    val.ratio = 0.1 + 0.2; // not representable as float32 or decimal text

    auto copy = roundTripThroughBinary(val);
    check(copy.ratio == val.ratio); // bit-exact, no text round trip
};

auto binaryFloatFits = test("Binary: float-exact double uses 4-byte wire slot") = []
{
    auto small = ClassWithDouble {};
    small.ratio = 1.5; // fits float32 exactly

    auto wide = ClassWithDouble {};
    wide.ratio = 1.0 / 3.0; // needs all 8 bytes

    check(toBinary(small).size() < toBinary(wide).size());
    check(roundTripThroughBinary(small).ratio == 1.5);
    check(roundTripThroughBinary(wide).ratio == 1.0 / 3.0);
};

auto binaryString = test("Binary: string round trip") = []
{
    auto val = ClassWithString {};
    val.name = "hello \n\t \"world\" \xE2\x9C\x93";

    check(roundTripThroughBinary(val).name == val.name);
};

auto binaryInt64 = test("Binary: int64 round trip") = []
{
    auto val = ClassWithInt64 {};
    val.epochMs = -9223372036854775807LL;

    check(roundTripThroughBinary(val).epochMs == val.epochMs);
};

auto binaryIntegrals = test("Binary: narrow integrals round trip") = []
{
    auto val = ClassWithIntegrals {};
    val.u = 4000000000U;
    val.s = -321;
    val.ll = 9876543210123LL;
    val.c = 'z';

    auto copy = roundTripThroughBinary(val);
    check(copy.u == val.u);
    check(copy.s == val.s);
    check(copy.ll == val.ll);
    check(copy.c == val.c);
};

// --- Containers ---

auto binaryVectorInts = test("Binary: vector<int> packed round trip") = []
{
    auto val = ClassWithVectorOfInts {};
    val.nums = {0, -1, 1, 127, -128, 1000000, -1000000};

    check(roundTripThroughBinary(val).nums == val.nums);
};

auto binaryVectorObjects = test("Binary: vector of objects round trip") = []
{
    auto val = ClassWithVectorOfObjects {};
    val.items = {{10}, {20}, {30}, {40}};

    auto copy = roundTripThroughBinary(val);
    check(copy.items.size() == 4);
    check(copy.items[0].x == 10);
    check(copy.items[3].x == 40);
};

auto binaryVectorStrings = test("Binary: vector of strings round trip") = []
{
    auto val = ClassWithVectorOfStrings {};
    val.tags = {"alpha", "", "gamma"};

    check(roundTripThroughBinary(val).tags == val.tags);
};

auto binaryEmptyVector = test("Binary: empty vector round trip") = []
{
    auto val = ClassWithVectorOfInts {};
    val.nums.clear();

    check(roundTripThroughBinary(val).nums.empty());
};

auto binaryArrayDoubles = test("Binary: std::array packed round trip") = []
{
    auto val = ClassWithArrayOfDoubles {};
    val.vals = {1.0 / 3.0, -0.0, std::numeric_limits<double>::max()};

    auto copy = roundTripThroughBinary(val);
    check(copy.vals == val.vals);
};

auto binaryNestedVectors = test("Binary: nested vectors round trip") = []
{
    struct Nested
    {
        std::vector<std::vector<int>> grid;

        void reflect(Miro::Reflector& ref) { MIRO_FIELDS(ref, grid); }
    };

    auto val = Nested {};
    val.grid = {{1, 2}, {}, {3, 4, 5}};

    check(roundTripThroughBinary(val).grid == val.grid);
};

auto binaryFloatVector = test("Binary: vector<float> stored losslessly") = []
{
    struct Floats
    {
        std::vector<float> samples;

        void reflect(Miro::Reflector& ref) { MIRO_FIELDS(ref, samples); }
    };

    auto val = Floats {};

    for (auto i = 0; i < 1000; ++i)
        val.samples.push_back(std::sin(static_cast<float>(i) * 0.1F));

    auto copy = roundTripThroughBinary(val);
    check(copy.samples.size() == val.samples.size());

    for (std::size_t i = 0; i < val.samples.size(); ++i)
        check(copy.samples[i] == val.samples[i]); // bit-exact

    // Packed float32 wire cost: ~4 bytes per element plus headers.
    check(toBinary(val).size() < 4 * val.samples.size() + 128);
};

auto binaryNarrowArrays =
    test("Binary: 8-, 16-bit and unsigned arrays round trip packed") = []
{
    struct NarrowArrays
    {
        std::vector<std::int8_t> signedBytes;
        std::vector<std::uint8_t> bytes;
        std::vector<std::int16_t> shorts;
        std::vector<std::uint16_t> unsignedShorts;
        std::vector<std::uint32_t> unsignedInts;
        std::vector<std::uint64_t> unsignedLongs;

        void reflect(Miro::Reflector& ref)
        {
            MIRO_FIELDS(ref,
                        signedBytes,
                        bytes,
                        shorts,
                        unsignedShorts,
                        unsignedInts,
                        unsignedLongs);
        }

        bool operator==(const NarrowArrays&) const = default;
    };

    using std::numeric_limits;
    auto val = NarrowArrays {
        .signedBytes = {numeric_limits<std::int8_t>::min(),
                        0,
                        numeric_limits<std::int8_t>::max()},
        .bytes = {0, 1, numeric_limits<std::uint8_t>::max()},
        .shorts = {numeric_limits<std::int16_t>::min(),
                   -1,
                   numeric_limits<std::int16_t>::max()},
        .unsignedShorts = {0, numeric_limits<std::uint16_t>::max()},
        .unsignedInts = {0, numeric_limits<std::uint32_t>::max()},
        .unsignedLongs = {0, numeric_limits<std::uint64_t>::max()},
    };

    check(roundTripThroughBinary(val) == val);
};

auto binaryByteBlob =
    test("Binary: a vector<uint8_t> costs one byte per element") = []
{
    struct Blob
    {
        std::vector<std::uint8_t> data;

        void reflect(Miro::Reflector& ref) { MIRO_FIELDS(ref, data); }
    };

    constexpr auto blobSize = 1000;
    constexpr auto headerAllowance = std::size_t {64};
    auto val = Blob {};

    for (auto i = 0; i < blobSize; ++i)
        val.data.push_back(static_cast<std::uint8_t>(i));

    check(roundTripThroughBinary(val).data == val.data);
    check(toBinary(val).size() < val.data.size() + headerAllowance);
};

auto binaryNarrowingOnLoad =
    test("Binary: a packed int16 array loaded as int8 refuses what doesn't fit") = []
{
    struct Shorts
    {
        std::vector<std::int16_t> values;

        void reflect(Miro::Reflector& ref) { MIRO_FIELDS(ref, values); }
    };

    struct SignedBytes
    {
        std::vector<std::int8_t> values;

        void reflect(Miro::Reflector& ref) { MIRO_FIELDS(ref, values); }
    };

    constexpr auto doesNotFitInInt8 = std::int16_t {300};
    auto loaded =
        createFromBinary<SignedBytes>(toBinary(Shorts {{1, doesNotFitInInt8, -2}}))
            .value();

    check(loaded.values == std::vector<std::int8_t> {1, 0, -2});
};

auto binaryStringMap = test("Binary: string map round trip") = []
{
    auto val = ClassWithStringMap {};
    val.data = {{"k1", "v1"}, {"weird key \x01", "v2"}, {"", "empty"}};

    check(roundTripThroughBinary(val).data == val.data);
};

auto binaryObjectMap = test("Binary: object map round trip") = []
{
    auto val = ClassWithObjectMap {};
    val.items = {{"a", {1}}, {"b", {2}}};

    auto copy = roundTripThroughBinary(val);
    check(copy.items.size() == 2);
    check(copy.items["a"].x == 1);
    check(copy.items["b"].x == 2);
};

// --- Optionals, enums, variants ---

auto binaryOptionals = test("Binary: optionals round trip") = []
{
    auto val = ClassWithOptional {};
    val.maybeInt = 7;

    auto copy = roundTripThroughBinary(val);
    check(copy.maybeInt.has_value());
    check(*copy.maybeInt == 7);
    check(!copy.maybeInner.has_value());

    val.maybeInt.reset();
    val.maybeInner = Inner {42};

    copy = roundTripThroughBinary(val);
    check(!copy.maybeInt.has_value());
    check(copy.maybeInner.has_value());
    check(copy.maybeInner->x == 42);
};

auto binaryEnums = test("Binary: enums round trip") = []
{
    auto val = ClassWithEnum {};
    val.color = Color::Blue;
    val.signal = Signal::Stop;
    val.mode = ModeOn;

    auto copy = roundTripThroughBinary(val);
    check(copy.color == Color::Blue);
    check(copy.signal == Signal::Stop);
    check(copy.mode == ModeOn);
};

auto binaryUser = test("Binary: full User round trip") = []
{
    auto val = User {};
    val.name = "Ada";
    val.age = 36;
    val.active = false;
    val.address = {"12 Crescent", "N1"};
    val.tags = {"math", "engines"};
    val.counters = {{"a", 1}, {"b", 2}};
    val.note = "note";
    val.shipping = Address {"PO Box", "E2"};
    val.color = Color::Green;
    val.priority = Priority::High;

    auto copy = roundTripThroughBinary(val);
    check(copy.name == val.name);
    check(copy.age == val.age);
    check(copy.active == val.active);
    check(copy.address.street == val.address.street);
    check(copy.tags == val.tags);
    check(copy.counters == val.counters);
    check(copy.note == val.note);
    check(copy.shipping.has_value());
    check(copy.shipping->zip == "E2");
    check(copy.color == val.color);
    check(copy.priority == val.priority);
    check(!copy.accent.has_value());
};

// --- Schema evolution & robustness ---

namespace
{

struct RecordV1
{
    int a = 0;
    std::string b;

    MIRO_REFLECT(a, b)
};

struct RecordV2
{
    int a = 0;
    std::string b;
    double added = 1.5;

    MIRO_REFLECT(a, b, added)
};

struct RecordV2Reordered
{
    void reflect(Miro::Reflector& ref)
    {
        ref["b"](b);
        ref["a"](a);
    }

    int a = 0;
    std::string b;
};

} // namespace

auto binaryMissingKeys = test("Binary: missing keys keep prior values") = []
{
    auto val = RecordV1 {};
    val.a = 9;
    val.b = "nine";

    auto copy = createFromBinary<RecordV2>(toBinary(val)).value();
    check(copy.a == 9);
    check(copy.b == "nine");
    check(copy.added == 1.5); // untouched default
};

auto binaryExtraKeys = test("Binary: extra keys are skipped") = []
{
    auto val = RecordV2 {};
    val.a = 4;
    val.b = "four";
    val.added = 2.5;

    auto copy = createFromBinary<RecordV1>(toBinary(val)).value();
    check(copy.a == 4);
    check(copy.b == "four");
};

auto binaryReordered = test("Binary: fields load in any order") = []
{
    auto val = RecordV1 {};
    val.a = 11;
    val.b = "eleven";

    auto copy = createFromBinary<RecordV2Reordered>(toBinary(val)).value();
    check(copy.a == 11);
    check(copy.b == "eleven");
};

auto binaryTypeMismatch = test("Binary: mismatched types keep prior values") = []
{
    struct Stringy
    {
        std::string count = "keep";

        void reflect(Miro::Reflector& ref) { MIRO_FIELDS(ref, count); }
    };

    auto val = ClassWithInt {};
    val.count = 5;

    auto copy = createFromBinary<Stringy>(toBinary(val)).value();
    check(copy.count == "keep");
};

auto binaryCorruptInput = test("Binary: corrupt input yields defaults") = []
{
    auto good = toBinary(ClassWithInt {});

    // Truncations at every length must not crash.
    for (std::size_t cut = 0; cut < good.size(); ++cut)
    {
        auto truncated = Calder::Detail::Buffer(
            good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        auto copy = loadIgnoringProblems<ClassWithInt>(truncated);
        ignoreUnused(copy);
    }

    // Flipping each byte must not crash either.
    for (std::size_t i = 0; i < good.size(); ++i)
    {
        auto mangled = good;
        mangled[i] = static_cast<std::uint8_t>(~mangled[i]);
        auto copy = loadIgnoringProblems<ClassWithInt>(mangled);
        ignoreUnused(copy);
    }

    check(loadIgnoringProblems<ClassWithInt>(Calder::Detail::Buffer {}).count == 42);
};

auto binaryEmptyStruct = test("Binary: empty struct round trip") = []
{
    auto copy = roundTripThroughBinary(MacroEmpty {});
    ignoreUnused(copy);
    check(true);
};

// --- View: zero-parse random access ---

auto binaryView = test("Binary: View jumps to nested values without decoding") = []
{
    auto val = ClassWithVectorOfObjects {};
    val.items = {{100}, {200}, {300}};

    auto file = File::fromBytes(toBinary(val));
    check(file.getStatus() == LoadStatus::Loaded);

    auto root = file.getRoot();
    check(root.isObject());

    auto items = root["items"];
    check(items.isArray());
    check(items.getSize() == 3);
    check(items[1]["x"].asInteger() == 200);
    check(items[2]["x"].asInteger() == 300);
    check(!items[3].isValid());
    check(!root["missing"].isValid());
};

auto binaryViewPacked = test("Binary: View indexes packed arrays") = []
{
    auto val = ClassWithArrayOfDoubles {};
    val.vals = {0.5, 1.0 / 3.0, -2.25};

    auto file = File::fromBytes(toBinary(val));

    auto vals = file.getRoot()["vals"];
    check(vals.isArray());
    check(vals.getSize() == 3);
    check(vals[0].asNumber() == 0.5);
    check(vals[1].asNumber() == 1.0 / 3.0);
    check(vals[2].asNumber() == -2.25);
};

auto binaryViewStrings = test("Binary: View reads strings and maps") = []
{
    auto val = ClassWithStringMap {};
    val.data = {{"greeting", "hello"}, {"farewell", "bye"}};

    auto file = File::fromBytes(toBinary(val));

    auto data = file.getRoot()["data"];
    check(data.isObject());
    check(data.getSize() == 2);
    check(data["greeting"].asString() == "hello");
    check(data["farewell"].asString() == "bye");
    check(!data["nope"].isValid());
};

// --- Journal: appendable inserts ---

namespace
{

using ProfileMap = std::map<std::string, Inner>;

} // namespace

auto journalRoundTrip = test("Journal: base plus frames replay in order") = []
{
    auto base = makeJournalOf(ProfileMap {{"a", {1}}, {"b", {2}}});
    auto keyTable = Calder::Detail::readJournalKeyTable(base);

    auto bytes = concatenateBuffers({
        base,
        makeSetFrame("c", Inner {3}, keyTable), // insert
        makeSetFrame("a", Inner {9}, keyTable), // overwrite
        makeRemoveFrame("b"), // remove
        makeSetFrame("b", Inner {5}, keyTable), // re-add after remove
    });

    auto loaded = createFromBinary<ProfileMap>(bytes).value();
    check(loaded.size() == 3);
    check(loaded["a"].x == 9);
    check(loaded["b"].x == 5);
    check(loaded["c"].x == 3);

    auto info = Calder::Detail::inspectJournal(bytes);
    check(info.valid);
    check(!info.truncated);
    check(Calder::Detail::readJournalFrames(bytes).size() == 4);
};

auto journalBaseOnly = test("Journal: frameless journal equals its base") = []
{
    auto base = ProfileMap {{"only", {7}}};
    auto loaded = createFromBinary<ProfileMap>(makeJournalOf(base)).value();

    check(loaded.size() == 1);
    check(loaded["only"].x == 7);
};

auto journalNonMap = test("Journal: non-map target reads the base alone") = []
{
    auto base = makeJournalOf(Inner {42});
    auto keyTable = Calder::Detail::readJournalKeyTable(base);
    auto bytes = concatenateBuffers({base, makeSetFrame("x", Inner {1}, keyTable)});

    check(createFromBinary<Inner>(bytes).value().x == 42);
};

auto journalTornTail = test("Journal: torn tail drops only incomplete frames") = []
{
    auto base = makeJournalOf(ProfileMap {{"a", {1}}});
    auto keyTable = Calder::Detail::readJournalKeyTable(base);
    auto intact = concatenateBuffers({
        base,
        makeSetFrame("b", Inner {2}, keyTable),
    });

    auto lastFrame = makeSetFrame("c", Inner {3}, keyTable);
    auto full = concatenateBuffers({intact, lastFrame});

    // Cut the last frame at every possible length: the first two
    // entries must survive, "c" must only appear when fully written.
    for (auto cut = intact.size(); cut <= full.size(); ++cut)
    {
        auto torn = Calder::Detail::Buffer(
            full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut));
        auto loaded = createFromBinary<ProfileMap>(torn).value();

        check(loaded["a"].x == 1);
        check(loaded["b"].x == 2);
        check(loaded.count("c") == (cut == full.size() ? 1U : 0U));

        auto info = Calder::Detail::inspectJournal(torn);
        check(info.valid);
        check(info.truncated == (cut != intact.size() && cut != full.size()));
    }
};

auto journalCorruptFrame =
    test("Journal: a corrupt frame is skipped and counted") = []
{
    auto base = makeJournalOf(ProfileMap {{"a", {1}}});
    auto keyTable = Calder::Detail::readJournalKeyTable(base);
    auto setB = makeSetFrame("b", Inner {2}, keyTable);
    auto setC = makeSetFrame("c", Inner {3}, keyTable);
    auto bytes = concatenateBuffers({base, setB, setC});

    constexpr auto checksumByteCount = std::size_t {4};
    auto lastPayloadByteOfB = base.size() + setB.size() - checksumByteCount - 1;
    bytes[lastPayloadByteOfB] ^= 0xFF;

    auto loaded = ProfileMap {};
    auto report = fromBinary(loaded, bytes);
    check(loaded.size() == 2);
    check(loaded["a"].x == 1);
    check(loaded["c"].x == 3);
    check(report.lostJournalFrameCount == 1);

    auto info = Calder::Detail::inspectJournal(bytes);
    check(info.skippedFrameCount == 1);
    check(!info.truncated);
    check(info.frameBytes == setB.size() + setC.size());
};

auto journalWholeDocumentFrame =
    test("Journal: a set frame holding a whole document is skipped") = []
{
    auto base = makeJournalOf(ProfileMap {{"a", {1}}});
    auto frameWithDocument = Calder::Detail::makeFrame(
        Calder::Detail::FrameKind::Set, "b", toBinary(Inner {2}));

    auto loaded = ProfileMap {};
    auto report = fromBinary(loaded, concatenateBuffers({base, frameWithDocument}));
    check(loaded.size() == 1);
    check(report.lostJournalFrameCount == 1);
};

auto journalTornTailReport =
    test("Journal: a torn tail is reported as one lost frame") = []
{
    auto base = makeJournalOf(ProfileMap {{"a", {1}}});
    auto keyTable = Calder::Detail::readJournalKeyTable(base);
    auto bytes = concatenateBuffers({
        base,
        makeSetFrame("b", Inner {2}, keyTable),
    });

    bytes.pop_back();

    auto loaded = ProfileMap {};
    auto report = fromBinary(loaded, bytes);
    check(loaded.size() == 1);
    check(report.hasLoaded());
    check(report.lostJournalFrameCount == 1);
};

auto journalGarbage = test("Journal: garbage input is rejected calmly") = []
{
    check(!Calder::Detail::inspectJournal({}).valid);

    auto junk = Calder::Detail::Buffer {'C', 'L', 'D', 'J', 0xFF, 0xFF, 0xFF, 0xFF};
    check(!Calder::Detail::inspectJournal(junk).valid);
    check(loadIgnoringProblems<ProfileMap>(junk).empty());
};

// --- Wire size sanity ---

auto binarySmallerThanJson = test("Binary: object array smaller than JSON") = []
{
    auto val = ClassWithVectorOfObjects {};

    for (auto i = 0; i < 500; ++i)
        val.items.push_back({i});

    auto binary = toBinary(val);
    auto json = toJSONString(val);

    // Keys are interned once; JSON repeats "x" 500 times.
    check(binary.size() < json.size());
};

// --- Omittable<T> and raw JSON fields (upstream features, binary wire) ---

namespace
{

struct BinaryPatch
{
    Omittable<std::string> name;
    Omittable<int> count;
    Omittable<Inner> inner;
    Omittable<std::optional<int>> cleared;
    int after = 0;

    MIRO_REFLECT(name, count, inner, cleared, after)
};

struct BinaryOmittableElements
{
    std::vector<Omittable<int>> items;

    MIRO_REFLECT(items)
};

struct BinaryRaw
{
    JSON value;
    int after = 0;

    MIRO_REFLECT(value, after)
};

} // namespace

auto binaryOmittableAbsent = test("Binary: disengaged Omittable leaves no key") = []
{
    auto patch = BinaryPatch {};
    patch.count = 3;
    patch.after = 9;

    auto bytes = toBinary(patch);
    auto file = File::fromBytes(bytes);
    auto root = file.getRoot();
    check(root.isValid());
    check(!root["name"].isValid());
    check(!root["inner"].isValid());
    check(!root["cleared"].isValid());
    check(root["count"].asInteger() == 3);
    check(root["after"].asInteger() == 9);

    auto loaded = createFromBinary<BinaryPatch>(bytes).value();
    check(!loaded.name.has_value());
    check(!loaded.inner.has_value());
    check(!loaded.cleared.has_value());
    check(loaded.count.has_value() && *loaded.count == 3);
    check(loaded.after == 9);
};

auto binaryOmittableEngaged =
    test("Binary: engaged Omittable round trips, null included") = []
{
    auto patch = BinaryPatch {};
    patch.name = std::string {"n"};
    patch.inner = Inner {5};
    patch.cleared = std::optional<int> {};

    auto loaded = roundTripThroughBinary(patch);
    check(loaded.name.has_value() && *loaded.name == "n");
    check(loaded.inner.has_value() && loaded.inner->x == 5);
    check(loaded.cleared.has_value() && !loaded.cleared->has_value());
    check(!loaded.count.has_value());
};

auto binaryOmittableElements =
    test("Binary: disengaged Omittable array element keeps the array intact") = []
{
    auto value = BinaryOmittableElements {};
    value.items = {Omittable<int> {1}, Omittable<int> {}, Omittable<int> {3}};

    auto loaded = roundTripThroughBinary(value);
    check(loaded.items.size() == 3);
    check(loaded.items[0].has_value() && *loaded.items[0] == 1);
    check(loaded.items[2].has_value() && *loaded.items[2] == 3);
};

auto binaryRawJson = test("Binary: raw JSON field round trips every kind") = []
{
    for (auto text: {R"({"a":1,"b":[true,null,"s",2.5],"c":{}})",
                     R"([1,2,{"x":"y"}])",
                     R"([])",
                     R"({})",
                     R"("text")",
                     R"(42)",
                     R"(-1.25)",
                     R"(true)",
                     R"(null)"})
    {
        auto value = BinaryRaw {};
        value.value = Json::parse(text);
        value.after = 7;

        auto loaded = roundTripThroughBinary(value);
        check(Json::print(loaded.value) == Json::print(value.value));
        check(loaded.after == 7);
    }
};

auto binaryRawJsonInteger = test("Binary: raw JSON keeps integers exact") = []
{
    auto value = BinaryRaw {};
    value.value = Json::parse("9007199254740993"); // 2^53 + 1

    auto loaded = roundTripThroughBinary(value);
    check(loaded.value.isInteger());
    check(loaded.value.asInteger() == 9007199254740993LL);
};
