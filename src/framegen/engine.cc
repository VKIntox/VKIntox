#include "framegen/engine.hh"

#include "render/device/logical_device.hh"
#include "util/logger.hh"

#include "mako-backend/mako.hpp"
#include "mako-backend/device_identity.hpp"
#include "context.hpp"

#include <dlfcn.h>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace VKIntox::FrameGen
{
    namespace
    {
        std::mutex apiMutex;

        // the backend throws backend::error with human-readable messages
        std::string describe(const std::exception& e)
        {
            return e.what();
        }

        // The device picker must match the game's GPU. Names compare equal to
        // what the backend formats; the layer also passes the PCI slot in
        // MAKO's decimal form when it knows one.
        bool pickGameDevice(LogicalDevice* logicalDevice,
                            const std::string& deviceName,
                            const std::pair<const std::string&, const std::string&>& ids,
                            const std::optional<std::string>& pci)
        {
            if (deviceName == logicalDevice->gpuName)
                return true;
            if (pci && logicalDevice->gpuPciSlot.size() == 12)
            {
                // layer form "0000:01:00.0" (hex, padded) -> MAKO form "1:0.0"
                unsigned domain = 0, bus = 0, device = 0, function = 0;
                if (std::sscanf(logicalDevice->gpuPciSlot.c_str(), "%4x:%2x:%2x.%1x",
                                &domain, &bus, &device, &function) == 4)
                {
                    return *pci == mako::backend::pciAddressText(bus, device, function);
                }
            }
            return false;
        }
    }

    bool engineAvailable()
    {
        // the backend links statically; it is available whenever it compiled
        return true;
    }

    std::string engineStatus()
    {
        return "MAKO FG backend linked";
    }

    std::string modelPackagePath()
    {
        // the host finds its model beside itself; the package check here is
        // for the settings tab only
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&engineStatus), &info) == 0 || !info.dli_fname)
            return {};
        std::string directory = info.dli_fname;
        const size_t slash = directory.rfind('/');
        if (slash == std::string::npos)
            return {};
        directory = directory.substr(0, slash);
        return directory + "/mako-fg";
    }

    EngineInstance::~EngineInstance() = default;

    void EngineInstance::ObjectDeleter::operator()(void* p) const
    {
        delete static_cast<mako::backend::Instance*>(p);
    }

    EngineInstance::EngineInstance(LogicalDevice* logicalDevice)
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        // the model package sits beside the layer (mako-fg/), shipped with the
        // install; without it the backend falls back to Lossless Scaling's DLL
        const std::string package = modelPackagePath();
        std::optional<std::filesystem::path> modelPackage;
        if (!package.empty() && std::filesystem::exists(package))
        {
            modelPackage = std::filesystem::path(package);
            Logger::info("MAKO FG model package: " + package);
        }
        // the host process inherits this snapshot; with it set the Vulkan
        // loader never loads VKIntox into the host, which would recurse. The
        // game's explicit layer-enable list would override that disable, so
        // it is removed from the snapshot too, and both restore after.
        setenv("DISABLE_VKINTOX", "1", 1);
        const char* savedLayersEnable = getenv("VK_LOADER_LAYERS_ENABLE");
        std::string layersEnable = savedLayersEnable ? savedLayersEnable : "";
        unsetenv("VK_LOADER_LAYERS_ENABLE");
        auto* instance = new mako::backend::Instance(
            [logicalDevice](const std::string& name,
                            const std::pair<const std::string&, const std::string&>& ids,
                            const std::optional<std::string>& pci) {
                return pickGameDevice(logicalDevice, name, ids, pci);
            },
            std::filesystem::path(),          // no Lossless Scaling model
            false,                            // full precision
            modelPackage,
            std::nullopt);                    // host matches the game's driver on its own
        unsetenv("DISABLE_VKINTOX");
        if (!layersEnable.empty())
            setenv("VK_LOADER_LAYERS_ENABLE", layersEnable.c_str(), 1);
        else
            unsetenv("VK_LOADER_LAYERS_ENABLE");
        m_object.reset(instance);
        Logger::info("MAKO FG backend ready");
    }

    EngineContext::~EngineContext() = default;

    void EngineContext::ObjectDeleter::operator()(void* p) const
    {
        // contexts are closed through the owning instance; this only runs
        // after m_instance is destroyed, so leak — the instance's dtor closes
        // every open context
        (void)p;
    }

    EngineContext::EngineContext(EngineInstance& instance,
                                 int sourceFdA, int sourceFdB,
                                 const std::vector<int>& destinationFds,
                                 int syncFd,
                                 uint32_t width, uint32_t height,
                                 bool highPrecision,
                                 uint32_t flowScale, bool performanceMode)
        : m_instance(instance.handle())
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        auto* backend = static_cast<mako::backend::Instance*>(m_instance);
        mako::backend::Context& context = backend->openContext(
            {sourceFdA, sourceFdB},
            destinationFds,
            syncFd,
            width, height,
            highPrecision ? mako::backend::FrameEncoding::SdrHighPrecision
                          : mako::backend::FrameEncoding::Sdr8,
            static_cast<float>(flowScale),
            performanceMode,
            false,   // reducedHdrPrecision: SDR path ignores it
            0);      // motionPeriod 0: model default
        m_object.reset(&context);
    }

    void EngineContext::scheduleFrames()
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        static_cast<mako::backend::Instance*>(m_instance)->scheduleFrames(
            *static_cast<mako::backend::Context*>(m_object.get()));
    }

    bool EngineContext::waitForIdle()
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        auto* context = static_cast<mako::backend::Context*>(m_object.get());
        // two bounded waits; the backend itself caps each at 2s
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (context->waitForIdle(2'000'000'000ull))
                return true;
        }
        Logger::warn("FG: host stayed busy past the idle wait");
        return false;
    }
} // namespace VKIntox::FrameGen
