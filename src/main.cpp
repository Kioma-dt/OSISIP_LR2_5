#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "app_error.hpp"
#include "benchmark.hpp"
#include "crc32.hpp"

#include <fcntl.h>
#include <io.h>
#include <iostream>

//?g++ -O2 -std=c++17 -municode -static src/*  -IInclude -o asynccopy.exe

int wmain(int argc, wchar_t **argv)
{
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0)
    {
        std::wcerr << L"QueryPerformanceFrequency failed\n";
        return 1;
    }
    InitCrcTables();
    Options options;
    int code = 0;
    if (!ParseArgs(argc, argv, options, code))
        return code;
    try
    {
        return Run(options);
    }
    catch (const AppError &error)
    {
        std::wcerr << L"\n"
                   << error.message << L"\n";
        return 1;
    }
    catch (const std::exception &error)
    {
        std::wcerr << L"\nException: " << error.what() << L"\n";
        return 1;
    }
}