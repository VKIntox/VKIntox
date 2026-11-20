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
            int sourceFd = -1;
            int destinationFd = -1;
            int syncFd = -1;
        };

        // Lazily created per device; the backend matches the game's GPU and
        // spawns the host program for it.
        class EngineInstance
        {
        public:
            explicit EngineInstance(LogicalDevice* logicalDevice);
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
                          int sourceFdA, int sourceFdB,
                          const std::vector<int>& destinationFds,
                          int syncFd,
                          uint32_t width, uint32_t height,
                          bool highPrecision,
                          uint32_t flowScale, bool performanceMode);
            ~EngineContext();
            EngineContext(const EngineContext&) = delete;
            EngineContext& operator=(const EngineContext&) = delete;

            // Schedules the standard fixed schedule for the opened capacity.
            void scheduleFrames();

            // Blocks until the host finished the batch's GPU work. The host
            // rejects a new schedule while its last batch is in flight.
            // False means the host stayed busy past the bounded wait.
            bool waitForIdle();

        private:
            struct ObjectDeleter { void operator()(void* p) const; };
            void* m_instance = nullptr;
            std::unique_ptr<void, ObjectDeleter> m_object;
        };
    } // namespace FrameGen
} // namespace VKIntox
