# calder

A pure-Python 3 (standard library only) decoder for Calder files: `CLD1` documents and `CLDJ` journals. It turns a file into JSON.

It was written from [`SPEC.md`](../../SPEC.md) alone, without reading the C++ code, to show that the format can be implemented from its spec.

```bash
cd python
python3 -m calder FILE.calder         # prints JSON; journal problems go to stderr
python3 -m unittest calder.test_calder # spec examples and malformed inputs
```
