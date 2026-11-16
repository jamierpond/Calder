# Calder

```cpp
namespace Calder
{
// Save and load a whole value
std::vector<std::uint8_t> toBinary(const T& value);
std::optional<T> createFromBinary<T>(const ByteRange auto& bytes);
LoadResult fromBinary(T& value, const ByteRange auto& bytes);

// Files (saving writes a temporary file, then renames it over the old one)
bool saveToFile(const T& value, const std::filesystem::path& file);
std::optional<T> createFromFile<T>(const std::filesystem::path& file);
LoadResult loadFromFile(T& value, const std::filesystem::path& file);

// Read part of a file without loading all of it
class File;  // File::open(path), getStatus(), getRoot()
class View;  // view["key"], view[index], getSize(), getKeys(), isString(), asInteger() ...
std::optional<T> createFromBinary<T>(const View& view);

// Append-only stores
class JournalWriter;  // open(path), appendSet(key, value), appendRemove(key), rewrite(entries)
}
```

```cpp
Calder::saveToFile(save, "save.calder");

if (auto save = Calder::createFromFile<SaveGame>("save.calder"))
    startGame(*save);

auto world = Calder::File::open("world.calder");
if (auto chunk = Calder::createFromBinary<WorldChunk>(world.getRoot()["12_7"]))
    streamIn(*chunk);
```

[`Calder/Calder.h`](Calder/Calder.h) is the whole API, one line each.

Calder is a binary format for reflected C++ data: fast to save and load, and
readable from any language without the C++ code. It is meant for apps and
games, such as an editor saving a level in the format the game will run.
Structs are reflected through [Miro](https://github.com/eyalamirmusic/Miro):

```cpp
struct SaveGame
{
    MIRO_REFLECT(playerName, level, position)

    std::string playerName;
    int level = 1;
    Vec3 position;
};
```

## When a load fails

Every load says what happened. `createFrom...` returns `std::nullopt` unless
the value loaded; `fromBinary` and `loadFromFile` return a `LoadResult`:

```cpp
auto save = SaveGame {};
auto result = Calder::loadFromFile(save, "save.calder");

if (!result.hasLoaded())        // FileMissing, NotCalderData, Damaged, ...
    showError(result.status);
else if (result.hasProblems())  // loaded, but fields were missing or refused
    logOlderSave(result.missingFieldCount, result.fieldProblems);
```

A field the file lacks keeps the value it had, a field stored as another type
is skipped, and a number too big for its field is refused; each is counted,
except a refused number in a field narrower than `int` or `double`, which
Miro refuses before Calder sees it.
Garbage, a cut-short file and nesting deeper than 256 levels never crash a
load.

## The format

A file holds one value: objects with named fields, arrays, maps, strings,
numbers, booleans and null. Each field name is stored once per file and fields
refer to it by a small id. Arrays of fixed-width numbers, and arrays of structs
made only of numbers, are stored as one block of raw values. Every container
records its size, so a reader can skip it without decoding it. Calder files
end in `.calder` and start with `CLD1` (a document) or `CLDJ` (a journal: a
document plus appended changes).

- [`SPEC.md`](SPEC.md): the format, byte by byte, with worked examples.
- [`python/calder`](python/calder): a decoder written from the spec alone, to
  show the format is a protocol, not a C++ memory dump. Standard library only.
  The tests check it reads every sample file the way the C++ does.

```sh
cd python && python3 -m calder some-file.calder   # prints the file as JSON
```

Tools that read the bytes directly (a byte-layout viewer, a store with its own
journal files) include [`Calder/Format.h`](Calder/Format.h); what it adds lives
in `Calder::Detail` and changes without notice.

## Building

Calder needs C++20. CMake fetches Miro and the test framework with
[CPM](https://github.com/cpm-cmake/CPM.cmake):

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build && ctest --test-dir build
```

Use it from another CMake project with
`CPMAddPackage(NAME Calder GITHUB_REPOSITORY jamierpond/Calder GIT_TAG <sha>)`
and link `calder`.

Calder pins the `jp/calder-emberstore` branch of
[jamierpond/Miro](https://github.com/jamierpond/Miro) by commit: Miro's
`develop` plus the pull requests to eyalamirmusic/Miro that Calder is built
on and that are not merged yet: reflector hooks for packed number arrays
(`visitPacked`, #26), missing lists keeping their defaults on load (#27),
range-checked number loading (#28), compile-time type names (#29) and packed
struct arrays, including `MIRO_REFLECT_EXTERNAL` structs (#32).

Calder is released under the [MIT license](LICENSE).
