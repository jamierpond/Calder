#pragma once

#include "Binary.h"
#include <Miro/Reflection/Reflector.h>

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace emberstore
{

using Miro::CustomOptions;
using Miro::Mode;
using Miro::Options;
using Miro::OwningPointer;
using Miro::PackedArrayRef;
using Miro::PrimitiveRef;
using Miro::Reflector;
using Miro::Shape;
using Miro::ValueKind;
using Miro::Vector;

// Streams a reflection walk straight into a Binary::Writer. Container
// slots write their tag and a placeholder size when they start and patch
// it when they finish, which happens before the next sibling starts.
// Each reflector keeps one child and restarts it for every field or
// element, so a walk allocates one reflector per nesting depth. Save-only.
class BinaryWriterReflector final : public Reflector
{
public:
    BinaryWriterReflector(Binary::Writer& writerToUse, Options optsToUse);
    ~BinaryWriterReflector() override;

    void visit(PrimitiveRef ref) override;
    bool visitPacked(PackedArrayRef ref) override;
    void writeNull() override;
    ValueKind kind() const override { return ValueKind::Absent; }

    Reflector& atKey(std::string_view key, Options childOpts) override;
    Reflector& atIndex(std::size_t index, Options childOpts) override;

    void resizeArray(std::size_t newSize) override;

    void markPresent() override { slot.omittableSlotStartOffset = Binary::npos; }

    void requirePolymorphicSupport(std::string_view) override {}

private:
    struct SlotState
    {
        bool isOpen = false;
        std::size_t slotStartOffset = 0;
        std::size_t sizeFieldOffset = Binary::npos;
        std::size_t countFieldOffset = Binary::npos;
        std::size_t elementCount = 0;
        bool isSlotReplacedByPackedOrNull = false;

        // Where an Omittable slot's bytes (its object key included) begin,
        // until markPresent() claims it. Unclaimed, finishing rewinds to
        // here; an array element leaves a Null so the count stays right.
        std::size_t omittableSlotStartOffset = Binary::npos;
        bool isOmittableSlotAnArrayElement = false;
    };

    void startSlot();
    void restartSlot(Options&& optsToUse);
    void finishSlot();
    void finishOpenChild();
    Reflector& spawnChild(std::size_t childStartOffset,
                          Options&& childOpts,
                          bool isArrayElement);
    void writeSizedHeader(Binary::Tag tag);
    void writeArrayHeader();
    void patchSizes();
    void discardPendingSlot();

    Binary::Writer& writer;
    SlotState slot;

    OwningPointer<BinaryWriterReflector> reusableChild;
};

// A value whose stored type differs from the C++ field it loads into,
// e.g. a string stored where the struct now has an int. The field is not
// loaded from it. fieldPath reads like "tracks[7].title".
struct BinaryTypeMismatch
{
    std::string fieldPath;
    ValueKind storedKind = ValueKind::Absent;
    ValueKind expectedKind = ValueKind::Absent;
};

// What fromBinary could not load as written. Loading never throws, so
// this is the only place a caller learns of it.
struct BinaryLoadReport
{
    bool hasProblems() const
    {
        return !typeMismatches.empty() || skippedJournalFrameCount != 0
               || isJournalTruncated;
    }

    std::vector<BinaryTypeMismatch> typeMismatches;
    std::size_t skippedJournalFrameCount = 0;
    bool isJournalTruncated = false;
};

// Loads straight from the encoded bytes. Objects and arrays keep a
// resume cursor, so reading fields or elements in written order costs
// O(1) each. Like the writer, each reflector keeps one child and restarts
// it for every field or element. Load-only; never throws: malformed input
// reads as absent.
class BinaryReaderReflector final : public Reflector
{
public:
    BinaryReaderReflector(const Binary::Document& documentToUse,
                          const Binary::Detail::ValueHeader& headerToUse,
                          Options optsToUse,
                          BinaryLoadReport& reportToUse);

    ~BinaryReaderReflector() override;

    void visit(PrimitiveRef ref) override;
    bool visitPacked(PackedArrayRef ref) override;
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

    BinaryReaderReflector(const BinaryReaderReflector& parentToUse,
                          StepFromParent stepToUse,
                          const Binary::Detail::ValueHeader& headerToUse,
                          Options optsToUse);

    void restartSlot(StepFromParent stepToUse,
                     const Binary::Detail::ValueHeader& headerToUse,
                     Options&& optsToUse);

    Reflector& spawnChild(StepFromParent step,
                          const Binary::Detail::ValueHeader& childHeader,
                          Options&& childOpts);
    Reflector& spawnMissingChild(StepFromParent step, Options&& childOpts);
    Reflector& spawnElementChild(std::size_t index,
                                 std::size_t elementOffset,
                                 Options&& childOpts);

    bool isStoredAs(ValueKind expectedKind) const;
    void reportTypeMismatch(ValueKind expectedKind) const;
    std::string buildFieldPath() const;

    bool hasCategory(Binary::Detail::ValueCategory category) const;
    std::optional<Binary::Detail::ObjectEntry> findEntry(std::string_view key);
    void rewindResumeOffsetIfPast(std::size_t index);

    const Binary::Document& document;
    Binary::Detail::ValueHeader header;
    BinaryLoadReport& report;
    std::optional<std::reference_wrapper<const BinaryReaderReflector>> parent;
    StepFromParent stepFromParent;
    bool hasReportedObjectMismatch = false;

    std::size_t resumeOffset = 0;
    std::size_t resumeElementIndex = 0;

    OwningPointer<BinaryReaderReflector> reusableChild;
};

} // namespace emberstore
