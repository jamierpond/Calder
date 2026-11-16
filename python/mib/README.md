# mib

A pure-Python 3 (standard library only) decoder for Calder files: `MIB1` documents and `MIBJ` journals. It turns a file into JSON.

It was written from `docs/binary-format-spec.md` alone, without reading the C++ code, to show that the format can be implemented from its spec.

```bash
cd tools/python
python3 -m mib FILE.ember          # prints JSON; journal problems go to stderr
python3 -m unittest mib.test_mib   # spec examples and malformed inputs
```
