// Writes one Calder file per sample into the directory given on the command
// line, each beside a .json file holding the tree Calder's own C++ reads
// back from it. CompareWithPython.py decodes the same files with the Python
// decoder and checks both trees agree, so the two readers stay one format.

#include <Calder/Calder.h>
#include <Miro/Json.h>
#include <Miro/Reflect.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace CrossCheckSamples
{

enum class Weather
{
    Clear,
    Rain,
    Snow,
};

enum class Layer : int
{
    Background = -1,
    World = 0,
    Interface = 7,
};

struct Vec3
{
    MIRO_REFLECT(x, y, z)

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Transform
{
    MIRO_REFLECT(position, scale)

    Vec3 position;
    double scale = 1.0;
};

struct Keyframe
{
    MIRO_REFLECT(frame, value, flags)

    std::int32_t frame = 0;
    double value = 0.0;
    std::uint8_t flags = 0;
};

struct Enemy
{
    MIRO_REFLECT(name, hp, alive, home, loot, nickname, weapon)

    std::string name;
    int hp = 0;
    bool alive = true;
    Transform home;
    std::vector<std::string> loot;
    std::optional<std::string> nickname;
    std::variant<int, std::string> weapon = 0;
};

struct Level
{
    MIRO_REFLECT(title,
                 weather,
                 layer,
                 enemies,
                 byName,
                 path,
                 placements,
                 animation,
                 heights,
                 samples,
                 bytes,
                 shorts,
                 bigNumbers,
                 smallSigned,
                 unsignedShorts,
                 ratios,
                 empty,
                 nothing,
                 counts)

    std::string title;
    Weather weather = Weather::Clear;
    Layer layer = Layer::World;
    std::vector<Enemy> enemies;
    std::map<std::string, Enemy> byName;
    std::vector<Vec3> path;
    std::vector<Transform> placements;
    std::vector<Keyframe> animation;
    std::vector<float> heights;
    std::vector<double> samples;
    std::vector<std::uint8_t> bytes;
    std::vector<std::int16_t> shorts;
    std::vector<std::int64_t> bigNumbers;
    std::vector<std::int8_t> smallSigned;
    std::vector<std::uint16_t> unsignedShorts;
    std::vector<std::int32_t> ratios;
    std::vector<int> empty;
    std::optional<int> nothing;
    std::map<std::string, int> counts;
};

Enemy makeEnemy(const std::string& name, int hp)
{
    return Enemy {.name = name,
                  .hp = hp,
                  .alive = hp > 0,
                  .home = {.position = {1.5f, -2.0f, 3.25f}, .scale = 0.1},
                  .loot = {"gold", "épée", "鍵"},
                  .nickname = std::nullopt,
                  .weapon = std::string {"bow"}};
}

Level makeLevel()
{
    constexpr auto largeInteger = std::int64_t {9007199254740993};

    auto boss = makeEnemy("Ünïcødé boss 🐉", 500);
    boss.nickname = "the big one";
    boss.weapon = 42;

    return Level {
        .title = "River \"crossing\"\n",
        .weather = Weather::Rain,
        .layer = Layer::Interface,
        .enemies = {makeEnemy("grunt", 10), boss, makeEnemy("ghost", 0)},
        .byName = {{"boss", boss}, {"", makeEnemy("nameless", 1)}},
        .path = {{0, 0, 0}, {1.5f, 2.5f, -3.5f}, {1e10f, -1e-10f, 0.1f}},
        .placements = {{{1, 2, 3}, 2.0}, {{4, 5, 6}, 0.3}},
        .animation = {{0, 0.5, 1}, {24, -1e300, 255}},
        .heights = {0.0f, 0.25f, -7.5f},
        .samples = {0.1, 1.0 / 3.0, -2.0},
        .bytes = {0, 1, 255},
        .shorts = {-32768, 0, 32767},
        .bigNumbers = {largeInteger, -largeInteger, 0},
        .smallSigned = {-128, 127},
        .unsignedShorts = {0, 65535},
        .ratios = {-2147483647, 2147483647},
        .empty = {},
        .nothing = std::nullopt,
        .counts = {{"a", 1}, {"b", -2}},
    };
}

} // namespace CrossCheckSamples

namespace
{

using namespace CrossCheckSamples;

Miro::Json::Value convertViewToJson(const Calder::View& view);

Miro::Json::Value convertArrayViewToJson(const Calder::View& view)
{
    auto array = Miro::Json::Array {};

    for (auto index = std::size_t {0}; index < view.getSize(); ++index)
        array.add(convertViewToJson(view[index]));

    return array;
}

Miro::Json::Value convertObjectViewToJson(const Calder::View& view)
{
    auto object = Miro::Json::Object {};

    for (auto key: view.getKeys())
        object.emplace(std::string {key}, convertViewToJson(view[key]));

    return object;
}

Miro::Json::Value convertViewToJson(const Calder::View& view)
{
    if (view.isBool())
        return view.asBool();

    if (view.isInteger())
        return view.asInteger();

    if (view.isNumber())
        return view.asNumber();

    if (view.isString())
        return std::string {view.asString()};

    if (view.isArray())
        return convertArrayViewToJson(view);

    if (view.isObject())
        return convertObjectViewToJson(view);

    return nullptr;
}

bool writeText(const std::filesystem::path& file, const std::string& text)
{
    auto stream = std::ofstream {file};
    stream << text;
    return static_cast<bool>(stream);
}

bool writeDocumentSample(const std::filesystem::path& directory,
                         const std::string& name,
                         const auto& value)
{
    auto file = directory / (name + ".calder");

    if (!Calder::saveToFile(value, file))
        return false;

    auto opened = Calder::File::open(file);
    auto tree = convertViewToJson(opened.getRoot());
    return writeText(directory / (name + ".json"), Miro::Json::print(tree));
}

bool writeJournalSample(const std::filesystem::path& directory)
{
    auto file = directory / "journal.calder";
    auto journal = Calder::JournalWriter::open(file);
    auto isWritten = journal.appendSet("grunt", makeEnemy("grunt", 10))
                     && journal.appendSet("ghost", makeEnemy("ghost", 0))
                     && journal.appendRemove("grunt")
                     && journal.appendSet("path", Level {.path = {{1, 2, 3}}})
                     && journal.appendSet("ghost", makeEnemy("ghost", 99));

    auto replayed = Calder::createFromFile<std::map<std::string, Miro::JSON>>(file);

    if (!isWritten || !replayed)
        return false;

    return writeText(directory / "journal.json", Miro::toJSONString(*replayed));
}

bool writeAllSamples(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
    return writeDocumentSample(directory, "level", makeLevel())
           && writeDocumentSample(directory, "enemy", makeEnemy("solo", 3))
           && writeDocumentSample(directory, "number", 2.5)
           && writeDocumentSample(directory, "text", std::string {"plain"})
           && writeDocumentSample(directory, "list", std::vector<Vec3> {{1, 2, 3}})
           && writeJournalSample(directory);
}

} // namespace

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 2)
    {
        std::fprintf(stderr, "usage: CalderCrossCheckWriter <output directory>\n");
        return 2;
    }

    if (!writeAllSamples(arguments[1]))
    {
        std::fprintf(stderr,
                     "CalderCrossCheckWriter: could not write the samples\n");
        return 1;
    }

    return 0;
}
