---
name: RHI Design
overview: This plan updates the to-do list in the main design document to accurately reflect the module-by-module design progress. It marks the finalization of the module structure and the detailed design of Module 1 as complete, and sets up the design tasks for the remaining five core modules as the next steps.
todos:
  - id: todo-0
    content: Define common RHI enums and structs.
    status: pending
  - id: todo-1
    content: Design core RHI interface classes.
    status: pending
  - id: todo-2
    content: Declare DX12 backend implementation classes.
    status: pending
  - id: todo-3
    content: Declare Metal backend implementation classes.
    status: pending
---

<!-- 3cfdb344-c526-4e42-bc94-a4526fcaa72f 28e9ab3e-168f-446f-8935-9f5bf5d2af51 -->

# RHI Final Design & Implementation Plan

This plan solidifies the comprehensive, Vulkan-inspired RHI architecture. It is organized into six core modules, which form the foundational hardware abstraction layer, and five higher-level engine modules that build upon this foundation. This document serves as the final design blueprint before C++ header file generation.

---

## Core RHI Modules

### Module 1: Core Infrastructure

**Purpose**: The entry point for the RHI. Responsible for discovering physical hardware, creating a logical device to work with, and serving as the root factory for all other RHI objects.

**Interfaces**:

-   `RHIInstance`: The global RHI context.
    -   `EnumerateAdapters()`: Lists available physical GPUs (`RHIAdapter`).
    -   `CreateDevice()`: Creates a logical device (`RHIDevice`) from an adapter.
-   `RHIAdapter`: Represents a physical GPU.
    -   `GetProperties()`: Queries the adapter's capabilities.
-   `RHIDevice`: Represents a logical connection to the GPU and is the factory for all other RHI objects.
    -   `GetQueue()`: Retrieves a command queue for work submission.
    -   `WaitIdle()`: Stalls the CPU until all GPU work is complete.

**Core Structs**:

-   `RHIAdapterProperties`: A structure filled by `GetProperties` to report adapter capabilities.
    ```cpp
    enum class RHIAdapterType { Discrete, Integrated, CPU, Unknown };
    
    struct RHIAdapterProperties {
        std::string deviceName;
        uint32_t vendorID;
        uint32_t deviceID;
        RHIAdapterType adapterType;
        uint64_t dedicatedVideoMemory;
        
        struct {
            bool raytracing = false;
            bool meshShaders = false;
        } features;
    };
    ```

-   `RHIDeviceDesc`: A detailed descriptor for creating the logical device.
    ```cpp
    struct RHIQueueCreateInfo {
        RHIQueueType queueType; // e.g., Graphics, Compute
        uint32_t queueCount;
        const float* pPriorities; // Relative priorities
    };
    
    struct RHIEnabledFeatures {
        bool raytracing = false;
        bool meshShaders = false;
    };
    
    struct RHIDeviceDesc {
        std::vector<RHIQueueCreateInfo> queueCreateInfos;
        RHIEnabledFeatures enabledFeatures;
        bool enableValidationLayer = false;
    };
    ```


### Module 2: Command Submission

**Purpose**: Manages command recording, execution, and synchronization.

-   **Interfaces**:
    -   `RHIQueue`: Executes command buffers and handles presentation. Methods: `Submit()`, `Present()`.
    -   `RHIFence`: For **CPU-GPU** synchronization.
    -   `RHISemaphore`: For **GPU-GPU** synchronization.
    -   `RHICommandPool`: Allocates memory for command buffers.
    -   `RHICommandBuffer`: Records GPU commands. Methods: `Begin()`, `End()`, `Cmd...()` functions for drawing, dispatching, copying, and setting state.
-   **Core Structs**:
    -   `RHISubmitInfo`: Describes a batch of work for submission.
    -   `RHIPresentInfo`: Describes a presentation operation.

### Module 3: Resources & Memory

**Purpose**: Defines GPU data entities like buffers and textures.

-   **Interfaces**:
    -   `RHIBuffer`, `RHITexture`: Represent GPU memory resources. Methods: `Map()`, `Unmap()`.
    -   `RHISampler`: Immutable object defining texture sampling parameters.
-   **Core Structs**:
    -   `RHIBufferDesc`, `RHITextureDesc`: Define resource properties, including `UsageFlags` and `MemoryProperty`.
    -   `RHISamplerDesc`: Defines sampler state.

### Module 4: Resource Views

**Purpose**: Defines how resources are interpreted by the pipeline.

-   **Interfaces**:
    -   `RHIResourceView` (Base), `RHIShaderResourceView` (SRV), `RHIUnorderedAccessView` (UAV), `RHIRenderTargetView` (RTV), `RHIDepthStencilView` (DSV).
-   **Core Structs**:
    -   `RHISRVDesc`, `RHIUAVDesc`, `RHIRTVDesc`, `RHIDSVDesc`: Used to create views from underlying resources.

### Module 5: Pipeline & Shaders

**Purpose**: Defines the GPU's processing state for rendering and compute tasks.

-   **Interfaces**:
    -   `RHIShader`: A compiled shader module.
    -   `RHIShaderBindingLayout` (SBL): Abstract representation of resource bindings (akin to `VkDescriptorSetLayout` or `D3D12_ROOT_SIGNATURE_DESC`).
    -   `RHIResourceBinder`: Binds actual resources to an SBL (akin to `VkDescriptorSet`).
    -   `RHIGraphicsPSO`, `RHIComputePSO`: Immutable pipeline state objects.
-   **Core Structs**:
    -   `RHIShaderDesc`: Describes a shader stage's bytecode and entry point.
    -   `RHIShaderBindingLayoutDesc`: Defines the structure of an SBL.
    -   `RHIGraphicsPSOCreateInfo`, `RHIComputePSOCreateInfo`: Complete descriptions for creating PSOs.

### Module 6: Render Flow

**Purpose**: Organizes rendering attachments and dependencies into logical passes.

-   **Interfaces**:
    -   `RHIRenderPass`: Defines attachments, subpasses, and dependencies to enable driver optimizations.
    -   `RHIFrameBuffer`: Binds concrete resource views to a `RHIRenderPass` definition.
    -   `RHISwapchain`: Manages a chain of presentable images for a window.
-   **Core Structs**:
    -   `RHIRenderPassDesc`: Defines the structure of a render pass.
    -   `RHIFrameBufferDesc`: Associates views with a render pass.
    -   `RHISwapchainDesc`: Defines swapchain properties.

---

## Higher-Level Engine Modules (Built upon RHI)

These modules are not part of the core RHI but are essential components of a modern rendering engine that utilize the RHI.

### Module 7: Render Graph

**Purpose**: To manage the complexity of a rendering frame by representing passes and their resource dependencies as a graph. It automatically handles resource allocation, memory aliasing, and inserts necessary synchronization barriers.

**Relationship to RHI**: It is a primary **client** of the RHI. The Render Graph processes the dependency graph to generate optimized sequences of commands using `RHICommandBuffer` and `RHIQueue`.

### Module 8: Resource Loading & Management

**Purpose**: To handle the loading of rendering assets from disk and their translation into GPU-ready resources. This includes loading texture files (PNG, DDS), 3D models (glTF, FBX), and compiling shader source code (HLSL/GLSL) into bytecode.

**Relationship to RHI**: This module calls RHI factory methods like `RHIDevice::CreateTexture`, `RHIDevice::CreateBuffer`, and `RHIDevice::CreateShader`.

### Module 9: Shader & Material System

**Purpose**: To abstract the management of shader permutations and material properties. It uses shader reflection to automatically generate `RHIShaderBindingLayout`s and provides a user-friendly interface to create material instances which bundle shaders with specific resources (textures, constants).

**Relationship to RHI**: This system simplifies the creation of `RHIShader`, `RHIShaderBindingLayout`, and `RHIResourceBinder` objects.

### Module 10: GPU Memory Allocator

**Purpose**: To manage GPU memory efficiently. Instead of creating a dedicated allocation for every small resource, this module requests large blocks of memory from the RHI and sub-allocates from them, reducing overhead and fragmentation.

**Relationship to RHI**: It wraps `RHIDevice::CreateBuffer` and `RHIDevice::CreateTexture`, providing a more sophisticated memory management layer on top.

### Module 11: Debug & Profiling

**Purpose**: To provide tools for debugging and performance analysis. This includes inserting debug markers into command lists, querying GPU timestamps to measure pass timings, and integrating with platform-specific tools like PIX, RenderDoc, or the Metal frame debugger.

**Relationship to RHI**: It requires RHI interfaces (likely on `RHICommandBuffer` and `RHIQueue`) to support debug labels and timestamp queries.

This finalized plan ensures a shared understanding of the entire RHI architecture and its surrounding ecosystem. Upon approval, I will proceed with generating the C++ header files for the core modules.

### Design & Implementation To-dos

- [x] Finalize Core & Engine Module Structure
- [x] Detailed Design: Module 1 (Core Infrastructure)
- [ ] Detailed Design: Module 2 (Command Submission)
- [ ] Detailed Design: Module 3 (Resources & Memory)
- [ ] Detailed Design: Module 4 (Resource Views)
- [ ] Detailed Design: Module 5 (Pipeline & Shaders)
- [ ] Detailed Design: Module 6 (Render Flow)
- [ ] Generate All RHI Header Files