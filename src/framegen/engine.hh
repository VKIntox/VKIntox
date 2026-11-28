#pragma once

// Frame generation through the MAKO backend (vendored under vendor/mako). The
// open-weight MAKO FG model runs in mako-fg-host, a separate program shipped
// beside the layer (src/framegen/mako-fg); frames cross between the game's
// device and the host's as exported Vulkan images.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace VKIntox
{
    struct LogicalDevice;

    namespace FrameGen
    {
        // Diagnostics for the FG tab: which stage failed, if any.
        std::string engineStatus();

        // True once the backend is ready to open contexts.
        bool engineAvailable();

        // Model package directory shipped beside the layer, empty if missing.
        std::string modelPackagePath();

        // File descriptors the caller exports and hands to openContext.
        struct FileDescriptors
        {
            int sourceA = -1;
            int sourceB = -1;
            int sync = -1;
            std::vector<int> destinations;
        };

        // Owns exported descriptors until openContext takes them. The backend
        // consumes the complete set on entry, including construction failures.
        class FileDescriptorScope
        {
        public:
            ~FileDescriptorScope();
            FileDescriptorScope() = default;
            FileDescriptorScope(const FileDescriptorScope&) = delete;
            FileDescriptorScope& operator=(const FileDescriptorScope&) = delete;

            void setSourceA(int fd);
            void setSourceB(int fd);
            void setSync(int fd);
            void addDestination(int fd);
            FileDescriptors release() noexcept;

        private:
            FileDescriptors m_descriptors;
        };

        // Lazily created per device; the backend matches the game's GPU and
        // spawns the host program for it.
        class EngineInstance
        {
        public:
            explicit EngineInstance(LogicalDevice* logicalDevice,
                                    bool halfPrecision = false,
                                    const std::string& dllPath = {});
            ~EngineInstance();
            EngineInstance(const EngineInstance&) = delete;
            EngineInstance& operator=(const EngineInstance&) = delete;

            void* handle() const { return m_object.get(); }

        private:
            struct ObjectDeleter { void operator()(void* p) const; };
            std::unique_ptr<void, ObjectDeleter> m_object;
        };

        // One context per swapchain extent; owns nothing, the caller keeps the
        // images alive for its lifetime.
        class EngineContext
        {
        public:
            EngineContext(EngineInstance& instance,
                          FileDescriptorScope& descriptors,
                          uint32_t width, uint32_t height,
                          bool highPrecision,
                          uint32_t flowScale, bool performanceMode);
            ~EngineContext();
            EngineContext(const EngineContext&) = delete;
            EngineContext& operator=(const EngineContext&) = delete;

            // Schedules the standard fixed schedule for the opened capacity.
            // False means the host is lost or rejected the schedule.
            bool scheduleFrames() noexcept;

            // Waits until the host finished the batch's GPU work. False means
            // the host stayed busy past the bounded wait. Never throws.
            bool waitForIdle(uint64_t timeoutNs = 0) noexcept;

            // Host-side loss checks do not perform IPC and never throw.
            bool contextLost() const noexcept;

            // The callback runs on the backend watcher when the host dies.
            bool setLossHandler(std::function<void()> handler) noexcept;

        private:
            struct ObjectDeleter { void operator()(void* p) const; };
            void* m_instance = nullptr;
            std::unique_ptr<void, ObjectDeleter> m_object;
        };
    } // namespace FrameGen
} // namespace VKIntox
