// Self-containment check: <emberstore/Binary.h> must compile as the first
// and only Miro include, and must supply the binary wire layer
// (Writer / Document / View) plus toBinary / fromBinary.

#include <emberstore/Binary.h>

#include <NanoTest/NanoTest.h>

using namespace nano;

namespace
{
struct BinaryPoint
{
    void reflect(Miro::Reflector& r)
    {
        r["x"](x);
        r["y"](y);
    }

    int x = 0;
    int y = 0;
};
} // namespace

auto binaryEntryHeader =
    test("ObjectEntry header: emberstore/Binary.h is self-contained") = []
{
    auto point = BinaryPoint {};
    point.x = 3;

    auto copy =
        emberstore::createFromBinary<BinaryPoint>(emberstore::toBinary(point));
    check(copy.x == 3);
    check(copy.y == 0);
};
