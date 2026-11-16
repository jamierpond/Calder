#pragma once

#include "Detail/Forward.h"

namespace Calder
{

// Any contiguous bytes: std::vector<std::uint8_t>, std::string, std::span<std::byte>
template <typename Range>
concept ByteRange = Detail::IsByteRange<Range>;

enum class LoadStatus
{
    Loaded,
    FileMissing, // no readable file at that path
    ValueMissing, // the View points at nothing, such as a key the file lacks
    NotCalderData,
    IsJournal, // File::open reads documents only; load a journal with loadFromFile
    Damaged, // cut short or inconsistent bytes, or nested too deep
    OutOfMemory,
};

struct FieldProblem
{
    enum class Kind
    {
        WrongType, // such as a string stored where the field is an int
        // Such as 2^40 into an int, or 1000 into a list of std::int8_t. A lone
        // field narrower than int or double is refused by Miro unreported.
        NumberDidNotFit,
    };

    std::string fieldPath; // "tracks[7].title"
    Kind kind = Kind::WrongType;
};

struct LoadResult
{
    // False: don't use the value. It is untouched, or partly loaded when the
    // bytes broke partway through (Damaged, OutOfMemory).
    bool hasLoaded() const;

    // Not loaded, or a field was refused or missing, or journal frames were lost.
    bool hasProblems() const;

    LoadStatus status = LoadStatus::Loaded;
    std::vector<FieldProblem> fieldProblems; // the first few
    std::size_t fieldProblemCount = 0; // all of them
    std::size_t missingFieldCount = 0;
    std::size_t lostJournalFrameCount = 0;
};

// Save and load a whole value. Loading never throws; fields the bytes lack
// keep the value they had.
template <typename T>
std::vector<std::uint8_t> toBinary(const T& value);
template <typename T>
[[nodiscard]] std::optional<T> createFromBinary(const ByteRange auto& bytes);
template <typename T>
[[nodiscard]] LoadResult fromBinary(T& value, const ByteRange auto& bytes);

// Files. Saving writes a temporary file, then renames it over the old one.
template <typename T>
[[nodiscard]] bool saveToFile(const T& value, const std::filesystem::path& file);
template <typename T>
[[nodiscard]] std::optional<T> createFromFile(const std::filesystem::path& file);
template <typename T>
[[nodiscard]] LoadResult loadFromFile(T& value, const std::filesystem::path& file);

// Read part of a file without loading all of it. A View points into its File:
// it, and the strings it returns, are valid while the File lives.
class CALDER_POINTER_TYPE View
{
public:
    View();

    bool isValid() const; // false for a missing key or index
    bool isNull() const;
    bool isBool() const;
    bool isNumber() const;
    bool isInteger() const;
    bool isString() const;
    bool isObject() const; // a struct or a map
    bool isArray() const;

    View operator[](std::string_view key) const;
    View operator[](std::size_t index) const;
    std::size_t getSize() const; // elements of an array, entries of an object
    std::vector<std::string_view> getKeys() const;

    bool asBool(bool fallback = false) const;
    std::int64_t asInteger(std::int64_t fallback = 0) const; // or if it doesn't fit
    double asNumber(double fallback = 0.0) const;
    std::string_view asString(std::string_view fallback = {}) const;

private:
    friend struct Detail::ViewAccess;

    std::reference_wrapper<const Detail::Document> document;
    Detail::ViewPosition position;
};

class File
{
public:
    static File open(const std::filesystem::path& file); // decodes nothing yet
    static File fromBytes(std::vector<std::uint8_t> bytes);

    LoadStatus getStatus() const;
    View getRoot() const& CALDER_LIFETIMEBOUND;
    View getRoot() const&& = delete; // no Views into a File that is about to go

    // Views stay valid when a File moves. Assigning over a File would free
    // the bytes its Views point into, so a File cannot be reassigned.
    File(File&& other) noexcept;
    File& operator=(File&& other) = delete;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File();

private:
    explicit File(std::unique_ptr<Detail::FileState> stateToUse);

    std::unique_ptr<Detail::FileState> state;
};

template <typename T>
[[nodiscard]] std::optional<T> createFromBinary(const View& view);
template <typename T>
[[nodiscard]] LoadResult fromBinary(T& value, const View& view);

// Append-only stores: a journal holds a map, and each change appends one
// small frame instead of rewriting the file. One writer per journal at a time.
class JournalWriter
{
public:
    // Creates the journal if it is missing, and cuts off a torn last frame.
    static JournalWriter open(const std::filesystem::path& journal);
    LoadStatus getStatus() const;

    template <typename T>
    [[nodiscard]] bool appendSet(std::string_view key, const T& value);
    [[nodiscard]] bool appendRemove(std::string_view key);
    template <typename Map>
    [[nodiscard]] bool rewrite(const Map& entries); // one fresh base, no frames

    JournalWriter(JournalWriter&& other) noexcept;
    JournalWriter& operator=(JournalWriter&& other) noexcept;
    ~JournalWriter();

private:
    explicit JournalWriter(std::unique_ptr<Detail::JournalWriterState> stateToUse);

    std::unique_ptr<Detail::JournalWriterState> state;
};

} // namespace Calder

#include "Detail/Implementation.h"
