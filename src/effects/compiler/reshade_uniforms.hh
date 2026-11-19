#ifndef RESHADE_UNIFORMS_HPP_INCLUDED
#define RESHADE_UNIFORMS_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <chrono>
#include <memory>

#include "render/device/vulkan_include.hh"

#include "vendor/reshade/effect_module.hh"

namespace VKIntox
{
    void enumerateReshadeUniforms(reshadefx::module module);

    class ReshadeUniform
    {
    public:
        void virtual update(void* mappedBuffer) = 0;
        virtual ~ReshadeUniform(){};

    protected:
        uint32_t offset;
        uint32_t size;
    };

    std::vector<std::shared_ptr<ReshadeUniform>> createReshadeUniforms(reshadefx::module module);

    class FrameTimeUniform : public ReshadeUniform
    {
    public:
        FrameTimeUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mappedBuffer) override;
        virtual ~FrameTimeUniform();

    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> lastFrame;
    };

    class FrameCountUniform : public ReshadeUniform
    {
    public:
        FrameCountUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mappedBuffer) override;
        virtual ~FrameCountUniform();

    private:
        int32_t count = 0;
    };

    class DateUniform : public ReshadeUniform
    {
    public:
        DateUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mappedBuffer) override;
        virtual ~DateUniform();
    };

    class TimerUniform : public ReshadeUniform
    {
    public:
        TimerUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mappedBuffer) override;
        virtual ~TimerUniform();

    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> start;
    };

    class PingPongUniform : public ReshadeUniform
    {
    public:
        PingPongUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mappedBuffer) override;
        virtual ~PingPongUniform();

    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> lastFrame;

        float min             = 0.0f;
        float max             = 0.0f;
        float stepMin         = 0.0f;
        float stepMax         = 0.0f;
        float smoothing       = 0.0f;
        float currentValue[2] = {0.0f, 1.0f};
    };

    class RandomUniform : public ReshadeUniform
    {
    public:
        RandomUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mappedBuffer) override;
        virtual ~RandomUniform();

    private:
        int max = 0;
        int min = 0;
    };

    // exported so the key uniform test can construct real instances against
    // the layer's live key state
    class __attribute__((visibility("default"))) KeyUniform : public ReshadeUniform
    {
    public:
        KeyUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mappedBuffer) override;
        virtual ~KeyUniform();

    private:
        enum class Mode { hold, press, toggle };

        void writeValue(void* mappedBuffer, bool value);

        // Windows virtual-key code the effect asked for, plus up to two X11
        // keysyms covering both sides of the generic modifier VKs
        uint32_t keysymA = 0;
        uint32_t keysymB = 0;
        Mode mode = Mode::hold;
        bool invert = false;
        reshadefx::type::datatype base = reshadefx::type::t_bool;
        uint32_t components = 1;

        // press/toggle edge tracking across frames
        bool lastDown = false;
        bool toggleState = false;
    };

    class MouseButtonUniform : public ReshadeUniform
    {
    public:
        MouseButtonUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mappedBuffer) override;
        virtual ~MouseButtonUniform();
    };

    class MousePointUniform : public ReshadeUniform
    {
    public:
        MousePointUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mappedBuffer) override;
        virtual ~MousePointUniform();
    };

    class MouseDeltaUniform : public ReshadeUniform
    {
    public:
        MouseDeltaUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mappedBuffer) override;
        virtual ~MouseDeltaUniform();
    };

    class DepthUniform : public ReshadeUniform
    {
    public:
        DepthUniform(reshadefx::uniform_info uniformInfo);
        void virtual update(void* mappedBuffer) override;
        void setDepthAvailable(bool available) { depthAvailable = available; }
        virtual ~DepthUniform();

    private:
        bool depthAvailable = false;
    };
} // namespace VKIntox

#endif // RESHADE_UNIFORMS_HPP_INCLUDED
