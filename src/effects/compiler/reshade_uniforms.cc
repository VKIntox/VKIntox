#include "effects/compiler/reshade_uniforms.hh"

#include <cstring>
#include <ctime>
#include <cstdlib>
#include <cmath>
#include <random>

#include <algorithm>

#include "util/logger.hh"
#include "input/common/keyboard_input.hh"

namespace VKIntox
{
    // Thread-safe RNG (avoids std::rand() which is not thread-safe)
    static thread_local std::mt19937 tlRng{std::random_device{}()};

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
    void FrameTimeUniform::update(void* mappedBuffer)
    {
        auto                                     currentFrame = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float, std::milli> duration     = currentFrame - lastFrame;
        lastFrame                                             = currentFrame;
        float frametime                                       = duration.count();
        std::memcpy((uint8_t*) mappedBuffer + offset, &(frametime), sizeof(float));
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
    void FrameCountUniform::update(void* mappedBuffer)
    {
        std::memcpy((uint8_t*) mappedBuffer + offset, &(count), sizeof(int32_t));
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
    void DateUniform::update(void* mappedBuffer)
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

        std::memcpy((uint8_t*) mappedBuffer + offset, cachedDate, sizeof(float) * 4);
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
    void TimerUniform::update(void* mappedBuffer)
    {
        auto                                     currentFrame = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float, std::milli> duration     = currentFrame - start;
        float                                    timer        = duration.count();
        std::memcpy((uint8_t*) mappedBuffer + offset, &(timer), sizeof(float));
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
    void PingPongUniform::update(void* mappedBuffer)
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
        std::memcpy((uint8_t*) mappedBuffer + offset, currentValue, sizeof(float) * 2);
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
    void RandomUniform::update(void* mappedBuffer)
    {
        if (min > max) std::swap(min, max);
        std::uniform_int_distribution<int32_t> dist(min, max);
        int32_t value = dist(tlRng);
        std::memcpy((uint8_t*) mappedBuffer + offset, &(value), sizeof(int32_t));
    }
    RandomUniform::~RandomUniform()
    {
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Windows virtual-key codes, which is what ReShade effects put in the
    // keycode annotation, mapped to the X11 keysyms the input layer speaks.
    // 0 for unknown keys.
    static uint32_t vkToKeySym(uint32_t vk)
    {
        switch (vk)
        {
            case 0x08: return 0xFF08;            // backspace
            case 0x09: return 0xFF09;            // tab
            case 0x0C: return 0xFF0B;            // clear
            case 0x0D: return 0xFF0D;            // return
            case 0x13: return 0xFF13;            // pause
            case 0x14: return 0xFFE5;            // caps lock
            case 0x1B: return 0xFF1B;            // escape
            case 0x20: return 0x0020;            // space
            case 0x21: return 0xFF55;            // page up
            case 0x22: return 0xFF56;            // page down
            case 0x23: return 0xFF57;            // end
            case 0x24: return 0xFF50;            // home
            case 0x25: return 0xFF51;            // left
            case 0x26: return 0xFF52;            // up
            case 0x27: return 0xFF53;            // right
            case 0x28: return 0xFF54;            // down
            case 0x2C: return 0xFF61;            // print screen
            case 0x2D: return 0xFF63;            // insert
            case 0x2E: return 0xFFFF;            // delete
            case 0x2F: return 0xFF6A;            // help
            case 0x5B: return 0xFFEB;            // left windows
            case 0x5C: return 0xFFEC;            // right windows
            case 0x5D: return 0xFF67;            // application
            case 0x6A: return 0xFFAA;            // numpad multiply
            case 0x6B: return 0xFFAB;            // numpad add
            case 0x6C: return 0xFFAC;            // numpad separator
            case 0x6D: return 0xFFAD;            // numpad subtract
            case 0x6E: return 0xFFAE;            // numpad decimal
            case 0x6F: return 0xFFAF;            // numpad divide
            case 0x90: return 0xFF7F;            // num lock
            case 0x91: return 0xFF14;            // scroll lock
            case 0xA0: return 0xFFE1;            // left shift
            case 0xA1: return 0xFFE2;            // right shift
            case 0xA2: return 0xFFE3;            // left control
            case 0xA3: return 0xFFE4;            // right control
            case 0xA4: return 0xFFE9;            // left alt
            case 0xA5: return 0xFFEA;            // right alt
            case 0xBA: return 0x003B;            // ;
            case 0xBB: return 0x003D;            // =
            case 0xBC: return 0x002C;            // ,
            case 0xBD: return 0x002D;            // -
            case 0xBE: return 0x002E;            // .
            case 0xBF: return 0x002F;            // /
            case 0xC0: return 0x0060;            // `
            case 0xDB: return 0x005B;            // [
            case 0xDC: return 0x005C;            // backslash
            case 0xDD: return 0x005D;            // ]
            case 0xDE: return 0x0027;            // '
            default: break;
        }

        if (vk >= '0' && vk <= '9')
            return vk;                         // XK_0..XK_9 share the VK codes
        if (vk >= 'A' && vk <= 'Z')
            return vk + 0x20;                  // XK_a..XK_z sit below the capitals
        if (vk >= 0x60 && vk <= 0x69)
            return 0xFFB0 + (vk - 0x60);       // numpad 0-9
        if (vk >= 0x70 && vk <= 0x87)
            return 0xFFBE + (vk - 0x70);       // F1..F24
        return 0;
    }

    KeyUniform::KeyUniform(reshadefx::uniform_info uniformInfo)
    {
        auto source = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; });
        if (source->value.string_data != "key")
        {
            Logger::err("Tried to create a KeyUniform from a non key uniform_info");
        }
        offset = uniformInfo.offset;
        size   = uniformInfo.size;
        base   = uniformInfo.type.base;
        components = std::max<uint32_t>(uniformInfo.type.components(), 1u);

        uint32_t vk = 0;
        if (auto keycode = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "keycode"; });
            keycode != uniformInfo.annotations.end())
        {
            vk = keycode->value.as_uint[0];
        }
        if (vk <= 7 || vk >= 256)
        {
            // ReShade leaves the uniform at its initializer value in this case
            Logger::warn("key uniform uses an unusable keycode (" + std::to_string(vk) + "); it will not react to input");
            keysymA = keysymB = 0;
            return;
        }

        keysymA = vkToKeySym(vk);
        // the generic modifier VKs stand for either physical side
        switch (vk)
        {
            case 0x10: keysymA = 0xFFE1; keysymB = 0xFFE2; break; // shift
            case 0x11: keysymA = 0xFFE3; keysymB = 0xFFE4; break; // control
            case 0x12: keysymA = 0xFFE9; keysymB = 0xFFEA; break; // alt
            default: break;
        }
        if (keysymA == 0)
        {
            Logger::warn("key uniform uses an unmapped keycode (" + std::to_string(vk) + "); it will not react to input");
            return;
        }

        if (auto modeAnnotation = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "mode"; });
            modeAnnotation != uniformInfo.annotations.end())
        {
            if (modeAnnotation->value.string_data == "toggle")
                mode = Mode::toggle;
            else if (modeAnnotation->value.string_data == "press")
                mode = Mode::press;
        }
        else if (auto toggle = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "toggle"; });
                 toggle != uniformInfo.annotations.end() && toggle->value.as_int[0] != 0)
        {
            mode = Mode::toggle;
        }

        if (auto invertAnnotation = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "invert"; });
            invertAnnotation != uniformInfo.annotations.end() && invertAnnotation->value.as_uint[0] != 0)
        {
            invert = true;
        }

        Logger::debug("key uniform keycode=" + std::to_string(vk) + " keysym=" + std::to_string(keysymA)
                      + " mode=" + (mode == Mode::toggle ? "toggle" : mode == Mode::press ? "press" : "hold")
                      + (invert ? " inverted" : ""));
    }

    // Bools are 4-byte VkBool32 in the uniform buffer (the SPIR-V codegen
    // lowers them to ints), ints and uints are 4 bytes, floats are floats.
    void KeyUniform::writeValue(void* mappedBuffer, bool value)
    {
        uint8_t* dst = (uint8_t*)mappedBuffer + offset;
        for (uint32_t i = 0; i < components; ++i)
        {
            switch (base)
            {
                case reshadefx::type::t_bool:
                {
                    VkBool32 v = value ? VK_TRUE : VK_FALSE;
                    std::memcpy(dst + i * sizeof(VkBool32), &v, sizeof(VkBool32));
                    break;
                }
                case reshadefx::type::t_int:
                {
                    int32_t v = value ? 1 : 0;
                    std::memcpy(dst + i * sizeof(int32_t), &v, sizeof(int32_t));
                    break;
                }
                case reshadefx::type::t_uint:
                {
                    uint32_t v = value ? 1u : 0u;
                    std::memcpy(dst + i * sizeof(uint32_t), &v, sizeof(uint32_t));
                    break;
                }
                case reshadefx::type::t_float:
                {
                    float v = value ? 1.0f : 0.0f;
                    std::memcpy(dst + i * sizeof(float), &v, sizeof(float));
                    break;
                }
                default:
                    return;
            }
        }
    }

    void KeyUniform::update(void* mappedBuffer)
    {
        // an unusable or unmapped keycode never writes: the uniform keeps its
        // initializer value, same as ReShade
        if (keysymA == 0)
            return;

        bool down = VKIntox::isKeyHeld(keysymA) || (keysymB != 0 && VKIntox::isKeyHeld(keysymB));
        bool value = false;
        switch (mode)
        {
            case Mode::hold:
                value = down;
                break;
            case Mode::press:
                value = down && !lastDown;
                break;
            case Mode::toggle:
                if (down && !lastDown)
                    toggleState = !toggleState;
                value = toggleState;
                break;
        }
        lastDown = down;
        if (invert)
            value = !value;

        writeValue(mappedBuffer, value);
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
    void MouseButtonUniform::update(void* mappedBuffer)
    {
        VkBool32 keyDown = VK_FALSE; // TODO
        std::memcpy((uint8_t*) mappedBuffer + offset, &(keyDown), sizeof(VkBool32));
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
    void MousePointUniform::update(void* mappedBuffer)
    {
        float point[2] = {0.0f, 0.0f}; // TODO
        std::memcpy((uint8_t*) mappedBuffer + offset, point, sizeof(float) * 2);
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
    void MouseDeltaUniform::update(void* mappedBuffer)
    {
        float delta[2] = {0.0f, 0.0f}; // TODO
        std::memcpy((uint8_t*) mappedBuffer + offset, delta, sizeof(float) * 2);
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
    void DepthUniform::update(void* mappedBuffer)
    {
        VkBool32 hasDepth = depthAvailable ? VK_TRUE : VK_FALSE;
        std::memcpy((uint8_t*) mappedBuffer + offset, &(hasDepth), sizeof(VkBool32));
    }
    DepthUniform::~DepthUniform()
    {
    }
} // namespace VKIntox
