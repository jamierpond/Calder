"""Runs the C++ sample writer, decodes every file it wrote with the Python
decoder, and checks the tree matches the one the C++ read back. Numbers
compare by value (an int equals the same float), and key order is ignored.

usage: CompareWithPython.py <CalderCrossCheckWriter> <scratch directory>
"""

import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "python"))

from calder import JournalReport, decode  # noqa: E402


def describe_difference(path, cpp, python):
    return f"{path or '<root>'}: C++ read {cpp!r}, Python read {python!r}"


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def find_difference(path, cpp, python):
    if is_number(cpp) and is_number(python):
        return None if cpp == python else describe_difference(path, cpp, python)
    if type(cpp) is not type(python):
        return describe_difference(path, cpp, python)
    if isinstance(cpp, dict):
        return find_object_difference(path, cpp, python)
    if isinstance(cpp, list):
        return find_array_difference(path, cpp, python)
    return None if cpp == python else describe_difference(path, cpp, python)


def find_object_difference(path, cpp, python):
    if cpp.keys() != python.keys():
        return describe_difference(path, sorted(cpp), sorted(python))
    for key in cpp:
        difference = find_difference(f"{path}.{key}", cpp[key], python[key])
        if difference:
            return difference
    return None


def find_array_difference(path, cpp, python):
    if len(cpp) != len(python):
        return describe_difference(path, f"{len(cpp)} elements", f"{len(python)} elements")
    for index, (cpp_item, python_item) in enumerate(zip(cpp, python)):
        difference = find_difference(f"{path}[{index}]", cpp_item, python_item)
        if difference:
            return difference
    return None


def decode_with_python(file):
    decoded = decode(file.read_bytes())
    if isinstance(decoded, JournalReport):
        if decoded.skipped_frames or decoded.truncated_at is not None or decoded.base_problem:
            raise ValueError(f"{file.name}: the journal did not replay cleanly: {decoded}")
        return decoded.tree
    return decoded


def main():
    writer, directory = sys.argv[1], pathlib.Path(sys.argv[2])
    subprocess.run([writer, str(directory)], check=True)
    files = sorted(directory.glob("*.calder"))
    failures = []
    for file in files:
        cpp_tree = json.loads(file.with_suffix(".json").read_text(encoding="utf-8"))
        difference = find_difference("", cpp_tree, decode_with_python(file))
        if difference:
            failures.append(f"{file.name}: {difference}")
    for failure in failures:
        print(failure, file=sys.stderr)
    print(f"cross-checked {len(files)} files, {len(failures)} differ")
    return 1 if failures or not files else 0


if __name__ == "__main__":
    sys.exit(main())
