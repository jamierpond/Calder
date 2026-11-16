#include "../Calder.h"

#include <random>
#include <string>
#include <system_error>

#if defined(_WIN32)
    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <cerrno>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace Calder::Detail
{
namespace
{

std::filesystem::path makeTemporaryPathBeside(const std::filesystem::path& file)
{
    auto randomSuffix = std::random_device {}();
    auto temporary = file;
    temporary += ".tmp-" + std::to_string(randomSuffix);
    return temporary;
}

#if defined(_WIN32)

bool writeAllBytes(HANDLE handle, std::span<const std::uint8_t> bytes)
{
    constexpr auto largestWrite = std::size_t {1} << 30;

    while (!bytes.empty())
    {
        auto chunk = static_cast<DWORD>(std::min(bytes.size(), largestWrite));
        auto written = DWORD {0};

        if (!WriteFile(handle, bytes.data(), chunk, &written, nullptr))
            return false;

        bytes = bytes.subspan(written);
    }

    return true;
}

// Windows keeps no POSIX permission bits to carry over; the new file takes
// the directory's access rules, as the old one did.
bool writeAndFlush(const std::filesystem::path& temporary,
                   [[maybe_unused]] const std::filesystem::path& file,
                   std::span<const std::uint8_t> bytes)
{
    auto handle = CreateFileW(temporary.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);

    if (handle == INVALID_HANDLE_VALUE)
        return false;

    auto isWritten = writeAllBytes(handle, bytes) && FlushFileBuffers(handle);
    CloseHandle(handle);
    return isWritten;
}

bool replaceFile(const std::filesystem::path& temporary,
                 const std::filesystem::path& file)
{
    constexpr auto flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
    return MoveFileExW(temporary.c_str(), file.c_str(), flags) != 0;
}

#else

bool writeAllBytes(int descriptor, std::span<const std::uint8_t> bytes)
{
    while (!bytes.empty())
    {
        auto written = ::write(descriptor, bytes.data(), bytes.size());

        if (written < 0 && errno == EINTR)
            continue;

        if (written <= 0)
            return false;

        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }

    return true;
}

// The permission bits of the file being replaced, so a private file stays
// private; new files get the usual 0666 less the umask.
std::optional<mode_t> findPermissionsToKeep(const std::filesystem::path& file)
{
    constexpr auto permissionBits = mode_t {07777};
    struct stat status {};

    if (::stat(file.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
        return std::nullopt;

    return status.st_mode & permissionBits;
}

bool writeAndFlush(const std::filesystem::path& temporary,
                   std::span<const std::uint8_t> bytes,
                   std::optional<mode_t> permissions)
{
    constexpr auto newFilePermissions = mode_t {0666};
    auto descriptor = ::open(temporary.c_str(),
                             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                             newFilePermissions);

    if (descriptor < 0)
        return false;

    auto isWritten = writeAllBytes(descriptor, bytes)
                     && (!permissions || ::fchmod(descriptor, *permissions) == 0)
                     && ::fsync(descriptor) == 0;
    return ::close(descriptor) == 0 && isWritten;
}

// Makes the rename itself survive a power cut. Some file systems refuse to
// sync a directory; the file is already replaced then, so that is not a
// failed save.
void flushDirectoryOf(const std::filesystem::path& file)
{
    auto directory = file.parent_path().empty() ? std::filesystem::path {"."}
                                                : file.parent_path();
    auto descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);

    if (descriptor < 0)
        return;

    ::fsync(descriptor);
    ::close(descriptor);
}

bool writeAndFlush(const std::filesystem::path& temporary,
                   const std::filesystem::path& file,
                   std::span<const std::uint8_t> bytes)
{
    return writeAndFlush(temporary, bytes, findPermissionsToKeep(file));
}

bool replaceFile(const std::filesystem::path& temporary,
                 const std::filesystem::path& file)
{
    if (::rename(temporary.c_str(), file.c_str()) != 0)
        return false;

    flushDirectoryOf(file);
    return true;
}

#endif

} // namespace

bool writeFileAtomically(const std::filesystem::path& file,
                         std::span<const std::uint8_t> bytes)
{
    auto temporary = makeTemporaryPathBeside(file);

    if (writeAndFlush(temporary, file, bytes) && replaceFile(temporary, file))
        return true;

    auto error = std::error_code {};
    std::filesystem::remove(temporary, error);
    return false;
}

} // namespace Calder::Detail
