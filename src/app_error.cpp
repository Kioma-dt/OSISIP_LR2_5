#include "app_error.hpp"

#include <sstream>

std::wstring WinErrText(DWORD code)
{
    LPWSTR buffer = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring result = length && buffer ? std::wstring(buffer, length) : L"(description unavailable)";
    if (buffer)
        LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
        result.pop_back();
    return result;
}

[[noreturn]] void ThrowWinCode(const wchar_t *op, const std::wstring &detail, DWORD code)
{
    std::wostringstream stream;
    stream << L"WinAPI Error\n  Operation     : " << op << L"\n";
    if (!detail.empty())
        stream << L"  Object       : " << detail << L"\n";
    stream << L"  GetLastError : " << code << L"\n  Description   : " << WinErrText(code);
    throw AppError{stream.str()};
}

[[noreturn]] void ThrowWin(const wchar_t *op, const std::wstring &detail)
{
    ThrowWinCode(op, detail, GetLastError());
}