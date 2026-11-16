// The public API in Calder.h, and the misuse it has to survive: loads that
// fail must say so, Views must not dangle, journals must not lose frames,
// and crafted bytes must not crash a load. Run under AddressSanitizer for
// the lifetime and crafted-input tests to mean anything.

#include "CalderTestHelpers.h"
#include <Calder/Calder.h>
#include <Miro/Json.h>

#include <NanoTest/NanoTest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>

#if defined(__linux__)
    #include <csignal>
    #include <sys/resource.h>
#endif
#include <string>
#include <vector>

using namespace nano;
using namespace Calder;
using namespace CalderTestHelpers;

namespace PublicApiTestTypes
{

struct Point
{
    MIRO_REFLECT(x, y)

    double x = 0.0;
    double y = 0.0;
};

struct Enemy
{
    MIRO_REFLECT(hp, name)

    int hp = 100;
    std::string name = "grunt";
};

struct PointRenamed
{
    MIRO_REFLECT(posX, y)

    double posX = 0.0;
    double y = 0.0;
};

struct WideCount
{
    MIRO_REFLECT(count)

    std::int64_t count = 0;
};

struct IntCount
{
    MIRO_REFLECT(count)

    int count = 0;
};

struct DoubleValue
{
    MIRO_REFLECT(value)

    double value = 0.0;
};

struct Vec3
{
    MIRO_REFLECT(x, y, z)

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    bool operator==(const Vec3&) const = default;
};

struct Mesh
{
    MIRO_REFLECT(name, positions)

    std::string name;
    std::vector<Vec3> positions;
};

struct Cell
{
    MIRO_REFLECT(label, props)

    std::string label;
    std::vector<Vec3> props;
};

struct Strings
{
    MIRO_REFLECT(values)

    std::vector<std::string> values;
};

struct Numbers
{
    MIRO_REFLECT(values)

    std::vector<int> values;
};

struct SmallSigned
{
    MIRO_REFLECT(values)

    std::vector<std::int8_t> values;
};

struct WideSigned
{
    MIRO_REFLECT(values)

    std::vector<int> values;
};

struct ListThenCount
{
    MIRO_REFLECT(list, count)

    std::vector<int> list;
    int count = 0;
};

struct Samples
{
    MIRO_REFLECT(values)

    std::vector<float> values;
};

struct Tree
{
    MIRO_REFLECT(children)

    std::vector<Tree> children;
};

struct TaggedEnemy
{
    MIRO_REFLECT(hp, name, tag)

    int hp = 100;
    std::string name = "grunt";
    std::string tag;
};

} // namespace PublicApiTestTypes

using namespace PublicApiTestTypes;

namespace
{

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        auto randomSuffix = std::to_string(std::random_device {}());
        path =
            std::filesystem::temp_directory_path() / ("calder-test-" + randomSuffix);
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory()
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path getFile(const std::string& name) const
    {
        return path / name;
    }

    std::size_t countFiles() const
    {
        auto entries = std::filesystem::directory_iterator {path};
        return static_cast<std::size_t>(std::distance(begin(entries), end(entries)));
    }

private:
    std::filesystem::path path;
};

void writeRawFile(const std::filesystem::path& file,
                  const std::vector<std::uint8_t>& bytes)
{
    auto stream = std::ofstream {file, std::ios::binary};
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void appendRawBytes(const std::filesystem::path& file,
                    const std::vector<std::uint8_t>& bytes)
{
    auto stream = std::ofstream {file, std::ios::binary | std::ios::app};
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void appendU32(Buffer& bytes, std::uint32_t value)
{
    constexpr auto bitsPerByte = 8;

    for (auto byteIndex = 0; byteIndex < 4; ++byteIndex)
        bytes.push_back(
            static_cast<std::uint8_t>(value >> (bitsPerByte * byteIndex)));
}

// {"children": [{"children": [... `depth` levels ...]}]}, written by hand
// because the writer would need the same depth of C++ recursion.
Buffer makeDeeplyNestedTree(int depth)
{
    constexpr auto childrenKeyId = std::uint8_t {0};
    auto inner = Buffer {static_cast<std::uint8_t>(Calder::Detail::Tag::Null)};

    for (auto level = 0; level < depth; ++level)
    {
        auto array = Buffer {static_cast<std::uint8_t>(Calder::Detail::Tag::Array)};
        appendU32(array,
                  static_cast<std::uint32_t>(inner.size() + sizeof(std::uint32_t)));
        appendU32(array, 1);
        array.insert(array.end(), inner.begin(), inner.end());

        auto object =
            Buffer {static_cast<std::uint8_t>(Calder::Detail::Tag::Object)};
        appendU32(object, static_cast<std::uint32_t>(array.size() + 1));
        object.push_back(childrenKeyId);
        object.insert(object.end(), array.begin(), array.end());
        inner = std::move(object);
    }

    auto document = Buffer {'C', 'L', 'D', '1'};
    appendU32(document,
              static_cast<std::uint32_t>(Calder::Detail::headerSize + inner.size()));
    document.insert(document.end(), inner.begin(), inner.end());
    auto keyName = std::string_view {"children"};
    document.push_back(1);
    document.push_back(static_cast<std::uint8_t>(keyName.size()));
    document.insert(document.end(), keyName.begin(), keyName.end());
    return document;
}

void appendVarint(Buffer& bytes, std::uint64_t value)
{
    constexpr auto continuationBit = std::uint8_t {0x80};
    constexpr auto bitsPerVarintByte = 7;

    while (value >= continuationBit)
    {
        bytes.push_back(static_cast<std::uint8_t>(value) | continuationBit);
        value >>= bitsPerVarintByte;
    }

    bytes.push_back(static_cast<std::uint8_t>(value));
}

// A document whose root object holds `fields` under key id 0, then a key
// table of the given names.
Buffer makeDocument(const Buffer& fields, const std::vector<std::string>& keyNames)
{
    auto object = Buffer {static_cast<std::uint8_t>(Calder::Detail::Tag::Object)};
    appendU32(object, static_cast<std::uint32_t>(fields.size()));
    object.insert(object.end(), fields.begin(), fields.end());

    auto document = Buffer {'C', 'L', 'D', '1'};
    appendU32(
        document,
        static_cast<std::uint32_t>(Calder::Detail::headerSize + object.size()));
    document.insert(document.end(), object.begin(), object.end());
    appendVarint(document, keyNames.size());

    for (const auto& name: keyNames)
    {
        appendVarint(document, name.size());
        document.insert(document.end(), name.begin(), name.end());
    }

    return document;
}

// {"p": [one record]} where the record has `fieldCount` one-byte fields,
// each named by its own key.
Buffer makeWidePackedRecord(std::size_t fieldCount)
{
    auto layout = Buffer {};
    appendVarint(layout, fieldCount);

    for (auto field = std::size_t {0}; field < fieldCount; ++field)
    {
        constexpr auto pathLength = 1;
        appendVarint(layout, pathLength);
        appendVarint(layout, field + 1);
        layout.push_back(
            static_cast<std::uint8_t>(Calder::Detail::PackedFieldType::UInt8));
    }

    constexpr auto recordCount = 1;
    auto payload = Buffer {};
    appendU32(payload, recordCount);
    payload.insert(payload.end(), layout.begin(), layout.end());
    payload.insert(payload.end(), fieldCount, std::uint8_t {7});

    auto fields = Buffer {
        0, static_cast<std::uint8_t>(Calder::Detail::Tag::PackedStructArray)};
    appendU32(fields, static_cast<std::uint32_t>(payload.size()));
    fields.insert(fields.end(), payload.begin(), payload.end());

    auto keyNames = std::vector<std::string> {"p"};

    for (auto field = std::size_t {0}; field < fieldCount; ++field)
        keyNames.push_back("f" + std::to_string(field));

    return makeDocument(fields, keyNames);
}

template <typename T>
concept CanTakeRoot = requires(T&& file) { std::forward<T>(file).getRoot(); };

View getRootOfMovedFile(std::optional<File>& destination)
{
    auto source = File::fromBytes(toBinary(Enemy {42, "boss"}));
    auto root = source.getRoot();
    destination.emplace(std::move(source));
    return root;
}

} // namespace

// --- Types that make misuse a compile error ---

static_assert(!CanTakeRoot<File>, "a View into a temporary File must not compile");
static_assert(CanTakeRoot<File&>);
static_assert(!std::is_copy_constructible_v<File>);
static_assert(!std::is_move_assignable_v<File>,
              "assigning would free bytes Views read");
static_assert(ByteRange<std::string>);
static_assert(ByteRange<std::vector<char>>);
static_assert(ByteRange<std::span<const std::byte>>);
static_assert(!ByteRange<char[12]>, "a file name literal is not bytes");
static_assert(!ByteRange<std::vector<int>>);

// --- Loads that fail say so ---

auto garbageIsNotCalderData =
    test("Public API: garbage bytes are not Calder data") = []
{
    auto enemy = Enemy {7, "kept"};
    auto garbage = Buffer {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto result = fromBinary(enemy, garbage);

    check(result.status == LoadStatus::NotCalderData);
    check(!result.hasLoaded());
    check(result.hasProblems());
    check(enemy.hp == 7 && enemy.name == "kept");
    check(fromBinary(enemy, Buffer {}).status == LoadStatus::NotCalderData);
};

auto jsonTextIsNotCalderData =
    test("Public API: JSON text passed as bytes loads nothing") = []
{
    auto json = std::string {R"({"hp":5,"name":"boss"})"};

    check(!createFromBinary<Enemy>(json).has_value());
};

auto truncatedIsDamaged = test("Public API: a file cut short is Damaged") = []
{
    auto bytes = toBinary(Mesh {"rock", {{1, 2, 3}, {4, 5, 6}}});

    for (auto size = Calder::Detail::headerSize; size < bytes.size(); ++size)
    {
        auto prefix = std::span {bytes}.first(size);
        auto mesh = Mesh {};
        check(fromBinary(mesh, prefix).status == LoadStatus::Damaged);
    }
};

auto otherStructIsReported =
    test("Public API: bytes of another struct report missing fields") = []
{
    auto enemy = Enemy {};
    auto result = fromBinary(enemy, toBinary(Point {1, 2}));

    check(result.hasLoaded());
    check(result.hasProblems());
    check(result.missingFieldCount == 2);
};

auto renamedFieldIsMissing =
    test("Public API: a renamed field counts as missing") = []
{
    auto point = PointRenamed {};
    auto result = fromBinary(point, toBinary(Point {1, 2}));

    check(result.missingFieldCount == 1);
    check(point.y == 2.0);
};

auto numberThatDoesNotFitIsReported =
    test("Public API: a number too big for its field is reported, not loaded") = []
{
    constexpr auto tooBigForInt = std::int64_t {1} << 40;
    auto narrow = IntCount {3};
    auto result = fromBinary(narrow, toBinary(WideCount {tooBigForInt}));

    check(narrow.count == 3);
    check(result.fieldProblemCount == 1);
    check(result.fieldProblems.at(0).fieldPath == "count");
    check(result.fieldProblems.at(0).kind == FieldProblem::Kind::NumberDidNotFit);
};

auto packedNumberThatDoesNotFitIsReported =
    test("Public API: a number too big for a packed list element is reported") = []
{
    constexpr auto tooBigForInt8 = 1000;
    auto loaded = SmallSigned {};
    auto result = fromBinary(loaded, toBinary(WideSigned {{1, tooBigForInt8}}));

    check(result.fieldProblemCount == 1);
    check(result.fieldProblems.at(0).fieldPath == "values[1]");
    check(result.fieldProblems.at(0).kind == FieldProblem::Kind::NumberDidNotFit);
};

auto fieldProblemsAreCapped =
    test("Public API: the field problem list is capped") = []
{
    constexpr auto mismatchCount = 1000;
    auto written = Strings {std::vector<std::string>(mismatchCount, "text")};
    auto loaded = Numbers {};
    auto result = fromBinary(loaded, toBinary(written));

    check(result.fieldProblemCount == mismatchCount);
    check(result.fieldProblems.size() < mismatchCount);
};

auto asIntegerNeverOverflows =
    test("Public API: asInteger on a huge double is the fallback") = []
{
    auto file = File::fromBytes(toBinary(DoubleValue {1e300}));
    constexpr auto fallback = std::int64_t {-1};

    check(file.getRoot()["value"].asInteger(fallback) == fallback);
    check(file.getRoot()["value"].asNumber() == 1e300);
};

// --- Byte inputs ---

auto anyByteRangeLoads =
    test("Public API: any contiguous bytes load without casts") = []
{
    auto bytes = toBinary(Enemy {5, "boss"});
    auto asText = std::string(bytes.begin(), bytes.end());
    auto asChars = std::vector<char>(bytes.begin(), bytes.end());
    auto asStdBytes = std::as_bytes(std::span {bytes});

    check(createFromBinary<Enemy>(asText)->name == "boss");
    check(createFromBinary<Enemy>(asChars)->hp == 5);
    check(createFromBinary<Enemy>(asStdBytes)->hp == 5);
};

// --- Files ---

auto filesRoundTrip =
    test("Public API: saveToFile and createFromFile round-trip") = []
{
    auto directory = TemporaryDirectory {};
    auto file = directory.getFile("save.calder");

    check(saveToFile(Enemy {3, "saved"}, file));
    check(saveToFile(Enemy {4, "replaced"}, file));
    check(directory.countFiles() == 1);
    check(createFromFile<Enemy>(file)->name == "replaced");
};

auto missingFile = test("Public API: a missing file is FileMissing") = []
{
    auto directory = TemporaryDirectory {};
    auto enemy = Enemy {};

    check(loadFromFile(enemy, directory.getFile("nope.calder")).status
          == LoadStatus::FileMissing);
    check(!createFromFile<Enemy>(directory.getFile("nope.calder")).has_value());
    check(File::open(directory.getFile("nope.calder")).getStatus()
          == LoadStatus::FileMissing);
};

auto emptyFile = test("Public API: an empty file is not Calder data") = []
{
    auto directory = TemporaryDirectory {};
    auto file = directory.getFile("empty.calder");
    writeRawFile(file, {});

    check(File::open(file).getStatus() == LoadStatus::NotCalderData);
};

// --- Reading part of a file ---

auto fileOpenReadsOneCell =
    test("Public API: one cell of a map decodes on its own") = []
{
    auto directory = TemporaryDirectory {};
    auto path = directory.getFile("world.calder");
    auto world = std::map<std::string, Cell> {{"0_0", {"origin", {}}},
                                              {"12_7", {"far", {{1, 2, 3}}}}};
    check(saveToFile(world, path));

    auto file = File::open(path);
    check(file.getStatus() == LoadStatus::Loaded);

    auto cell = createFromBinary<Cell>(file.getRoot()["12_7"]);
    check(cell.has_value());
    check(cell->label == "far");
    check(cell->props.size() == 1);

    auto missing = Cell {};
    check(fromBinary(missing, file.getRoot()["99_99"]).status
          == LoadStatus::ValueMissing);
    check(!createFromBinary<Cell>(file.getRoot()["99_99"]).has_value());
};

auto viewReadsPackedRecords =
    test("Public API: a View steps into lists of number-only structs") = []
{
    auto file = File::fromBytes(toBinary(Mesh {"rock", {{1, 2, 3}, {4, 5, 6}}}));
    auto positions = file.getRoot()["positions"];

    check(positions.isArray());
    check(positions.getSize() == 2);
    check(positions[1].isObject());
    check(positions[1]["y"].asNumber() == 5.0);
    check(positions[1].getKeys() == std::vector<std::string_view> {"x", "y", "z"});
    check(!positions[1]["w"].isValid());
    check(!positions[2].isValid());
    check(createFromBinary<Vec3>(positions[1]) == Vec3 {4, 5, 6});
};

auto viewTypeQueries =
    test("Public API: View type queries match the stored value") = []
{
    auto file = File::fromBytes(toBinary(Enemy {5, "boss"}));
    auto root = file.getRoot();

    check(root.isObject() && !root.isArray());
    check(root["hp"].isNumber() && root["hp"].isInteger());
    check(root["name"].isString() && root["name"].asString() == "boss");
    check(root["name"].asInteger(-1) == -1);
    check(root["missing"].asString("fallback") == "fallback");
    check(root.getKeys() == std::vector<std::string_view> {"hp", "name"});
};

// --- Lifetime: these crashed under AddressSanitizer before ---

auto movingFileKeepsViews =
    test("Public API: Views stay valid when their File moves") = []
{
    auto destination = std::optional<File> {};
    auto root = getRootOfMovedFile(destination);

    check(root["hp"].asInteger() == 42);
    check(root["name"].asString() == "boss");
};

auto filesInVectorKeepViews =
    test("Public API: Views stay valid when a vector of Files grows") = []
{
    auto files = std::vector<File> {};
    files.push_back(File::fromBytes(toBinary(Enemy {42, "boss"})));
    auto root = files[0].getRoot();

    for (auto i = 0; i < 10; ++i)
        files.push_back(File::fromBytes(toBinary(Enemy {i, "other"})));

    check(root["hp"].asInteger() == 42);
};

auto fileOwnsItsBytes =
    test("Public API: a File keeps its bytes when the source changes") = []
{
    auto bytes = toBinary(std::map<std::string, Enemy> {{"a", {42, "boss"}}});
    auto file = File::fromBytes(bytes);
    auto entry = file.getRoot()["a"];
    bytes.assign(bytes.size() * 64, 0);

    check(entry["hp"].asInteger() == 42);
};

// --- Journals ---

auto journalWriterRoundTrip =
    test("Public API: a JournalWriter appends sets and removes") = []
{
    auto directory = TemporaryDirectory {};
    auto path = directory.getFile("store.calder");
    auto journal = JournalWriter::open(path);

    check(journal.getStatus() == LoadStatus::Loaded);
    check(journal.appendSet("a", Enemy {1, "a"}));
    check(journal.appendSet("b", Enemy {2, "b"}));
    check(journal.appendRemove("a"));

    auto loaded = createFromFile<std::map<std::string, Enemy>>(path);
    check(loaded.has_value());
    check(loaded->size() == 1);
    check(loaded->at("b").hp == 2);
};

auto journalWriterNewFieldName =
    test("Public API: a new field name extends the base and keeps every frame") = []
{
    auto directory = TemporaryDirectory {};
    auto path = directory.getFile("store.calder");
    auto journal = JournalWriter::open(path);

    check(journal.appendSet("a", Enemy {1, "a"}));
    check(journal.appendSet("b", TaggedEnemy {2, "b", "elite"}));
    check(journal.appendSet("c", Enemy {3, "c"}));

    auto loaded = std::map<std::string, TaggedEnemy> {};
    auto result = loadFromFile(loaded, path);
    check(result.hasLoaded());
    check(loaded.size() == 3);
    check(loaded.at("a").hp == 1);
    check(loaded.at("b").tag == "elite");
    check(loaded.at("c").name == "c");
};

auto journalWriterTrimsTornTail =
    test("Public API: reopening a journal with a torn tail keeps later frames") = []
{
    auto directory = TemporaryDirectory {};
    auto path = directory.getFile("store.calder");

    {
        auto journal = JournalWriter::open(path);
        check(journal.appendSet("a", Enemy {1, "a"}));
    }

    constexpr auto tornFrameByteCount = 3;
    auto torn = Calder::Detail::makeJournalRemoveFrame("a");
    torn.resize(tornFrameByteCount);
    appendRawBytes(path, torn);

    auto journal = JournalWriter::open(path);
    check(journal.appendSet("b", Enemy {2, "b"}));

    auto loaded = std::map<std::string, Enemy> {};
    auto result = loadFromFile(loaded, path);
    check(result.lostJournalFrameCount == 0);
    check(loaded.size() == 2);
};

auto journalWriterRewrite =
    test("Public API: rewrite replaces a journal with one base") = []
{
    auto directory = TemporaryDirectory {};
    auto path = directory.getFile("store.calder");
    auto journal = JournalWriter::open(path);
    check(journal.appendSet("a", Enemy {1, "a"}));

    check(journal.rewrite(std::map<std::string, Enemy> {{"z", {26, "z"}}}));
    check(journal.appendSet("y", Enemy {25, "y"}));

    auto loaded = createFromFile<std::map<std::string, Enemy>>(path);
    check(loaded->size() == 2);
    check(!loaded->contains("a"));
};

auto journalWriterRefusesForeignFile =
    test("Public API: a JournalWriter refuses a file that is not Calder") = []
{
    auto directory = TemporaryDirectory {};
    auto path = directory.getFile("notes.txt");
    writeRawFile(path, {'h', 'e', 'l', 'l', 'o'});
    auto journal = JournalWriter::open(path);

    check(journal.getStatus() == LoadStatus::NotCalderData);
    check(!journal.appendSet("a", Enemy {}));
};

auto fileOpenRefusesJournal =
    test("Public API: File::open on a journal says IsJournal") = []
{
    auto directory = TemporaryDirectory {};
    auto path = directory.getFile("store.calder");
    check(JournalWriter::open(path).appendSet("a", Enemy {}));

    check(File::open(path).getStatus() == LoadStatus::IsJournal);
};

auto journalIntoMapWithComparator =
    test("Public API: a journal replays into a map with any comparator") = []
{
    auto directory = TemporaryDirectory {};
    auto path = directory.getFile("store.calder");
    auto journal = JournalWriter::open(path);
    check(journal.appendSet("a", Enemy {1, "a"}));
    check(journal.appendSet("b", Enemy {2, "b"}));

    auto loaded = std::map<std::string, Enemy, std::less<>> {};
    check(loadFromFile(loaded, path).hasLoaded());
    check(loaded.size() == 2);
};

auto journalIntoNonMapLosesFrames = test(
    "Public API: a journal loaded into a non-map reports its frames as lost") = []
{
    auto bytes =
        concatenateBuffers({makeJournalOf(Enemy {1, "base"}), makeRemoveFrame("a")});
    auto enemy = Enemy {};
    auto result = fromBinary(enemy, bytes);

    check(result.hasLoaded());
    check(result.lostJournalFrameCount == 1);
    check(enemy.name == "base");
};

auto emptyListsRoundTrip = test("Public API: empty lists save and load") = []
{
    auto mesh = createFromBinary<Mesh>(toBinary(Mesh {"empty", {}}));
    auto samples = createFromBinary<Samples>(toBinary(Samples {}));

    check(mesh.has_value() && mesh->positions.empty());
    check(samples.has_value() && samples->values.empty());
};

#if !defined(_WIN32)
auto saveKeepsPermissions =
    test("Public API: saveToFile keeps the replaced file's permissions") = []
{
    auto directory = TemporaryDirectory {};
    auto file = directory.getFile("private.calder");
    check(saveToFile(Enemy {}, file));

    constexpr auto ownerOnly =
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::filesystem::permissions(file, ownerOnly);
    check(saveToFile(Enemy {1, "again"}, file));

    check(std::filesystem::status(file).permissions() == ownerOnly);
    check(createFromFile<Enemy>(file)->name == "again");
};
#endif

#if defined(__linux__)
// Caps the size of files this process may write, so an append fails partway
// as it would on a full disk.
class FileSizeLimit
{
public:
    explicit FileSizeLimit(rlim_t largestFileSize)
    {
        getrlimit(RLIMIT_FSIZE, &previousLimit);
        previousHandler = std::signal(SIGXFSZ, SIG_IGN);
        auto limit = previousLimit;
        limit.rlim_cur = largestFileSize;
        setrlimit(RLIMIT_FSIZE, &limit);
    }

    ~FileSizeLimit()
    {
        setrlimit(RLIMIT_FSIZE, &previousLimit);
        std::signal(SIGXFSZ, previousHandler);
    }

private:
    rlimit previousLimit {};
    void (*previousHandler)(int) = nullptr;
};

auto failedAppendLeavesJournalIntact =
    test("Public API: a failed journal append leaves no part of its frame") = []
{
    auto directory = TemporaryDirectory {};
    auto path = directory.getFile("store.calder");
    auto journal = JournalWriter::open(path);
    check(journal.appendSet("a", Enemy {1, "a"}));

    constexpr auto bytesOfRoomLeft = 4;
    auto sizeBefore = std::filesystem::file_size(path);

    {
        auto limit =
            FileSizeLimit {static_cast<rlim_t>(sizeBefore + bytesOfRoomLeft)};
        check(!journal.appendSet("b", Enemy {2, std::string(64, 'b')}));
    }

    check(std::filesystem::file_size(path) == sizeBefore);
    check(journal.appendSet("c", Enemy {3, "c"}));

    auto loaded = std::map<std::string, Enemy> {};
    auto result = loadFromFile(loaded, path);
    check(result.lostJournalFrameCount == 0);
    check(loaded.size() == 2 && loaded.contains("c"));
};
#endif

// --- Crafted input ---

auto oversizedPackedIntegerCountIsDamaged =
    test("Public API: a packed integer list longer than its payload is Damaged") = []
{
    constexpr auto listKeyId = std::uint8_t {0};
    constexpr auto countKeyId = std::uint8_t {1};
    constexpr auto claimedCount = 2;
    constexpr auto storedCount = 9;
    auto list = Buffer {0x80, 0x01};

    auto fields = Buffer {
        listKeyId, static_cast<std::uint8_t>(Calder::Detail::Tag::PackedInt32)};
    appendU32(fields,
              static_cast<std::uint32_t>(sizeof(std::uint32_t) + list.size()));
    appendU32(fields, claimedCount);
    fields.insert(fields.end(), list.begin(), list.end());
    fields.push_back(countKeyId);
    fields.push_back(static_cast<std::uint8_t>(Calder::Detail::Tag::Int));
    appendVarint(fields, storedCount * 2);

    auto loaded = ListThenCount {};
    auto result = fromBinary(loaded, makeDocument(fields, {"list", "count"}));

    check(result.status == LoadStatus::Damaged);
    check(loaded.count == storedCount);
};

auto arrayCountBeyondItsBytesIsDamaged =
    test("Public API: an array claiming more elements than bytes is Damaged") = []
{
    constexpr auto claimedCount = 1000000;
    constexpr auto nullCount = 10;
    auto nulls =
        Buffer(nullCount, static_cast<std::uint8_t>(Calder::Detail::Tag::Null));

    auto fields = Buffer {0, static_cast<std::uint8_t>(Calder::Detail::Tag::Array)};
    appendU32(fields,
              static_cast<std::uint32_t>(sizeof(std::uint32_t) + nulls.size()));
    appendU32(fields, claimedCount);
    fields.insert(fields.end(), nulls.begin(), nulls.end());

    auto loaded = Mesh {};
    auto result = fromBinary(loaded, makeDocument(fields, {"positions"}));

    check(result.status == LoadStatus::Damaged);
    check(loaded.positions.empty());
};

auto widePackedRecordLoadsInLinearTime = test(
    "Public API: a packed record with 40,000 fields loads in well under a second") =
    []
{
    constexpr auto fieldCount = std::size_t {40000};
    constexpr auto timeLimit = std::chrono::seconds {2};
    auto bytes = makeWidePackedRecord(fieldCount);
    auto start = std::chrono::steady_clock::now();

    auto loaded = Miro::JSON {};
    auto result = fromBinary(loaded, bytes);
    auto file = File::fromBytes(bytes);
    auto keys = file.getRoot()["p"][0].getKeys();

    check(std::chrono::steady_clock::now() - start < timeLimit);
    check(result.hasLoaded());
    check(keys.size() == fieldCount);
};

auto deepNestingIsDamaged =
    test("Public API: nesting 10,000 levels deep is Damaged") = []
{
    constexpr auto craftedDepth = 10000;
    auto tree = Tree {};

    check(fromBinary(tree, makeDeeplyNestedTree(craftedDepth)).status
          == LoadStatus::Damaged);
};

auto moderateNestingLoads = test("Public API: nesting 100 levels deep loads") = []
{
    constexpr auto moderateDepth = 100;
    auto tree = Tree {};

    check(fromBinary(tree, makeDeeplyNestedTree(moderateDepth)).hasLoaded());
    check(tree.children.size() == 1);
};

auto randomCorruptionNeverCrashes =
    test("Public API: randomly corrupted files never crash a load or a View") = []
{
    auto level = std::map<std::string, Enemy> {};

    for (auto i = 0; i < 20; ++i)
        level["k" + std::to_string(i)] = {
            i, std::string(static_cast<std::size_t>(i), 'x')};

    auto good = toBinary(level);
    auto random = std::mt19937 {1};
    constexpr auto corruptionCount = 2000;
    constexpr auto maxFlipsPerFile = 4;

    for (auto iteration = 0; iteration < corruptionCount; ++iteration)
    {
        auto bad = good;
        auto flipCount = 1 + random() % maxFlipsPerFile;

        for (auto flip = std::uint32_t {0}; flip < flipCount; ++flip)
            bad[random() % bad.size()] = static_cast<std::uint8_t>(random());

        auto loaded = std::map<std::string, Enemy> {};
        static_cast<void>(fromBinary(loaded, bad));
        auto file = File::fromBytes(bad);
        auto root = file.getRoot();

        for (const auto& [key, enemy]: level)
            check(root[key]["name"].asString().size() <= bad.size());
    }
};
