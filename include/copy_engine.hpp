#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

void CopySync(const std::wstring &src, const std::wstring &dst, DWORD block, bool noBuffering);
void CopyAsync(const std::wstring &src, const std::wstring &dst, ULONGLONG fileSize, DWORD block, int numOps,
               bool noBuffering);