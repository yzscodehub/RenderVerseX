#pragma once

#include "Core/Types.h"
#include <memory>

// Undefine Windows macros that conflict with our API
#ifdef CreateWindow
#undef CreateWindow
#endif

namespace RVX
{
    struct WindowDesc
    {
        uint32 width = 1280;
        uint32 height = 720;
        const char* title = "RenderVerseX";
        bool resizable = true;
        bool fullscreen = false;
    };

    class Window
    {
    public:
        virtual ~Window() = default;

        virtual void PollEvents() = 0;
        virtual bool ShouldClose() const = 0;
        virtual void GetFramebufferSize(uint32& width, uint32& height) const = 0;
        virtual float GetDpiScale() const = 0;
        virtual void* GetNativeHandle() const = 0;
    };

    std::unique_ptr<Window> CreateWindow(const WindowDesc& desc);
} // namespace RVX
