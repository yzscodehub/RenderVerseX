#include "VulkanDevice.h"
#include "VulkanResources.h"
#include "VulkanSwapChain.h"
#include "VulkanCommandContext.h"
#include "VulkanPipeline.h"

#include <set>
#include <algorithm>
#include <cstdio>

namespace RVX
{
    // =============================================================================
    // Validation Layer Callback
    // =============================================================================
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData)
    {
        // Filter out harmless loader messages about missing external overlay layers
        // These occur when third-party software (Epic Games, Steam, etc.) registers
        // Vulkan layers but the layer files are missing or moved
        const char* msg = pCallbackData->pMessage;
        if (msg)
        {
            // Skip loader_get_json errors (missing layer JSON files)
            if (strstr(msg, "loader_get_json") != nullptr)
                return VK_FALSE;
            // Skip other common harmless loader messages
            if (strstr(msg, "OverlayVkLayer") != nullptr)
                return VK_FALSE;
        }

        if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            RVX_RHI_ERROR("Vulkan Validation: {}", pCallbackData->pMessage);
        }
        else if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        {
            RVX_RHI_WARN("Vulkan Validation: {}", pCallbackData->pMessage);
        }
        else
        {
            RVX_RHI_DEBUG("Vulkan Validation: {}", pCallbackData->pMessage);
        }
        return VK_FALSE;
    }

    // Helper to create debug messenger
    static VkResult CreateDebugUtilsMessengerEXT(
        VkInstance instance,
        const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDebugUtilsMessengerEXT* pDebugMessenger)
    {
        auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (func)
        {
            return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
        }
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    static void DestroyDebugUtilsMessengerEXT(
        VkInstance instance,
        VkDebugUtilsMessengerEXT debugMessenger,
        const VkAllocationCallbacks* pAllocator)
    {
        auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (func)
        {
            func(instance, debugMessenger, pAllocator);
        }
    }

    // =============================================================================
    // Required Extensions
    // =============================================================================
    static const std::vector<const char*> s_validationLayers = {
        "VK_LAYER_KHRONOS_validation"
    };

    static const std::vector<const char*> s_deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
    };

    // =============================================================================
    // Constructor / Destructor
    // =============================================================================
    VulkanDevice::VulkanDevice(const RHIDeviceDesc& desc)
    {
        RVX_RHI_INFO("Initializing Vulkan Device...");

        m_validationEnabled = desc.enableDebugLayer;

        if (!CreateInstance(desc.enableDebugLayer))
        {
            RVX_RHI_ERROR("Failed to create Vulkan instance");
            return;
        }

        if (!SelectPhysicalDevice())
        {
            RVX_RHI_ERROR("Failed to find suitable GPU");
            return;
        }

        if (!CreateLogicalDevice())
        {
            RVX_RHI_ERROR("Failed to create logical device");
            return;
        }

        if (!CreateAllocator())
        {
            RVX_RHI_ERROR("Failed to create VMA allocator");
            return;
        }

        if (!CreateCommandPools())
        {
            RVX_RHI_ERROR("Failed to create command pools");
            return;
        }

        if (!CreateDescriptorPool())
        {
            RVX_RHI_ERROR("Failed to create descriptor pool");
            return;
        }

        if (!CreatePipelineCache())
        {
            RVX_RHI_ERROR("Failed to create pipeline cache");
            // Non-fatal - continue without caching
        }

        // Create per-frame sync objects
        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

        for (uint32 i = 0; i < RVX_MAX_FRAME_COUNT; ++i)
        {
            VK_CHECK(vkCreateFence(m_device, &fenceInfo, nullptr, &m_frameFences[i]));
            VK_CHECK(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[i]));
            VK_CHECK(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]));
        }

        QueryDeviceCapabilities();

        // Load debug utils functions if validation is enabled
        LoadDebugUtilsFunctions();

        RVX_RHI_INFO("Vulkan Device initialized successfully");
        RVX_RHI_INFO("  Adapter: {}", m_capabilities.adapterName);
        RVX_RHI_INFO("  VRAM: {} MB", m_capabilities.dedicatedVideoMemory / (1024 * 1024));
    }

    VulkanDevice::~VulkanDevice()
    {
        WaitIdle();

        // Destroy sync objects
        for (uint32 i = 0; i < RVX_MAX_FRAME_COUNT; ++i)
        {
            if (m_frameFences[i])
                vkDestroyFence(m_device, m_frameFences[i], nullptr);
            if (m_imageAvailableSemaphores[i])
                vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
            if (m_renderFinishedSemaphores[i])
                vkDestroySemaphore(m_device, m_renderFinishedSemaphores[i], nullptr);
        }

        if (m_descriptorPool)
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);

        const VkCommandPool graphicsPool = m_graphicsCommandPool;
        const VkCommandPool computePool = m_computeCommandPool;
        const VkCommandPool transferPool = m_transferCommandPool;

        if (graphicsPool)
        {
            vkDestroyCommandPool(m_device, graphicsPool, nullptr);
        }
        if (computePool && computePool != graphicsPool)
        {
            vkDestroyCommandPool(m_device, computePool, nullptr);
        }
        if (transferPool && transferPool != graphicsPool && transferPool != computePool)
        {
            vkDestroyCommandPool(m_device, transferPool, nullptr);
        }

        m_graphicsCommandPool = VK_NULL_HANDLE;
        m_computeCommandPool = VK_NULL_HANDLE;
        m_transferCommandPool = VK_NULL_HANDLE;

        // Save and destroy pipeline cache
        SavePipelineCache();
        if (m_pipelineCache)
        {
            vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
            m_pipelineCache = VK_NULL_HANDLE;
        }

        if (m_allocator)
            vmaDestroyAllocator(m_allocator);

        if (m_device)
            vkDestroyDevice(m_device, nullptr);

        if (m_debugMessenger)
            DestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);

        if (m_instance)
            vkDestroyInstance(m_instance, nullptr);

        RVX_RHI_INFO("Vulkan Device shutdown complete");
    }

    // =============================================================================
    // Instance Creation
    // =============================================================================
    bool VulkanDevice::CreateInstance(bool enableValidation)
    {
        // Check validation layer support
        if (enableValidation)
        {
            uint32 layerCount;
            vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            std::vector<VkLayerProperties> availableLayers(layerCount);
            vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

            for (const char* layerName : s_validationLayers)
            {
                bool layerFound = false;
                for (const auto& layerProps : availableLayers)
                {
                    if (strcmp(layerName, layerProps.layerName) == 0)
                    {
                        layerFound = true;
                        break;
                    }
                }
                if (!layerFound)
                {
                    RVX_RHI_WARN("Validation layer {} not available", layerName);
                    enableValidation = false;
                    break;
                }
            }
        }

        VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        appInfo.pApplicationName = "RenderVerseX";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "RenderVerseX";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3;

        std::vector<const char*> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
        };

        if (enableValidation)
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        VkInstanceCreateInfo createInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        if (enableValidation)
        {
            createInfo.enabledLayerCount = static_cast<uint32>(s_validationLayers.size());
            createInfo.ppEnabledLayerNames = s_validationLayers.data();

            debugCreateInfo.messageSeverity = 
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugCreateInfo.messageType = 
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugCreateInfo.pfnUserCallback = DebugCallback;

            createInfo.pNext = &debugCreateInfo;
        }

        VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create Vulkan instance: {}", VkResultToString(result));
            return false;
        }

        // Create debug messenger
        if (enableValidation)
        {
            result = CreateDebugUtilsMessengerEXT(m_instance, &debugCreateInfo, nullptr, &m_debugMessenger);
            if (result == VK_SUCCESS)
            {
                RVX_RHI_INFO("Vulkan validation layers enabled");
            }
        }

        return true;
    }

    // =============================================================================
    // Physical Device Selection
    // =============================================================================
    bool VulkanDevice::SelectPhysicalDevice()
    {
        uint32 deviceCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);

        if (deviceCount == 0)
        {
            RVX_RHI_ERROR("No Vulkan-capable GPUs found");
            return false;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

        // Score and select best device
        int bestScore = -1;
        VkPhysicalDevice bestDevice = VK_NULL_HANDLE;

        for (const auto& device : devices)
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);

            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(device, &memProps);

            // Calculate dedicated VRAM
            VkDeviceSize vram = 0;
            for (uint32 i = 0; i < memProps.memoryHeapCount; ++i)
            {
                if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                {
                    vram = std::max(vram, memProps.memoryHeaps[i].size);
                }
            }

            RVX_RHI_DEBUG("Found GPU {}: {} (VRAM: {} MB)",
                &device - devices.data(), props.deviceName, vram / (1024 * 1024));

            // Check required features
            QueueFamilyIndices indices = FindQueueFamilies(device);
            if (!indices.graphicsFamily.has_value())
                continue;

            if (!CheckDeviceExtensionSupport(device))
                continue;

            // Score device - prioritize discrete GPUs heavily
            // Discrete GPUs get 100000 base score to ensure they're always preferred
            // over integrated GPUs with inflated shared memory VRAM values
            int score = 0;
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                score += 100000;
            else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
                score += 100;

            // Add VRAM bonus (in MB, capped at 64GB to prevent overflow)
            VkDeviceSize vramMB = std::min(vram / (1024 * 1024), static_cast<VkDeviceSize>(65536));
            score += static_cast<int>(vramMB);

            if (score > bestScore)
            {
                bestScore = score;
                bestDevice = device;
            }
        }

        if (bestDevice == VK_NULL_HANDLE)
        {
            RVX_RHI_ERROR("No suitable GPU found");
            return false;
        }

        m_physicalDevice = bestDevice;
        m_queueFamilies = FindQueueFamilies(m_physicalDevice);

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        RVX_RHI_INFO("Selected GPU: {}", props.deviceName);

        return true;
    }

    QueueFamilyIndices VulkanDevice::FindQueueFamilies(VkPhysicalDevice device)
    {
        QueueFamilyIndices indices;

        uint32 queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        for (uint32 i = 0; i < queueFamilyCount; ++i)
        {
            const auto& family = queueFamilies[i];

            // Graphics queue
            if (family.queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                indices.graphicsFamily = i;
                // Graphics queue can also present
                indices.presentFamily = i;
            }

            // Dedicated compute queue
            if ((family.queueFlags & VK_QUEUE_COMPUTE_BIT) && !(family.queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                indices.computeFamily = i;
            }

            // Dedicated transfer queue
            if ((family.queueFlags & VK_QUEUE_TRANSFER_BIT) && 
                !(family.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                !(family.queueFlags & VK_QUEUE_COMPUTE_BIT))
            {
                indices.transferFamily = i;
            }
        }

        // Fallback: use graphics queue for compute/transfer if no dedicated queue
        if (!indices.computeFamily.has_value() && indices.graphicsFamily.has_value())
        {
            indices.computeFamily = indices.graphicsFamily;
        }
        if (!indices.transferFamily.has_value() && indices.graphicsFamily.has_value())
        {
            indices.transferFamily = indices.graphicsFamily;
        }

        return indices;
    }

    bool VulkanDevice::CheckDeviceExtensionSupport(VkPhysicalDevice device)
    {
        uint32 extensionCount;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

        std::set<std::string> requiredExtensions(s_deviceExtensions.begin(), s_deviceExtensions.end());

        for (const auto& extension : availableExtensions)
        {
            requiredExtensions.erase(extension.extensionName);
        }

        return requiredExtensions.empty();
    }

    // =============================================================================
    // Logical Device Creation
    // =============================================================================
    bool VulkanDevice::CreateLogicalDevice()
    {
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::set<uint32> uniqueQueueFamilies = {
            m_queueFamilies.graphicsFamily.value()
        };

        if (m_queueFamilies.computeFamily.has_value())
            uniqueQueueFamilies.insert(m_queueFamilies.computeFamily.value());
        if (m_queueFamilies.transferFamily.has_value())
            uniqueQueueFamilies.insert(m_queueFamilies.transferFamily.value());

        float queuePriority = 1.0f;
        for (uint32 queueFamily : uniqueQueueFamilies)
        {
            VkDeviceQueueCreateInfo queueCreateInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        // Enable device features
        VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan12Features vulkan12Features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features vulkan13Features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        
        features2.pNext = &vulkan12Features;
        vulkan12Features.pNext = &vulkan13Features;

        vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features2);

        // Enable specific features
        features2.features.samplerAnisotropy = VK_TRUE;
        features2.features.fillModeNonSolid = VK_TRUE;
        features2.features.multiDrawIndirect = VK_TRUE;
        
        vulkan12Features.descriptorIndexing = VK_TRUE;
        vulkan12Features.descriptorBindingPartiallyBound = VK_TRUE;
        vulkan12Features.runtimeDescriptorArray = VK_TRUE;
        vulkan12Features.timelineSemaphore = VK_TRUE;
        vulkan12Features.bufferDeviceAddress = VK_TRUE;

        vulkan13Features.dynamicRendering = VK_TRUE;
        vulkan13Features.synchronization2 = VK_TRUE;

        VkDeviceCreateInfo createInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        createInfo.pNext = &features2;
        createInfo.queueCreateInfoCount = static_cast<uint32>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.enabledExtensionCount = static_cast<uint32>(s_deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = s_deviceExtensions.data();

        if (m_validationEnabled)
        {
            createInfo.enabledLayerCount = static_cast<uint32>(s_validationLayers.size());
            createInfo.ppEnabledLayerNames = s_validationLayers.data();
        }

        VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create logical device: {}", VkResultToString(result));
            return false;
        }

        // Get queues
        vkGetDeviceQueue(m_device, m_queueFamilies.graphicsFamily.value(), 0, &m_graphicsQueue);
        if (m_queueFamilies.computeFamily.has_value())
            vkGetDeviceQueue(m_device, m_queueFamilies.computeFamily.value(), 0, &m_computeQueue);
        else
            m_computeQueue = m_graphicsQueue;
        if (m_queueFamilies.transferFamily.has_value())
            vkGetDeviceQueue(m_device, m_queueFamilies.transferFamily.value(), 0, &m_transferQueue);
        else
            m_transferQueue = m_graphicsQueue;

        RVX_RHI_DEBUG("Command queues created (Graphics, Compute, Transfer)");
        return true;
    }

    // =============================================================================
    // VMA Allocator
    // =============================================================================
    bool VulkanDevice::CreateAllocator()
    {
        // Set up Vulkan function pointers for VMA
        VmaVulkanFunctions vulkanFunctions = {};
        vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        vulkanFunctions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
        vulkanFunctions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
        vulkanFunctions.vkAllocateMemory = vkAllocateMemory;
        vulkanFunctions.vkFreeMemory = vkFreeMemory;
        vulkanFunctions.vkMapMemory = vkMapMemory;
        vulkanFunctions.vkUnmapMemory = vkUnmapMemory;
        vulkanFunctions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
        vulkanFunctions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
        vulkanFunctions.vkBindBufferMemory = vkBindBufferMemory;
        vulkanFunctions.vkBindImageMemory = vkBindImageMemory;
        vulkanFunctions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
        vulkanFunctions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
        vulkanFunctions.vkCreateBuffer = vkCreateBuffer;
        vulkanFunctions.vkDestroyBuffer = vkDestroyBuffer;
        vulkanFunctions.vkCreateImage = vkCreateImage;
        vulkanFunctions.vkDestroyImage = vkDestroyImage;
        vulkanFunctions.vkCmdCopyBuffer = vkCmdCopyBuffer;
        // Vulkan 1.1+ functions
        vulkanFunctions.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2;
        vulkanFunctions.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2;
        vulkanFunctions.vkBindBufferMemory2KHR = vkBindBufferMemory2;
        vulkanFunctions.vkBindImageMemory2KHR = vkBindImageMemory2;
        vulkanFunctions.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2;
        // Vulkan 1.3 functions
        vulkanFunctions.vkGetDeviceBufferMemoryRequirements = vkGetDeviceBufferMemoryRequirements;
        vulkanFunctions.vkGetDeviceImageMemoryRequirements = vkGetDeviceImageMemoryRequirements;

        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        allocatorInfo.physicalDevice = m_physicalDevice;
        allocatorInfo.device = m_device;
        allocatorInfo.instance = m_instance;
        allocatorInfo.pVulkanFunctions = &vulkanFunctions;
        
        // Enable buffer device address for bindless/raytracing
        allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        
        // Enable memory budget extension for better memory tracking (if available)
        allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;

        VkResult result = vmaCreateAllocator(&allocatorInfo, &m_allocator);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create VMA allocator: {}", VkResultToString(result));
            return false;
        }

        RVX_RHI_INFO("Vulkan Memory Allocator initialized");
        return true;
    }

    // =============================================================================
    // Command Pools
    // =============================================================================
    bool VulkanDevice::CreateCommandPools()
    {
        VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        poolInfo.queueFamilyIndex = m_queueFamilies.graphicsFamily.value();
        VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_graphicsCommandPool));

        if (m_queueFamilies.computeFamily.has_value() && 
            m_queueFamilies.computeFamily.value() != m_queueFamilies.graphicsFamily.value())
        {
            poolInfo.queueFamilyIndex = m_queueFamilies.computeFamily.value();
            VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_computeCommandPool));
        }
        else
        {
            m_computeCommandPool = m_graphicsCommandPool;
        }

        if (m_queueFamilies.transferFamily.has_value() &&
            m_queueFamilies.transferFamily.value() != m_queueFamilies.graphicsFamily.value())
        {
            poolInfo.queueFamilyIndex = m_queueFamilies.transferFamily.value();
            VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_transferCommandPool));
        }
        else
        {
            m_transferCommandPool = m_graphicsCommandPool;
        }

        return true;
    }

    VkCommandPool VulkanDevice::GetCommandPool(RHICommandQueueType type)
    {
        switch (type)
        {
            case RHICommandQueueType::Graphics: return m_graphicsCommandPool;
            case RHICommandQueueType::Compute:  return m_computeCommandPool;
            case RHICommandQueueType::Copy:     return m_transferCommandPool;
            default: return m_graphicsCommandPool;
        }
    }

    // =============================================================================
    // Descriptor Pool
    // =============================================================================
    bool VulkanDevice::CreateDescriptorPool()
    {
        std::vector<VkDescriptorPoolSize> poolSizes = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 10000},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10000},
        };

        VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 10000;
        poolInfo.poolSizeCount = static_cast<uint32>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        VK_CHECK(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool));
        return true;
    }

    // =============================================================================
    // Pipeline Cache
    // =============================================================================
    static const char* s_pipelineCacheFilename = "vulkan_pipeline_cache.bin";

    bool VulkanDevice::CreatePipelineCache()
    {
        VkPipelineCacheCreateInfo cacheInfo = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        cacheInfo.initialDataSize = 0;
        cacheInfo.pInitialData = nullptr;

        // Try to load existing cache from disk
        std::vector<uint8> cacheData;
        FILE* cacheFile = fopen(s_pipelineCacheFilename, "rb");
        if (cacheFile)
        {
            fseek(cacheFile, 0, SEEK_END);
            size_t fileSize = ftell(cacheFile);
            fseek(cacheFile, 0, SEEK_SET);

            if (fileSize > 0)
            {
                cacheData.resize(fileSize);
                size_t readSize = fread(cacheData.data(), 1, fileSize, cacheFile);
                if (readSize == fileSize)
                {
                    // Validate cache header
                    if (fileSize >= sizeof(uint32) * 4)
                    {
                        const uint32* header = reinterpret_cast<const uint32*>(cacheData.data());
                        // Check header length and header version
                        if (header[0] >= 16 && header[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
                        {
                            cacheInfo.initialDataSize = fileSize;
                            cacheInfo.pInitialData = cacheData.data();
                            RVX_RHI_INFO("Loaded pipeline cache: {} bytes", fileSize);
                        }
                        else
                        {
                            RVX_RHI_WARN("Pipeline cache header invalid, creating new cache");
                        }
                    }
                }
            }
            fclose(cacheFile);
        }

        VkResult result = vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_pipelineCache);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create pipeline cache: {}", VkResultToString(result));
            return false;
        }

        RVX_RHI_DEBUG("Pipeline cache created successfully");
        return true;
    }

    void VulkanDevice::SavePipelineCache()
    {
        if (!m_pipelineCache)
            return;

        size_t cacheSize = 0;
        VkResult result = vkGetPipelineCacheData(m_device, m_pipelineCache, &cacheSize, nullptr);
        if (result != VK_SUCCESS || cacheSize == 0)
        {
            RVX_RHI_DEBUG("No pipeline cache data to save");
            return;
        }

        std::vector<uint8> cacheData(cacheSize);
        result = vkGetPipelineCacheData(m_device, m_pipelineCache, &cacheSize, cacheData.data());
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to get pipeline cache data: {}", VkResultToString(result));
            return;
        }

        FILE* cacheFile = fopen(s_pipelineCacheFilename, "wb");
        if (cacheFile)
        {
            fwrite(cacheData.data(), 1, cacheSize, cacheFile);
            fclose(cacheFile);
            RVX_RHI_INFO("Saved pipeline cache: {} bytes", cacheSize);
        }
        else
        {
            RVX_RHI_WARN("Failed to save pipeline cache to file");
        }
    }

    // =============================================================================
    // Capabilities Query
    // =============================================================================
    // =============================================================================
    // Debug Utils Loading
    // =============================================================================
    void VulkanDevice::LoadDebugUtilsFunctions()
    {
        if (!m_validationEnabled || !m_device)
            return;

        vkCmdBeginDebugUtilsLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(m_device, "vkCmdBeginDebugUtilsLabelEXT"));
        vkCmdEndDebugUtilsLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(m_device, "vkCmdEndDebugUtilsLabelEXT"));
        vkCmdInsertDebugUtilsLabel = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(m_device, "vkCmdInsertDebugUtilsLabelEXT"));
        vkSetDebugUtilsObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetDeviceProcAddr(m_device, "vkSetDebugUtilsObjectNameEXT"));

        if (vkCmdBeginDebugUtilsLabel)
        {
            RVX_RHI_DEBUG("Debug Utils functions loaded successfully");
        }
    }

    // =============================================================================
    // Capabilities Query
    // =============================================================================
    void VulkanDevice::QueryDeviceCapabilities()
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);

        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

        // Basic info
        m_capabilities.backendType = RHIBackendType::Vulkan;
        m_capabilities.adapterName = props.deviceName;

        // Calculate VRAM
        for (uint32 i = 0; i < memProps.memoryHeapCount; ++i)
        {
            if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            {
                m_capabilities.dedicatedVideoMemory = std::max(
                    m_capabilities.dedicatedVideoMemory,
                    static_cast<uint64>(memProps.memoryHeaps[i].size));
            }
        }

        // Query available device extensions for capability detection
        uint32 extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extensionCount, availableExtensions.data());

        auto hasExtension = [&availableExtensions](const char* extensionName) {
            for (const auto& ext : availableExtensions)
            {
                if (strcmp(ext.extensionName, extensionName) == 0)
                    return true;
            }
            return false;
        };

        // Feature support
        VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan12Features vulkan12Features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features2.pNext = &vulkan12Features;
        vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features2);

        m_capabilities.supportsBindless = vulkan12Features.descriptorIndexing;

        // Check for raytracing support (VK_KHR_ray_tracing_pipeline + VK_KHR_acceleration_structure)
        m_capabilities.supportsRaytracing = 
            hasExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
            hasExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
            hasExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

        // Check for mesh shader support (VK_EXT_mesh_shader)
        m_capabilities.supportsMeshShaders = hasExtension(VK_EXT_MESH_SHADER_EXTENSION_NAME);

        // Check for variable rate shading support (VK_KHR_fragment_shading_rate)
        m_capabilities.supportsVariableRateShading = hasExtension(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);

        RVX_RHI_DEBUG("Vulkan Capabilities: Raytracing={}, MeshShaders={}, VRS={}", 
            m_capabilities.supportsRaytracing, 
            m_capabilities.supportsMeshShaders, 
            m_capabilities.supportsVariableRateShading);

        // Limits
        m_capabilities.maxTextureSize = props.limits.maxImageDimension2D;
        m_capabilities.maxTextureSize2D = props.limits.maxImageDimension2D;
        m_capabilities.maxTextureSize3D = props.limits.maxImageDimension3D;
        m_capabilities.maxTextureSizeCube = props.limits.maxImageDimensionCube;
        m_capabilities.maxTextureArrayLayers = props.limits.maxImageArrayLayers;
        m_capabilities.maxTextureLayers = props.limits.maxImageArrayLayers;
        m_capabilities.maxColorAttachments = props.limits.maxColorAttachments;
        m_capabilities.maxComputeWorkGroupSize[0] = props.limits.maxComputeWorkGroupSize[0];
        m_capabilities.maxComputeWorkGroupSize[1] = props.limits.maxComputeWorkGroupSize[1];
        m_capabilities.maxComputeWorkGroupSize[2] = props.limits.maxComputeWorkGroupSize[2];
        m_capabilities.maxComputeWorkGroupSizeX = props.limits.maxComputeWorkGroupSize[0];
        m_capabilities.maxComputeWorkGroupSizeY = props.limits.maxComputeWorkGroupSize[1];
        m_capabilities.maxComputeWorkGroupSizeZ = props.limits.maxComputeWorkGroupSize[2];
        m_capabilities.maxPushConstantSize = props.limits.maxPushConstantsSize;

        // Vulkan-specific
        m_capabilities.vulkan.maxPushConstantSize = props.limits.maxPushConstantsSize;
        m_capabilities.vulkan.supportsDescriptorIndexing = vulkan12Features.descriptorIndexing;
        m_capabilities.vulkan.supportsBufferDeviceAddress = vulkan12Features.bufferDeviceAddress;
        m_capabilities.vulkan.apiVersion = props.apiVersion;
    }

    // =============================================================================
    // Frame Management
    // =============================================================================
    void VulkanDevice::BeginFrame()
    {
        // Wait for the frame's fence before reusing resources
        VK_CHECK(vkWaitForFences(m_device, 1, &m_frameFences[m_currentFrameIndex], VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(m_device, 1, &m_frameFences[m_currentFrameIndex]));
    }

    void VulkanDevice::EndFrame()
    {
        m_currentFrameIndex = (m_currentFrameIndex + 1) % RVX_MAX_FRAME_COUNT;
    }

    VkSemaphore VulkanDevice::GetRenderFinishedSemaphore() const
    {
        if (m_primarySwapChain)
        {
            VkSemaphore semaphore = m_primarySwapChain->GetCurrentRenderFinishedSemaphore();
            if (semaphore != VK_NULL_HANDLE)
            {
                return semaphore;
            }
        }
        return m_renderFinishedSemaphores[m_currentFrameIndex];
    }

    void VulkanDevice::WaitIdle()
    {
        if (m_device)
        {
            vkDeviceWaitIdle(m_device);
        }
    }

    // =============================================================================
    // Resource Creation (Forward declarations - implemented in VulkanResources.cpp)
    // =============================================================================
    RHIBufferRef VulkanDevice::CreateBuffer(const RHIBufferDesc& desc)
    {
        return CreateVulkanBuffer(this, desc);
    }

    RHITextureRef VulkanDevice::CreateTexture(const RHITextureDesc& desc)
    {
        return CreateVulkanTexture(this, desc);
    }

    RHITextureViewRef VulkanDevice::CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc)
    {
        return CreateVulkanTextureView(this, texture, desc);
    }

    RHISamplerRef VulkanDevice::CreateSampler(const RHISamplerDesc& desc)
    {
        return CreateVulkanSampler(this, desc);
    }

    RHIShaderRef VulkanDevice::CreateShader(const RHIShaderDesc& desc)
    {
        return CreateVulkanShader(this, desc);
    }

    // =============================================================================
    // Memory Heap Management
    // =============================================================================
    RHIHeapRef VulkanDevice::CreateHeap(const RHIHeapDesc& desc)
    {
        return CreateVulkanHeap(this, desc);
    }

    RHITextureRef VulkanDevice::CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc)
    {
        return CreateVulkanPlacedTexture(this, heap, offset, desc);
    }

    RHIBufferRef VulkanDevice::CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc)
    {
        return CreateVulkanPlacedBuffer(this, heap, offset, desc);
    }

    IRHIDevice::MemoryRequirements VulkanDevice::GetTextureMemoryRequirements(const RHITextureDesc& desc)
    {
        // Create a temporary image to query memory requirements
        VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        
        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
                imageInfo.imageType = VK_IMAGE_TYPE_1D;
                break;
            case RHITextureDimension::Texture2D:
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                break;
            case RHITextureDimension::Texture3D:
                imageInfo.imageType = VK_IMAGE_TYPE_3D;
                break;
            case RHITextureDimension::TextureCube:
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                break;
        }

        imageInfo.extent.width = desc.width;
        imageInfo.extent.height = desc.height;
        imageInfo.extent.depth = desc.depth;
        imageInfo.mipLevels = desc.mipLevels;
        imageInfo.arrayLayers = desc.arraySize;
        imageInfo.format = ToVkFormat(desc.format);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);

        // Usage flags
        imageInfo.usage = 0;
        if (HasFlag(desc.usage, RHITextureUsage::ShaderResource))
            imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::UnorderedAccess))
            imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::RenderTarget))
            imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (HasFlag(desc.usage, RHITextureUsage::DepthStencil))
            imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.flags |= VK_IMAGE_CREATE_ALIAS_BIT;

        VkImage tempImage = VK_NULL_HANDLE;
        VkResult result = vkCreateImage(m_device, &imageInfo, nullptr, &tempImage);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create temp image for memory requirements query");
            return {0, 65536};  // Fallback
        }

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, tempImage, &memReqs);
        
        vkDestroyImage(m_device, tempImage, nullptr);

        return {memReqs.size, memReqs.alignment};
    }

    IRHIDevice::MemoryRequirements VulkanDevice::GetBufferMemoryRequirements(const RHIBufferDesc& desc)
    {
        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = desc.size;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // Usage flags
        bufferInfo.usage = 0;
        if (HasFlag(desc.usage, RHIBufferUsage::Vertex))
            bufferInfo.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (HasFlag(desc.usage, RHIBufferUsage::Index))
            bufferInfo.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (HasFlag(desc.usage, RHIBufferUsage::Constant))
            bufferInfo.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (HasFlag(desc.usage, RHIBufferUsage::ShaderResource))
            bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (HasFlag(desc.usage, RHIBufferUsage::UnorderedAccess))
            bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VkBuffer tempBuffer = VK_NULL_HANDLE;
        VkResult result = vkCreateBuffer(m_device, &bufferInfo, nullptr, &tempBuffer);
        if (result != VK_SUCCESS)
        {
            RVX_RHI_ERROR("Failed to create temp buffer for memory requirements query");
            return {desc.size, 256};  // Fallback
        }

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(m_device, tempBuffer, &memReqs);
        
        vkDestroyBuffer(m_device, tempBuffer, nullptr);

        return {memReqs.size, memReqs.alignment};
    }

    RHIDescriptorSetLayoutRef VulkanDevice::CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc)
    {
        return CreateVulkanDescriptorSetLayout(this, desc);
    }

    RHIPipelineLayoutRef VulkanDevice::CreatePipelineLayout(const RHIPipelineLayoutDesc& desc)
    {
        return CreateVulkanPipelineLayout(this, desc);
    }

    RHIPipelineRef VulkanDevice::CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc)
    {
        return CreateVulkanGraphicsPipeline(this, desc);
    }

    RHIPipelineRef VulkanDevice::CreateComputePipeline(const RHIComputePipelineDesc& desc)
    {
        return CreateVulkanComputePipeline(this, desc);
    }

    RHIDescriptorSetRef VulkanDevice::CreateDescriptorSet(const RHIDescriptorSetDesc& desc)
    {
        return CreateVulkanDescriptorSet(this, desc);
    }

    RHIQueryPoolRef VulkanDevice::CreateQueryPool(const RHIQueryPoolDesc& /*desc*/)
    {
        // TODO: Implement Vulkan query pool support
        RVX_RHI_WARN("Vulkan query pools not yet implemented");
        return nullptr;
    }

    RHICommandContextRef VulkanDevice::CreateCommandContext(RHICommandQueueType type)
    {
        return CreateVulkanCommandContext(this, type);
    }

    void VulkanDevice::SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence)
    {
        SubmitVulkanCommandContext(this, context, signalFence);
    }

    void VulkanDevice::SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence)
    {
        SubmitVulkanCommandContexts(this, contexts, signalFence);
    }

    RHISwapChainRef VulkanDevice::CreateSwapChain(const RHISwapChainDesc& desc)
    {
        auto swapChain = CreateVulkanSwapChain(this, desc);
        m_primarySwapChain = static_cast<VulkanSwapChain*>(swapChain.Get());
        return swapChain;
    }

    RHIFenceRef VulkanDevice::CreateFence(uint64 initialValue)
    {
        return CreateVulkanFence(this, initialValue);
    }

    void VulkanDevice::WaitForFence(RHIFence* fence, uint64 value)
    {
        WaitForVulkanFence(this, fence, value);
    }

    // =============================================================================
    // Factory Function
    // =============================================================================
    std::unique_ptr<IRHIDevice> CreateVulkanDevice(const RHIDeviceDesc& desc)
    {
        RVX_RHI_INFO("Creating RHI Device with backend: Vulkan");
        auto device = std::make_unique<VulkanDevice>(desc);
        
        if (!device->GetDevice())
        {
            return nullptr;
        }
        
        return device;
    }

} // namespace RVX
