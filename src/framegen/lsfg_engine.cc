#include "framegen/lsfg_engine.hh"

#include "util/logger.hh"

#include <dlfcn.h>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

// mangled entrypoints of lsfg-vk's exported C++ API (built unmodified from the
// vendored submodule; see src/vendor/lsfg-vk)
typedef void LsfgInstanceCtor(void* self,
                              const std::string& deviceId,
                              const std::filesystem::path& dllPath,
                              bool allowFp16,
                              bool leakInstance,
                              std::function<void(const std::string&)> log);
typedef void LsfgInstanceDtor(void* self);
typedef void LsfgContextCtor(void* self,
                             const void* instance,
                             uint32_t width,
                             uint32_t height,
                             float flowScale,
                             bool performanceMode);
typedef void LsfgContextDtor(void* self);

// lsfgvk::FileDescriptors is three ints; the C ABI returns it in registers
struct LsfgFdsRaw { int sourceFd; int destinationFd; int syncFd; };
typedef LsfgFdsRaw LsfgExportFds(void* self);
typedef void LsfgDispatch(void* self, uint32_t total, bool unsignaled);
typedef void LsfgAcquire(void* self, bool unsignaled);
typedef void LsfgIdle(void* self);

namespace VKIntox::FrameGen
{
    namespace
    {
        struct Api
        {
            void* library = nullptr;
            LsfgInstanceCtor* instanceCtor = nullptr;
            LsfgInstanceDtor* instanceDtor = nullptr;
            LsfgContextCtor* contextCtor = nullptr;
            LsfgContextDtor* contextDtor = nullptr;
            LsfgExportFds* contextExportFds = nullptr;
            LsfgDispatch* contextDispatch = nullptr;
            LsfgAcquire* contextAcquire = nullptr;
            LsfgIdle* contextIdle = nullptr;
        };

        std::mutex apiMutex;
        Api api;
        std::string apiError;

        template <typename T>
        T symbol(void* library, const char* name)
        {
            void* p = dlsym(library, name);
            if (!p)
                throw std::runtime_error(std::string("missing engine symbol: ") + name);
            return reinterpret_cast<T>(p);
        }

        // the engine is looked up next to the layer itself first: in a Flatpak
        // runtime extension both libraries ship together, and outside it the
        // installed layer directory is the only stable reference point
        std::string ownDirectory()
        {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(&engineAvailable), &info) == 0 || !info.dli_fname)
                return "";
            std::string path = info.dli_fname;
            const size_t slash = path.rfind('/');
            return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
        }

        bool loadLibrary()
        {
            if (api.library)
                return true;
            if (!apiError.empty())
                return false;

            const std::string soname = "liblsfg-vk-pipeline.so.2";
            std::string lastError;
            std::vector<std::string> candidates;
            const std::string own = ownDirectory();
            if (!own.empty())
                candidates.push_back(own + "/" + soname);
            candidates.push_back(soname);

            for (const auto& candidate : candidates)
            {
                void* library = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (library)
                {
                    api.library = library;
                    break;
                }
                lastError = dlerror() ? dlerror() : candidate;
            }
            if (!api.library)
            {
                apiError = "engine library not found (" + lastError + ")";
                return false;
            }

            try
            {
                api.instanceCtor = symbol<LsfgInstanceCtor*>(api.library, "_ZN6lsfgvk8InstanceC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKNSt10filesystem7__cxx114pathEbbSt8functionIFvS8_EE");
                api.instanceDtor = symbol<LsfgInstanceDtor*>(api.library, "_ZN6lsfgvk8InstanceD1Ev");
                api.contextCtor = symbol<LsfgContextCtor*>(api.library, "_ZN6lsfgvk7ContextC1ERKNS_8InstanceEjjfbNS_8BitDepthENS_10ColorSpaceENS_16TransferFunctionE");
                api.contextDtor = symbol<LsfgContextDtor*>(api.library, "_ZN6lsfgvk7ContextD1Ev");
                api.contextExportFds = symbol<LsfgExportFds*>(api.library, "_ZNK6lsfgvk7Context9exportFdsEv");
                api.contextDispatch = symbol<LsfgDispatch*>(api.library, "_ZN6lsfgvk7Context8dispatchEjb");
                api.contextAcquire = symbol<LsfgAcquire*>(api.library, "_ZN6lsfgvk7Context7acquireEb");
                api.contextIdle = symbol<LsfgIdle*>(api.library, "_ZN6lsfgvk7Context4idleEv");
            }
            catch (const std::exception& e)
            {
                dlclose(api.library);
                api.library = nullptr;
                apiError = e.what();
                return false;
            }
            Logger::info("LSFG engine loaded from " + candidates[0]);
            return true;
        }
    }

    bool engineAvailable()
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        return loadLibrary();
    }

    std::string engineStatus()
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        if (api.library)
            return "engine loaded";
        if (!apiError.empty())
            return apiError;
        loadLibrary();
        return api.library ? "engine loaded" : apiError;
    }

    void EngineInstance::ObjectDeleter::operator()(void* p) const
    {
        if (!p)
            return;
        std::lock_guard<std::mutex> lock(apiMutex);
        if (api.instanceDtor)
            api.instanceDtor(p);
    }

    EngineInstance::EngineInstance(const std::string& deviceId,
                                   const std::filesystem::path& dllPath,
                                   bool allowFp16,
                                   std::function<void(const std::string&)> log)
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        if (!loadLibrary())
            throw std::runtime_error(apiError);
        void* storage = new unsigned char[64];
        try
        {
            api.instanceCtor(storage, deviceId, dllPath, allowFp16, true, std::move(log));
        }
        catch (...)
        {
            delete[] (unsigned char*)storage;
            throw;
        }
        m_object.reset(storage);
    }

    EngineInstance::~EngineInstance() = default;

    void EngineContext::ObjectDeleter::operator()(void* p) const
    {
        if (!p)
            return;
        std::lock_guard<std::mutex> lock(apiMutex);
        if (api.contextDtor)
            api.contextDtor(p); // calls idle() internally
    }

    EngineContext::EngineContext(const EngineInstance& instance,
                                 uint32_t width, uint32_t height,
                                 float flowScale, bool performanceMode)
        : m_instance(&instance)
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        void* storage = new unsigned char[64];
        try
        {
            api.contextCtor(storage, instance.handle(), width, height, flowScale, performanceMode);
        }
        catch (...)
        {
            delete[] (unsigned char*)storage;
            throw;
        }
        m_object.reset(storage);
    }

    EngineContext::~EngineContext() = default;

    FileDescriptors EngineContext::exportFds()    {
        std::lock_guard<std::mutex> lock(apiMutex);
        LsfgFdsRaw raw = api.contextExportFds(m_object.get());
        return FileDescriptors{raw.sourceFd, raw.destinationFd, raw.syncFd};
    }

    void EngineContext::dispatch(uint32_t total, bool unsignaled)
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        api.contextDispatch(m_object.get(), total, unsignaled);
    }

    void EngineContext::acquire(bool unsignaled)
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        api.contextAcquire(m_object.get(), unsignaled);
    }

    void EngineContext::idle()
    {
        std::lock_guard<std::mutex> lock(apiMutex);
        api.contextIdle(m_object.get());
    }
} // namespace VKIntox::FrameGen
