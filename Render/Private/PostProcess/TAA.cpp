/**
 * @file TAA.cpp
 * @brief TAA implementation
 */

#include "Render/PostProcess/TAA.h"
#include <cmath>

namespace RVX
{

TAA::~TAA()
{
    Shutdown();
}

void TAA::Initialize(IRHIDevice* device, uint32 width, uint32 height)
{
    m_device = device;
    m_width = width;
    m_height = height;

    CreateResources(width, height);
}

void TAA::Shutdown()
{
    m_history[0].Reset();
    m_history[1].Reset();
    m_result.Reset();
    m_taaPipeline.Reset();
    m_sharpenPipeline.Reset();
    m_copyPipeline.Reset();
    m_constantBuffer.Reset();
    m_device = nullptr;
}

void TAA::Resize(uint32 width, uint32 height)
{
    m_width = width;
    m_height = height;
    CreateResources(width, height);
    ResetHistory();
}

void TAA::SetConfig(const TAAConfig& config)
{
    m_config = config;
}

void TAA::CreateResources(uint32 width, uint32 height)
{
    if (!m_device) return;

    RHITextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = RHIFormat::RGBA16_FLOAT;
    desc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;

    // Double-buffered history
    m_history[0] = m_device->CreateTexture(desc);
    m_history[1] = m_device->CreateTexture(desc);

    // Output
    m_result = m_device->CreateTexture(desc);

    // Constant buffer
    RHIBufferDesc bufDesc;
    bufDesc.size = 256;
    bufDesc.usage = RHIBufferUsage::Constant;
    bufDesc.memoryType = RHIMemoryType::Upload;
    m_constantBuffer = m_device->CreateBuffer(bufDesc);
}

float TAA::Halton(int index, int base)
{
    float result = 0.0f;
    float f = 1.0f / base;
    int i = index;
    
    while (i > 0)
    {
        result += f * (i % base);
        i = i / base;
        f = f / base;
    }
    
    return result;
}

Vec2 TAA::HaltonSequence(int index)
{
    return Vec2(Halton(index + 1, 2), Halton(index + 1, 3));
}

Vec2 TAA::GetJitterOffset(uint64 frameIndex) const
{
    int phase = static_cast<int>(frameIndex % m_config.jitterPhase);
    Vec2 halton = HaltonSequence(phase);
    
    // Map from [0,1] to [-0.5, 0.5]
    return (halton - Vec2(0.5f)) * m_config.jitterScale;
}

Vec2 TAA::GetJitterOffsetPixels(uint64 frameIndex) const
{
    Vec2 offset = GetJitterOffset(frameIndex);
    return Vec2(offset.x * m_width, offset.y * m_height);
}

Mat4 TAA::JitterProjectionMatrix(const Mat4& projMatrix, uint64 frameIndex) const
{
    Vec2 offset = GetJitterOffset(frameIndex);
    
    // Convert to clip space offset
    Vec2 clipOffset(
        offset.x * 2.0f / m_width,
        offset.y * 2.0f / m_height
    );
    
    Mat4 jitteredProj = projMatrix;
    jitteredProj[2][0] += clipOffset.x;
    jitteredProj[2][1] += clipOffset.y;
    
    return jitteredProj;
}

void TAA::Resolve(RHICommandContext& ctx,
                  RHITexture* currentColor,
                  RHITexture* depthTexture,
                  RHITexture* motionVectors,
                  uint64 frameIndex)
{
    if (!m_enabled || !m_device) return;

    // On first frame or after reset, just copy input
    if (!m_historyValid)
    {
        // TODO: Copy currentColor to history[m_currentHistory]
        m_historyValid = true;
        SwapHistory();
        return;
    }

    // Update constants
    struct TAAConstants
    {
        Vec4 jitterOffset;
        Vec4 params;  // feedbackMin, feedbackMax, motionScale, velocityWeight
        Vec4 screenSize;
    } constants;

    Vec2 jitter = GetJitterOffset(frameIndex);
    constants.jitterOffset = Vec4(jitter.x, jitter.y, 0, 0);
    constants.params = Vec4(
        m_config.feedbackMin,
        m_config.feedbackMax,
        m_config.motionScale,
        m_config.velocityWeight
    );
    constants.screenSize = Vec4(
        static_cast<float>(m_width),
        static_cast<float>(m_height),
        1.0f / m_width,
        1.0f / m_height
    );

    m_constantBuffer->Upload(&constants, 1);

    // TODO: Dispatch TAA resolve shader
    // - Sample current color
    // - Reproject history using motion vectors
    // - Neighborhood clamping (min/max of 3x3 or plus pattern)
    // - Blend based on velocity and confidence
    // - Output to result

    // Apply sharpening if enabled
    if (m_config.sharpen)
    {
        // TODO: Apply CAS or similar sharpening
    }

    SwapHistory();
}

void TAA::ResetHistory()
{
    m_historyValid = false;
    m_currentHistory = 0;
}

void TAA::SwapHistory()
{
    m_currentHistory = 1 - m_currentHistory;
}

} // namespace RVX
