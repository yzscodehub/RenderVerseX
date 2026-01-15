#include "VulkanDevice.h"
#include "VulkanResources.h"
#include "VulkanSwapChain.h"
#include "VulkanCommandContext.h"
#include "VulkanPipeline.h"

#include <set>
#include <algorithm>

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

            // Score device
            int score = 0;
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                score += 1000;
            else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
                score += 100;

            score += static_cast<int>(vram / (1024 * 1024));

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
        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        allocatorInfo.physicalDevice = m_physicalDevice;
        allocatorInfo.device = m_device;
        allocatorInfo.instance = m_instance;
        allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

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

        // Feature support
        VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan12Features vulkan12Features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features2.pNext = &vulkan12Features;
        vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features2);

        m_capabilities.supportsBindless = vulkan12Features.descriptorIndexing;
        m_capabilities.supportsRaytracing = false;  // Would need to check VK_KHR_ray_tracing_pipeline
        m_capabilities.supportsMeshShaders = false;  // Would need to check VK_EXT_mesh_shader
        m_capabilities.supportsVariableRateShading = false;

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
