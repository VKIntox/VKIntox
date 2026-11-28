#include "framegen/engine.hh"

#include "util/logger.hh"

#ifdef VKINTOX_HAVE_FG_ENGINE
#include "render/device/logical_device.hh"
#include "mako-backend/mako.hpp"
#include "mako-backend/device_identity.hpp"
#include "context.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace VKIntox::FrameGen
{
    namespace
    {
#ifdef VKINTOX_HAVE_FG_ENGINE
        std::mutex apiMutex;
#endif

        class ScopedEnvironment
        {
        public:
            ScopedEnvironment(std::string name, const std::optional<std::string>& value)
                : m_name(std::move(name))
            {
                if (const char* previous = std::getenv(m_name.c_str()))
                    m_previous = previous;
                set(value);
            }

            ~ScopedEnvironment()
            {
                if (m_previous)
                    setenv(m_name.c_str(), m_previous->c_str(), 1);
                else
                    unsetenv(m_name.c_str());
            }

            ScopedEnvironment(const ScopedEnvironment&) = delete;
            ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

        private:
            void set(const std::optional<std::string>& value)
            {
                if (value)
                    setenv(m_name.c_str(), value->c_str(), 1);
                else
                    unsetenv(m_name.c_str());
            }

            std::string m_name;
            std::optional<std::string> m_previous;
        };

        std::string trim(std::string value)
        {
            const auto first = value.find_first_not_of(" \t");
            if (first == std::string::npos)
                return {};
            const auto last = value.find_last_not_of(" \t");
            return value.substr(first, last - first + 1);
        }

        std::optional<std::string> withoutVkintoxLayers(const char* raw)
        {
            if (!raw)
                return std::nullopt;
            const std::string value(raw);

            std::string filtered;
            size_t begin = 0;
            while (begin <= value.size())
            {
                const size_t end = value.find(',', begin);
                std::string layer = trim(value.substr(
                    begin, end == std::string::npos ? std::string::npos : end - begin));
                std::string lower = layer;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (!layer.empty() && lower.find("vkintox") == std::string::npos)
                {
                    if (!filtered.empty())
                        filtered += ',';
                    filtered += layer;
                }
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
            if (filtered.empty())
                return std::nullopt;
            return filtered;
        }

        std::vector<std::filesystem::path> packageRoots()
        {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(&engineStatus), &info) == 0 || !info.dli_fname)
                return {};
            const std::filesystem::path library(info.dli_fname);
            const auto directory = library.parent_path();
            return {
                directory / "mako-fg" / "model",
                directory.parent_path() / "lib" / "mako-fg" / "model",
                directory.parent_path().parent_path() / "lib64" / "mako-fg" / "model",
            };
        }

        bool validModelPackage(const std::filesystem::path& path)
        {
            for (const char* file : {"weights.bin", "ops.spv", "transport.spv", "mixture.spv"})
            {
                std::error_code error;
                if (!std::filesystem::is_regular_file(path / file, error))
                    return false;
            }
            return true;
        }

        std::optional<std::filesystem::path> hostProgram()
        {
            if (const char* overridePath = std::getenv("MAKO_FG_HOST"); overridePath && *overridePath)
            {
                if (access(overridePath, X_OK) == 0)
                    return std::filesystem::path(overridePath);
                return std::nullopt;
            }

            for (const auto& package : packageRoots())
            {
                const auto candidate = package.parent_path() / "mako-fg-host";
                if (access(candidate.c_str(), X_OK) == 0)
                    return candidate;
            }

            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(&engineStatus), &info) == 0 || !info.dli_fname)
                return std::nullopt;
            const std::filesystem::path library(info.dli_fname);
            const auto directory = library.parent_path();
            for (const auto& candidate : {
                     directory / "mako-fg" / "mako-fg-host",
                     directory.parent_path() / "lib" / "mako-fg" / "mako-fg-host",
                     directory.parent_path().parent_path() / "lib64" / "mako-fg" / "mako-fg-host"})
            {
                if (access(candidate.c_str(), X_OK) == 0)
                    return candidate;
            }
            return std::nullopt;
        }

#ifdef VKINTOX_HAVE_FG_ENGINE
        bool pickGameDevice(LogicalDevice* logicalDevice,
                            const std::string& deviceName,
                            const std::pair<const std::string&, const std::string&>& ids,
                            const std::optional<std::string>& pci)
        {
            (void)ids;
            if (pci && logicalDevice->gpuPciSlot.size() == 12)
            {
                unsigned domain = 0, bus = 0, device = 0, function = 0;
                if (std::sscanf(logicalDevice->gpuPciSlot.c_str(), "%4x:%2x:%2x.%1x",
                                &domain, &bus, &device, &function) == 4
                    && *pci == mako::backend::pciAddressText(bus, device, function))
                {
                    return true;
                }
            }
            return deviceName == logicalDevice->gpuName;
        }
#endif
    }

    FileDescriptorScope::~FileDescriptorScope()
    {
        auto close = [](int& fd) {
            if (fd >= 0)
            {
                ::close(fd);
                fd = -1;
            }
        };
        close(m_descriptors.sourceA);
        close(m_descriptors.sourceB);
        close(m_descriptors.sync);
        for (int fd : m_descriptors.destinations)
            ::close(fd);
    }

    void FileDescriptorScope::setSourceA(int fd)
    {
        if (m_descriptors.sourceA >= 0)
            ::close(m_descriptors.sourceA);
        m_descriptors.sourceA = fd;
    }

    void FileDescriptorScope::setSourceB(int fd)
    {
        if (m_descriptors.sourceB >= 0)
            ::close(m_descriptors.sourceB);
        m_descriptors.sourceB = fd;
    }

    void FileDescriptorScope::setSync(int fd)
    {
        if (m_descriptors.sync >= 0)
            ::close(m_descriptors.sync);
        m_descriptors.sync = fd;
    }

    void FileDescriptorScope::addDestination(int fd)
    {
        try
        {
            m_descriptors.destinations.push_back(fd);
        }
        catch (...)
        {
            ::close(fd);
            throw;
        }
    }

    FileDescriptors FileDescriptorScope::release() noexcept
    {
        FileDescriptors result = std::move(m_descriptors);
        m_descriptors.sourceA = -1;
        m_descriptors.sourceB = -1;
        m_descriptors.sync = -1;
        m_descriptors.destinations.clear();
        return result;
    }

    bool engineAvailable()
    {
#ifdef VKINTOX_HAVE_FG_ENGINE
        try
        {
            if (const char* package = std::getenv("MAKO_FG_PACKAGE"); package && *package && modelPackagePath().empty())
                return false;
            return hostProgram().has_value();
        }
        catch (...)
        {
            return false;
        }
#else
        return false;
#endif
    }

    std::string engineStatus()
    {
#ifdef VKINTOX_HAVE_FG_ENGINE
        if (!hostProgram())
            return "MAKO FG host is missing";
        if (const char* package = std::getenv("MAKO_FG_PACKAGE"); package && *package && modelPackagePath().empty())
            return "MAKO_FG_PACKAGE does not contain a valid model";
        if (modelPackagePath().empty())
            return "MAKO FG host ready; using its embedded model";
        return "MAKO FG host and model package ready";
#else
        return "FG engine disabled at build time";
#endif
    }

    std::string modelPackagePath()
    {
        if (const char* overridePath = std::getenv("MAKO_FG_PACKAGE"); overridePath && *overridePath)
        {
            const std::filesystem::path path(overridePath);
            if (validModelPackage(path))
                return path.string();
            return {};
        }
        for (const auto& path : packageRoots())
        {
            if (validModelPackage(path))
                return path.string();
        }
        return {};
    }

#ifdef VKINTOX_HAVE_FG_ENGINE
    EngineInstance::~EngineInstance() = default;

    void EngineInstance::ObjectDeleter::operator()(void* p) const
    {
        delete static_cast<mako::backend::Instance*>(p);
    }

    EngineInstance::EngineInstance(LogicalDevice* logicalDevice,
                                   bool halfPrecision,
                                   const std::string& dllPath)
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        const std::string package = modelPackagePath();
        std::optional<std::filesystem::path> modelPackage;
        std::unique_ptr<ScopedEnvironment> packageEnvironment;
        if (!package.empty())
        {
            modelPackage = std::filesystem::path(package);
            packageEnvironment = std::make_unique<ScopedEnvironment>(
                "MAKO_FG_PACKAGE", std::optional<std::string>(package));
            Logger::info("MAKO FG model package: " + package);
        }

        ScopedEnvironment disableVkintox("DISABLE_VKINTOX", std::optional<std::string>("1"));
        ScopedEnvironment enableVkintox("ENABLE_VKINTOX", std::nullopt);
        ScopedEnvironment instanceLayers("VK_INSTANCE_LAYERS",
                                         withoutVkintoxLayers(std::getenv("VK_INSTANCE_LAYERS")));
        ScopedEnvironment loaderLayers("VK_LOADER_LAYERS_ENABLE",
                                       withoutVkintoxLayers(std::getenv("VK_LOADER_LAYERS_ENABLE")));

        const bool useDll = !modelPackage && !dllPath.empty();
        auto* instance = new mako::backend::Instance(
            [logicalDevice](const std::string& name,
                            const std::pair<const std::string&, const std::string&>& ids,
                            const std::optional<std::string>& pci) {
                return pickGameDevice(logicalDevice, name, ids, pci);
            },
            useDll ? std::filesystem::path(dllPath) : std::filesystem::path(),
            halfPrecision,
            modelPackage,
            std::nullopt);
        m_object.reset(instance);
        Logger::info("MAKO FG backend ready");
    }

    EngineContext::~EngineContext()
    {
        if (!m_object || !m_instance)
            return;
        try
        {
            std::lock_guard<std::mutex> lock(apiMutex);
            auto* backend = static_cast<mako::backend::Instance*>(m_instance);
            auto* context = static_cast<mako::backend::Context*>(m_object.get());
            backend->setContextLossHandler(*context, {});
            backend->closeContext(*context);
        }
        catch (...)
        {
        }
        m_object.release();
    }

    void EngineContext::ObjectDeleter::operator()(void* p) const
    {
        (void)p;
    }

    EngineContext::EngineContext(EngineInstance& instance,
                                 FileDescriptorScope& descriptors,
                                 uint32_t width, uint32_t height,
                                 bool highPrecision,
                                 uint32_t flowScale, bool performanceMode)
        : m_instance(instance.handle())
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        FileDescriptors fds = descriptors.release();
        auto* backend = static_cast<mako::backend::Instance*>(m_instance);
        mako::backend::Context& context = backend->openContext(
            {fds.sourceA, fds.sourceB},
            fds.destinations,
            fds.sync,
            width, height,
            highPrecision ? mako::backend::FrameEncoding::SdrHighPrecision
                          : mako::backend::FrameEncoding::Sdr8,
            static_cast<float>(flowScale),
            performanceMode,
            false,
            0);
        m_object.reset(&context);
    }

    bool EngineContext::scheduleFrames() noexcept
    {
        if (!m_object || !m_instance || contextLost())
            return false;
        try
        {
            std::lock_guard<std::mutex> lock(apiMutex);
            if (contextLost())
                return false;
            static_cast<mako::backend::Instance*>(m_instance)->scheduleFrames(
                *static_cast<mako::backend::Context*>(m_object.get()));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool EngineContext::waitForIdle(uint64_t timeoutNs) noexcept
    {
        if (!m_object || !m_instance || contextLost())
            return true;
        try
        {
            std::lock_guard<std::mutex> lock(apiMutex);
            if (contextLost())
                return true;
            return static_cast<mako::backend::Context*>(m_object.get())->waitForIdle(timeoutNs);
        }
        catch (...)
        {
            return false;
        }
    }

    bool EngineContext::contextLost() const noexcept
    {
        if (!m_object)
            return true;
        try
        {
            return static_cast<mako::backend::Context*>(m_object.get())->lost();
        }
        catch (...)
        {
            return true;
        }
    }

    bool EngineContext::setLossHandler(std::function<void()> handler) noexcept
    {
        if (!m_object || !m_instance)
            return false;
        try
        {
            std::lock_guard<std::mutex> lock(apiMutex);
            static_cast<mako::backend::Instance*>(m_instance)->setContextLossHandler(
                *static_cast<mako::backend::Context*>(m_object.get()), std::move(handler));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
#else
    EngineInstance::~EngineInstance() = default;

    void EngineInstance::ObjectDeleter::operator()(void* p) const
    {
        (void)p;
    }

    EngineInstance::EngineInstance(LogicalDevice*, bool, const std::string&)
    {
        throw std::runtime_error("FG engine disabled at build time");
    }

    EngineContext::~EngineContext() = default;

    void EngineContext::ObjectDeleter::operator()(void* p) const
    {
        (void)p;
    }

    EngineContext::EngineContext(EngineInstance&, FileDescriptorScope&, uint32_t, uint32_t,
                                 bool, uint32_t, bool)
    {
        throw std::runtime_error("FG engine disabled at build time");
    }

    bool EngineContext::scheduleFrames() noexcept { return false; }
    bool EngineContext::waitForIdle(uint64_t) noexcept { return true; }
    bool EngineContext::contextLost() const noexcept { return true; }
    bool EngineContext::setLossHandler(std::function<void()>) noexcept { return false; }
#endif
} // namespace VKIntox::FrameGen
