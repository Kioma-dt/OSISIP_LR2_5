#include "resources.hpp"

UniqueHandle &UniqueHandle::operator=(UniqueHandle &&other) noexcept
{
    if (this != &other)
    {
        close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

UniqueHandle::~UniqueHandle() { close(); }

void UniqueHandle::reset(HANDLE handle) noexcept
{
    close();
    handle_ = handle;
}

void UniqueHandle::close() noexcept
{
    if (valid())
        CloseHandle(handle_);
    handle_ = nullptr;
}

VBuffer::VBuffer(SIZE_T bytes)
{
    data_ = static_cast<BYTE *>(VirtualAlloc(nullptr, bytes ? bytes : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!data_)
        ThrowWin(L"VirtualAlloc");
}

VBuffer::~VBuffer()
{
    if (data_)
        VirtualFree(data_, 0, MEM_RELEASE);
}

FileCleaner::~FileCleaner()
{
    if (!path.empty())
        DeleteFileW(path.c_str());
}