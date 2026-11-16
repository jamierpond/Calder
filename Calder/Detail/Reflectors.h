#pragma once

#include "Wire.h"
#include <Miro/Reflection/Reflector.h>

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Calder::Detail
{

using Miro::CustomOptions;
using Miro::Mode;
using Miro::Options;
using Miro::OwningPointer;
using Miro::PackedArrayRef;
using Miro::PackedStructArrayRef;
using Miro::PrimitiveRef;
using Miro::Reflector;
using Miro::Shape;
using Miro::ValueKind;
using Miro::Vector;

// Streams a reflection walk straight into a ByteWriter. Container
// slots write their tag and a placeholder size when they start and patch
// it when they finish, which happens before the next sibling starts.
// Each reflector keeps one child and restarts it for every field or
// element, so a walk allocates one reflector per nesting depth. Save-only.
class Writer final : public Reflector
{
public:
    Writer(ByteWriter& writerToUse, Options optsToUse);
    ~Writer() override;

    void visit(PrimitiveRef ref) override;
    bool visitPacked(PackedArrayRef ref) override;
    bool visitPackedStructs(PackedStructArrayRef ref) override;
    void writeNull() override;
    ValueKind kind() const override { return ValueKind::Absent; }

    Reflector& atKey(std::string_view key, Options childOpts) override;
    Reflector& atIndex(std::size_t index, Options childOpts) override;

    void resizeArray(std::size_t newSize) override;

    void markPresent() override { slot.omittableSlotStartOffset = npos; }

    void requirePolymorphicSupport(std::string_view) override {}

private:
    struct SlotState
    {
        bool isOpen = false;
        std::size_t slotStartOffset = 0;
        std::size_t sizeFieldOffset = npos;
        std::size_t countFieldOffset = npos;
        std::size_t elementCount = 0;
        bool isSlotReplacedByPackedOrNull = false;

        // Where an Omittable slot's bytes (its object key included) begin,
        // until markPresent() claims it. Unclaimed, finishing rewinds to
        // here; an array element leaves a Null so the count stays right.
        std::size_t omittableSlotStartOffset = npos;
        bool isOmittableSlotAnArrayElement = false;
    };

    void startSlot();
    void restartSlot(Options&& optsToUse);
    void finishSlot();
    void finishOpenChild();
    Reflector& spawnChild(std::size_t childStartOffset,
                          Options&& childOpts,
                          bool isArrayElement);
    void writeSizedHeader(Tag tag);
    void writeArrayHeader();
    void patchSizes();
    void discardPendingSlot();

    ByteWriter& writer;
    SlotState slot;

    OwningPointer<Writer> reusableChild;
};

// Nesting deeper than this is refused as Damaged: each level costs the
// loader a stack frame, so a crafted file could otherwise overflow the stack.
// The Python decoder uses the same limit.
inline constexpr auto maxNestingDepth = std::size_t {256};

struct NestingTooDeep
{
};

enum class ReadOutcome
{
    Read,
    OutOfBounds,
    NumberDidNotFit,
};

using RefusedElementReporter = std::function<void(std::size_t elementIndex)>;

// A slot inside one record of a PackedStructArray, read as an object
// when the C++ element type is not packable.
struct PackedRecordSlot
{
    std::reference_wrapper<const StoredPackedLayout> layout;
    std::size_t recordOffset = 0;
    PackedPathPrefix path;
};

// Where a load starts: one value's header, and the packed record it sits
// in when it is a field of one.
struct ReadStart
{
    std::reference_wrapper<const Document> document;
    ValueHeader header;
    std::optional<PackedRecordSlot> packedRecord;
};

// Loads straight from the encoded bytes. Objects and arrays keep a
// resume cursor, so reading fields or elements in written order costs
// O(1) each. Like the writer, each reflector keeps one child and restarts
// it for every field or element. Load-only; never throws: malformed input
// reads as absent.
class Reader final : public Reflector
{
public:
    Reader(const ReadStart& start, Options optsToUse, LoadResult& resultToUse);

    ~Reader() override;

    void visit(PrimitiveRef ref) override;
    bool visitPacked(PackedArrayRef ref) override;
    bool visitPackedStructs(PackedStructArrayRef ref) override;
    void writeNull() override {}
    ValueKind kind() const override;
    bool isIntegerNumber() const override;

    Reflector& atKey(std::string_view key, Options childOpts) override;
    Reflector& atIndex(std::size_t index, Options childOpts) override;

    std::size_t arraySize() const override;
    Vector<std::string> mapKeys() const override;

    void requirePolymorphicSupport(std::string_view) override {}

private:
    using StepFromParent =
        std::variant<std::monostate, std::string_view, std::size_t>;

    Reader(const Reader& parentToUse,
           StepFromParent stepToUse,
           const ValueHeader& headerToUse,
           Options optsToUse);

    void restartSlot(StepFromParent stepToUse,
                     const ValueHeader& headerToUse,
                     Options&& optsToUse,
                     std::optional<PackedRecordSlot> packedRecordToUse);

    Reflector& spawnChild(StepFromParent step,
                          const ValueHeader& childHeader,
                          Options&& childOpts,
                          std::optional<PackedRecordSlot> childPackedRecord = {});
    Reflector& spawnPackedRecordChild(std::size_t index, Options&& childOpts);
    Reflector& spawnPackedFieldChild(std::string_view key, Options&& childOpts);
    std::optional<std::size_t>
        findPackedFieldAtThisDepth(std::string_view key) const;
    void appendPackedRecordKeys(Vector<std::string>& keys) const;
    bool readStoredPackedLayoutOnce();
    Reflector& spawnMissingChild(StepFromParent step, Options&& childOpts);
    Reflector& spawnStoredChild(StepFromParent step,
                                const ValueHeader& childHeader,
                                Options&& childOpts);
    Reflector& spawnElementChild(std::size_t index,
                                 std::size_t elementOffset,
                                 Options&& childOpts);

    bool isStoredAs(ValueKind expectedKind) const;
    void reportTypeMismatch() const;
    void reportFieldProblem(FieldProblem::Kind kind) const;
    void reportFieldProblemAtIndex(FieldProblem::Kind kind, std::size_t index) const;
    void reportDamage() const;
    void reportDamageIfStopped(const ObjectEntryCursor& cursor) const;
    void reportReadOutcome(ReadOutcome outcome) const;
    void countMissingField(const Options& childOpts) const;
    void countMissingElements(std::size_t wantedCount,
                              std::size_t storedCount) const;
    RefusedElementReporter makeRefusedElementReporter() const;
    std::string buildFieldPath() const;

    bool hasCategory(ValueCategory category) const;
    std::optional<ObjectEntry> findEntry(std::string_view key);
    void rewindResumeOffsetIfPast(std::size_t index);

    const Document& document;
    ValueHeader header;
    LoadResult& result;
    std::optional<std::reference_wrapper<const Reader>> parent;
    std::size_t depth = 0;
    StepFromParent stepFromParent;
    bool hasReportedObjectMismatch = false;

    std::size_t resumeOffset = 0;
    std::size_t resumeElementIndex = 0;

    std::optional<PackedRecordSlot> packedRecord;
    StoredPackedLayout storedPackedLayout;
    bool hasReadStoredPackedLayout = false;
    bool isStoredPackedLayoutValid = false;

    OwningPointer<Reader> reusableChild;
};

} // namespace Calder::Detail
