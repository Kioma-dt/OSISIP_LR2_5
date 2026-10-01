#pragma once

#include <string>
#include <windows.h>

struct AppError
{
    std::wstring message;
};

std::wstring WinErrText(DWORD code);
[[noreturn]] void ThrowWinCode(const wchar_t *op, const std::wstring &detail, DWORD code);
[[noreturn]] void ThrowWin(const wchar_t *op, const std::wstring &detail = std::wstring());