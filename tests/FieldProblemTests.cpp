// A value Calder or Miro refuses on load keeps the field's prior value and is
// reported in LoadResult::fieldProblems, whichever of the two refused it.

#include <Calder/Calder.h>
#include <Miro/Reflect.h>

#include <NanoTest/NanoTest.h>

#include <cstdint>

using namespace nano;
using namespace Calder;

namespace
{

struct SmallCount
{
    MIRO_REFLECT(count)

    std::int8_t count = 0;
};

struct FloatValue
{
    MIRO_REFLECT(value)

    float value = 0.0f;
};

enum class Weapon : std::uint8_t
{
    sword,
    bow
};

enum class RenamedWeapon : std::uint8_t
{
    blade,
    bow
};

struct Loadout
{
    MIRO_REFLECT(weapon)

    Weapon weapon = Weapon::bow;
};

struct RenamedLoadout
{
    MIRO_REFLECT(weapon)

    RenamedWeapon weapon = RenamedWeapon::blade;
};

struct NumberedLoadout
{
    MIRO_REFLECT(weapon)

    int weapon = 0;
};

struct WideCount
{
    MIRO_REFLECT(count)

    std::int64_t count = 0;
};

struct DoubleValue
{
    MIRO_REFLECT(value)

    double value = 0.0;
};

} // namespace

auto loneNarrowNumbersThatDoNotFitAreReported = test(
    "Field problems: 1000 into an int8 and 1e300 into a float are reported") = []
{
    constexpr auto tooBigForInt8 = std::int64_t {1000};
    constexpr auto tooBigForFloat = 1e300;
    constexpr auto priorCount = std::int8_t {3};
    constexpr auto priorValue = 2.5f;
    auto count = SmallCount {priorCount};
    auto value = FloatValue {priorValue};
    auto countResult = fromBinary(count, toBinary(WideCount {tooBigForInt8}));
    auto valueResult = fromBinary(value, toBinary(DoubleValue {tooBigForFloat}));

    check(count.count == priorCount && value.value == priorValue);
    check(countResult.fieldProblems.at(0).fieldPath == "count");
    check(countResult.fieldProblems.at(0).kind
          == FieldProblem::Kind::NumberDidNotFit);
    check(valueResult.fieldProblems.at(0).kind
          == FieldProblem::Kind::NumberDidNotFit);
};

auto enumRefusalsAreReported =
    test("Field problems: an enum number or name that doesn't fit is reported") = []
{
    constexpr auto tooBigForUint8 = 999;
    auto loadout = Loadout {};
    auto numberResult =
        fromBinary(loadout, toBinary(NumberedLoadout {tooBigForUint8}));

    check(loadout.weapon == Weapon::bow);
    check(numberResult.fieldProblems.at(0).fieldPath == "weapon");
    check(numberResult.fieldProblems.at(0).kind
          == FieldProblem::Kind::NumberDidNotFit);

    auto nameResult = fromBinary(loadout, toBinary(RenamedLoadout {}));

    check(loadout.weapon == Weapon::bow);
    check(nameResult.fieldProblems.at(0).fieldPath == "weapon");
    check(nameResult.fieldProblems.at(0).kind
          == FieldProblem::Kind::NoSuchEnumerator);
};
