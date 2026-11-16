#include "../Calder.h"

#include <fstream>
#include <map>

namespace Calder
{
namespace Detail
{

JournalWriterState::JournalWriterState(std::filesystem::path journalToUse)
    : path(std::move(journalToUse))
{
}

std::unique_ptr<JournalWriterState>
    JournalWriterState::open(const std::filesystem::path& journal)
{
    auto state =
        std::unique_ptr<JournalWriterState> {new JournalWriterState {journal}};
    auto bytes = FileBytes::read(journal);

    if (bytes)
        state->openExisting(bytes->getBytes());
    else if (std::filesystem::exists(journal)
             || !state->replaceJournal(
                 makeJournal(writeDocument(std::map<std::string, int> {}))))
        state->status = LoadStatus::FileMissing;

    return state;
}

void JournalWriterState::openExisting(std::span<const std::uint8_t> bytes)
{
    if (isJournal(bytes))
        openJournal(bytes);
    else if (!isDocument(bytes))
        status = LoadStatus::NotCalderData;
    else if (!Document {bytes}.isValid())
        status = LoadStatus::Damaged;
    else if (!replaceJournal(makeJournal(bytes)))
        status = LoadStatus::FileMissing;
}

void JournalWriterState::openJournal(std::span<const std::uint8_t> bytes)
{
    auto journalInfo = inspectJournal(bytes);

    if (!journalInfo.valid
        || !Document {bytes.subspan(headerSize, journalInfo.baseSize)}.isValid())
    {
        status = LoadStatus::Damaged;
        return;
    }

    readBase(bytes);

    if (!journalInfo.truncated)
        return;

    auto intactSize = headerSize + journalInfo.baseSize + journalInfo.frameBytes;
    auto error = std::error_code {};
    std::filesystem::resize_file(path, intactSize, error);

    if (error)
        status = LoadStatus::Damaged;
}

void JournalWriterState::readBase(std::span<const std::uint8_t> journal)
{
    baseSize = inspectJournal(journal).baseSize;
    keyTable = readJournalKeyTable(journal);
    auto base = Document {journal.subspan(headerSize, baseSize)};

    // A base naming one key twice would shift every later id.
    if (keyTable.getKeyCount() != base.getKeyCount())
        status = LoadStatus::Damaged;
}

bool JournalWriterState::appendFrame(std::span<const std::uint8_t> frame)
{
    auto error = std::error_code {};
    auto sizeBeforeAppend = std::filesystem::file_size(path, error);

    if (error)
    {
        status = LoadStatus::FileMissing;
        return false;
    }

    if (writeFrameAtEnd(frame))
        return true;

    cutBackTo(sizeBeforeAppend);
    return false;
}

bool JournalWriterState::writeFrameAtEnd(std::span<const std::uint8_t> frame) const
{
    auto stream = std::ofstream {path, std::ios::binary | std::ios::app};
    stream.write(reinterpret_cast<const char*>(frame.data()),
                 static_cast<std::streamsize>(frame.size()));
    stream.close();
    return !stream.fail();
}

// Part of a frame may have reached the file. Later frames appended after it
// would be lost behind a torn frame, so the writer stops if it cannot cut
// the part off.
void JournalWriterState::cutBackTo(std::uintmax_t sizeInBytes)
{
    auto error = std::error_code {};
    std::filesystem::resize_file(path, sizeInBytes, error);

    if (error)
        status = LoadStatus::Damaged;
}

bool JournalWriterState::addKeysToBase(const std::vector<std::string>& keys)
{
    auto bytes = FileBytes::read(path);

    if (!bytes)
        return false;

    auto journal = bytes->getBytes();
    auto extendedKeyTable = keyTable;

    for (const auto& key: keys)
        extendedKeyTable.getInternedKeyId(key);

    auto base = journal.subspan(headerSize, baseSize);
    auto rewritten = makeJournal(replaceKeyTable(base, extendedKeyTable));
    auto frames = journal.subspan(headerSize + baseSize);
    rewritten.insert(rewritten.end(), frames.begin(), frames.end());
    return replaceJournal(rewritten);
}

bool JournalWriterState::replaceJournal(std::span<const std::uint8_t> journal)
{
    if (!writeFileAtomically(path, journal))
        return false;

    status = LoadStatus::Loaded;
    readBase(journal);
    return true;
}

} // namespace Detail

JournalWriter::JournalWriter(std::unique_ptr<Detail::JournalWriterState> stateToUse)
    : state(std::move(stateToUse))
{
}

JournalWriter::JournalWriter(JournalWriter&& other) noexcept = default;
JournalWriter& JournalWriter::operator=(JournalWriter&& other) noexcept = default;
JournalWriter::~JournalWriter() = default;

JournalWriter JournalWriter::open(const std::filesystem::path& journal)
{
    return JournalWriter {Detail::JournalWriterState::open(journal)};
}

LoadStatus JournalWriter::getStatus() const
{
    if (!state)
        return LoadStatus::FileMissing;

    return state->getStatus();
}

bool JournalWriter::appendRemove(std::string_view key)
{
    if (getStatus() != LoadStatus::Loaded)
        return false;

    return state->appendFrame(Detail::makeJournalRemoveFrame(key));
}

} // namespace Calder
