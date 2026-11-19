#pragma once

// Runtime loader for the lsfg-vk frame generation engine. The engine lives in
// its own shared library (liblsfg-vk-pipeline.so.2, built unmodified from the
// vendored submodule), so the layer keeps working when it is absent: frame
// generation simply reports unavailable.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace VKIntox::FrameGen
{
    // Diagnostics for the LSFG tab: which stage failed, if any.
    std::string engineStatus();

    // True once the engine library has been loaded and its symbols resolved.
    bool engineAvailable();

    struct FileDescriptors
    {
        int sourceFd = -1;
        int destinationFd = -1;
        int syncFd = -1;
    };

    class EngineInstance
    {
    public:
        // deviceId may be a device name, "vid:did" hex, or a PCI bus id
        EngineInstance(const std::string& deviceId,
                       const std::filesystem::path& dllPath,
                       bool allowFp16,
                       std::function<void(const std::string&)> log);
        ~EngineInstance();
        EngineInstance(const EngineInstance&) = delete;
        EngineInstance& operator=(const EngineInstance&) = delete;
        void* handle() const { return m_object.get(); }

    private:
        struct ObjectDeleter { void operator()(void* p) const; };
        std::unique_ptr<void, ObjectDeleter> m_object;
    };

    class EngineContext
    {
    public:
        EngineContext(const EngineInstance& instance,
                      uint32_t width, uint32_t height,
                      float flowScale, bool performanceMode);
        ~EngineContext();
        EngineContext(const EngineContext&) = delete;
        EngineContext& operator=(const EngineContext&) = delete;

        FileDescriptors exportFds();
        void dispatch(uint32_t total, bool unsignaled = false);
        void acquire(bool unsignaled = false);
        void idle();

    private:
        struct ObjectDeleter { void operator()(void* p) const; };
        const EngineInstance* m_instance;
        std::unique_ptr<void, ObjectDeleter> m_object;
    };
} // namespace VKIntox::FrameGen
