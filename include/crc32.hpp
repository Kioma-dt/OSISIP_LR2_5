#pragma once

#include <cstdint>
#include <string>
#include <windows.h>

void InitCrcTables();
uint32_t Crc32File(const std::wstring &path, ULONGLONG &totalBytes);