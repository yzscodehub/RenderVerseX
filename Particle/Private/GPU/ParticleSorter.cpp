#include "Particle/GPU/ParticleSorter.h"
#include "Core/Log.h"

namespace RVX::Particle
{

ParticleSorter::~ParticleSorter()
{
    Shutdown();
}

void ParticleSorter::Initialize(IRHIDevice* device, uint32 maxParticles)
{
    if (m_device)
        Shutdown();

    m_device = device;
    m_maxParticles = maxParticles;

    // Sort key buffers (double-buffered for ping-pong)
    RHIBufferDesc keyDesc;
    keyDesc.size = sizeof(ParticleSortKey) * maxParticles;
    keyDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess | RHIBufferUsage::ShaderResource;
    keyDesc.stride = sizeof(ParticleSortKey);
    keyDesc.debugName = "SortKeyBuffer";
    m_sortKeyBuffer = m_device->CreateBuffer(keyDesc);
    
    keyDesc.debugName = "SortKeyBufferBack";
    m_sortKeyBufferBack = m_device->CreateBuffer(keyDesc);

    // Sort constants
    RHIBufferDesc constDesc;
    constDesc.size = 64; // Enough for sort parameters
    constDesc.usage = RHIBufferUsage::Constant;
    constDesc.memoryType = RHIMemoryType::Upload;
    constDesc.debugName = "SortConstantsBuffer";
    m_sortConstantsBuffer = m_device->CreateBuffer(constDesc);

    RVX_CORE_INFO("ParticleSorter: Initialized for {} particles", maxParticles);
}

void ParticleSorter::Shutdown()
{
    m_sortKeyBuffer.Reset();
    m_sortKeyBufferBack.Reset();
    m_sortConstantsBuffer.Reset();
    m_device = nullptr;
    m_maxParticles = 0;
}

void ParticleSorter::Sort(RHICommandContext& ctx,
                          RHIBuffer* particleBuffer,
                          RHIBuffer* indexBuffer,
                          uint32 particleCount,
                          const Vec3& cameraPosition)
{
    if (particleCount == 0)
        return;

    // Step 1: Generate sort keys (distance from camera)
    GenerateSortKeys(ctx, particleBuffer, particleCount, cameraPosition);

    // Step 2: Bitonic sort
    BitonicSort(ctx, particleCount);

    // Step 3: Extract sorted indices
    ScatterToOutput(ctx, indexBuffer, particleCount);
}

void ParticleSorter::GenerateSortKeys(RHICommandContext& ctx,
                                      RHIBuffer* particleBuffer,
                                      uint32 particleCount,
                                      const Vec3& cameraPosition)
{
    if (!m_keyGenPipeline)
        return;

    // Upload camera position
    struct KeyGenConstants
    {
        Vec4 cameraPos;
        uint32 particleCount;
        uint32 pad[3];
    } constants;
    
    constants.cameraPos = Vec4(cameraPosition, 0.0f);
    constants.particleCount = particleCount;
    m_sortConstantsBuffer->Upload(&constants, 1);

    // Dispatch key generation
    ctx.SetPipeline(m_keyGenPipeline);
    // ctx.BindBuffer(0, particleBuffer);
    // ctx.BindBuffer(1, m_sortKeyBuffer.Get());
    // ctx.BindConstantBuffer(0, m_sortConstantsBuffer.Get());

    uint32 groups = (particleCount + 255) / 256;
    ctx.Dispatch(groups, 1, 1);
}

void ParticleSorter::BitonicSort(RHICommandContext& ctx, uint32 count)
{
    if (!m_bitonicSortPipeline || !m_bitonicMergePipeline)
        return;

    // Round up to next power of 2
    uint32 n = 1;
    while (n < count) n *= 2;

    // Bitonic sort phases
    for (uint32 k = 2; k <= n; k *= 2)
    {
        // Bitonic merge at level k
        for (uint32 j = k / 2; j > 0; j /= 2)
        {
            struct SortPassConstants
            {
                uint32 k;
                uint32 j;
                uint32 count;
                uint32 pad;
            } constants;
            
            constants.k = k;
            constants.j = j;
            constants.count = count;
            m_sortConstantsBuffer->Upload(&constants, 1);

            ctx.SetPipeline(j == k / 2 ? m_bitonicSortPipeline : m_bitonicMergePipeline);
            
            uint32 groups = (count / 2 + 255) / 256;
            ctx.Dispatch(groups, 1, 1);

            // Barrier between passes
            // ctx.BufferBarrier(m_sortKeyBuffer.Get(), ...);
        }
    }
}

void ParticleSorter::ScatterToOutput(RHICommandContext& ctx, 
                                     RHIBuffer* indexBuffer, 
                                     uint32 count)
{
    // Extract indices from sorted keys
    // This could be done with a simple copy shader or during the final sort pass
    (void)ctx;
    (void)indexBuffer;
    (void)count;
}

} // namespace RVX::Particle
