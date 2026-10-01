#include "file_io.hpp"

ULONGLONG RoundUp(ULONGLONG value, ULONGLONG alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

UniqueHandle OpenSource(const std::wstring &path, bool overlapped, bool noBuffering)
{
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0) | (noBuffering ? FILE_FLAG_NO_BUFFERING : 0);
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (open source)", path);
    return UniqueHandle(handle);
}

UniqueHandle OpenDest(const std::wstring &path, bool overlapped, bool noBuffering)
{
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0) | (noBuffering ? FILE_FLAG_NO_BUFFERING : 0);
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (create destination)", path);
    return UniqueHandle(handle);
}

ULONGLONG GetFileSizeByPath(const std::wstring &path)
{
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (size determination)", path);
    UniqueHandle file(handle);
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file.get(), &size))
        ThrowWin(L"GetFileSizeEx", path);
    return static_cast<ULONGLONG>(size.QuadPart);
}

void DeleteIfExists(const std::wstring &path)
{
    if (!DeleteFileW(path.c_str()))
    {
        DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
            ThrowWinCode(L"DeleteFileW", path, code);
    }
}

void TruncateFile(const std::wstring &path, ULONGLONG size)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (truncate file)", path);
    UniqueHandle file(handle);
    LARGE_INTEGER offset;
    offset.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN))
        ThrowWin(L"SetFilePointerEx (truncate)", path);
    if (!SetEndOfFile(file.get()))
        ThrowWin(L"SetEndOfFile (truncate)", path);
}