# Calder

Calder is a binary format for reflected C++ data: fast to save and load, and
readable from any language without the C++ code. It is meant for apps and
games, such as an editor saving a level in the format the game will run.

A file holds one value: objects with named fields, arrays, maps, strings,
numbers, booleans and null. Each field name is stored once per file and fields
refer to it by a small id. Arrays of fixed-width numbers, and arrays of structs
made only of numbers, are stored as one block of raw values. Every container
records its size, so a reader can skip it without decoding it.

- [`SPEC.md`](SPEC.md): the format, byte by byte, with worked examples.
- [`python/mib`](python/mib): a decoder written from the spec alone, to show
  the format is a protocol, not a C++ memory dump. Standard library only.

```sh
cd python && python3 -m mib some-file.mib     # prints the file as JSON
python3 -m unittest mib.test_mib              # the decoder's tests
```

The C++ encoder and decoder are in [`Calder/`](Calder) and reflect plain
structs through [Miro](https://github.com/eyalamirmusic/Miro):

```cpp
#include <Calder/Calder.h>

struct Point
{
    MIRO_REFLECT(x, y)

    double x = 0.0;
    double y = 0.0;
};

auto bytes = Calder::toBinary(Point {1.0, 2.0});
auto point = Calder::createFromBinary<Point>(bytes);
```

Calder needs a Miro with the reflector hooks it is built on (`visitPacked`,
range-checked number loading, missing lists keeping their defaults) and packed
struct arrays (`visitPackedStructs`). Neither is in Miro's `develop` yet, so
`CMakeLists.txt` pins the `jp/packed-structs-on-hooks` branch of
[jamierpond/Miro](https://github.com/jamierpond/Miro) by commit. The packed
struct change is under review as
[eyalamirmusic/Miro#32](https://github.com/eyalamirmusic/Miro/pull/32).

Build and test with CMake (Miro and the test framework are fetched by
[CPM](https://github.com/cpm-cmake/CPM.cmake)):

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build && ctest --test-dir build
```

Use it from another CMake project with
`CPMAddPackage(NAME Calder GITHUB_REPOSITORY jamierpond/Calder GIT_TAG <sha>)`
and link `calder`. Files start
with the magic `MIB1` (a document) or `MIBJ` (a journal); the magic will follow
the name in a later version.
