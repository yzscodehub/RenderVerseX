#pragma once

#include "DX11Common.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHISampler.h"
#include <unordered_map>
#include <mutex>

namespace RVX
{
    class DX11Device;

    // =============================================================================
    // Hash Helpers
    // =============================================================================
    inline size_t HashCombine(size_t hash1, size_t hash2)
    {
        return hash1 ^ (hash2 + 0x9e3779b9 + (hash1 << 6) + (hash1 >> 2));
    }

    // =============================================================================
    // DX11 State Object Cache
    // =============================================================================
    // Caches D3D11 state objects to avoid redundant creation
    // DX11 has a limit on unique state objects (~4096), so caching is important

    class DX11StateCache
    {
    public:
        DX11StateCache(DX11Device* device);
        ~DX11StateCache();

        // Get or create cached state objects
        ID3D11RasterizerState* GetRasterizerState(const RHIRasterizerState& desc);
        ID3D11DepthStencilState* GetDepthStencilState(const RHIDepthStencilState& desc);
        ID3D11BlendState* GetBlendState(const RHIBlendState& desc);
        ID3D11SamplerState* GetSamplerState(const RHISamplerDesc& desc);

        // Input layout cache
        ID3D11InputLayout* GetInputLayout(
            const std::vector<RHIInputElement>& elements,
            const void* vsBytecode,
            size_t bytecodeSize);

        // Statistics
        struct CacheStats
        {
            uint32 rasterizerStateCount = 0;
            uint32 depthStencilStateCount = 0;
            uint32 blendStateCount = 0;
            uint32 samplerStateCount = 0;
            uint32 inputLayoutCount = 0;
            uint32 cacheHits = 0;
            uint32 cacheMisses = 0;
        };

        const CacheStats& GetStats() const { return m_stats; }
        void Clear();

    private:
        DX11Device* m_device = nullptr;

        // State caches
        std::unordered_map<size_t, ComPtr<ID3D11RasterizerState>> m_rasterizerStates;
        std::unordered_map<size_t, ComPtr<ID3D11DepthStencilState>> m_depthStencilStates;
        std::unordered_map<size_t, ComPtr<ID3D11BlendState>> m_blendStates;
        std::unordered_map<size_t, ComPtr<ID3D11SamplerState>> m_samplerStates;
        std::unordered_map<size_t, ComPtr<ID3D11InputLayout>> m_inputLayouts;

        mutable std::mutex m_mutex;
        CacheStats m_stats;

        // Hash functions
        size_t HashRasterState(const RHIRasterizerState& desc) const;
        size_t HashDepthStencilState(const RHIDepthStencilState& desc) const;
        size_t HashBlendState(const RHIBlendState& desc) const;
        size_t HashSamplerState(const RHISamplerDesc& desc) const;
        size_t HashInputLayout(const std::vector<RHIInputElement>& elements) const;
    };

} // namespace RVX
