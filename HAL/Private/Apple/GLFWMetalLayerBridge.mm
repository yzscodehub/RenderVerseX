/**
 * @file GLFWMetalLayerBridge.mm
 * @brief Main-thread Apple presentation-layer attachment for GLFW windows
 */

#include "Apple/GLFWMetalLayerBridge.h"

#include "Core/Assert.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

namespace RVX::HAL
{
    uintptr_t AttachGLFWMetalLayer(GLFWwindow* window, uintptr_t& nativeWindow)
    {
        RVX_ASSERT_MSG([NSThread isMainThread],
                       "CAMetalLayer attachment must run on the application main thread");
        if (!window)
        {
            return 0;
        }

        NSWindow* cocoaWindow = glfwGetCocoaWindow(window);
        nativeWindow = reinterpret_cast<uintptr_t>((__bridge void*)cocoaWindow);
        NSView* view = cocoaWindow.contentView;
        if (!view)
        {
            return 0;
        }

        CAMetalLayer* metalLayer = nil;
        if ([view.layer isKindOfClass:[CAMetalLayer class]])
        {
            metalLayer = (CAMetalLayer*)view.layer;
        }
        else
        {
            metalLayer = [CAMetalLayer layer];
            view.wantsLayer = YES;
            view.layer = metalLayer;
        }

        return reinterpret_cast<uintptr_t>((__bridge void*)metalLayer);
    }

    void DetachGLFWMetalLayer(GLFWwindow* window)
    {
        RVX_ASSERT_MSG([NSThread isMainThread],
                       "CAMetalLayer detachment must run on the application main thread");
        if (!window)
        {
            return;
        }

        NSView* view = glfwGetCocoaWindow(window).contentView;
        if (view && [view.layer isKindOfClass:[CAMetalLayer class]])
        {
            view.layer = nil;
        }
    }

} // namespace RVX::HAL
