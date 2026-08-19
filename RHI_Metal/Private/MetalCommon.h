#pragma once

// Metal and Objective-C imports
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#if RVX_PLATFORM_MACOS
#import <Cocoa/Cocoa.h>
#elif RVX_PLATFORM_IOS
#import <UIKit/UIKit.h>
#endif

// RHI includes
#include "RHI/RHI.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#include <string>
#include <vector>
#include <array>

namespace RVX
{
    // =============================================================================
    // Constants
    // =============================================================================
    constexpr uint32_t kMetalMaxFramesInFlight = 3;

    // Metal exposes 31 vertex-stage buffer bindings, indexed 0 through 30.
    // Descriptor buffers occupy the low indices, so vertex streams are mapped
    // downwards from the highest legal index. This keeps every stream used by
    // the renderer (including the instancing stream at RHI slot 6) in range
    // without overlapping the descriptor-buffer range.
    constexpr uint32 kMetalHighestBufferIndex = 30;

    [[nodiscard]] constexpr uint32 MetalVertexBufferIndex(uint32 inputSlot)
    {
        RVX_ASSERT(inputSlot < RVX_MAX_VERTEX_BUFFERS);
        return kMetalHighestBufferIndex - inputSlot;
    }

    // =============================================================================
    // Metal Error Checking
    // =============================================================================
    inline void MTLCheck(NSError* error, const char* message = "Metal Error")
    {
        if (error)
        {
            RVX_RHI_ERROR("{}: {}", message, [[error localizedDescription] UTF8String]);
            RVX_ASSERT_MSG(false, "Metal Error");
        }
    }

    // =============================================================================
    // ARC Bridge Helpers
    // =============================================================================
    // These help manage Objective-C object lifetimes in C++ containers

    template<typename T>
    class MetalRef
    {
    public:
        MetalRef() : m_object(nil) {}
        MetalRef(T object) : m_object(object) {}
        ~MetalRef() { m_object = nil; }

        MetalRef(const MetalRef& other) : m_object(other.m_object) {}
        MetalRef& operator=(const MetalRef& other) { m_object = other.m_object; return *this; }

        MetalRef(MetalRef&& other) noexcept : m_object(other.m_object) { other.m_object = nil; }
        MetalRef& operator=(MetalRef&& other) noexcept { m_object = other.m_object; other.m_object = nil; return *this; }

        T Get() const { return m_object; }
        T operator->() const { return m_object; }
        operator T() const { return m_object; }
        explicit operator bool() const { return m_object != nil; }

        void Reset(T object = nil) { m_object = object; }

    private:
        T m_object;
    };

} // namespace RVX
