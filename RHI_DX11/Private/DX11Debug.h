#pragma once

#include "DX11Common.h"
#include <d3d11sdklayers.h>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace RVX
{
    // =============================================================================
    // Resource Type for Tracking
    // =============================================================================
    enum class DX11ResourceType : uint8
    {
        Buffer,
        Texture,
        ShaderResourceView,
        UnorderedAccessView,
        RenderTargetView,
        DepthStencilView,
        VertexShader,
        PixelShader,
        GeometryShader,
        HullShader,
        DomainShader,
        ComputeShader,
        InputLayout,
        BlendState,
        RasterizerState,
        DepthStencilState,
        SamplerState,
        Unknown
    };

    inline const char* ToString(DX11ResourceType type)
    {
        switch (type)
        {
            case DX11ResourceType::Buffer:             return "Buffer";
            case DX11ResourceType::Texture:            return "Texture";
            case DX11ResourceType::ShaderResourceView: return "SRV";
            case DX11ResourceType::UnorderedAccessView: return "UAV";
            case DX11ResourceType::RenderTargetView:   return "RTV";
            case DX11ResourceType::DepthStencilView:   return "DSV";
            case DX11ResourceType::VertexShader:       return "VS";
            case DX11ResourceType::PixelShader:        return "PS";
            case DX11ResourceType::ComputeShader:      return "CS";
            case DX11ResourceType::InputLayout:        return "InputLayout";
            case DX11ResourceType::BlendState:         return "BlendState";
            case DX11ResourceType::RasterizerState:    return "RasterizerState";
            case DX11ResourceType::DepthStencilState:  return "DepthStencilState";
            case DX11ResourceType::SamplerState:       return "SamplerState";
            default:                                   return "Unknown";
        }
    }

    // =============================================================================
    // Debug Statistics
    // =============================================================================
    struct DX11DebugStats
    {
        std::atomic<uint32> drawCalls{0};
        std::atomic<uint32> dispatchCalls{0};
        std::atomic<uint32> stateChanges{0};
        std::atomic<uint32> bufferBinds{0};
        std::atomic<uint32> textureBinds{0};
        std::atomic<uint32> errorCount{0};
        std::atomic<uint32> warningCount{0};

        void ResetFrameCounters()
        {
            drawCalls = 0;
            dispatchCalls = 0;
            stateChanges = 0;
            bufferBinds = 0;
            textureBinds = 0;
        }
    };

    // =============================================================================
    // DX11 Debug System
    // =============================================================================
    class DX11Debug
    {
    public:
        static DX11Debug& Get()
        {
            static DX11Debug instance;
            return instance;
        }

        bool Initialize(ID3D11Device* device, bool enableDebugLayer);
        void Shutdown();

        void BeginFrame(uint64 frameIndex);
        void EndFrame();

        // InfoQueue message processing
        void ProcessInfoQueueMessages();
        void SetBreakOnError(bool enable);
        void SetBreakOnWarning(bool enable);

        // Debug markers (PIX/RenderDoc)
        void BeginEvent(const wchar_t* name);
        void EndEvent();
        void SetMarker(const wchar_t* name);

        // Device removed diagnostics
        void DiagnoseDeviceRemoved(ID3D11Device* device);
        std::string GetDeviceRemovedReason(ID3D11Device* device);

        // Report live objects
        void ReportLiveObjects();

        // Statistics
        DX11DebugStats& GetStats() { return m_stats; }
        const DX11DebugStats& GetStats() const { return m_stats; }

        bool IsDebugEnabled() const { return m_debugEnabled; }
        uint64 GetCurrentFrame() const { return m_currentFrame; }

    private:
        DX11Debug() = default;
        ~DX11Debug() = default;

        bool m_debugEnabled = false;
        uint64 m_currentFrame = 0;

        ComPtr<ID3D11Debug> m_d3dDebug;
        ComPtr<ID3D11InfoQueue> m_infoQueue;

        // D3DPERF functions
        using PFN_D3DPERF_BeginEvent = int (WINAPI*)(DWORD, LPCWSTR);
        using PFN_D3DPERF_EndEvent = int (WINAPI*)(void);
        using PFN_D3DPERF_SetMarker = int (WINAPI*)(DWORD, LPCWSTR);
        
        PFN_D3DPERF_BeginEvent m_pfnBeginEvent = nullptr;
        PFN_D3DPERF_EndEvent m_pfnEndEvent = nullptr;
        PFN_D3DPERF_SetMarker m_pfnSetMarker = nullptr;

        DX11DebugStats m_stats;
    };

    // =============================================================================
    // Debug Macros
    // =============================================================================
#ifdef RVX_DX11_DEBUG
    #define DX11_DEBUG_STAT_INC(stat) ++DX11Debug::Get().GetStats().stat
    #define DX11_DEBUG_EVENT_BEGIN(name) DX11Debug::Get().BeginEvent(L##name)
    #define DX11_DEBUG_EVENT_END() DX11Debug::Get().EndEvent()
    #define DX11_DEBUG_MARKER(name) DX11Debug::Get().SetMarker(L##name)
#else
    #define DX11_DEBUG_STAT_INC(stat) ((void)0)
    #define DX11_DEBUG_EVENT_BEGIN(name) ((void)0)
    #define DX11_DEBUG_EVENT_END() ((void)0)
    #define DX11_DEBUG_MARKER(name) ((void)0)
#endif

} // namespace RVX
