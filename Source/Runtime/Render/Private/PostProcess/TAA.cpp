/**
 * @file TAA.cpp
 * @brief TAA implementation
 */

#include "Render/PostProcess/TAA.h"
#include "Core/Log.h"
#include <cmath>

namespace RVX
{

TAA::~TAA()
{
    Shutdown();
}

void TAA::Initialize(IRHIDevice* device, uint32 width, uint32 height)
{
    if (!device)
    {
        RVX_CORE_ERROR("TAA: Cannot initialize without an RHI device");
        m_unsupportedReason = "No RHI device";
        return;
    }

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
    m_supported = false;
    m_unsupportedReason = "TAA resolve resources are not initialized";
    m_lastResolveStats = {};
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
    if (m_config.jitterPhase <= 0)
    {
        m_config.jitterPhase = 1;
    }
}

void TAA::CreateResources(uint32 width, uint32 height)
{
    if (!m_device) return;
    if (width == 0 || height == 0)
    {
        m_supported = false;
        m_unsupportedReason = "TAA requires a non-zero render extent";
        return;
    }

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

    m_supported = m_history[0] && m_history[1] && m_result && m_constantBuffer;
    m_unsupportedReason = m_supported
                              ? std::string()
                              : "TAA history/result resources could not be created";
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
    const int jitterPhase = m_config.jitterPhase > 0 ? m_config.jitterPhase : 1;
    int phase = static_cast<int>(frameIndex % static_cast<uint64>(jitterPhase));
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
    if (m_width == 0 || m_height == 0)
    {
        return projMatrix;
    }

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
    m_lastResolveStats = {};
    m_lastResolveStats.requested = IsRequestedEnabled();
    m_lastResolveStats.supported = IsSupported();
    m_lastResolveStats.frameIndex = frameIndex;
    m_lastResolveStats.jitterOffset = GetJitterOffset(frameIndex);
    m_lastResolveStats.historyValidBefore = m_historyValid;
    m_lastResolveStats.depthAvailable = depthTexture != nullptr;
    m_lastResolveStats.motionVectorsAvailable = motionVectors != nullptr;

    if (!IsEnabled() || !m_device)
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("TAA: unsupported resolve skipped: {}", GetUnsupportedReason());
        }
        m_lastResolveStats.fallbackReason = GetUnsupportedReason();
        m_lastResolveStats.historyValidAfter = m_historyValid;
        return;
    }

    if (!currentColor)
    {
        m_lastResolveStats.fallbackReason = "TAA resolve skipped because current color is unavailable";
        m_lastResolveStats.historyValidAfter = m_historyValid;
        RVX_CORE_WARN("TAA: {}", m_lastResolveStats.fallbackReason);
        return;
    }

    if (m_config.useMotionVectors && !motionVectors)
    {
        m_lastResolveStats.motionVectorFallbackUsed = true;
        m_lastResolveStats.fallbackReason =
            "TAA minimal resolve used current-frame copy because motion vectors are unavailable";
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

    if (m_result)
    {
        ctx.CopyTexture(currentColor, m_result.Get());
    }
    if (m_history[m_currentHistory])
    {
        ctx.CopyTexture(currentColor, m_history[m_currentHistory].Get());
    }

    m_lastResolveStats.copiedCurrentFrame = true;
    m_lastResolveStats.resolved = true;
    m_historyValid = true;
    m_lastResolveStats.historyValidAfter = m_historyValid;

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
