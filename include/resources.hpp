#pragma once

#include "app_error.hpp"

class UniqueHandle
{
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle_(h) {}
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    UniqueHandle &operator=(UniqueHandle &&other) noexcept;
    ~UniqueHandle();

    void reset(HANDLE handle) noexcept;
    void close() noexcept;
    bool valid() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

class VBuffer
{
public:
    explicit VBuffer(SIZE_T bytes);
    VBuffer(const VBuffer &) = delete;
    VBuffer &operator=(const VBuffer &) = delete;
    ~VBuffer();
    BYTE *data() const noexcept { return data_; }

private:
    BYTE *data_ = nullptr;
};

struct FileCleaner
{
    std::wstring path;
    ~FileCleaner();
};