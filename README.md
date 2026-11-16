# Calder

Calder is a binary format for reflected C++ data: fast to save and load, and
readable from any language without the C++ code. It is meant for apps and
games, such as an editor saving a level in the format the game will run.

A file holds one value: objects with named fields, arrays, maps, strings,
numbers, booleans and null. Each field name is stored once per file and fields
refer to it by a small id. Arrays of fixed-width numbers are stored as one
block of raw values. Every container records its
size, so a reader can skip it without decoding it.

- [`SPEC.md`](SPEC.md): the format, byte by byte, with worked examples.
- [`python/mib`](python/mib): a decoder written from the spec alone, to show
  the format is a protocol, not a C++ memory dump. Standard library only.

```sh
cd python && python3 -m mib some-file.mib     # prints the file as JSON
python3 -m unittest mib.test_mib              # the decoder's tests
```

The C++ encoder lives in emberstore (`emberstore/Binary`) and reflects plain
structs through [Miro](https://github.com/eyalamirmusic/Miro). Files start
with the magic `MIB1` (a document) or `MIBJ` (a journal); the magic will follow
the name in a later version.
