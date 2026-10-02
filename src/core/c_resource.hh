#ifndef C_RESOURCE_HPP_INCLUDED
#define C_RESOURCE_HPP_INCLUDED

#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <utility>

namespace VKIntox
{
// The layer still has to hand descriptors and streams to C APIs that have no
// C++ equivalent, and the compiler cannot see those resources at all. An
// exception thrown between open and close therefore leaks a descriptor silently.
// These owners make that impossible: the descriptor is closed on every path out
// of the scope, including the ones an exception takes.

class UniqueFd
{
public:
    UniqueFd() noexcept = default;

    // -1 is the C convention for "no descriptor", so adopting one stays
    // distinguishable from owning a valid one without a separate valid flag
    explicit UniqueFd(int fd) noexcept
        : handle(fd)
    {
    }

    UniqueFd(UniqueFd&& other) noexcept
        : handle(std::exchange(other.handle, -1))
    {
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            handle = std::exchange(other.handle, -1);
        }
        return *this;
    }

    // copying would hand the same number to two owners, and whichever closes
    // first leaves the other closing a descriptor the kernel may have reused
    UniqueFd(const UniqueFd&)            = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    ~UniqueFd()
    {
        reset();
    }

    int get() const noexcept
    {
        return handle;
    }

    bool valid() const noexcept
    {
        return handle >= 0;
    }

    explicit operator bool() const noexcept
    {
        return valid();
    }

    // release() hands ownership to the caller instead of closing. only for the
    // APIs that consume a descriptor and take over the closing themselves
    int release() noexcept
    {
        return std::exchange(handle, -1);
    }

    void reset(int fd = -1) noexcept
    {
        // qualified because this class also has a close() member that would
        // otherwise hide the syscall
        if (handle >= 0 && handle != fd)
            ::close(handle);
        handle = fd;
    }

    // close() reports the result instead of swallowing it. writers that promise
    // the data reached the disk need this, because with deferred writeback the
    // error surfaces at close, long after the last write() returned success
    bool close() noexcept
    {
        if (handle < 0)
            return true;
        const int result = ::close(handle);
        handle           = -1;
        return result == 0;
    }

private:
    int handle = -1;
};

class UniqueCFile
{
public:
    UniqueCFile() noexcept = default;

    explicit UniqueCFile(std::FILE* file) noexcept
        : handle(file)
    {
    }

    UniqueCFile(UniqueCFile&& other) noexcept
        : handle(std::exchange(other.handle, nullptr))
    {
    }

    UniqueCFile& operator=(UniqueCFile&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            handle = std::exchange(other.handle, nullptr);
        }
        return *this;
    }

    UniqueCFile(const UniqueCFile&)            = delete;
    UniqueCFile& operator=(const UniqueCFile&) = delete;

    ~UniqueCFile()
    {
        reset();
    }

    std::FILE* get() const noexcept
    {
        return handle;
    }

    bool valid() const noexcept
    {
        return handle != nullptr;
    }

    explicit operator bool() const noexcept
    {
        return valid();
    }

    // the decoders take a plain FILE* and read from the current offset, so
    // ownership has to be given up for the duration of the call
    std::FILE* release() noexcept
    {
        return std::exchange(handle, nullptr);
    }

    void reset(std::FILE* file = nullptr) noexcept
    {
        if (handle != nullptr && handle != file)
            std::fclose(handle);
        handle = file;
    }

private:
    std::FILE* handle = nullptr;
};

class UniqueDir
{
public:
    UniqueDir() noexcept = default;

    explicit UniqueDir(DIR* dir) noexcept
        : handle(dir)
    {
    }

    UniqueDir(UniqueDir&& other) noexcept
        : handle(std::exchange(other.handle, nullptr))
    {
    }

    UniqueDir& operator=(UniqueDir&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            handle = std::exchange(other.handle, nullptr);
        }
        return *this;
    }

    UniqueDir(const UniqueDir&)            = delete;
    UniqueDir& operator=(const UniqueDir&) = delete;

    ~UniqueDir()
    {
        reset();
    }

    DIR* get() const noexcept
    {
        return handle;
    }

    bool valid() const noexcept
    {
        return handle != nullptr;
    }

    explicit operator bool() const noexcept
    {
        return valid();
    }

    void reset(DIR* dir = nullptr) noexcept
    {
        if (handle != nullptr && handle != dir)
            closedir(handle);
        handle = dir;
    }

private:
    DIR* handle = nullptr;
};

} // namespace VKIntox

#endif // C_RESOURCE_HPP_INCLUDED
