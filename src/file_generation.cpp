#include "file_generation.hpp"

#include "app_error.hpp"
#include "resources.hpp"

#include <random>
#include <vector>

namespace
{
    std::mt19937_64 randomGenerator{std::random_device{}()};
}

void GenerateFile(const std::wstring &path, std::uint64_t bytes)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (create test file)", path);
    UniqueHandle file(handle);
    const DWORD chunkSize = 1u << 20;
    std::vector<uint64_t> buffer(chunkSize / 8);
    std::uint64_t remaining = bytes;
    while (remaining)
    {
        DWORD count = static_cast<DWORD>(std::min<ULONGLONG>(chunkSize, remaining));
        for (uint64_t &value : buffer)
            value = randomGenerator();
        DWORD written = 0;
        if (!WriteFile(file.get(), buffer.data(), count, &written, nullptr))
            ThrowWin(L"WriteFile (generate test file)", path);
        if (written != count)
            throw AppError{L"WriteFile (generate test file): write less bytes than requested"};
        remaining -= count;
    }
}