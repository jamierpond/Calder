#pragma once

#include "Wire.h"

namespace Calder::Detail
{

struct UnmapOnDestroy
{
    void operator()(const std::uint8_t* address) const;

    std::size_t sizeInBytes = 0;
};

// A file's whole contents. On POSIX the file is mapped, so opening a big
// file reads nothing until a View touches it. On Windows it is read into
// memory: a mapped file there cannot be replaced by a rename, which is
// how saveToFile writes.
class FileBytes
{
public:
    explicit FileBytes(std::vector<std::uint8_t> bytesToOwn);

    // nullopt when there is no readable file at `file`.
    static std::optional<FileBytes> read(const std::filesystem::path& file);

    std::span<const std::uint8_t> getBytes() const;

private:
    using MappedBytes = std::unique_ptr<const std::uint8_t, UnmapOnDestroy>;

    explicit FileBytes(MappedBytes mappedBytesToOwn);

    std::vector<std::uint8_t> ownedBytes;
    MappedBytes mappedBytes;
};

class FileState
{
public:
    explicit FileState(FileBytes bytesToUse);

    LoadStatus getStatus() const { return status; }
    const Document& getDocument() const { return document; }

private:
    FileBytes bytes;
    Document document;
    LoadStatus status = LoadStatus::Loaded;
};

// Writes `bytes` beside `file` under a temporary name, then renames it over
// `file`, so a reader sees the old file or the new one, never half of one.
bool writeFileAtomically(const std::filesystem::path& file,
                         std::span<const std::uint8_t> bytes);

} // namespace Calder::Detail
