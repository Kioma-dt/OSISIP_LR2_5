#include "copy_engine.hpp"

#include "app_error.hpp"
#include "file_io.hpp"

#include <cstring>
#include <vector>

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
        UniqueHandle eventHandle;
        BYTE *buffer = nullptr;
        ULONGLONG offset = 0;
        DWORD expected = 0;
        DWORD writeLength = 0;
        State state = State::Idle;
    };
}

void CopySync(const std::wstring &src, const std::wstring &dst, ULONGLONG fileSize, DWORD block, bool noBuffering)
{
    UniqueHandle source = OpenSource(src, false, noBuffering);
    UniqueHandle destination = OpenDest(dst, false, noBuffering);

    ULONGLONG allocatedSize = noBuffering ? RoundUp(fileSize, kAlign) : fileSize;
    if (allocatedSize)
    {
        LARGE_INTEGER offset;
        offset.QuadPart = static_cast<LONGLONG>(allocatedSize);
        if (!SetFilePointerEx(destination.get(), offset, nullptr, FILE_BEGIN))
            ThrowWin(L"SetFilePointerEx (file enlargement)", dst);
        if (!SetEndOfFile(destination.get()))
            ThrowWin(L"SetEndOfFile (file enlargement)", dst);

        offset.QuadPart = 0;
        if (!SetFilePointerEx(destination.get(), offset, nullptr, FILE_BEGIN))
            ThrowWin(L"SetFilePointerEx (positioning)", dst);
    }

    VBuffer buffer(static_cast<SIZE_T>(RoundUp(block, kAlign)));
    ULONGLONG total = 0;
    for (;;)
    {
        DWORD bytesRead = 0;
        if (!ReadFile(source.get(), buffer.data(), block, &bytesRead, nullptr))
            ThrowWin(L"ReadFile (sync read)", src);
        if (!bytesRead)
            break;
        DWORD writeLength = bytesRead;
        if (noBuffering)
        {
            writeLength = static_cast<DWORD>(RoundUp(bytesRead, kAlign));
            if (writeLength > bytesRead)
                std::memset(buffer.data() + bytesRead, 0, writeLength - bytesRead);
        }
        DWORD bytesWritten = 0;
        if (!WriteFile(destination.get(), buffer.data(), writeLength, &bytesWritten, nullptr))
            ThrowWin(L"WriteFile (sync write)", dst);
        if (bytesWritten != writeLength)
            throw AppError{L"WriteFile (sync write): written less bytes than requested"};
        total += bytesRead;
    }
    source.close();
    destination.close();
    if (noBuffering)
        TruncateFile(dst, total);
}

void CopyAsync(const std::wstring &src, const std::wstring &dst, ULONGLONG fileSize, DWORD block, int numOps, bool noBuffering)
{
    UniqueHandle source = OpenSource(src, true, noBuffering);
    UniqueHandle destination = OpenDest(dst, true, noBuffering);
    ULONGLONG allocatedSize = noBuffering ? RoundUp(fileSize, kAlign) : fileSize;
    if (allocatedSize)
    {
        LARGE_INTEGER offset;
        offset.QuadPart = static_cast<LONGLONG>(allocatedSize);
        if (!SetFilePointerEx(destination.get(), offset, nullptr, FILE_BEGIN))
            ThrowWin(L"SetFilePointerEx (file enlargement)", dst);
        if (!SetEndOfFile(destination.get()))
            ThrowWin(L"SetEndOfFile (file enlargement)", dst);
    }

    SIZE_T stride = static_cast<SIZE_T>(RoundUp(block, kAlign));
    VBuffer memory(stride * static_cast<SIZE_T>(numOps));
    std::vector<Slot> slots(static_cast<size_t>(numOps));
    std::vector<HANDLE> events;
    for (int i = 0; i < numOps; ++i)
    {
        slots[i].eventHandle.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!slots[i].eventHandle.valid())
            ThrowWin(L"CreateEventW");
        slots[i].buffer = memory.data() + static_cast<size_t>(i) * stride;
        events.push_back(slots[i].eventHandle.get());
    }

    ULONGLONG nextOffset = 0;
    int activeSlots = 0;
    auto prepare = [](Slot &slot, ULONGLONG offset)
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
        prepare(slot, slot.offset);
        if (!ReadFile(source.get(), slot.buffer, requestSize, nullptr, &slot.overlapped) && GetLastError() != ERROR_IO_PENDING)
            ThrowWin(L"ReadFile (async read)", src);
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
        prepare(slot, slot.offset);
        if (!WriteFile(destination.get(), slot.buffer, slot.writeLength, nullptr, &slot.overlapped) && GetLastError() != ERROR_IO_PENDING)
            ThrowWin(L"WriteFile (async write)", dst);
        slot.state = State::Writing;
    };
    auto drain = [&]() noexcept
    {
        for (Slot &slot : slots)
        {
            if (slot.state == State::Reading || slot.state == State::Writing)
            {
                HANDLE handle = slot.state == State::Reading ? source.get() : destination.get();
                CancelIoEx(handle, &slot.overlapped);
                DWORD transferred = 0;
                GetOverlappedResult(handle, &slot.overlapped, &transferred, TRUE);
                slot.state = State::Idle;
            }
        }
    };

    try
    {
        for (Slot &slot : slots)
            if (nextOffset < fileSize)
            {
                startRead(slot);
                ++activeSlots;
            }
        while (activeSlots > 0)
        {
            DWORD result = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE);
            if (result == WAIT_FAILED)
                ThrowWin(L"WaitForMultipleObjects");
            if (result >= WAIT_OBJECT_0 + events.size())
                throw AppError{L"WaitForMultipleObjects: unexpected return value"};
            Slot &slot = slots[result - WAIT_OBJECT_0];
            bool wasRead = slot.state == State::Reading;
            HANDLE handle = wasRead ? source.get() : destination.get();
            DWORD transferred = 0;
            if (!GetOverlappedResult(handle, &slot.overlapped, &transferred, FALSE))
            {
                DWORD code = GetLastError();
                slot.state = State::Idle;
                ThrowWinCode(wasRead ? L"GetOverlappedResult (async read)" : L"GetOverlappedResult (async write)", wasRead ? src : dst, code);
            }
            slot.state = State::Idle;
            if (wasRead)
            {
                if (transferred != slot.expected)
                    throw AppError{L"Async read: read unexpected number of bytes"};
                startWrite(slot, transferred);
            }
            else
            {
                if (transferred != slot.writeLength)
                    throw AppError{L"Async write: written unexpected number of bytes"};
                if (nextOffset < fileSize)
                    startRead(slot);
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
    source.close();
    destination.close();
    if (noBuffering)
        TruncateFile(dst, fileSize);
}