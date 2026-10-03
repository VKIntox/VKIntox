#include "reshade_uniforms.hh"

#include <cstring>
#include <ctime>
#include <cstdlib>
#include <cmath>
#include <random>

#include <algorithm>

#include "logger.hh"
#include "keyboard_input.hh"

namespace VKIntox
{
    // Thread-safe RNG (avoids std::rand() which is not thread-safe)
    static thread_local std::mt19937 tlRng{std::random_device{}()};

    // ReShade's source="key" annotation carries a Windows virtual-key code.
    // Map the keys that exist on XKB onto keysyms; the rest stay unmapped so the
    // uniform reads false rather than guessing at a key.
    static uint32_t vkKeyToKeysym(uint32_t vk)
    {
        if (vk >= 0x30 && vk <= 0x39) return vk;                    // 0-9
        if (vk >= 0x41 && vk <= 0x5A) return vk + 0x20;             // A-Z to lower keysyms
        if (vk >= 0x60 && vk <= 0x69) return 0xFFB0 + (vk - 0x60);  // numpad 0-9
        if (vk >= 0x70 && vk <= 0x7B) return 0xFFBE + (vk - 0x70);  // F1-F12

        switch (vk)
        {
            case 0x08: return 0xFF08;  // Backspace
            case 0x09: return 0xFF09;  // Tab
            case 0x0D: return 0xFF0D;  // Return
            case 0x10: return 0xFFE1;  // Shift
            case 0x11: return 0xFFE3;  // Control
            case 0x12: return 0xFFE9;  // Alt
            case 0x13: return 0xFF13;  // Pause
            case 0x14: return 0xFFE5;  // Caps Lock
            case 0x1B: return 0xFF1B;  // Escape
            case 0x20: return 0x0020;  // space
            case 0x21: return 0xFF55;  // Page Up
            case 0x22: return 0xFF56;  // Page Down
            case 0x23: return 0xFF57;  // End
            case 0x24: return 0xFF50;  // Home
            case 0x25: return 0xFF51;  // Left
            case 0x26: return 0xFF52;  // Up
            case 0x27: return 0xFF53;  // Right
            case 0x28: return 0xFF54;  // Down
            case 0x2C: return 0xFF61;  // Print Screen
            case 0x2D: return 0xFF63;  // Insert
            case 0x2E: return 0xFFFF;  // Delete
            case 0x5B: return 0xFFEB;  // Super
            case 0x5C: return 0xFFEC;  // Super
            case 0x5D: return 0xFF67;  // Menu
            case 0x6A: return 0xFFAA;  // numpad multiply
            case 0x6B: return 0xFFAB;  // numpad add
            case 0x6D: return 0xFFAD;  // numpad subtract
            case 0x6E: return 0xFFAE;  // numpad decimal
            case 0x6F: return 0xFFAF;  // numpad divide
            case 0x90: return 0xFF7F;  // Num Lock
            case 0x91: return 0xFF14;  // Scroll Lock
            case 0xBA: return 0x003B;  // ;
            case 0xBB: return 0x003D;  // =
            case 0xBC: return 0x002C;  // ,
            case 0xBD: return 0x002D;  // -
            case 0xBE: return 0x002E;  // .
            case 0xBF: return 0x002F;  // /
            case 0xC0: return 0x0060;  // `
            case 0xDB: return 0x005B;  // [
            case 0xDC: return 0x005C;  // backslash
            case 0xDD: return 0x005D;  // ]
            case 0xDE: return 0x0027;  // '
            default:   return 0;
        }
    }

    void enumerateReshadeUniforms(reshadefx::module module)
    {
        for (auto& uniform : module.uniforms)
        {
            auto it = std::find_if(uniform.annotations.begin(), uniform.annotations.end(), [](const auto& a) {
                          return a.name == "source";
                      });
            if (it == uniform.annotations.end())
            {
                Logger::debug("uniform without source annotation (offset: " + std::to_string(uniform.offset) + ")");
                continue;
            }
            Logger::debug(it->value.string_data);
            Logger::debug("size: " + std::to_string(uniform.size));
            Logger::debug("offset: " + std::to_string(uniform.offset));
        }
    }

    std::vector<std::shared_ptr<ReshadeUniform>> createReshadeUniforms(reshadefx::module module)
    {
        std::vector<std::shared_ptr<ReshadeUniform>> uniforms;
        for (auto& uniform : module.uniforms)
        {
            auto it = std::find_if(uniform.annotations.begin(), uniform.annotations.end(), [](const auto& a) {
                           return a.name == "source";
                       });
            if (it == uniform.annotations.end())
                continue;
            auto source = it->value.string_data;
            if (source == "frametime")
            {
                uniforms.push_back(std::make_shared<FrameTimeUniform>(uniform));
            }
            else if (source == "framecount")
            {
                uniforms.push_back(std::make_shared<FrameCountUniform>(uniform));
            }
            else if (source == "date")
            {
                uniforms.push_back(std::make_shared<DateUniform>(uniform));
            }
            else if (source == "timer")
            {
                uniforms.push_back(std::make_shared<TimerUniform>(uniform));
            }
            else if (source == "pingpong")
            {
                uniforms.push_back(std::make_shared<PingPongUniform>(uniform));
            }
            else if (source == "random")
            {
                uniforms.push_back(std::make_shared<RandomUniform>(uniform));
            }
            else if (source == "key")
            {
                uniforms.push_back(std::make_shared<KeyUniform>(uniform));
            }
            else if (source == "mousebutton")
            {
                uniforms.push_back(std::make_shared<MouseButtonUniform>(uniform));
            }
            else if (source == "mousepoint")
            {
                uniforms.push_back(std::make_shared<MousePointUniform>(uniform));
            }
            else if (source == "mousedelta")
            {
                uniforms.push_back(std::make_shared<MouseDeltaUniform>(uniform));
            }
            else if (source == "bufready_depth")
            {
                uniforms.push_back(std::make_shared<DepthUniform>(uniform));
            }
        }
        return uniforms;
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    FrameTimeUniform::FrameTimeUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "frametime")
        {
            Logger::err("Tried to create a FrameTimeUniform from a non frametime uniform_info");
        }
        lastFrame = std::chrono::high_resolution_clock::now();
        offset    = uniformInfo.offset;
        size      = uniformInfo.size;
    }
    void FrameTimeUniform::update(void* mapedBuffer)
    {
        auto                                     currentFrame = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float, std::milli> duration     = currentFrame - lastFrame;
        lastFrame                                             = currentFrame;
        float frametime                                       = duration.count();
        std::memcpy((uint8_t*) mapedBuffer + offset, &(frametime), sizeof(float));
    }
    FrameTimeUniform::~FrameTimeUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    FrameCountUniform::FrameCountUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "framecount")
        {
            Logger::err("Tried to create a FrameCountUniform from a non framecount uniform_info");
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void FrameCountUniform::update(void* mapedBuffer)
    {
        std::memcpy((uint8_t*) mapedBuffer + offset, &(count), sizeof(int32_t));
        count++;
    }
    FrameCountUniform::~FrameCountUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    DateUniform::DateUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "date")
        {
            Logger::err("Tried to create a DateUniform from a non date uniform_info");
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void DateUniform::update(void* mapedBuffer)
    {
        // Date only changes once per second — cache the result and only
        // recompute when the second changes. Saves ~234 localtime_r calls/sec
        // at 235 FPS.
        static thread_local std::time_t lastSecond = 0;
        static thread_local float cachedDate[4] = {};

        auto        now  = std::chrono::system_clock::now();
        std::time_t nowC = std::chrono::system_clock::to_time_t(now);

        if (nowC != lastSecond)
        {
            lastSecond = nowC;
            struct tm currentTimeBuf;
            localtime_r(&nowC, &currentTimeBuf);
            cachedDate[0] = 1900.0f + static_cast<float>(currentTimeBuf.tm_year);
            cachedDate[1] = 1.0f + static_cast<float>(currentTimeBuf.tm_mon);
            cachedDate[2] = static_cast<float>(currentTimeBuf.tm_mday);
            cachedDate[3] = static_cast<float>((currentTimeBuf.tm_hour * 60 + currentTimeBuf.tm_min) * 60 + currentTimeBuf.tm_sec);
        }

        std::memcpy((uint8_t*) mapedBuffer + offset, cachedDate, sizeof(float) * 4);
    }
    DateUniform::~DateUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    TimerUniform::TimerUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "timer")
        {
            Logger::err("Tried to create a TimerUniform from a non timer uniform_info");
        }
        start  = std::chrono::high_resolution_clock::now();
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void TimerUniform::update(void* mapedBuffer)
    {
        auto                                     currentFrame = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float, std::milli> duration     = currentFrame - start;
        float                                    timer        = duration.count();
        std::memcpy((uint8_t*) mapedBuffer + offset, &(timer), sizeof(float));
    }
    TimerUniform::~TimerUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    PingPongUniform::PingPongUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "pingpong")
        {
            Logger::err("Tried to create a PingPongUniform from a non pingpong uniform_info");
        }
        if (auto minAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "min"; });
            minAnnotation != uniformInfo.annotations.end())
        {
            min = minAnnotation->type.is_floating_point() ? minAnnotation->value.as_float[0] : static_cast<float>(minAnnotation->value.as_int[0]);
        }
        if (auto maxAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "max"; });
            maxAnnotation != uniformInfo.annotations.end())
        {
            max = maxAnnotation->type.is_floating_point() ? maxAnnotation->value.as_float[0] : static_cast<float>(maxAnnotation->value.as_int[0]);
        }
        if (auto smoothingAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "smoothing"; });
            smoothingAnnotation != uniformInfo.annotations.end())
        {
            smoothing = smoothingAnnotation->type.is_floating_point() ? smoothingAnnotation->value.as_float[0]
                                                                      : static_cast<float>(smoothingAnnotation->value.as_int[0]);
        }
        if (auto stepAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "step"; });
            stepAnnotation != uniformInfo.annotations.end())
        {
            stepMin =
                stepAnnotation->type.is_floating_point() ? stepAnnotation->value.as_float[0] : static_cast<float>(stepAnnotation->value.as_int[0]);
            stepMax =
                stepAnnotation->type.is_floating_point() ? stepAnnotation->value.as_float[1] : static_cast<float>(stepAnnotation->value.as_int[1]);
        }

        lastFrame = std::chrono::high_resolution_clock::now();

        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void PingPongUniform::update(void* mapedBuffer)
    {
        auto currentFrame = std::chrono::high_resolution_clock::now();

        std::chrono::duration<float, std::ratio<1>> frameTime = currentFrame - lastFrame;

        float increment = stepMax == 0 ? stepMin : (stepMin + std::fmod(static_cast<float>(tlRng()), stepMax - stepMin + 1.0f));
        if (currentValue[1] >= 0)
        {
            increment = std::max(increment - std::max(0.0f, smoothing - (max - currentValue[0])), 0.05f);
            increment *= frameTime.count();

            if ((currentValue[0] += increment) >= max)
            {
                currentValue[0] = max, currentValue[1] = -1.0f;
            }
        }
        else
        {
            increment = std::max(increment - std::max(0.0f, smoothing - (currentValue[0] - min)), 0.05f);
            increment *= frameTime.count();

            if ((currentValue[0] -= increment) <= min)
            {
                currentValue[0] = min, currentValue[1] = 1.0f;
            }
        }
        std::memcpy((uint8_t*) mapedBuffer + offset, currentValue, sizeof(float) * 2);
    }
    PingPongUniform::~PingPongUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    RandomUniform::RandomUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "random")
        {
            Logger::err("Tried to create a RandomUniform from a non random uniform_info");
        }
        if (auto minAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "min"; });
            minAnnotation != uniformInfo.annotations.end())
        {
            min = minAnnotation->type.is_integral() ? minAnnotation->value.as_int[0] : static_cast<int>(minAnnotation->value.as_float[0]);
        }
        if (auto maxAnnotation =
                std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "max"; });
            maxAnnotation != uniformInfo.annotations.end())
        {
            max = maxAnnotation->type.is_integral() ? maxAnnotation->value.as_int[0] : static_cast<int>(maxAnnotation->value.as_float[0]);
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void RandomUniform::update(void* mapedBuffer)
    {
        if (min > max) std::swap(min, max);
        std::uniform_int_distribution<int32_t> dist(min, max);
        int32_t value = dist(tlRng);
        std::memcpy((uint8_t*) mapedBuffer + offset, &(value), sizeof(int32_t));
    }
    RandomUniform::~RandomUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    KeyUniform::KeyUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "key")
        {
            Logger::err("Tried to create a KeyUniform from a non key uniform_info");
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;

        auto keycode = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "keycode"; });
        if (keycode != uniformInfo.annotations.end())
        {
            const uint32_t vk = keycode->type.is_integral()
                                    ? static_cast<uint32_t>(keycode->value.as_int[0])
                                    : static_cast<uint32_t>(keycode->value.as_float[0]);
            keysym = vkKeyToKeysym(vk);
        }

        auto modeAnnotation = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "mode"; });
        if (modeAnnotation != uniformInfo.annotations.end())
        {
            const std::string& name = modeAnnotation->value.string_data;
            if (name == "press")
                mode = Mode::Press;
            else if (name == "toggle")
                mode = Mode::Toggle;
        }

        if (keysym == 0)
            Logger::warn("KeyUniform: source=\"key\" without a mappable keycode");
    }
    void KeyUniform::update(void* mapedBuffer)
    {
        const bool down = keysym != 0 && isKeyDown(keysym);

        bool value = false;
        switch (mode)
        {
            case Mode::Held:
                value = down;
                break;
            case Mode::Press:
                value = down && !wasDown;
                break;
            case Mode::Toggle:
                if (down && !wasDown)
                    toggled = !toggled;
                value = toggled;
                break;
        }
        wasDown = down;

        VkBool32 keyDown = value ? VK_TRUE : VK_FALSE;
        std::memcpy((uint8_t*) mapedBuffer + offset, &(keyDown), sizeof(VkBool32));
    }
    KeyUniform::~KeyUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    MouseButtonUniform::MouseButtonUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "mousebutton")
        {
            Logger::err("Tried to create a MouseButtonUniform from a non mousebutton uniform_info");
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void MouseButtonUniform::update(void* mapedBuffer)
    {
        VkBool32 keyDown = VK_FALSE; // TODO
        std::memcpy((uint8_t*) mapedBuffer + offset, &(keyDown), sizeof(VkBool32));
    }
    MouseButtonUniform::~MouseButtonUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    MousePointUniform::MousePointUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "mousepoint")
        {
            Logger::err("Tried to create a MousePointUniform from a non mousepoint uniform_info");
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void MousePointUniform::update(void* mapedBuffer)
    {
        float point[2] = {0.0f, 0.0f}; // TODO
        std::memcpy((uint8_t*) mapedBuffer + offset, point, sizeof(float) * 2);
    }
    MousePointUniform::~MousePointUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    MouseDeltaUniform::MouseDeltaUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "mousedelta")
        {
            Logger::err("Tried to create a MouseDeltaUniform from a non mousedelta uniform_info");
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void MouseDeltaUniform::update(void* mapedBuffer)
    {
        float delta[2] = {0.0f, 0.0f}; // TODO
        std::memcpy((uint8_t*) mapedBuffer + offset, delta, sizeof(float) * 2);
    }
    MouseDeltaUniform::~MouseDeltaUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    DepthUniform::DepthUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "bufready_depth")
        {
            Logger::err("Tried to create a DepthUniform from a non bufready_depth uniform_info");
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
    }
    void DepthUniform::update(void* mapedBuffer)
    {
        VkBool32 hasDepth = depthAvailable ? VK_TRUE : VK_FALSE;
        std::memcpy((uint8_t*) mapedBuffer + offset, &(hasDepth), sizeof(VkBool32));
    }
    DepthUniform::~DepthUniform()
    {
    }
} // namespace VKIntox
