#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct Options
{
    std::vector<ULONGLONG> sizesMB{10, 100, 500};
    std::vector<ULONGLONG> blocksKB{64};
    std::vector<ULONGLONG> ops{1, 2, 4, 8};
    int runs = 3;
    std::wstring src;
    std::wstring dir;
    bool noBuffering = false;
    bool keep = false;
};

void PrintUsage();
bool ParseArgs(int argc, wchar_t **argv, Options &options, int &code);