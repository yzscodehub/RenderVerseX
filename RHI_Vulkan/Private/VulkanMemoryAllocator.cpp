// VulkanMemoryAllocator.cpp
// This file provides the VMA implementation
// VMA is header-only but needs exactly ONE cpp file to define the implementation

#define VMA_IMPLEMENTATION

// Prevent Windows macro conflicts
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
