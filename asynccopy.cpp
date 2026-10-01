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
    LPWSTR buf = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s;
    if (n && buf)
    {
        s.assign(buf, n);
        LocalFree(buf);
    }
    else
    {
        s = L"(description unavailable)";
    }
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
        s.pop_back();
    return s;
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

static UniqueHandle OpenSource(const std::wstring &path, bool overlapped, bool nobuf)
{
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0) | (nobuf ? FILE_FLAG_NO_BUFFERING : 0);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (open source)", path);
    return UniqueHandle(h);
}

static UniqueHandle OpenDest(const std::wstring &path, bool overlapped, bool nobuf)
{
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0) | (nobuf ? FILE_FLAG_NO_BUFFERING : 0);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (create destination)", path);
    return UniqueHandle(h);
}

static ULONGLONG GetFileSizeByPath(const std::wstring &path)
{
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (size determination)", path);
    UniqueHandle uh(h);
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(uh.get(), &sz))
        ThrowWin(L"GetFileSizeEx", path);
    return static_cast<ULONGLONG>(sz.QuadPart);
}

static void DeleteIfExists(const std::wstring &path)
{
    if (!DeleteFileW(path.c_str()))
    {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
            ThrowWinCode(L"DeleteFileW", path, e);
    }
}

// Устанавливает точный размер файла (нужно после записи с NO_BUFFERING,
// где последний блок записывается с добиванием до границы сектора).
static void TruncateFile(const std::wstring &path, ULONGLONG size)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (truncate file)", path);
    UniqueHandle uh(h);
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(uh.get(), li, nullptr, FILE_BEGIN))
        ThrowWin(L"SetFilePointerEx (truncate)", path);
    if (!SetEndOfFile(uh.get()))
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
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (расчёт CRC32)", path);
    UniqueHandle uh(h);
    std::vector<uint8_t> buf(1u << 20);
    uint32_t crc = 0xFFFFFFFFu;
    totalBytes = 0;
    for (;;)
    {
        DWORD got = 0;
        if (!ReadFile(uh.get(), buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr))
            ThrowWin(L"ReadFile (расчёт CRC32)", path);
        if (got == 0)
            break;
        crc = Crc32Update(crc, buf.data(), got);
        totalBytes += got;
    }
    return crc ^ 0xFFFFFFFFu;
}

// ----------------------------------------------------------------------------
//  Генерация тестового файла (псевдослучайные несжимаемые данные)
// ----------------------------------------------------------------------------

static void GenerateFile(const std::wstring &path, ULONGLONG bytes)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (create test file)", path);
    UniqueHandle uh(h);

    const DWORD chunk = 1u << 20;
    std::vector<uint64_t> buf(chunk / 8);
    uint64_t x = 0x9E3779B97F4A7C15ull ^ bytes;
    ULONGLONG left = bytes;
    while (left)
    {
        DWORD n = static_cast<DWORD>(std::min<ULONGLONG>(chunk, left));
        // for (auto &v : buf)
        // { // xorshift64*
        //     x ^= x << 13;
        //     x ^= x >> 7;
        //     x ^= x << 17;
        //     v = x * 0x2545F4914F6CDD1Dull;
        // }
        for (auto &value : buf)
        {
            value = g_random();
        }
        DWORD w = 0;
        if (!WriteFile(uh.get(), buf.data(), n, &w, nullptr))
            ThrowWin(L"WriteFile (generate test file)", path);
        if (w != n)
            throw AppError{L"WriteFile (generate test file): write less bytes than requested"};
        left -= n;
    }
}

// ----------------------------------------------------------------------------
//  Синхронное копирование: ReadFile -> WriteFile
// ----------------------------------------------------------------------------

static void CopySync(const std::wstring &src, const std::wstring &dst, DWORD block, bool nobuf)
{
    UniqueHandle hs = OpenSource(src, false, nobuf);
    UniqueHandle hd = OpenDest(dst, false, nobuf);
    VBuffer buf(static_cast<SIZE_T>(RoundUp(block, kAlign)));

    ULONGLONG total = 0;
    for (;;)
    {
        DWORD got = 0;
        if (!ReadFile(hs.get(), buf.data(), block, &got, nullptr))
            ThrowWin(L"ReadFile (sync read)", src);
        if (got == 0)
            break;

        DWORD wlen = got;
        if (nobuf)
        { // при NO_BUFFERING длина записи кратна сектору
            wlen = static_cast<DWORD>(RoundUp(got, kAlign));
            if (wlen > got)
                std::memset(buf.data() + got, 0, wlen - got);
        }
        DWORD wr = 0;
        if (!WriteFile(hd.get(), buf.data(), wlen, &wr, nullptr))
            ThrowWin(L"WriteFile (sync write)", dst);
        if (wr != wlen)
            throw AppError{L"WriteFile (sync write): written less bytes than requested"};
        total += got;
    }
    hs.close();
    hd.close();
    if (nobuf)
        TruncateFile(dst, total);
}

// ----------------------------------------------------------------------------
//  Асинхронное копирование: FILE_FLAG_OVERLAPPED, несколько операций одновременно
//
//  Есть ops «слотов». Каждый слот владеет своим буфером, событием и OVERLAPPED.
//  Жизненный цикл слота:  чтение блока -> запись этого же блока -> следующий блок.
//  Таким образом, одновременно «в полёте» находится до ops операций ввода-вывода.
// ----------------------------------------------------------------------------

namespace
{

    enum class St
    {
        Idle,
        Reading,
        Writing,
        Done
    };

    struct Slot
    {
        OVERLAPPED ov{};
        UniqueHandle ev; // событие завершения операции (manual-reset)
        BYTE *buf = nullptr;
        ULONGLONG offset = 0;
        DWORD expected = 0; // сколько полезных байт ожидаем прочитать
        DWORD wlen = 0;     // сколько байт пишем
        St state = St::Idle;
    };

} // namespace

static void CopyAsync(const std::wstring &src, const std::wstring &dst, ULONGLONG fileSize, DWORD block, int ops,
                      bool nobuf)
{
    UniqueHandle hs = OpenSource(src, true, nobuf);
    UniqueHandle hd = OpenDest(dst, true, nobuf);

    // Предварительное выделение размера: записи ложатся внутрь файла, а не
    // расширяют его (расширяющие записи Windows выполняет синхронно).
    const ULONGLONG alloc = nobuf ? RoundUp(fileSize, kAlign) : fileSize;
    if (alloc > 0)
    {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(alloc);
        if (!SetFilePointerEx(hd.get(), li, nullptr, FILE_BEGIN))
            ThrowWin(L"SetFilePointerEx (file enlargement)", dst);
        if (!SetEndOfFile(hd.get()))
            ThrowWin(L"SetEndOfFile (file enlargement)", dst);
    }

    const SIZE_T stride = static_cast<SIZE_T>(RoundUp(block, kAlign));
    VBuffer mem(stride * static_cast<SIZE_T>(ops));
    std::vector<Slot> slots(static_cast<size_t>(ops));
    std::vector<HANDLE> evs;
    for (int i = 0; i < ops; ++i)
    {
        slots[i].ev.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!slots[i].ev.valid())
            ThrowWin(L"CreateEventW");
        slots[i].buf = mem.data() + static_cast<size_t>(i) * stride;
        evs.push_back(slots[i].ev.get());
    }

    ULONGLONG next = 0; // смещение следующего непрочитанного блока
    int active = 0;     // число слотов, ещё не закончивших работу

    auto prepareOv = [](Slot &s, ULONGLONG off)
    {
        ResetEvent(s.ev.get());
        ZeroMemory(&s.ov, sizeof(OVERLAPPED));
        s.ov.hEvent = s.ev.get();
        s.ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFull);
        s.ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    };

    auto startRead = [&](Slot &s)
    {
        s.offset = next;
        next += block;
        s.expected = static_cast<DWORD>(std::min<ULONGLONG>(block, fileSize - s.offset));
        DWORD req = nobuf ? static_cast<DWORD>(RoundUp(s.expected, kAlign)) : s.expected;
        prepareOv(s, s.offset);
        if (!ReadFile(hs.get(), s.buf, req, nullptr, &s.ov))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                ThrowWin(L"ReadFile (async read)", src);
        }
        s.state = St::Reading;
    };

    auto startWrite = [&](Slot &s, DWORD bytes)
    {
        s.wlen = bytes;
        if (nobuf)
        {
            s.wlen = static_cast<DWORD>(RoundUp(bytes, kAlign));
            if (s.wlen > bytes)
                std::memset(s.buf + bytes, 0, s.wlen - bytes);
        }
        prepareOv(s, s.offset);
        if (!WriteFile(hd.get(), s.buf, s.wlen, nullptr, &s.ov))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                ThrowWin(L"WriteFile (async write)", dst);
        }
        s.state = St::Writing;
    };

    // При ошибке нельзя уничтожать буферы/OVERLAPPED, пока ядро ещё работает с ними.
    auto drain = [&]() noexcept
    {
        for (auto &s : slots)
        {
            if (s.state == St::Reading || s.state == St::Writing)
            {
                HANDLE h = (s.state == St::Reading) ? hs.get() : hd.get();
                CancelIoEx(h, &s.ov);
                DWORD d = 0;
                GetOverlappedResult(h, &s.ov, &d, TRUE);
                s.state = St::Idle;
            }
        }
    };

    try
    {
        for (auto &s : slots)
        {
            if (next < fileSize)
            {
                startRead(s);
                ++active;
            }
        }

        while (active > 0)
        {
            DWORD w = WaitForMultipleObjects(static_cast<DWORD>(evs.size()), evs.data(), FALSE, INFINITE);
            if (w == WAIT_FAILED)
                ThrowWin(L"WaitForMultipleObjects");
            if (w >= WAIT_OBJECT_0 + evs.size())
                throw AppError{L"WaitForMultipleObjects: unexpected return value"};

            Slot &s = slots[w - WAIT_OBJECT_0];
            const bool wasRead = (s.state == St::Reading);
            HANDLE h = wasRead ? hs.get() : hd.get();

            DWORD done = 0;
            if (!GetOverlappedResult(h, &s.ov, &done, FALSE))
            {
                DWORD e = GetLastError();
                s.state = St::Idle;
                ThrowWinCode(wasRead ? L"GetOverlappedResult (async read)" : L"GetOverlappedResult (async write)",
                             wasRead ? src : dst, e);
            }
            s.state = St::Idle;

            if (wasRead)
            {
                if (done != s.expected)
                    throw AppError{L"Async read: read unexpected number of bytes"};
                startWrite(s, done);
            }
            else
            {
                if (done != s.wlen)
                    throw AppError{L"Async write: written unexpected number of bytes"};
                if (next < fileSize)
                {
                    startRead(s);
                }
                else
                {
                    ResetEvent(s.ev.get());
                    s.state = St::Done;
                    --active;
                }
            }
        }
    }
    catch (...)
    {
        drain();
        throw;
    }

    hs.close();
    hd.close();
    if (nobuf)
        TruncateFile(dst, fileSize);
    // события и буферы освобождаются деструкторами (slots, mem)
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
    std::wstring csv; // файл для экспорта результатов
    bool nobuf = false;
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
  --csv <файл>      сохранить результаты всех прогонов в CSV
  --keep            не удалять сгенерированный исходный файл
  -h, --help        эта справка

Без параметров выполняется полный эксперимент: 10/100/500 МБ, блок 64 КБ,
синхронный режим и асинхронный с 1/2/4/8 операциями, по 3 прогона.
Исходные файлы пользователя только читаются; копии создаются под служебными
именами asynccopy_<pid>_*.bin и удаляются после проверки.
)";
}

static bool ParseList(const std::wstring &s, std::vector<ULONGLONG> &out)
{
    out.clear();
    size_t pos = 0;
    while (pos <= s.size())
    {
        size_t c = s.find(L',', pos);
        if (c == std::wstring::npos)
            c = s.size();
        std::wstring tok = s.substr(pos, c - pos);
        if (tok.empty() || tok.size() > 12 || tok.find_first_not_of(L"0123456789") != std::wstring::npos)
            return false;
        ULONGLONG v = _wcstoui64(tok.c_str(), nullptr, 10);
        if (v == 0)
            return false;
        out.push_back(v);
        pos = c + 1;
    }
    return !out.empty();
}

// true — продолжать работу; false — выйти (code содержит код возврата)
static bool ParseArgs(int argc, wchar_t **argv, Options &o, int &code)
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
        std::wstring a = argv[i];
        auto value = [&](std::wstring &v)
        {
            if (i + 1 >= argc)
                return false;
            v = argv[++i];
            return true;
        };
        std::wstring v;
        if (a == L"-h" || a == L"--help" || a == L"/?" || a == L"-?")
        {
            PrintUsage();
            return false;
        }
        else if (a == L"--nobuf")
        {
            o.nobuf = true;
        }
        else if (a == L"--keep")
        {
            o.keep = true;
        }
        else if (a == L"--src")
        {
            if (!value(o.src) || o.src.empty())
                return fail(L"--src requires a file path");
        }
        else if (a == L"--dir")
        {
            if (!value(o.dir) || o.dir.empty())
                return fail(L"--dir requires a directory path");
        }
        else if (a == L"--csv")
        {
            if (!value(o.csv) || o.csv.empty())
                return fail(L"--csv requires a file path");
        }
        else if (a == L"--sizes")
        {
            if (!value(v) || !ParseList(v, o.sizesMB))
                return fail(L"--sizes: required list of positive numbers (MB)");
        }
        else if (a == L"--blocks")
        {
            if (!value(v) || !ParseList(v, o.blocksKB))
                return fail(L"--blocks: expected list of positive numbers (KB)");
        }
        else if (a == L"--ops")
        {
            if (!value(v) || !ParseList(v, o.ops))
                return fail(L"--ops: expected list of positive numbers");
        }
        else if (a == L"--runs")
        {
            std::vector<ULONGLONG> t;
            if (!value(v) || !ParseList(v, t) || t.size() != 1)
                return fail(L"--runs: expected one positive number");
            o.runs = static_cast<int>(std::min<ULONGLONG>(t[0], 1000));
        }
        else
        {
            return fail(L"unknown option " + a);
        }
    }
    for (ULONGLONG n : o.ops)
        if (n > 64)
            return fail(L"operations cannot exceed 64 (limit of WaitForMultipleObjects)");
    for (ULONGLONG b : o.blocksKB)
    {
        if (b > 262144)
            return fail(L"block size cannot exceed 256 МБ (262144 КБ)");
        if (o.nobuf && b % 4 != 0)
            return fail(L"with --nobuf block size must be a multiple of 4 KB");
    }
    for (ULONGLONG s : o.sizesMB)
        if (s > 4ull * 1024 * 1024)
            return fail(L"file size in --sizes cannot exceed 4 GB (4096 МБ)");
    return true;
}

// ----------------------------------------------------------------------------
//  Статистика и вывод
// ----------------------------------------------------------------------------

static double Median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}
static double Mean(const std::vector<double> &v)
{
    double s = 0;
    for (double x : v)
        s += x;
    return s / static_cast<double>(v.size());
}

struct Config
{
    std::wstring label;
    std::string csvName;
    bool async;
    int ops;
};

struct Row
{
    Config cfg;
    std::vector<double> t; // время каждого прогона, мс
    bool sizeOk = true;
    bool crcOk = true;
};

static std::wstring JoinPath(const std::wstring &dir, const std::wstring &name)
{
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
        return dir + L"\\" + name;
    return dir + name;
}

static void CheckEnoughSpace(const std::wstring &dir, ULONGLONG need)
{
    ULARGE_INTEGER avail{}, total{}, freeB{};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &avail, &total, &freeB))
        ThrowWin(L"GetDiskFreeSpaceExW", dir);
    if (avail.QuadPart < need)
    {
        std::wostringstream ss;
        ss << L"Lack of disk space: need ~" << (need >> 20) << L" MB, available " << (avail.QuadPart >> 20) << L" MB";
        throw AppError{ss.str()};
    }
}

static void BenchmarkFile(const Options &o, const std::wstring &srcPath, const std::wstring &dstPath,
                          std::string &csv)
{
    const ULONGLONG size = GetFileSizeByPath(srcPath);

    std::wcout << L"\n================================================================================\n"
               << L" Source: " << srcPath << L"\n Size  : " << size << L" bytes (" << std::fixed << std::setprecision(2)
               << static_cast<double>(size) / 1048576.0 << L" MB)\n"
               << L" Calculating CRC32 of the source... " << std::flush;
    ULONGLONG crcBytes = 0;
    const uint32_t crcSrc = Crc32File(srcPath, crcBytes);
    if (crcBytes != size)
        throw AppError{L"Size read while calculating CRC32 does not match file size"};
    std::wcout << Hex32(crcSrc) << L"\n";

    for (ULONGLONG bkb : o.blocksKB)
    {
        const DWORD block = static_cast<DWORD>(bkb * 1024);

        std::vector<Row> rows;
        rows.push_back(Row{Config{L"Sync", "sync", false, 0}, {}, true, true});
        for (ULONGLONG n : o.ops)
        {
            std::wstring lbl = L"Async x" + std::to_wstring(n);
            rows.push_back(Row{Config{lbl, "async", true, static_cast<int>(n)}, {}, true, true});
        }

        // Прогоны чередуются по режимам, чтобы влияние кэша ОС распределялось равномерно.
        for (int run = 1; run <= o.runs; ++run)
        {
            for (auto &r : rows)
            {
                std::wcout << L"\r  block " << bkb << L" KB, run " << run << L"/" << o.runs << L": " << std::left
                           << std::setw(14) << r.cfg.label << L" ...                    " << std::flush;

                DeleteIfExists(dstPath);
                FileCleaner cleaner{dstPath};

                double t0 = NowMs();
                if (r.cfg.async)
                    CopyAsync(srcPath, dstPath, size, block, r.cfg.ops, o.nobuf);
                else
                    CopySync(srcPath, dstPath, block, o.nobuf);
                double ms = NowMs() - t0;

                // Проверки (вне замера времени)
                const ULONGLONG dsize = GetFileSizeByPath(dstPath);
                ULONGLONG cb = 0;
                const uint32_t crcDst = Crc32File(dstPath, cb);
                const bool sOk = (dsize == size);
                const bool cOk = (crcDst == crcSrc) && (cb == size);
                r.sizeOk = r.sizeOk && sOk;
                r.crcOk = r.crcOk && cOk;
                r.t.push_back(ms);

                csv += std::to_string(size) + "," + std::to_string(bkb) + "," + r.cfg.csvName + "," +
                       std::to_string(r.cfg.ops) + "," + std::to_string(run) + "," + std::to_string(ms) + "," +
                       (sOk ? "1" : "0") + "," + (cOk ? "1" : "0") + "\n";
            }
        }
        std::wcout << L"\r" << std::wstring(78, L' ') << L"\r";

        // ---- Таблица ----
        const double syncMed = Median(rows[0].t);
        const double mb = static_cast<double>(size) / 1048576.0;
        std::wcout << L"\n Block " << bkb << L" KB, runs: " << o.runs << L", mode: "
                   << (o.nobuf ? L"without OS cache (NO_BUFFERING)" : L"with OS cache") << L"\n";
        std::wcout << std::left << std::setw(15) << L" Mode" << std::right << std::setw(12) << L"Median,ms" << std::setw(12)
                   << L"Mean,ms" << std::setw(10) << L"Min,ms" << std::setw(10) << L"Max,ms" << std::setw(9) << L"MB/s"
                   << std::setw(11) << L"Speedup" << std::setw(8) << L"Size" << std::setw(8) << L"CRC32" << L"\n"
                   << L" " << std::wstring(94, L'-') << L"\n";
        size_t best = 0;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const Row &r = rows[i];
            double med = Median(r.t);
            if (med < Median(rows[best].t))
                best = i;
            double mn = *std::min_element(r.t.begin(), r.t.end());
            double mx = *std::max_element(r.t.begin(), r.t.end());
            double mbps = med > 0 ? mb / (med / 1000.0) : 0;
            std::wcout << L" " << std::left << std::setw(14) << r.cfg.label << std::right << std::fixed << std::setprecision(1)
                       << std::setw(12) << med << std::setw(12) << Mean(r.t) << std::setw(10) << mn << std::setw(10) << mx
                       << std::setw(9) << mbps << std::setprecision(2) << std::setw(10) << (med > 0 ? syncMed / med : 0.0)
                       << L"x" << std::setw(8) << (r.sizeOk ? L"OK" : L"ERROR") << std::setw(8)
                       << (r.crcOk ? L"OK" : L"ERROR") << L"\n";
        }
        std::wcout << L" Fastest of all (by median): " << rows[best].cfg.label << L"\n";
        for (const auto &r : rows)
            if (!r.sizeOk || !r.crcOk)
                std::wcout << L" WARNING: mode «" << r.cfg.label << L"» produced an incorrect copy!\n";
    }
}

static void WriteTextFile(const std::wstring &path, const std::string &text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        ThrowWin(L"CreateFileW (CSV)", path);
    UniqueHandle uh(h);
    size_t off = 0;
    while (off < text.size())
    {
        DWORD n = static_cast<DWORD>(std::min<size_t>(text.size() - off, 1u << 20)), w = 0;
        if (!WriteFile(uh.get(), text.data() + off, n, &w, nullptr))
            ThrowWin(L"WriteFile (CSV)", path);
        off += w;
    }
}

// ----------------------------------------------------------------------------
//  Основная логика
// ----------------------------------------------------------------------------

static int Run(const Options &o)
{
    // Рабочий каталог
    std::wstring dir = o.dir;
    if (dir.empty())
    {
        wchar_t tmp[MAX_PATH + 2];
        DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
        if (n == 0 || n > MAX_PATH)
            ThrowWin(L"GetTempPathW");
        dir.assign(tmp, n);
    }
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        ThrowWin(L"GetFileAttributesW (working directory)", dir);
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
        throw AppError{L"--dir: specified path is not a directory: " + dir};

    const std::wstring pid = std::to_wstring(GetCurrentProcessId());
    const std::wstring dstPath = JoinPath(dir, L"asynccopy_" + pid + L"_dst.bin");

    std::wcout << L"Async copy (WinAPI, FILE_FLAG_OVERLAPPED)\n"
               << L"Working directory: " << dir << L"\n"
               << L"Blocks, KB: ";
    for (auto b : o.blocksKB)
        std::wcout << b << L" ";
    std::wcout << L"| async operations: ";
    for (auto n : o.ops)
        std::wcout << n << L" ";
    std::wcout << L"| runs: " << o.runs << L"\n";

    std::string csv = "size_bytes,block_kb,mode,ops,run,time_ms,size_ok,crc_ok\n";

    if (!o.src.empty())
    {
        // ---- Пользовательский входной файл (только чтение) ----
        DWORD a = GetFileAttributesW(o.src.c_str());
        if (a == INVALID_FILE_ATTRIBUTES)
            ThrowWin(L"GetFileAttributesW (source file)", o.src);
        if (a & FILE_ATTRIBUTE_DIRECTORY)
            throw AppError{L"--src: source is a directory, not a file: " + o.src};
        if (_wcsicmp(o.src.c_str(), dstPath.c_str()) == 0)
            throw AppError{L"Input and output files are the same"};
        CheckEnoughSpace(dir, GetFileSizeByPath(o.src) + (64ull << 20));
        BenchmarkFile(o, o.src, dstPath, csv);
    }
    else
    {
        // ---- Генерируемые тестовые файлы ----
        for (ULONGLONG mb : o.sizesMB)
        {
            const ULONGLONG bytes = mb << 20;
            const std::wstring srcPath = JoinPath(dir, L"asynccopy_" + pid + L"_src_" + std::to_wstring(mb) + L"MB.bin");
            try
            {
                CheckEnoughSpace(dir, bytes * 2 + (64ull << 20));
            }
            catch (const AppError &e)
            {
                std::wcout << L"\nFile " << mb << L" MB skipped.\n"
                           << e.message << L"\n";
                continue;
            }
            std::wcout << L"\nGenerating test file " << mb << L" MB... " << std::flush;
            FileCleaner cleaner{o.keep ? std::wstring() : srcPath};
            GenerateFile(srcPath, bytes);
            std::wcout << L"done\n";
            BenchmarkFile(o, srcPath, dstPath, csv);
            if (o.keep)
                std::wcout << L" Source file saved: " << srcPath << L"\n";
        }
    }

    if (!o.csv.empty())
    {
        WriteTextFile(o.csv, csv);
        std::wcout << L"\nResults saved to " << o.csv << L"\n";
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

    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0)
    {
        std::wcerr << L"QueryPerformanceFrequency недоступен\n";
        return 1;
    }
    g_qpcFreq = static_cast<double>(f.QuadPart);
    InitCrcTables();

    Options o;
    int code = 0;
    if (!ParseArgs(argc, argv, o, code))
        return code;

    try
    {
        return Run(o);
    }
    catch (const AppError &e)
    {
        std::wcerr << L"\n"
                   << e.message << L"\n";
        return 1;
    }
    catch (const std::exception &e)
    {
        std::wcerr << L"\nException: " << e.what() << L"\n";
        return 1;
    }
}