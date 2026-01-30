---
name: OpenGL RHI Optimization
overview: Complete and optimize the OpenGL RHI backend implementation by fixing incomplete features, adding proper memory barriers, improving state caching, and implementing multi-bind optimizations for better performance.
todos:
  - id: phase1-barriers
    content: Implement proper memory barriers with RHIResourceState to GL barrier bit mapping
    status: completed
  - id: phase1-dynamic-ubo
    content: Add dynamic uniform buffer offset support in SetDescriptorSet and Bind
    status: completed
  - id: phase1-texture-copy
    content: Fix CopyTexture to use srcSubresource/dstSubresource for mip levels
    status: completed
  - id: phase1-deletion-queue
    content: Route all resource destructors through OpenGLDeletionQueue
    status: completed
  - id: phase2-storage-access
    content: Add storage texture access mode (read/write/read-write) support
    status: completed
  - id: phase2-multi-viewport
    content: Implement multi-viewport using glViewportArrayv
    status: completed
  - id: phase2-mrt-blend
    content: Apply blend states to all render targets, not just index 0
    status: completed
  - id: phase3-multi-bind
    content: Use glBindTextures/glBindSamplers for batch binding when available
    status: completed
  - id: phase3-vao-cache
    content: Add dirty tracking to avoid rebuilding VAO cache key every draw
    status: completed
  - id: phase3-stencil-cache
    content: Improve stencil state caching with per-parameter comparison
    status: completed
  - id: phase3-push-constant
    content: Add dirty flag for push constant buffer binding
    status: completed
  - id: phase3-state-cache
    content: Add caching for line width, polygon offset, sample shading
    status: completed
---

# OpenGL RHI Implementation Improvement Plan

## Current State Analysis

The OpenGL RHI backend is functional but has several incomplete implementations and performance optimization opportunities. This plan addresses critical issues first, then optimizations.

---

## Phase 1: Critical Bug Fixes

### 1.1 Implement Proper Memory Barriers

**File**: [RHI_OpenGL/Private/OpenGLCommandContext.cpp](RHI_OpenGL/Private/OpenGLCommandContext.cpp)

Current implementation ignores resource states. Need to map `RHIResourceState` to appropriate `glMemoryBarrier` bits:

```cpp
void OpenGLCommandContext::Barriers(...)
{
    GLbitfield barrierBits = 0;
    
    // Map buffer states
    for (const auto& barrier : bufferBarriers)
    {
        if (IsUAVState(barrier.stateAfter) || IsUAVState(barrier.stateBefore))
            barrierBits |= GL_SHADER_STORAGE_BARRIER_BIT;
        if (barrier.stateAfter == RHIResourceState::VertexBuffer)
            barrierBits |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT;
        // ... more mappings
    }
    
    if (barrierBits) glMemoryBarrier(barrierBits);
}
```

### 1.2 Fix Dynamic Uniform Buffer Offsets

**File**: [RHI_OpenGL/Private/OpenGLCommandContext.cpp](RHI_OpenGL/Private/OpenGLCommandContext.cpp)

- Add `std::vector<uint32> dynamicOffsets` storage