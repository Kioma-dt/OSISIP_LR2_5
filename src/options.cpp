#include "options.hpp"

#include <algorithm>
#include <cwchar>
#include <iostream>

namespace
{
    bool ParseList(const std::wstring &text, std::vector<ULONGLONG> &values)
    {
        values.clear();
        size_t position = 0;
        while (position <= text.size())
        {
            size_t comma = text.find(L',', position);
            if (comma == std::wstring::npos)
                comma = text.size();
            std::wstring token = text.substr(position, comma - position);
            if (token.empty() || token.size() > 12 || token.find_first_not_of(L"0123456789") != std::wstring::npos)
                return false;
            ULONGLONG value = _wcstoui64(token.c_str(), nullptr, 10);
            if (!value)
                return false;
            values.push_back(value);
            position = comma + 1;
        }
        return !values.empty();
    }
}

void PrintUsage()
{
    std::wcout << LR"(Использование: asynccopy.exe [параметры]

  --src <файл>      копировать существующий файл
  --sizes <список>  размеры генерируемых файлов в МБ, через запятую [10,100,500]
  --blocks <список> размеры блока в КБ, через запятую [64]
  --ops <список>    числа одновременных асинхронных операций [1,2,4,8]
  --runs <N>        число прогонов каждого режима [3]
  --dir <каталог>   каталог для тестовых файлов [%TEMP%]
  --nobuf           FILE_FLAG_NO_BUFFERING, блок кратен 4 КБ
  --keep            не удалять сгенерированный исходный файл
  -h, --help        эта справка
)";
}

bool ParseArgs(int argc, wchar_t **argv, Options &options, int &code)
{
    code = 0;
    auto fail = [&](const std::wstring &message)
    { std::wcerr << L"Args error: " << message << L"\n"; code = 2; return false; };
    for (int i = 1; i < argc; ++i)
    {
        std::wstring argument = argv[i];
        auto getValue = [&](std::wstring &value)
        { if (i + 1 >= argc) return false; value = argv[++i]; return true; };
        std::wstring value;
        if (argument == L"-h" || argument == L"--help" || argument == L"/?" || argument == L"-?")
        {
            PrintUsage();
            return false;
        }
        if (argument == L"--nobuf")
            options.noBuffering = true;
        else if (argument == L"--keep")
            options.keep = true;
        else if (argument == L"--src")
        {
            if (!getValue(options.src) || options.src.empty())
                return fail(L"--src requires a file path");
        }
        else if (argument == L"--dir")
        {
            if (!getValue(options.dir) || options.dir.empty())
                return fail(L"--dir requires a directory path");
        }
        else if (argument == L"--sizes")
        {
            if (!getValue(value) || !ParseList(value, options.sizesMB))
                return fail(L"--sizes: expected positive numbers");
        }
        else if (argument == L"--blocks")
        {
            if (!getValue(value) || !ParseList(value, options.blocksKB))
                return fail(L"--blocks: expected positive numbers");
        }
        else if (argument == L"--ops")
        {
            if (!getValue(value) || !ParseList(value, options.ops))
                return fail(L"--ops: expected positive numbers");
        }
        else if (argument == L"--runs")
        {
            std::vector<ULONGLONG> parsed;
            if (!getValue(value) || !ParseList(value, parsed) || parsed.size() != 1)
                return fail(L"--runs: expected one positive number");
            options.runs = static_cast<int>(std::min<ULONGLONG>(parsed[0], 1000));
        }
        else
            return fail(L"unknown option " + argument);
    }
    for (ULONGLONG count : options.ops)
        if (count > 64)
            return fail(L"operations cannot exceed 64");
    for (ULONGLONG block : options.blocksKB)
    {
        if (block > 262144)
            return fail(L"block size cannot exceed 256 MB");
        if (options.noBuffering && block % 4)
            return fail(L"with --nobuf block size must be a multiple of 4 KB");
    }
    for (ULONGLONG size : options.sizesMB)
        if (size > 4ull * 1024 * 1024)
            return fail(L"file size cannot exceed 4 GB");
    return true;
}