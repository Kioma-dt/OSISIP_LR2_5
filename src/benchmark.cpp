#include "benchmark.hpp"

#include "app_error.hpp"
#include "copy_engine.hpp"
#include "crc32.hpp"
#include "file_generation.hpp"
#include "file_io.hpp"

#include <algorithm>
#include <fcntl.h>
#include <iomanip>
#include <io.h>
#include <iostream>
#include <sstream>

namespace
{
    double QueryQpcFrequency()
    {
        LARGE_INTEGER frequency;
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0)
            ThrowWin(L"QueryPerformanceFrequency");
        return static_cast<double>(frequency.QuadPart);
    }

    double NowMs()
    {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        static const double frequency = QueryQpcFrequency();
        return static_cast<double>(counter.QuadPart) * 1000.0 / frequency;
    }

    std::wstring Hex32(uint32_t value)
    {
        std::wostringstream stream;
        stream << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0') << value;
        return stream.str();
    }

    std::wstring JoinPath(const std::wstring &directory, const std::wstring &name)
    {
        if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/')
            return directory + L"\\" + name;
        return directory + name;
    }

    void CheckEnoughSpace(const std::wstring &directory, ULONGLONG required)
    {
        ULARGE_INTEGER available{}, total{}, freeBytes{};
        if (!GetDiskFreeSpaceExW(directory.c_str(), &available, &total, &freeBytes))
            ThrowWin(L"GetDiskFreeSpaceExW", directory);
        if (available.QuadPart < required)
        {
            std::wostringstream stream;
            stream << L"Lack of disk space: need ~" << (required >> 20) << L" MB, available " << (available.QuadPart >> 20) << L" MB";
            throw AppError{stream.str()};
        }
    }

    double Median(std::vector<double> values)
    {
        std::sort(values.begin(), values.end());
        size_t count = values.size();
        return count % 2 ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2.0;
    }

    double Mean(const std::vector<double> &values)
    {
        double sum = 0;
        for (double value : values)
            sum += value;
        return sum / static_cast<double>(values.size());
    }

    struct Row
    {
        std::wstring label;
        bool async;
        int ops;
        std::vector<double> times;
        bool sizeOk = true;
        bool crcOk = true;
    };
}

void BenchmarkFile(const Options &options, const std::wstring &srcPath, const std::wstring &dstPath)
{
    ULONGLONG size = GetFileSizeByPath(srcPath);
    std::wcout << L"\n================================================================================\n Source: " << srcPath << L"\n Size  : " << size
               << L" bytes (" << std::fixed << std::setprecision(2) << static_cast<double>(size) / 1048576.0 << L" MB)\n Calculating CRC32... " << std::flush;
    ULONGLONG crcBytes = 0;
    uint32_t sourceCrc = Crc32File(srcPath, crcBytes);
    if (crcBytes != size)
        throw AppError{L"Size read while calculating CRC32 does not match file size"};
    std::wcout << Hex32(sourceCrc) << L"\n";

    for (ULONGLONG blockKB : options.blocksKB)
    {
        DWORD block = static_cast<DWORD>(blockKB * 1024);
        std::vector<Row> rows{{L"Sync", false, 0}};
        for (ULONGLONG ops : options.ops)
            rows.push_back({L"Async x" + std::to_wstring(ops), true, static_cast<int>(ops)});
        for (int run = 1; run <= options.runs; ++run)
            for (Row &row : rows)
            {
                std::wcout << L"\r block " << blockKB << L" KB, run " << run << L"/" << options.runs << L": " << std::left << std::setw(14) << row.label << L" ..." << std::flush;
                DeleteIfExists(dstPath);
                FileCleaner cleaner{dstPath};
                double start = NowMs();
                if (row.async)
                    CopyAsync(srcPath, dstPath, size, block, row.ops, options.noBuffering);
                else
                    CopySync(srcPath, dstPath, size, block, options.noBuffering);
                row.times.push_back(NowMs() - start);
                ULONGLONG destinationSize = GetFileSizeByPath(dstPath);
                ULONGLONG destinationBytes = 0;
                uint32_t destinationCrc = Crc32File(dstPath, destinationBytes);
                row.sizeOk = row.sizeOk && destinationSize == size;
                row.crcOk = row.crcOk && destinationCrc == sourceCrc && destinationBytes == size;
            }
        std::wcout << L"\r" << std::wstring(78, L' ') << L"\r\n Block " << blockKB << L" KB, runs: " << options.runs << L"\n";
        double syncMean = Mean(rows[0].times);
        size_t best = 0;
        std::wcout << std::left << std::setw(15) << L" Mode" << std::right << std::setw(12) << L"Median,ms" << std::setw(12) << L"Mean,ms"
                   << std::setw(10) << L"Min,ms" << std::setw(10) << L"Max,ms" << std::setw(9) << L"MB/s" << std::setw(11) << L"Speedup"
                   << std::setw(8) << L"Size" << std::setw(8) << L"CRC32" << L"\n";
        for (size_t i = 0; i < rows.size(); ++i)
        {
            Row &row = rows[i];
            double mean = Mean(row.times);
            if (mean < Mean(rows[best].times))
                best = i;
            double speed = mean > 0 ? (static_cast<double>(size) / 1048576.0) / (mean / 1000.0) : 0;
            std::wcout << L" " << std::left << std::setw(14) << row.label << std::right << std::fixed << std::setprecision(1)
                       << std::setw(12) << Median(row.times) << std::setw(12) << Mean(row.times) << std::setw(10) << *std::min_element(row.times.begin(), row.times.end())
                       << std::setw(10) << *std::max_element(row.times.begin(), row.times.end()) << std::setw(9) << speed << std::setprecision(2)
                       << std::setw(10) << (mean > 0 ? syncMean / mean : 0.0) << L"x" << std::setw(8) << (row.sizeOk ? L"OK" : L"ERROR")
                       << std::setw(8) << (row.crcOk ? L"OK" : L"ERROR") << L"\n";
        }
        std::wcout << L" Fastest of all (by mean): " << rows[best].label << L"\n";
    }
}

int Run(const Options &options)
{
    std::wstring directory = options.dir;
    if (directory.empty())
    {
        wchar_t tempPath[MAX_PATH + 2];
        DWORD length = GetTempPathW(MAX_PATH + 1, tempPath);
        if (!length || length > MAX_PATH)
            ThrowWin(L"GetTempPathW");
        directory.assign(tempPath, length);
    }
    DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        ThrowWin(L"GetFileAttributesW (working directory)", directory);
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        throw AppError{L"--dir: specified path is not a directory: " + directory};
    std::wstring pid = std::to_wstring(GetCurrentProcessId());
    std::wstring destination = JoinPath(directory, L"asynccopy_" + pid + L"_dst.bin");
    if (!options.src.empty())
    {
        DWORD sourceAttributes = GetFileAttributesW(options.src.c_str());
        if (sourceAttributes == INVALID_FILE_ATTRIBUTES)
            ThrowWin(L"GetFileAttributesW (source file)", options.src);
        if (sourceAttributes & FILE_ATTRIBUTE_DIRECTORY)
            throw AppError{L"--src: source is a directory, not a file: " + options.src};
        CheckEnoughSpace(directory, GetFileSizeByPath(options.src) + (64ull << 20));
        BenchmarkFile(options, options.src, destination);
    }
    else
    {
        for (ULONGLONG sizeMB : options.sizesMB)
        {
            ULONGLONG bytes = sizeMB << 20;
            std::wstring source = JoinPath(directory, L"asynccopy_" + pid + L"_src_" + std::to_wstring(sizeMB) + L"MB.bin");
            try
            {
                CheckEnoughSpace(directory, bytes * 2 + (64ull << 20));
            }
            catch (const AppError &error)
            {
                std::wcout << L"\nFile " << sizeMB << L" MB skipped.\n"
                           << error.message << L"\n";
                continue;
            }
            std::wcout << L"\nGenerating test file " << sizeMB << L" MB... " << std::flush;
            FileCleaner cleaner{options.keep ? std::wstring() : source};
            GenerateFile(source, bytes);
            std::wcout << L"done\n";
            BenchmarkFile(options, source, destination);
        }
    }
    return 0;
}