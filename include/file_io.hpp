#pragma once

#include "resources.hpp"

#include <cstdint>

constexpr DWORD kAlign = 4096;
ULONGLONG RoundUp(ULONGLONG value, ULONGLONG alignment);
UniqueHandle OpenSource(const std::wstring &path, bool overlapped, bool noBuffering);
UniqueHandle OpenDest(const std::wstring &path, bool overlapped, bool noBuffering);
ULONGLONG GetFileSizeByPath(const std::wstring &path);
void DeleteIfExists(const std::wstring &path);
void TruncateFile(const std::wstring &path, ULONGLONG size);