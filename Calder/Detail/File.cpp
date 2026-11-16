#include "../Calder.h"

#include <fstream>
#include <system_error>

#if !defined(_WIN32)
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace Calder
{
namespace Detail
{
namespace
{

LoadStatus findFileStatus(std::span<const std::uint8_t> bytes,
                          const Document& document)
{
    if (isJournal(bytes))
        return LoadStatus::IsJournal;

    if (!isDocument(bytes))
        return LoadStatus::NotCalderData;

    if (!document.isValid())
        return LoadStatus::Damaged;

    return LoadStatus::Loaded;
}

} // namespace

#if defined(_WIN32)

namespace
{

std::optional<std::vector<std::uint8_t>>
    readWholeFile(const std::filesystem::path& file)
{
    auto stream = std::ifstream {file, std::ios::binary | std::ios::ate};

    if (!stream)
        return std::nullopt;

    auto size = static_cast<std::size_t>(stream.tellg());
    auto bytes = std::vector<std::uint8_t>(size);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(size));

    if (!stream)
        return std::nullopt;

    return bytes;
}

} // namespace

void UnmapOnDestroy::operator()(const std::uint8_t*) const {}

std::optional<FileBytes> FileBytes::read(const std::filesystem::path& file)
{
    auto bytes = readWholeFile(file);

    if (!bytes)
        return std::nullopt;

    return FileBytes {std::move(*bytes)};
}

#else

void UnmapOnDestroy::operator()(const std::uint8_t* address) const
{
    munmap(const_cast<std::uint8_t*>(address), sizeInBytes);
}

std::optional<FileBytes> FileBytes::read(const std::filesystem::path& file)
{
    auto descriptor = ::open(file.c_str(), O_RDONLY);

    if (descriptor < 0)
        return std::nullopt;

    struct stat status {};
    auto isRegularFile = fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode);
    auto size = static_cast<std::size_t>(status.st_size);

    // mmap refuses a zero-length mapping, so an empty file owns no bytes.
    if (!isRegularFile || size == 0)
    {
        close(descriptor);
        return isRegularFile
                   ? std::optional {FileBytes {std::vector<std::uint8_t> {}}}
                   : std::nullopt;
    }

    auto* address = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, descriptor, 0);
    close(descriptor);

    if (address == MAP_FAILED)
        return std::nullopt;

    const auto* bytes = static_cast<const std::uint8_t*>(address);
    return FileBytes {MappedBytes {bytes, UnmapOnDestroy {size}}};
}

#endif

FileBytes::FileBytes(std::vector<std::uint8_t> bytesToOwn)
    : ownedBytes(std::move(bytesToOwn))
{
}

FileBytes::FileBytes(MappedBytes mappedBytesToOwn)
    : mappedBytes(std::move(mappedBytesToOwn))
{
}

std::span<const std::uint8_t> FileBytes::getBytes() const
{
    if (mappedBytes)
        return {mappedBytes.get(), mappedBytes.get_deleter().sizeInBytes};

    return ownedBytes;
}

FileState::FileState(FileBytes bytesToUse)
    : bytes(std::move(bytesToUse))
    , document(bytes.getBytes())
    , status(findFileStatus(bytes.getBytes(), document))
{
}

} // namespace Detail

File::File(std::unique_ptr<Detail::FileState> stateToUse)
    : state(std::move(stateToUse))
{
}

File::File(File&& other) noexcept = default;
File::~File() = default;

File File::open(const std::filesystem::path& file)
{
    auto bytes = Detail::FileBytes::read(file);

    if (!bytes)
        return File {nullptr};

    return File {std::make_unique<Detail::FileState>(std::move(*bytes))};
}

File File::fromBytes(std::vector<std::uint8_t> bytes)
{
    return File {
        std::make_unique<Detail::FileState>(Detail::FileBytes {std::move(bytes)})};
}

LoadStatus File::getStatus() const
{
    if (!state)
        return LoadStatus::FileMissing;

    return state->getStatus();
}

View File::getRoot() const&
{
    if (getStatus() != LoadStatus::Loaded)
        return {};

    const auto& document = state->getDocument();
    return Detail::ViewAccess::makeView(
        document, {.valueOffset = document.getRootValueOffset()});
}

} // namespace Calder
