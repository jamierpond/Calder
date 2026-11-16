#pragma once

#include "Binary.h"
#include <Miro/Reflection/Reflector.h>

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Calder
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

// Streams a reflection walk straight into a Detail::ByteWriter. Container
// slots write their tag and a placeholder size when they start and patch
// it when they finish, which happens before the next sibling starts.
// Each reflector keeps one child and restarts it for every field or
// element, so a walk allocates one reflector per nesting depth. Save-only.
class Writer final : public Reflector
{
public:
    Writer(Detail::ByteWriter& writerToUse, Options optsToUse);
    ~Writer() override;

    void visit(PrimitiveRef ref) override;
    bool visitPacked(PackedArrayRef ref) override;
    bool visitPackedStructs(PackedStructArrayRef ref) override;
    void writeNull() override;
    ValueKind kind() const override { return ValueKind::Absent; }

    Reflector& atKey(std::string_view key, Options childOpts) override;
    Reflector& atIndex(std::size_t index, Options childOpts) override;

    void resizeArray(std::size_t newSize) override;

    void markPresent() override { slot.omittableSlotStartOffset = Calder::npos; }

    void requirePolymorphicSupport(std::string_view) override {}

private:
    struct SlotState
    {
        bool isOpen = false;
        std::size_t slotStartOffset = 0;
        std::size_t sizeFieldOffset = Calder::npos;
        std::size_t countFieldOffset = Calder::npos;
        std::size_t elementCount = 0;
        bool isSlotReplacedByPackedOrNull = false;

        // Where an Omittable slot's bytes (its object key included) begin,
        // until markPresent() claims it. Unclaimed, finishing rewinds to
        // here; an array element leaves a Null so the count stays right.
        std::size_t omittableSlotStartOffset = Calder::npos;
        bool isOmittableSlotAnArrayElement = false;
    };

    void startSlot();
    void restartSlot(Options&& optsToUse);
    void finishSlot();
    void finishOpenChild();
    Reflector& spawnChild(std::size_t childStartOffset,
                          Options&& childOpts,
                          bool isArrayElement);
    void writeSizedHeader(Calder::Tag tag);
    void writeArrayHeader();
    void patchSizes();
    void discardPendingSlot();

    Detail::ByteWriter& writer;
    SlotState slot;

    OwningPointer<Writer> reusableChild;
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
class Reader final : public Reflector
{
public:
    Reader(const Calder::Document& documentToUse,
           const Calder::Detail::ValueHeader& headerToUse,
           Options optsToUse,
           BinaryLoadReport& reportToUse);

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

    // A slot inside one record of a PackedStructArray, read as an object
    // when the C++ element type is not packable. `pathDepth` segments of
    // `prefixField`'s path lead here.
    struct PackedRecordSlot
    {
        std::reference_wrapper<const Calder::Detail::StoredPackedLayout> layout;
        std::size_t recordOffset = 0;
        std::size_t pathDepth = 0;
        std::size_t prefixField = 0;
    };

    Reader(const Reader& parentToUse,
           StepFromParent stepToUse,
           const Calder::Detail::ValueHeader& headerToUse,
           Options optsToUse);

    void restartSlot(StepFromParent stepToUse,
                     const Calder::Detail::ValueHeader& headerToUse,
                     Options&& optsToUse,
                     std::optional<PackedRecordSlot> packedRecordToUse);

    Reflector& spawnChild(StepFromParent step,
                          const Calder::Detail::ValueHeader& childHeader,
                          Options&& childOpts,
                          std::optional<PackedRecordSlot> childPackedRecord = {});
    Reflector& spawnPackedRecordChild(std::size_t index, Options&& childOpts);
    Reflector& spawnPackedFieldChild(std::string_view key, Options&& childOpts);
    std::optional<std::size_t>
        findPackedFieldAtThisDepth(std::string_view key) const;
    std::span<const std::string_view> getPackedRecordPrefix() const;
    void appendPackedRecordKeys(Vector<std::string>& keys) const;
    bool readStoredPackedLayoutOnce();
    Reflector& spawnMissingChild(StepFromParent step, Options&& childOpts);
    Reflector& spawnElementChild(std::size_t index,
                                 std::size_t elementOffset,
                                 Options&& childOpts);

    bool isStoredAs(ValueKind expectedKind) const;
    void reportTypeMismatch(ValueKind expectedKind) const;
    std::string buildFieldPath() const;

    bool hasCategory(Calder::Detail::ValueCategory category) const;
    std::optional<Calder::Detail::ObjectEntry> findEntry(std::string_view key);
    void rewindResumeOffsetIfPast(std::size_t index);

    const Calder::Document& document;
    Calder::Detail::ValueHeader header;
    BinaryLoadReport& report;
    std::optional<std::reference_wrapper<const Reader>> parent;
    StepFromParent stepFromParent;
    bool hasReportedObjectMismatch = false;

    std::size_t resumeOffset = 0;
    std::size_t resumeElementIndex = 0;

    std::optional<PackedRecordSlot> packedRecord;
    Calder::Detail::StoredPackedLayout storedPackedLayout;
    bool hasReadStoredPackedLayout = false;
    bool isStoredPackedLayoutValid = false;

    OwningPointer<Reader> reusableChild;
};

} // namespace Calder
