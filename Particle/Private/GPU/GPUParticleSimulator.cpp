#include "Particle/GPU/GPUParticleSimulator.h"
#include "Core/Log.h"

namespace RVX::Particle
{

GPUParticleSimulator::~GPUParticleSimulator()
{
    Shutdown();
}

void GPUParticleSimulator::Initialize(IRHIDevice* device, uint32 maxParticles)
{
    if (m_device)
        Shutdown();

    m_device = device;
    m_maxParticles = maxParticles;
    m_aliveCount = 0;

    CreateBuffers();
    InitializeDeadList();

    RVX_CORE_INFO("GPUParticleSimulator: Initialized with {} max particles", maxParticles);
}

void GPUParticleSimulator::Shutdown()
{
    m_particleBuffer.Reset();
    m_particleBufferBack.Reset();
    m_aliveIndexBuffer.Reset();
    m_aliveIndexBufferBack.Reset();
    m_deadIndexBuffer.Reset();
    m_counterBuffer.Reset();
    m_indirectDrawBuffer.Reset();
    m_emitConstantsBuffer.Reset();
    m_simulateConstantsBuffer.Reset();

    m_device = nullptr;
    m_maxParticles = 0;
    m_aliveCount = 0;
}

void GPUParticleSimulator::CreateBuffers()
{
    // Particle data buffers (double-buffered for read/write separation)
    RHIBufferDesc particleDesc;
    particleDesc.size = sizeof(GPUParticle) * m_maxParticles;
    particleDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess | RHIBufferUsage::ShaderResource;
    particleDesc.stride = sizeof(GPUParticle);
    particleDesc.debugName = "ParticleBuffer";

    m_particleBuffer = m_device->CreateBuffer(particleDesc);
    particleDesc.debugName = "ParticleBufferBack";
    m_particleBufferBack = m_device->CreateBuffer(particleDesc);

    // Alive index buffers (double-buffered)
    RHIBufferDesc indexDesc;
    indexDesc.size = sizeof(uint32) * m_maxParticles;
    indexDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess | RHIBufferUsage::ShaderResource;
    indexDesc.stride = sizeof(uint32);
    indexDesc.debugName = "AliveIndexBuffer";

    m_aliveIndexBuffer = m_device->CreateBuffer(indexDesc);
    indexDesc.debugName = "AliveIndexBufferBack";
    m_aliveIndexBufferBack = m_device->CreateBuffer(indexDesc);

    // Dead index buffer
    indexDesc.debugName = "DeadIndexBuffer";
    m_deadIndexBuffer = m_device->CreateBuffer(indexDesc);

    // Counter buffer (alive count, dead count, emit count)
    RHIBufferDesc counterDesc;
    counterDesc.size = sizeof(uint32) * 4;
    counterDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess | RHIBufferUsage::IndirectArgs;
    counterDesc.debugName = "CounterBuffer";
    m_counterBuffer = m_device->CreateBuffer(counterDesc);

    // Indirect draw arguments
    RHIBufferDesc indirectDesc;
    indirectDesc.size = sizeof(IndirectDrawArgs);
    indirectDesc.usage = RHIBufferUsage::IndirectArgs | RHIBufferUsage::UnorderedAccess;
    indirectDesc.debugName = "IndirectDrawBuffer";
    m_indirectDrawBuffer = m_device->CreateBuffer(indirectDesc);

    // Emit constants buffer
    RHIBufferDesc constDesc;
    constDesc.size = sizeof(EmitterGPUData) + 64; // Extra for alignment
    constDesc.usage = RHIBufferUsage::Constant;
    constDesc.memoryType = RHIMemoryType::Upload;
    constDesc.debugName = "EmitConstantsBuffer";
    m_emitConstantsBuffer = m_device->CreateBuffer(constDesc);

    // Simulate constants buffer
    constDesc.size = sizeof(SimulationGPUData) + 64;
    constDesc.debugName = "SimulateConstantsBuffer";
    m_simulateConstantsBuffer = m_device->CreateBuffer(constDesc);
}

void GPUParticleSimulator::InitializeDeadList()
{
    // Initialize dead list with all indices (all particles are dead initially)
    // This would be done via a compute shader or initial upload
    
    // Set counters: alive=0, dead=maxParticles
    uint32 counters[4] = { 0, m_maxParticles, 0, 0 };
    m_counterBuffer->Upload(counters, 4);

    // Initialize indirect draw args (0 instances initially)
    IndirectDrawArgs args = {};
    args.indexCountPerInstance = 6;  // 2 triangles for billboard quad
    args.instanceCount = 0;
    args.startIndexLocation = 0;
    args.baseVertexLocation = 0;
    args.startInstanceLocation = 0;
    m_indirectDrawBuffer->Upload(&args, 1);
}

void GPUParticleSimulator::Emit(const EmitParams& params)
{
    if (params.emitCount == 0)
        return;

    m_pendingEmit = params;
    m_hasPendingEmit = true;
}

void GPUParticleSimulator::Simulate(float deltaTime, const SimulateParams& params)
{
    m_simulateParams = params;
    m_simulateParams.deltaTime = deltaTime;
}

void GPUParticleSimulator::PrepareRender(RHICommandContext& ctx)
{
    // Execute GPU passes
    if (m_hasPendingEmit)
    {
        ExecuteEmitPass(ctx);
        m_hasPendingEmit = false;
    }

    ExecuteSimulatePass(ctx, m_simulateParams.deltaTime);
    ExecuteCompactPass(ctx);
}

void GPUParticleSimulator::ExecuteEmitPass(RHICommandContext& ctx)
{
    if (!m_emitPipeline)
        return;

    // Upload emit constants
    m_emitConstantsBuffer->Upload(&m_pendingEmit.emitterData, 1);

    // Bind pipeline and resources
    ctx.SetPipeline(m_emitPipeline);
    
    // Bind buffers (would use descriptor sets in real implementation)
    // ctx.BindBuffer(0, m_particleBuffer.Get());
    // ctx.BindBuffer(1, m_deadIndexBuffer.Get());
    // ctx.BindBuffer(2, m_aliveIndexBuffer.Get());
    // ctx.BindBuffer(3, m_counterBuffer.Get());
    // ctx.BindConstantBuffer(0, m_emitConstantsBuffer.Get());

    // Dispatch
    uint32 groupCount = (m_pendingEmit.emitCount + 63) / 64;
    ctx.Dispatch(groupCount, 1, 1);
}

void GPUParticleSimulator::ExecuteSimulatePass(RHICommandContext& ctx, float deltaTime)
{
    if (!m_simulatePipeline || m_aliveCount == 0)
        return;

    // Upload simulation constants
    m_simulateConstantsBuffer->Upload(&m_simulateParams.simulationData, 1);

    // Bind pipeline
    ctx.SetPipeline(m_simulatePipeline);

    // Dispatch
    uint32 groupCount = (m_aliveCount + 255) / 256;
    ctx.Dispatch(groupCount, 1, 1);
}

void GPUParticleSimulator::ExecuteCompactPass(RHICommandContext& ctx)
{
    if (!m_compactPipeline)
        return;

    // Bind pipeline
    ctx.SetPipeline(m_compactPipeline);

    // Dispatch
    uint32 groupCount = (m_aliveCount + 255) / 256;
    ctx.Dispatch(groupCount, 1, 1);

    // Swap buffers
    std::swap(m_particleBuffer, m_particleBufferBack);
    std::swap(m_aliveIndexBuffer, m_aliveIndexBufferBack);
}

void GPUParticleSimulator::Clear()
{
    m_aliveCount = 0;
    m_hasPendingEmit = false;
    InitializeDeadList();
}

} // namespace RVX::Particle
