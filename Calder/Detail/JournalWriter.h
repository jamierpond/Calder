#pragma once

#include "Wire.h"

namespace Calder::Detail
{

// The journal file a JournalWriter appends to, and the base's key table
// that every set frame's key ids index.
class JournalWriterState
{
public:
    static std::unique_ptr<JournalWriterState>
        open(const std::filesystem::path& journal);

    LoadStatus getStatus() const { return status; }
    InternedKeyTable& getKeyTable() { return keyTable; }

    bool appendFrame(std::span<const std::uint8_t> frame);

    // Rewrites the base with these names added to its key table. Existing
    // ids keep their names, so the frames already written stay valid.
    bool addKeysToBase(const std::vector<std::string>& keys);

    bool replaceJournal(std::span<const std::uint8_t> journal);

private:
    explicit JournalWriterState(std::filesystem::path journalToUse);

    void openExisting(std::span<const std::uint8_t> bytes);
    void openJournal(std::span<const std::uint8_t> bytes);
    void readBase(std::span<const std::uint8_t> journal);

    std::filesystem::path path;
    LoadStatus status = LoadStatus::Loaded;
    InternedKeyTable keyTable;
    std::size_t baseSize = 0;
};

} // namespace Calder::Detail
