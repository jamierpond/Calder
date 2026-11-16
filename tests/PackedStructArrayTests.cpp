#include <Calder/Calder.h>
#include <Miro/Json.h>
#include <Miro/Reflect.h>

#include <NanoTest/NanoTest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace nano;
using namespace Miro;
using namespace Calder;

namespace PackedStructArrayTestTypes
{

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    bool operator==(const Vec3&) const = default;

    MIRO_REFLECT(x, y, z)
};

struct Path
{
    std::vector<Vec3> points;

    bool operator==(const Path&) const = default;

    MIRO_REFLECT(points)
};

struct Quaternion
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    bool operator==(const Quaternion&) const = default;

    MIRO_REFLECT(x, y, z, w)
};

struct Keyframe
{
    float time = 0.0f;
    Quaternion value;
    Quaternion inTangent;

    bool operator==(const Keyframe&) const = default;

    MIRO_REFLECT(time, value, inTangent)
};

struct Track
{
    std::vector<Keyframe> keys;

    bool operator==(const Track&) const = default;

    MIRO_REFLECT(keys)
};

struct ReorderedPoint
{
    float z = 0.0f;
    float x = 0.0f;
    float w = 9.0f;

    MIRO_REFLECT(z, x, w)
};

struct ReorderedCloud
{
    std::vector<ReorderedPoint> points;

    MIRO_REFLECT(points)
};

struct SavedSample
{
    float level = 0.0f;
    std::int32_t count = 0;

    MIRO_REFLECT(level, count)
};

struct LoadedSample
{
    double level = 0.0;
    std::int16_t count = -1;

    MIRO_REFLECT(level, count)
};

struct SavedSamples
{
    std::vector<SavedSample> samples;

    MIRO_REFLECT(samples)
};

struct LoadedSamples
{
    std::vector<LoadedSample> samples;

    MIRO_REFLECT(samples)
};

struct ExtendedSample
{
    float level = 0.0f;
    float gain = 2.0f;
    std::int32_t count = 0;

    MIRO_REFLECT(level, gain, count)
};

struct ExtendedSamples
{
    std::vector<ExtendedSample> samples;

    MIRO_REFLECT(samples)
};

struct Triangle
{
    std::array<Vec3, 3> corners;

    bool operator==(const Triangle&) const = default;

    MIRO_REFLECT(corners)
};

struct PaddedWeight
{
    std::uint8_t flags = 0;
    float weight = 0.0f;

    bool operator==(const PaddedWeight&) const = default;

    MIRO_REFLECT(flags, weight)
};

struct PaddedWeights
{
    std::vector<PaddedWeight> weights;

    bool operator==(const PaddedWeights&) const = default;

    MIRO_REFLECT(weights)
};

struct LabelledPoint
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::string label = "unnamed";

    MIRO_REFLECT(x, y, z, label)
};

struct LabelledPath
{
    std::vector<LabelledPoint> points;

    MIRO_REFLECT(points)
};

struct NamedKeyframe
{
    float time = 0.0f;
    Quaternion value;
    std::string name = "key";

    MIRO_REFLECT(time, value, name)
};

struct NamedTrack
{
    std::vector<NamedKeyframe> keys;

    MIRO_REFLECT(keys)
};

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;

    MIRO_REFLECT(x, y)
};

struct Marker
{
    std::int16_t id = 0;
    Vec2 at;

    MIRO_REFLECT(id, at)
};

struct Rig
{
    std::vector<Marker> markers;

    MIRO_REFLECT(markers)
};

struct ExternalVec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ExternalPath
{
    std::vector<ExternalVec3> points;

    MIRO_REFLECT(points)
};

} // namespace PackedStructArrayTestTypes

MIRO_REFLECT_EXTERNAL(PackedStructArrayTestTypes::ExternalVec3, x, y, z)

namespace
{

using namespace PackedStructArrayTestTypes;

static_assert(Miro::Detail::PackableStruct<Vec3>);
static_assert(Miro::Detail::PackableStruct<Keyframe>);
static_assert(Miro::Detail::PackableStruct<PaddedWeight>);
static_assert(!Miro::Detail::PackableStruct<LabelledPoint>);

Track makeTrack()
{
    return Track {{
        {0.0f, {0.0f, 0.0f, 0.0f, 1.0f}, {0.5f, 0.25f, 0.0f, 1.0f}},
        {1.5f, {0.1f, 0.2f, 0.3f, 0.9f}, {}},
        {3.0f, {-1.0f, 2.0f, -3.0f, 4.0f}, {8.0f, 7.0f, 6.0f, 5.0f}},
    }};
}

auto vectorOfStructsRoundTrips =
    test("Packed struct arrays: a vector of Vec3 round-trips") = []
{
    auto path = Path {{{1.0f, 2.0f, 3.0f}, {-4.5f, 0.0f, 1e9f}, {}}};
    auto bytes = toBinary(path);

    check(createFromBinary<Path>(bytes) == path);
    check(Calder::Document {bytes}.root()["points"].size() == path.points.size());
};

auto packedIsSmallerThanObjects =
    test("Packed struct arrays: records cost their field bytes, not a key each") = []
{
    constexpr auto pointCount = 100;
    constexpr auto bytesPerVec3 = sizeof(float) * 3;
    auto path = Path {std::vector<Vec3>(pointCount, Vec3 {1.0f, 2.0f, 3.0f})};
    auto labelled = LabelledPath {std::vector<LabelledPoint>(pointCount)};

    auto packedSize = toBinary(path).size();

    check(packedSize < pointCount * bytesPerVec3 + pointCount);
    check(packedSize < toBinary(labelled).size());
};

auto nestedStructsRoundTrip =
    test("Packed struct arrays: Keyframes of Quaternions round-trip") = []
{
    auto track = makeTrack();

    check(createFromBinary<Track>(toBinary(track)) == track);
};

auto layoutMismatchMatchesByName =
    test("Packed struct arrays: a changed layout loads field by field by name") = []
{
    auto path = Path {{{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}}};
    auto loaded = createFromBinary<ReorderedCloud>(toBinary(path));

    check(loaded.points.size() == 2);
    check(loaded.points[1].x == 4.0f);
    check(loaded.points[1].z == 6.0f);
    check(loaded.points[1].w == 9.0f);
};

auto typeChangeConvertsWithRangeChecks =
    test("Packed struct arrays: a changed field type converts, or keeps its default "
         "when the value does not fit") = []
{
    constexpr auto countPastInt16 = 70000;
    auto saved = SavedSamples {{{0.5f, 5}, {0.25f, countPastInt16}}};
    auto loaded = createFromBinary<LoadedSamples>(toBinary(saved));

    check(loaded.samples.size() == 2);
    check(loaded.samples[0].level == 0.5);
    check(loaded.samples[0].count == 5);
    check(loaded.samples[1].level == 0.25);
    check(loaded.samples[1].count == LoadedSample {}.count);
};

auto fixedArrayRoundTrips =
    test("Packed struct arrays: a std::array of structs round-trips") = []
{
    auto triangle =
        Triangle {{Vec3 {1.0f, 0.0f, 0.0f}, Vec3 {0.0f, 1.0f, 0.0f}, Vec3 {}}};

    check(createFromBinary<Triangle>(toBinary(triangle)) == triangle);
};

auto paddedStructSkipsPadding =
    test("Packed struct arrays: a padded struct stores no padding bytes") = []
{
    constexpr auto recordBytes = sizeof(std::uint8_t) + sizeof(float);
    static_assert(sizeof(PaddedWeight) > recordBytes);

    auto one = PaddedWeights {{{1, 0.5f}}};
    auto two = PaddedWeights {{{1, 0.5f}, {200, -2.0f}}};

    check(toBinary(two).size() - toBinary(one).size() == recordBytes);
    check(createFromBinary<PaddedWeights>(toBinary(two)) == two);
};

auto packedLoadsIntoUnpackableType = test(
    "Packed struct arrays: records load into a type that can't pack, by name") = []
{
    auto path = Path {{{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}}};
    auto loaded = createFromBinary<LabelledPath>(toBinary(path));

    check(loaded.points.size() == 2);
    check(loaded.points[1].y == 5.0f);
    check(loaded.points[1].label == "unnamed");
};

auto nestedRecordsLoadIntoUnpackableType = test(
    "Packed struct arrays: nested record fields load into an unpackable type") = []
{
    auto track = makeTrack();
    auto loaded = createFromBinary<NamedTrack>(toBinary(track));

    check(loaded.keys.size() == track.keys.size());
    check(loaded.keys[2].time == 3.0f);
    check(loaded.keys[2].value == track.keys[2].value);
    check(loaded.keys[2].name == "key");
};

auto objectArraysLoadIntoPackableType = test(
    "Packed struct arrays: an array of objects still loads into a packable type") =
    []
{
    auto labelled =
        LabelledPath {{{1.0f, 2.0f, 3.0f, "a"}, {4.0f, 5.0f, 6.0f, "b"}}};
    auto bytes = toBinary(labelled);

    check(Calder::Document {bytes}.root()["points"][0].isObject());
    check(createFromBinary<Path>(bytes)
          == Path {{{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}}});
};

auto rawJsonSeesObjects =
    test("Packed struct arrays: a raw JSON load sees an array of objects") = []
{
    auto track = makeTrack();
    auto json = createFromBinary<JSON>(toBinary(track));

    check(Json::print(json) == toJSONString(track));
};

auto missingFieldKeepsDefault = test(
    "Packed struct arrays: a field the stored layout lacks keeps its default") = []
{
    auto saved = SavedSamples {{{0.5f, 3}}};
    auto loaded = createFromBinary<ExtendedSamples>(toBinary(saved));

    check(loaded.samples.size() == 1);
    check(loaded.samples[0].level == 0.5f);
    check(loaded.samples[0].gain == ExtendedSample {}.gain);
    check(loaded.samples[0].count == 3);
};

auto corruptRecordsNeverCrash = test(
    "Packed struct arrays: every truncation and flipped byte loads without a crash") =
    []
{
    auto bytes = toBinary(makeTrack());

    for (auto length = std::size_t {0}; length < bytes.size(); ++length)
    {
        auto truncated = Calder::Buffer(bytes.begin(), bytes.begin() + length);
        createFromBinary<Track>(truncated);
        createFromBinary<NamedTrack>(truncated);
        createFromBinary<JSON>(truncated);
    }

    for (auto position = std::size_t {0}; position < bytes.size(); ++position)
    {
        auto flipped = bytes;
        flipped[position] = static_cast<std::uint8_t>(~flipped[position]);
        createFromBinary<Track>(flipped);
        createFromBinary<NamedTrack>(flipped);
        createFromBinary<JSON>(flipped);
    }

    check(createFromBinary<Track>(bytes) == makeTrack());
};

auto externalStructsPack = test(
    "Packed struct arrays: an external Vec3 list writes the same bytes as Vec3") = []
{
    auto path = Path {{{1.0f, 2.0f, 3.0f}, {-4.5f, 0.0f, 1e9f}}};
    auto externalPath = ExternalPath {};

    for (const auto& point: path.points)
        externalPath.points.push_back({point.x, point.y, point.z});

    check(toBinary(externalPath) == toBinary(path));
};

auto journalFramesCarryPackedRecords =
    test("Packed struct arrays: a journal set frame writes and replays them") = []
{
    auto base = std::map<std::string, Track> {{"a", makeTrack()}};
    auto journal = toBinaryJournal(base);
    auto keyTable = Calder::readJournalKeyTable(journal);
    constexpr auto changedTime = 9.0f;
    auto changed = makeTrack();
    changed.keys.front().time = changedTime;

    auto frame = binaryJournalSet("b", changed, keyTable);
    check(frame.has_value());

    auto bytes = journal;
    bytes.insert(bytes.end(), frame->begin(), frame->end());

    auto loaded = createFromBinary<std::map<std::string, Track>>(bytes);
    check(loaded.size() == 2);
    check(loaded.at("a") == makeTrack());
    check(loaded.at("b") == changed);
};

auto specExampleBytes = test(
    "Packed struct arrays: the spec's worked example 11.7 is what toBinary writes") =
    []
{
    auto rig = Rig {{{1, {0.5f, 1.0f}}, {-2, {2.0f, 0.25f}}}};
    auto expected = Calder::Buffer {
        0x4D, 0x49, 0x42, 0x31, 0x37, 0x00, 0x00, 0x00, 0x07, 0x2A, 0x00, 0x00, 0x00,
        0x00, 0x12, 0x24, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x01, 0x01,
        0x02, 0x02, 0x02, 0x03, 0x08, 0x02, 0x02, 0x04, 0x08, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0xFE, 0xFF, 0x00, 0x00, 0x00, 0x40, 0x00,
        0x00, 0x80, 0x3E, 0x05, 0x07, 0x6D, 0x61, 0x72, 0x6B, 0x65, 0x72, 0x73, 0x02,
        0x69, 0x64, 0x02, 0x61, 0x74, 0x01, 0x78, 0x01, 0x79};

    check(toBinary(rig) == expected);
};

} // namespace
