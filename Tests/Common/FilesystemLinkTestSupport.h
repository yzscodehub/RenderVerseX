#pragma once

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#endif

namespace RVX::TestSupport
{
/** @brief Remove a test-owned directory link before parent directory cleanup. */
class ScopedDirectoryLink final
{
public:
    explicit ScopedDirectoryLink(std::filesystem::path path)
        : m_path(std::move(path))
    {
    }

    ~ScopedDirectoryLink()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    ScopedDirectoryLink(const ScopedDirectoryLink&) = delete;
    ScopedDirectoryLink& operator=(const ScopedDirectoryLink&) = delete;

private:
    std::filesystem::path m_path;
};

/** @brief Create an unprivileged directory link for filesystem boundary tests. */
inline bool CreateDirectoryLink(const std::filesystem::path& target,
                                const std::filesystem::path& link,
                                std::error_code& error)
{
    error.clear();
    std::filesystem::create_directory_symlink(target, link, error);
    if (!error)
    {
        return true;
    }

#if defined(_WIN32)
    error.clear();
    const std::filesystem::path absoluteTarget =
        std::filesystem::absolute(target, error).lexically_normal();
    if (error)
    {
        return false;
    }

    if (!std::filesystem::create_directory(link, error))
    {
        if (!error)
        {
            error = std::make_error_code(std::errc::file_exists);
        }
        return false;
    }

    struct MountPointReparseData
    {
        DWORD reparseTag;
        WORD reparseDataLength;
        WORD reserved;
        WORD substituteNameOffset;
        WORD substituteNameLength;
        WORD printNameOffset;
        WORD printNameLength;
        WCHAR pathBuffer[1];
    };
    static_assert(offsetof(MountPointReparseData, pathBuffer) == 16u);

    const std::wstring printName = absoluteTarget.native();
    const std::wstring substituteName = L"\\??\\" + printName;
    const size_t substituteBytes = substituteName.size() * sizeof(WCHAR);
    const size_t printBytes = printName.size() * sizeof(WCHAR);
    const size_t pathBytes = substituteBytes + sizeof(WCHAR) +
                             printBytes + sizeof(WCHAR);
    const size_t bufferSize = offsetof(MountPointReparseData, pathBuffer) +
                              pathBytes;
    if (bufferSize > MAXIMUM_REPARSE_DATA_BUFFER_SIZE ||
        substituteBytes > MAXWORD || printBytes > MAXWORD)
    {
        std::filesystem::remove(link, error);
        error = std::make_error_code(std::errc::filename_too_long);
        return false;
    }

    std::vector<std::byte> storage(bufferSize);
    auto* reparseData =
        reinterpret_cast<MountPointReparseData*>(storage.data());
    reparseData->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    reparseData->reparseDataLength =
        static_cast<WORD>(
            bufferSize - offsetof(MountPointReparseData, substituteNameOffset));
    reparseData->reserved = 0;
    reparseData->substituteNameOffset = 0;
    reparseData->substituteNameLength = static_cast<WORD>(substituteBytes);
    reparseData->printNameOffset =
        static_cast<WORD>(substituteBytes + sizeof(WCHAR));
    reparseData->printNameLength = static_cast<WORD>(printBytes);
    std::memcpy(reparseData->pathBuffer,
                substituteName.data(),
                substituteBytes);
    std::memcpy(reinterpret_cast<std::byte*>(reparseData->pathBuffer) +
                    reparseData->printNameOffset,
                printName.data(),
                printBytes);

    const HANDLE handle = CreateFileW(
        link.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD nativeError = GetLastError();
        std::filesystem::remove(link, error);
        error = std::error_code(static_cast<int>(nativeError),
                                std::system_category());
        return false;
    }

    DWORD bytesReturned = 0;
    const BOOL setResult = DeviceIoControl(
        handle,
        FSCTL_SET_REPARSE_POINT,
        reparseData,
        static_cast<DWORD>(bufferSize),
        nullptr,
        0,
        &bytesReturned,
        nullptr);
    const DWORD nativeError = setResult ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!setResult)
    {
        std::filesystem::remove(link, error);
        error = std::error_code(static_cast<int>(nativeError),
                                std::system_category());
        return false;
    }

    error.clear();
    return true;
#else
    return false;
#endif
}
} // namespace RVX::TestSupport
