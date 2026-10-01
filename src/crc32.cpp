#include "crc32.hpp"

#include "app_error.hpp"
#include "resources.hpp"

#include <cstring>
#include <vector>

namespace
{
    uint32_t crcTable[8][256];

    uint32_t Update(uint32_t crc, const uint8_t *data, size_t size)
    {
        while (size >= 8)
        {
            uint32_t first, second;
            std::memcpy(&first, data, 4);
            std::memcpy(&second, data + 4, 4);
            first ^= crc;
            crc = crcTable[7][first & 0xFF] ^ crcTable[6][(first >> 8) & 0xFF] ^ crcTable[5][(first >> 16) & 0xFF] ^
                  crcTable[4][first >> 24] ^ crcTable[3][second & 0xFF] ^ crcTable[2][(second >> 8) & 0xFF] ^
                  crcTable[1][(second >> 16) & 0xFF] ^ crcTable[0][second >> 24];
            data += 8;
            size -= 8;
        }
        while (size--)
            crc = crcTable[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
        return crc;
    }
}

void InitCrcTables()
{
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
        crcTable[0][i] = value;
    }
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t value = crcTable[0][i];
        for (int table = 1; table < 8; ++table)
        {
            value = crcTable[0][value & 0xFF] ^ (value >> 8);
            crcTable[table][i] = value;
        }
    }
}

uint32_t Crc32File(const std::wstring &path, ULONGLONG &totalBytes)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (calc CRC32)", path);
    UniqueHandle file(handle);
    std::vector<uint8_t> buffer(1u << 20);
    uint32_t crc = 0xFFFFFFFFu;
    totalBytes = 0;
    for (;;)
    {
        DWORD bytesRead = 0;
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr))
            ThrowWin(L"ReadFile (calc CRC32)", path);
        if (!bytesRead)
            break;
        crc = Update(crc, buffer.data(), bytesRead);
        totalBytes += bytesRead;
    }
    return crc ^ 0xFFFFFFFFu;
}