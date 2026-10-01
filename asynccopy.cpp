// ============================================================================
//  asynccopy.cpp
//  Вариант 1. Асинхронное копирование файла (WinAPI, FILE_FLAG_OVERLAPPED)
//
//  Режимы копирования:
//    1) синхронный:   ReadFile() -> WriteFile()
//    2) асинхронный:  FILE_FLAG_OVERLAPPED, N одновременно инициированных
//                     операций (по умолчанию N = 1, 2, 4, 8), блок 64 КБ.
//
//  Для каждого режима: замер времени (QueryPerformanceCounter), проверка
//  совпадения размеров, контрольная сумма CRC-32 исходного и результирующего
//  файлов, медиана/среднее по нескольким прогонам, сравнение производительности.
//
//  Сборка (файл сохранён в UTF-8 с BOM):
//    MSVC : cl /EHsc /O2 /std:c++17 /utf-8 asynccopy.cpp
//    MinGW: g++ -O2 -std=c++17 -municode -static asynccopy.cpp -o asynccopy.exe
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <random>

// ----------------------------------------------------------------------------
//  Ошибки
// ----------------------------------------------------------------------------

struct AppError
{
    std::wstring message;
};

static std::wstring WinErrText(DWORD code)
{
    LPWSTR buffer = nullptr;
    DWORD msgLength = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring errorMessage;
    if (msgLength && buffer)
    {
        errorMessage.assign(buffer, msgLength);
        LocalFree(buffer);
    }
    else
    {
        errorMessage = L"(description unavailable)";
    }
    while (!errorMessage.empty() && (errorMessage.back() == L'\r' || errorMessage.back() == L'\n' || errorMessage.back() == L' '))
        errorMessage.pop_back();
    return errorMessage;
}

// Формирует исключение с названием операции, кодом GetLastError() и описанием.
[[noreturn]] static void ThrowWinCode(const wchar_t *op, const std::wstring &detail, DWORD code)
{
    std::wostringstream ss;
    ss << L"WinAPI Error\n"
       << L"  Operation     : " << op << L"\n";
    if (!detail.empty())
        ss << L"  Object       : " << detail << L"\n";
    ss << L"  GetLastError : " << code << L"\n"
       << L"  Description   : " << WinErrText(code);
    throw AppError{ss.str()};
}

[[noreturn]] static void ThrowWin(const wchar_t *op, const std::wstring &detail = std::wstring())
{
    DWORD code = GetLastError(); // читаем сразу, пока код не затёрт
    ThrowWinCode(op, detail, code);
}

// ----------------------------------------------------------------------------
//  RAII-обёртки над системными ресурсами
// ----------------------------------------------------------------------------

class UniqueHandle
{
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : h_(h) {}
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    UniqueHandle &operator=(UniqueHandle &&o) noexcept
    {
        if (this != &o)
        {
            close();
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }
    ~UniqueHandle() { close(); }

    void reset(HANDLE h) noexcept
    {
        close();
        h_ = h;
    }
    void close() noexcept
    {
        if (valid())
            CloseHandle(h_);
        h_ = nullptr;
    }
    bool valid() const noexcept { return h_ && h_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const noexcept { return h_; }

private:
    HANDLE h_ = nullptr;
};

// Страничный (а значит выровненный по сектору) буфер.
class VBuffer
{
public:
    explicit VBuffer(SIZE_T bytes)
    {
        p_ = static_cast<BYTE *>(VirtualAlloc(nullptr, bytes ? bytes : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!p_)
            ThrowWin(L"VirtualAlloc");
    }
    VBuffer(const VBuffer &) = delete;
    VBuffer &operator=(const VBuffer &) = delete;
    ~VBuffer()
    {
        if (p_)
            VirtualFree(p_, 0, MEM_RELEASE);
    }
    BYTE *data() const noexcept { return p_; }

private:
    BYTE *p_ = nullptr;
};

// Удаляет файл при выходе из области видимости (только наши временные файлы).
struct FileCleaner
{
    std::wstring path;
    ~FileCleaner()
    {
        if (!path.empty())
            DeleteFileW(path.c_str());
    }
};

// ----------------------------------------------------------------------------
//  Вспомогательные функции
// ----------------------------------------------------------------------------

static constexpr DWORD kAlign = 4096; // выравнивание для FILE_FLAG_NO_BUFFERING

static inline ULONGLONG RoundUp(ULONGLONG v, ULONGLONG a) { return (v + a - 1) / a * a; }

static double g_qpcFreq = 1.0;

std::mt19937_64 g_random{std::random_device{}()};

static double NowMs()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / g_qpcFreq;
}

static std::wstring Hex32(uint32_t v)
{
    std::wostringstream o;
    o << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0') << v;
    return o.str();
}

static UniqueHandle OpenSource(const std::wstring &path, bool isOverlapped, bool noBuffering)
{
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (isOverlapped ? FILE_FLAG_OVERLAPPED : 0) | (noBuffering ? FILE_FLAG_NO_BUFFERING : 0);
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (open source)", path);
    return UniqueHandle(handle);
}

static UniqueHandle OpenDest(const std::wstring &path, bool isOverlapped, bool noBuffering)
{
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (isOverlapped ? FILE_FLAG_OVERLAPPED : 0) | (noBuffering ? FILE_FLAG_NO_BUFFERING : 0);
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (create destination)", path);
    return UniqueHandle(handle);
}

static ULONGLONG GetFileSizeByPath(const std::wstring &path)
{
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (size determination)", path);
    UniqueHandle uniqueHandle(handle);
    LARGE_INTEGER fileSizeResult;
    if (!GetFileSizeEx(uniqueHandle.get(), &fileSizeResult))
        ThrowWin(L"GetFileSizeEx", path);
    return static_cast<ULONGLONG>(fileSizeResult.QuadPart);
}

static void DeleteIfExists(const std::wstring &path)
{
    if (!DeleteFileW(path.c_str()))
    {
        DWORD errorCode = GetLastError();
        if (errorCode != ERROR_FILE_NOT_FOUND && errorCode != ERROR_PATH_NOT_FOUND)
            ThrowWinCode(L"DeleteFileW", path, errorCode);
    }
}

// Устанавливает точный размер файла (нужно после записи с NO_BUFFERING,
// где последний блок записывается с добиванием до границы сектора).
static void TruncateFile(const std::wstring &path, ULONGLONG size)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (truncate file)", path);
    UniqueHandle uniqueHandle(handle);
    LARGE_INTEGER largeIntOffset;
    largeIntOffset.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(uniqueHandle.get(), largeIntOffset, nullptr, FILE_BEGIN))
        ThrowWin(L"SetFilePointerEx (truncate)", path);
    if (!SetEndOfFile(uniqueHandle.get()))
        ThrowWin(L"SetEndOfFile (truncate)", path);
}

// ----------------------------------------------------------------------------
//  CRC-32 (IEEE 802.3, полином 0xEDB88320), алгоритм slicing-by-8
// ----------------------------------------------------------------------------

static uint32_t g_crcTab[8][256];

static void InitCrcTables()
{
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
        g_crcTab[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t c = g_crcTab[0][i];
        for (int t = 1; t < 8; ++t)
        {
            c = g_crcTab[0][c & 0xFF] ^ (c >> 8);
            g_crcTab[t][i] = c;
        }
    }
}

// crc — промежуточное состояние (начальное значение 0xFFFFFFFF).
static uint32_t Crc32Update(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n >= 8)
    {
        uint32_t a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= crc;
        crc = g_crcTab[7][a & 0xFF] ^ g_crcTab[6][(a >> 8) & 0xFF] ^ g_crcTab[5][(a >> 16) & 0xFF] ^
              g_crcTab[4][a >> 24] ^ g_crcTab[3][b & 0xFF] ^ g_crcTab[2][(b >> 8) & 0xFF] ^
              g_crcTab[1][(b >> 16) & 0xFF] ^ g_crcTab[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--)
        crc = g_crcTab[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

// Читает файл средствами WinAPI и возвращает CRC-32.
static uint32_t Crc32File(const std::wstring &path, ULONGLONG &totalBytes)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (calc CRC32)", path);
    UniqueHandle uniqueHandle(handle);
    std::vector<uint8_t> buffer(1u << 20);
    uint32_t crc = 0xFFFFFFFFu;
    totalBytes = 0;
    for (;;)
    {
        DWORD bytesRead = 0;
        if (!ReadFile(uniqueHandle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr))
            ThrowWin(L"ReadFile (calc CRC32)", path);
        if (bytesRead == 0)
            break;
        crc = Crc32Update(crc, buffer.data(), bytesRead);
        totalBytes += bytesRead;
    }
    return crc ^ 0xFFFFFFFFu;
}

// ----------------------------------------------------------------------------
//  Генерация тестового файла (псевдослучайные несжимаемые данные)
// ----------------------------------------------------------------------------

static void GenerateFile(const std::wstring &path, ULONGLONG bytes)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (create test file)", path);
    UniqueHandle uniqueHandle(handle);

    const DWORD chunkSize = 1u << 20;
    std::vector<uint64_t> buffer(chunkSize / 8);
    uint64_t x = 0x9E3779B97F4A7C15ull ^ bytes;
    ULONGLONG bytesLeft = bytes;
    while (bytesLeft)
    {
        DWORD bytesToWrite = static_cast<DWORD>(std::min<ULONGLONG>(chunkSize, bytesLeft));
        for (auto &value : buffer)
        {
            value = g_random();
        }
        DWORD bytesWritten = 0;
        if (!WriteFile(uniqueHandle.get(), buffer.data(), bytesToWrite, &bytesWritten, nullptr))
            ThrowWin(L"WriteFile (generate test file)", path);
        if (bytesWritten != bytesToWrite)
            throw AppError{L"WriteFile (generate test file): write less bytes than requested"};
        bytesLeft -= bytesToWrite;
    }
}

// ----------------------------------------------------------------------------
//  Синхронное копирование: ReadFile -> WriteFile
// ----------------------------------------------------------------------------

static void CopySync(const std::wstring &src, const std::wstring &dst, DWORD block, bool noBuffering)
{
    UniqueHandle hSource = OpenSource(src, false, noBuffering);
    UniqueHandle hDest = OpenDest(dst, false, noBuffering);
    VBuffer buffer(static_cast<SIZE_T>(RoundUp(block, kAlign)));

    ULONGLONG total = 0;
    for (;;)
    {
        DWORD bytesRead = 0;
        if (!ReadFile(hSource.get(), buffer.data(), block, &bytesRead, nullptr))
            ThrowWin(L"ReadFile (sync read)", src);
        if (bytesRead == 0)
            break;

        DWORD writeLength = bytesRead;
        if (noBuffering)
        { // при NO_BUFFERING длина записи кратна сектору
            writeLength = static_cast<DWORD>(RoundUp(bytesRead, kAlign));
            if (writeLength > bytesRead)
                std::memset(buffer.data() + bytesRead, 0, writeLength - bytesRead);
        }
        DWORD bytesWritten = 0;
        if (!WriteFile(hDest.get(), buffer.data(), writeLength, &bytesWritten, nullptr))
            ThrowWin(L"WriteFile (sync write)", dst);
        if (bytesWritten != writeLength)
            throw AppError{L"WriteFile (sync write): written less bytes than requested"};
        total += bytesRead;
    }
    hSource.close();
    hDest.close();
    if (noBuffering)
        TruncateFile(dst, total);
}

// ----------------------------------------------------------------------------
//  Асинхронное копирование: FILE_FLAG_OVERLAPPED, несколько операций одновременно
//
//  Есть numOps «слотов». Каждый слот владеет своим буфером, событием и OVERLAPPED.
//  Жизненный цикл слота:  чтение блока -> запись этого же блока -> следующий блок.
//  Таким образом, одновременно «в полёте» находится до numOps операций ввода-вывода.
// ----------------------------------------------------------------------------

namespace
{

    enum class State
    {
        Idle,
        Reading,
        Writing,
        Done
    };

    struct Slot
    {
        OVERLAPPED overlapped{};
        UniqueHandle eventHandle; // событие завершения операции (manual-reset)
        BYTE *buffer = nullptr;
        ULONGLONG offset = 0;
        DWORD expected = 0;       // сколько полезных байт ожидаем прочитать
        DWORD writeLength = 0;    // сколько байт пишем
        State state = State::Idle;
    };

} // namespace

static void CopyAsync(const std::wstring &src, const std::wstring &dst, ULONGLONG fileSize, DWORD block, int numOps,
                      bool noBuffering)
{
    UniqueHandle hSource = OpenSource(src, true, noBuffering);
    UniqueHandle hDest = OpenDest(dst, true, noBuffering);

    // Предварительное выделение размера: записи ложатся внутрь файла, а не
    // расширяют его (расширяющие записи Windows выполняет синхронно).
    const ULONGLONG allocSize = noBuffering ? RoundUp(fileSize, kAlign) : fileSize;
    if (allocSize > 0)
    {
        LARGE_INTEGER largeIntOffset;
        largeIntOffset.QuadPart = static_cast<LONGLONG>(allocSize);
        if (!SetFilePointerEx(hDest.get(), largeIntOffset, nullptr, FILE_BEGIN))
            ThrowWin(L"SetFilePointerEx (file enlargement)", dst);
        if (!SetEndOfFile(hDest.get()))
            ThrowWin(L"SetEndOfFile (file enlargement)", dst);
    }

    const SIZE_T stride = static_cast<SIZE_T>(RoundUp(block, kAlign));
    VBuffer memory(stride * static_cast<SIZE_T>(numOps));
    std::vector<Slot> slots(static_cast<size_t>(numOps));
    std::vector<HANDLE> eventHandles;
    for (int i = 0; i < numOps; ++i)
    {
        slots[i].eventHandle.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!slots[i].eventHandle.valid())
            ThrowWin(L"CreateEventW");
        slots[i].buffer = memory.data() + static_cast<size_t>(i) * stride;
        eventHandles.push_back(slots[i].eventHandle.get());
    }

    ULONGLONG nextOffset = 0; // смещение следующего непрочитанного блока
    int activeSlots = 0;      // число слотов, ещё не закончивших работу

    auto prepareOverlapped = [](Slot &slot, ULONGLONG offset)
    {
        ResetEvent(slot.eventHandle.get());
        ZeroMemory(&slot.overlapped, sizeof(OVERLAPPED));
        slot.overlapped.hEvent = slot.eventHandle.get();
        slot.overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
        slot.overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    };

    auto startRead = [&](Slot &slot)
    {
        slot.offset = nextOffset;
        nextOffset += block;
        slot.expected = static_cast<DWORD>(std::min<ULONGLONG>(block, fileSize - slot.offset));
        DWORD requestSize = noBuffering ? static_cast<DWORD>(RoundUp(slot.expected, kAlign)) : slot.expected;
        prepareOverlapped(slot, slot.offset);
        if (!ReadFile(hSource.get(), slot.buffer, requestSize, nullptr, &slot.overlapped))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                ThrowWin(L"ReadFile (async read)", src);
        }
        slot.state = State::Reading;
    };

    auto startWrite = [&](Slot &slot, DWORD bytesToWrite)
    {
        slot.writeLength = bytesToWrite;
        if (noBuffering)
        {
            slot.writeLength = static_cast<DWORD>(RoundUp(bytesToWrite, kAlign));
            if (slot.writeLength > bytesToWrite)
                std::memset(slot.buffer + bytesToWrite, 0, slot.writeLength - bytesToWrite);
        }
        prepareOverlapped(slot, slot.offset);
        if (!WriteFile(hDest.get(), slot.buffer, slot.writeLength, nullptr, &slot.overlapped))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                ThrowWin(L"WriteFile (async write)", dst);
        }
        slot.state = State::Writing;
    };

    // При ошибке нельзя уничтожать буферы/OVERLAPPED, пока ядро ещё работает с ними.
    auto drain = [&]() noexcept
    {
        for (auto &slot : slots)
        {
            if (slot.state == State::Reading || slot.state == State::Writing)
            {
                HANDLE handle = (slot.state == State::Reading) ? hSource.get() : hDest.get();
                CancelIoEx(handle, &slot.overlapped);
                DWORD bytesTransferred = 0;
                GetOverlappedResult(handle, &slot.overlapped, &bytesTransferred, TRUE);
                slot.state = State::Idle;
            }
        }
    };

    try
    {
        for (auto &slot : slots)
        {
            if (nextOffset < fileSize)
            {
                startRead(slot);
                ++activeSlots;
            }
        }

        while (activeSlots > 0)
        {
            DWORD waitResult = WaitForMultipleObjects(static_cast<DWORD>(eventHandles.size()), eventHandles.data(), FALSE, INFINITE);
            if (waitResult == WAIT_FAILED)
                ThrowWin(L"WaitForMultipleObjects");
            if (waitResult >= WAIT_OBJECT_0 + eventHandles.size())
                throw AppError{L"WaitForMultipleObjects: unexpected return value"};

            Slot &slot = slots[waitResult - WAIT_OBJECT_0];
            const bool wasRead = (slot.state == State::Reading);
            HANDLE handle = wasRead ? hSource.get() : hDest.get();

            DWORD bytesTransferred = 0;
            if (!GetOverlappedResult(handle, &slot.overlapped, &bytesTransferred, FALSE))
            {
                DWORD errorCode = GetLastError();
                slot.state = State::Idle;
                ThrowWinCode(wasRead ? L"GetOverlappedResult (async read)" : L"GetOverlappedResult (async write)",
                             wasRead ? src : dst, errorCode);
            }
            slot.state = State::Idle;

            if (wasRead)
            {
                if (bytesTransferred != slot.expected)
                    throw AppError{L"Async read: read unexpected number of bytes"};
                startWrite(slot, bytesTransferred);
            }
            else
            {
                if (bytesTransferred != slot.writeLength)
                    throw AppError{L"Async write: written unexpected number of bytes"};
                if (nextOffset < fileSize)
                {
                    startRead(slot);
                }
                else
                {
                    ResetEvent(slot.eventHandle.get());
                    slot.state = State::Done;
                    --activeSlots;
                }
            }
        }
    }
    catch (...)
    {
        drain();
        throw;
    }

    hSource.close();
    hDest.close();
    if (noBuffering)
        TruncateFile(dst, fileSize);
    // события и буферы освобождаются деструкторами (slots, memory)
}

// ----------------------------------------------------------------------------
//  Параметры командной строки
// ----------------------------------------------------------------------------

struct Options
{
    std::vector<ULONGLONG> sizesMB{10, 100, 500};
    std::vector<ULONGLONG> blocksKB{64};
    std::vector<ULONGLONG> ops{1, 2, 4, 8};
    int runs = 3;
    std::wstring src; // если задан — используем существующий файл
    std::wstring dir; // рабочий каталог для тестовых файлов
    bool noBuffering = false;
    bool keep = false;
};

static void PrintUsage()
{
    std::wcout << LR"(Использование: asynccopy.exe [параметры]

  --src <файл>      копировать существующий файл (вместо генерации тестовых)
  --sizes <список>  размеры генерируемых файлов в МБ, через запятую  [10,100,500]
  --blocks <список> размеры блока в КБ, через запятую                [64]
                    (для доп. исследования: 4,64,1024,4096)
  --ops <список>    числа одновременных асинхронных операций         [1,2,4,8]
  --runs <N>        число прогонов каждого режима                    [3]
  --dir <каталог>   каталог для тестовых файлов                      [%TEMP%]
  --nobuf           FILE_FLAG_NO_BUFFERING (обход файлового кэша ОС;
                    блок должен быть кратен 4 КБ)
  --keep            не удалять сгенерированный исходный файл
  -h, --help        эта справка

Без параметров выполняется полный эксперимент: 10/100/500 МБ, блок 64 КБ,
синхронный режим и асинхронный с 1/2/4/8 операциями, по 3 прогона.
Исходные файлы пользователя только читаются; копии создаются под служебными
именами asynccopy_<pid>_*.bin и удаляются после проверки.
)";
}

static bool ParseList(const std::wstring &listString, std::vector<ULONGLONG> &out)
{
    out.clear();
    size_t pos = 0;
    while (pos <= listString.size())
    {
        size_t c = listString.find(L',', pos);
        if (c == std::wstring::npos)
            c = listString.size();
        std::wstring token = listString.substr(pos, c - pos);
        if (token.empty() || token.size() > 12 || token.find_first_not_of(L"0123456789") != std::wstring::npos)
            return false;
        ULONGLONG parsedValue = _wcstoui64(token.c_str(), nullptr, 10);
        if (parsedValue == 0)
            return false;
        out.push_back(parsedValue);
        pos = c + 1;
    }
    return !out.empty();
}

// true — продолжать работу; false — выйти (code содержит код возврата)
static bool ParseArgs(int argc, wchar_t **argv, Options &options, int &code)
{
    code = 0;
    auto fail = [&](const std::wstring &msg)
    {
        std::wcerr << L"Args error: " << msg << L"\n";
        code = 2;
        return false;
    };
    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        auto getValue = [&](std::wstring &val)
        {
            if (i + 1 >= argc)
                return false;
            val = argv[++i];
            return true;
        };
        std::wstring valStr;
        if (arg == L"-h" || arg == L"--help" || arg == L"/?" || arg == L"-?")
        {
            PrintUsage();
            return false;
        }
        else if (arg == L"--nobuf")
        {
            options.noBuffering = true;
        }
        else if (arg == L"--keep")
        {
            options.keep = true;
        }
        else if (arg == L"--src")
        {
            if (!getValue(options.src) || options.src.empty())
                return fail(L"--src requires a file path");
        }
        else if (arg == L"--dir")
        {
            if (!getValue(options.dir) || options.dir.empty())
                return fail(L"--dir requires a directory path");
        }
        else if (arg == L"--sizes")
        {
            if (!getValue(valStr) || !ParseList(valStr, options.sizesMB))
                return fail(L"--sizes: required list of positive numbers (MB)");
        }
        else if (arg == L"--blocks")
        {
            if (!getValue(valStr) || !ParseList(valStr, options.blocksKB))
                return fail(L"--blocks: expected list of positive numbers (KB)");
        }
        else if (arg == L"--ops")
        {
            if (!getValue(valStr) || !ParseList(valStr, options.ops))
                return fail(L"--ops: expected list of positive numbers");
        }
        else if (arg == L"--runs")
        {
            std::vector<ULONGLONG> t;
            if (!getValue(valStr) || !ParseList(valStr, t) || t.size() != 1)
                return fail(L"--runs: expected one positive number");
            options.runs = static_cast<int>(std::min<ULONGLONG>(t[0], 1000));
        }
        else
        {
            return fail(L"unknown option " + arg);
        }
    }
    for (ULONGLONG n : options.ops)
        if (n > 64)
            return fail(L"operations cannot exceed 64 (limit of WaitForMultipleObjects)");
    for (ULONGLONG b : options.blocksKB)
    {
        if (b > 262144)
            return fail(L"block size cannot exceed 256 МБ (262144 КБ)");
        if (options.noBuffering && b % 4 != 0)
            return fail(L"with --nobuf block size must be a multiple of 4 KB");
    }
    for (ULONGLONG size : options.sizesMB)
        if (size > 4ull * 1024 * 1024)
            return fail(L"file size in --sizes cannot exceed 4 GB (4096 МБ)");
    return true;
}

// ----------------------------------------------------------------------------
//  Статистика и вывод
// ----------------------------------------------------------------------------

static double Median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    size_t n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}
static double Mean(const std::vector<double> &values)
{
    double sum = 0;
    for (double x : values)
        sum += x;
    return sum / static_cast<double>(values.size());
}

struct Config
{
    std::wstring label;
    bool async;
    int ops;
};

struct Row
{
    Config cfg;
    std::vector<double> times; // время каждого прогона, мс
    bool isSizeOk = true;
    bool isCrcOk = true;
};

static std::wstring JoinPath(const std::wstring &dir, const std::wstring &name)
{
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
        return dir + L"\\" + name;
    return dir + name;
}

static void CheckEnoughSpace(const std::wstring &dir, ULONGLONG needBytes)
{
    ULARGE_INTEGER available{}, total{}, freeBytes{};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &available, &total, &freeBytes))
        ThrowWin(L"GetDiskFreeSpaceExW", dir);
    if (available.QuadPart < needBytes)
    {
        std::wostringstream ss;
        ss << L"Lack of disk space: need ~" << (needBytes >> 20) << L" MB, available " << (available.QuadPart >> 20) << L" MB";
        throw AppError{ss.str()};
    }
}

static void BenchmarkFile(const Options &options, const std::wstring &srcPath, const std::wstring &dstPath)
{
    const ULONGLONG size = GetFileSizeByPath(srcPath);

    std::wcout << L"\n================================================================================\n"
               << L" Source: " << srcPath << L"\n Size  : " << size << L" bytes (" << std::fixed << std::setprecision(2)
               << static_cast<double>(size) / 1048576.0 << L" MB)\n"
               << L" Calculating CRC32 of the source... " << std::flush;
    ULONGLONG crcBytesTotal = 0;
    const uint32_t crcSrc = Crc32File(srcPath, crcBytesTotal);
    if (crcBytesTotal != size)
        throw AppError{L"Size read while calculating CRC32 does not match file size"};
    std::wcout << Hex32(crcSrc) << L"\n";

    for (ULONGLONG blockSizeKB : options.blocksKB)
    {
        const DWORD blockSize = static_cast<DWORD>(blockSizeKB * 1024);

        std::vector<Row> rows;
        rows.push_back(Row{Config{L"Sync", false, 0}, {}, true, true});
        for (ULONGLONG n : options.ops)
        {
            std::wstring labelStr = L"Async x" + std::to_wstring(n);
            rows.push_back(Row{Config{labelStr, true, static_cast<int>(n)}, {}, true, true});
        }

        // Прогоны чередуются по режимам, чтобы влияние кэша ОС распределялось равномерно.
        for (int run = 1; run <= options.runs; ++run)
        {
            for (auto &row : rows)
            {
                std::wcout << L"\r  block " << blockSizeKB << L" KB, run " << run << L"/" << options.runs << L": " << std::left
                           << std::setw(14) << row.cfg.label << L" ...                    " << std::flush;

                DeleteIfExists(dstPath);
                FileCleaner cleaner{dstPath};

                double startTime = NowMs();
                if (row.cfg.async)
                    CopyAsync(srcPath, dstPath, size, blockSize, row.cfg.ops, options.noBuffering);
                else
                    CopySync(srcPath, dstPath, blockSize, options.noBuffering);
                double elapsedMs = NowMs() - startTime;

                // Проверки (вне замера времени)
                const ULONGLONG destSize = GetFileSizeByPath(dstPath);
                ULONGLONG crcBytesDst = 0;
                const uint32_t crcDst = Crc32File(dstPath, crcBytesDst);
                const bool isSizeOk = (destSize == size);
                const bool isCrcOk = (crcDst == crcSrc) && (crcBytesDst == size);
                row.isSizeOk = row.isSizeOk && isSizeOk;
                row.isCrcOk = row.isCrcOk && isCrcOk;
                row.times.push_back(elapsedMs);
            }
        }
        std::wcout << L"\r" << std::wstring(78, L' ') << L"\r";

        // ---- Таблица ----
        const double syncMed = Median(rows[0].times);
        const double mb = static_cast<double>(size) / 1048576.0;
        std::wcout << L"\n Block " << blockSizeKB << L" KB, runs: " << options.runs << L", mode: "
                   << (options.noBuffering ? L"without OS cache (NO_BUFFERING)" : L"with OS cache") << L"\n";
        std::wcout << std::left << std::setw(15) << L" Mode" << std::right << std::setw(12) << L"Median,ms" << std::setw(12)
                   << L"Mean,ms" << std::setw(10) << L"Min,ms" << std::setw(10) << L"Max,ms" << std::setw(9) << L"MB/s"
                   << std::setw(11) << L"Speedup" << std::setw(8) << L"Size" << std::setw(8) << L"CRC32" << L"\n"
                   << L" " << std::wstring(94, L'-') << L"\n";
        size_t bestIndex = 0;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const Row &row = rows[i];
            double medianTime = Median(row.times);
            if (medianTime < Median(rows[bestIndex].times))
                bestIndex = i;
            double minTime = *std::min_element(row.times.begin(), row.times.end());
            double maxTime = *std::max_element(row.times.begin(), row.times.end());
            double speedMBps = medianTime > 0 ? mb / (medianTime / 1000.0) : 0;
            std::wcout << L" " << std::left << std::setw(14) << row.cfg.label << std::right << std::fixed << std::setprecision(1)
                       << std::setw(12) << medianTime << std::setw(12) << Mean(row.times) << std::setw(10) << minTime << std::setw(10) << maxTime
                       << std::setw(9) << speedMBps << std::setprecision(2) << std::setw(10) << (medianTime > 0 ? syncMed / medianTime : 0.0)
                       << L"x" << std::setw(8) << (row.isSizeOk ? L"OK" : L"ERROR") << std::setw(8)
                       << (row.isCrcOk ? L"OK" : L"ERROR") << L"\n";
        }
        std::wcout << L" Fastest of all (by median): " << rows[bestIndex].cfg.label << L"\n";
        for (const auto &row : rows)
            if (!row.isSizeOk || !row.isCrcOk)
                std::wcout << L" WARNING: mode «" << row.cfg.label << L"» produced an incorrect copy!\n";
    }
}

// ----------------------------------------------------------------------------
//  Основная логика
// ----------------------------------------------------------------------------

static int Run(const Options &options)
{
    // Рабочий каталог
    std::wstring directory = options.dir;
    if (directory.empty())
    {
        wchar_t tempPath[MAX_PATH + 2];
        DWORD pathLength = GetTempPathW(MAX_PATH + 1, tempPath);
        if (pathLength == 0 || pathLength > MAX_PATH)
            ThrowWin(L"GetTempPathW");
        directory.assign(tempPath, pathLength);
    }
    DWORD fileAttr = GetFileAttributesW(directory.c_str());
    if (fileAttr == INVALID_FILE_ATTRIBUTES)
        ThrowWin(L"GetFileAttributesW (working directory)", directory);
    if (!(fileAttr & FILE_ATTRIBUTE_DIRECTORY))
        throw AppError{L"--dir: specified path is not a directory: " + directory};

    const std::wstring pidStr = std::to_wstring(GetCurrentProcessId());
    const std::wstring dstPath = JoinPath(directory, L"asynccopy_" + pidStr + L"_dst.bin");

    std::wcout << L"Async copy (WinAPI, FILE_FLAG_OVERLAPPED)\n"
               << L"Working directory: " << directory << L"\n"
               << L"Blocks, KB: ";
    for (auto b : options.blocksKB)
        std::wcout << b << L" ";
    std::wcout << L"| async operations: ";
    for (auto n : options.ops)
        std::wcout << n << L" ";
    std::wcout << L"| runs: " << options.runs << L"\n";

    if (!options.src.empty())
    {
        // ---- Пользовательский входной файл (только чтение) ----
        DWORD attr = GetFileAttributesW(options.src.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
            ThrowWin(L"GetFileAttributesW (source file)", options.src);
        if (attr & FILE_ATTRIBUTE_DIRECTORY)
            throw AppError{L"--src: source is a directory, not a file: " + options.src};
        if (_wcsicmp(options.src.c_str(), dstPath.c_str()) == 0)
            throw AppError{L"Input and output files are the same"};
        CheckEnoughSpace(directory, GetFileSizeByPath(options.src) + (64ull << 20));
        BenchmarkFile(options, options.src, dstPath);
    }
    else
    {
        // ---- Генерируемые тестовые файлы ----
        for (ULONGLONG sizeMB : options.sizesMB)
        {
            const ULONGLONG bytes = sizeMB << 20;
            const std::wstring srcPath = JoinPath(directory, L"asynccopy_" + pidStr + L"_src_" + std::to_wstring(sizeMB) + L"MB.bin");
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
            FileCleaner cleaner{options.keep ? std::wstring() : srcPath};
            GenerateFile(srcPath, bytes);
            std::wcout << L"done\n";
            BenchmarkFile(options, srcPath, dstPath);
            if (options.keep)
                std::wcout << L" Source file saved: " << srcPath << L"\n";
        }
    }

    std::wcout << LR"(
Notes:
 * Without --nobuf, reading is primarily from the OS file cache (source already
   read during CRC32 calculation), and writing goes to the write cache, so the numbers
   reflect the speed of memory operations and API overhead. To evaluate the speed of
   the disk itself, run with --nobuf (and on files larger than RAM capacity).
 * The benchmark includes: opening files, allocating buffers/events, pre-allocating
   copy size (only in asynchronous mode), and closing file handles.
   CRC32 calculation and size verification are not included in the benchmark.
)";
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    LARGE_INTEGER freq;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
    {
        std::wcerr << L"QueryPerformanceFrequency failed\n";
        return 1;
    }
    g_qpcFreq = static_cast<double>(freq.QuadPart);
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
    catch (const std::exception &exception)
    {
        std::wcerr << L"\nException: " << exception.what() << L"\n";
        return 1;
    }
}