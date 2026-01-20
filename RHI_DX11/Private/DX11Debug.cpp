#include "DX11Debug.h"

namespace RVX
{
    bool DX11Debug::Initialize(ID3D11Device* device, bool enableDebugLayer)
    {
        m_debugEnabled = enableDebugLayer;

        if (!enableDebugLayer)
        {
            RVX_RHI_INFO("DX11 Debug System: disabled");
            return true;
        }

        // Get ID3D11Debug interface
        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&m_d3dDebug));
        if (FAILED(hr))
        {
            RVX_RHI_WARN("Failed to get ID3D11Debug interface - debug layer may not be installed");
        }

        // Get ID3D11InfoQueue interface
        hr = device->QueryInterface(IID_PPV_ARGS(&m_infoQueue));
        if (SUCCEEDED(hr))
        {
            // Don't break on errors - just log them
            // Breaking causes issues when no debugger is attached
            m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);

            // Filter out some common noisy messages
            D3D11_MESSAGE_ID hide[] =
            {
                D3D11_MESSAGE_ID_SETPRIVATEDATA_CHANGINGPARAMS,
            };

            D3D11_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumIDs = _countof(hide);
            filter.DenyList.pIDList = hide;
            m_infoQueue->AddStorageFilterEntries(&filter);

            RVX_RHI_INFO("DX11 InfoQueue initialized");
        }
        else
        {
            RVX_RHI_WARN("Failed to get ID3D11InfoQueue interface");
        }

        // Load D3DPERF functions for PIX/RenderDoc markers
        HMODULE d3d9Module = GetModuleHandleW(L"d3d9.dll");
        if (!d3d9Module)
        {
            d3d9Module = LoadLibraryW(L"d3d9.dll");
        }

        if (d3d9Module)
        {
            m_pfnBeginEvent = reinterpret_cast<PFN_D3DPERF_BeginEvent>(
                GetProcAddress(d3d9Module, "D3DPERF_BeginEvent"));
            m_pfnEndEvent = reinterpret_cast<PFN_D3DPERF_EndEvent>(
                GetProcAddress(d3d9Module, "D3DPERF_EndEvent"));
            m_pfnSetMarker = reinterpret_cast<PFN_D3DPERF_SetMarker>(
                GetProcAddress(d3d9Module, "D3DPERF_SetMarker"));

            if (m_pfnBeginEvent && m_pfnEndEvent)
            {
                RVX_RHI_DEBUG("PIX/RenderDoc event markers available");
            }
        }

        RVX_RHI_INFO("DX11 Debug System initialized");
        return true;
    }

    void DX11Debug::Shutdown()
    {
        if (m_d3dDebug)
        {
            RVX_RHI_INFO("DX11 Debug: Reporting live device objects...");
            m_d3dDebug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
        }

        m_infoQueue.Reset();
        m_d3dDebug.Reset();
    }

    void DX11Debug::BeginFrame(uint64 frameIndex)
    {
        m_currentFrame = frameIndex;
        m_stats.ResetFrameCounters();
    }

    void DX11Debug::EndFrame()
    {
        if (m_debugEnabled)
        {
            ProcessInfoQueueMessages();
        }
    }

    void DX11Debug::ProcessInfoQueueMessages()
    {
        if (!m_infoQueue) return;

        UINT64 messageCount = m_infoQueue->GetNumStoredMessages();

        for (UINT64 i = 0; i < messageCount; ++i)
        {
            SIZE_T messageLength = 0;
            m_infoQueue->GetMessage(i, nullptr, &messageLength);

            if (messageLength == 0) continue;

            std::vector<char> messageData(messageLength);
            D3D11_MESSAGE* message = reinterpret_cast<D3D11_MESSAGE*>(messageData.data());

            if (SUCCEEDED(m_infoQueue->GetMessage(i, message, &messageLength)))
            {
                switch (message->Severity)
                {
                    case D3D11_MESSAGE_SEVERITY_CORRUPTION:
                        RVX_RHI_ERROR("[DX11 CORRUPTION] {}", message->pDescription);
                        ++m_stats.errorCount;
                        break;
                    case D3D11_MESSAGE_SEVERITY_ERROR:
                        RVX_RHI_ERROR("[DX11 ERROR] {}", message->pDescription);
                        ++m_stats.errorCount;
                        break;
                    case D3D11_MESSAGE_SEVERITY_WARNING:
                        RVX_RHI_WARN("[DX11 WARNING] {}", message->pDescription);
                        ++m_stats.warningCount;
                        break;
                    case D3D11_MESSAGE_SEVERITY_INFO:
                        RVX_RHI_DEBUG("[DX11 INFO] {}", message->pDescription);
                        break;
                    case D3D11_MESSAGE_SEVERITY_MESSAGE:
                        RVX_RHI_DEBUG("[DX11 MSG] {}", message->pDescription);
                        break;
                }
            }
        }

        m_infoQueue->ClearStoredMessages();
    }

    void DX11Debug::SetBreakOnError(bool enable)
    {
        if (m_infoQueue)
        {
            m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, enable ? TRUE : FALSE);
        }
    }

    void DX11Debug::SetBreakOnWarning(bool enable)
    {
        if (m_infoQueue)
        {
            m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, enable ? TRUE : FALSE);
        }
    }

    void DX11Debug::BeginEvent(const wchar_t* name)
    {
        if (m_pfnBeginEvent)
        {
            m_pfnBeginEvent(0xFF00FF00, name);  // Green color
        }
    }

    void DX11Debug::EndEvent()
    {
        if (m_pfnEndEvent)
        {
            m_pfnEndEvent();
        }
    }

    void DX11Debug::SetMarker(const wchar_t* name)
    {
        if (m_pfnSetMarker)
        {
            m_pfnSetMarker(0xFFFF0000, name);  // Red color
        }
    }

    void DX11Debug::DiagnoseDeviceRemoved(ID3D11Device* device)
    {
        HRESULT reason = device->GetDeviceRemovedReason();

        RVX_RHI_ERROR("=== DX11 DEVICE REMOVED DIAGNOSTIC ===");
        RVX_RHI_ERROR("Reason: {} (0x{:08X})", GetDeviceRemovedReason(device), static_cast<uint32>(reason));

        switch (reason)
        {
            case DXGI_ERROR_DEVICE_HUNG:
                RVX_RHI_ERROR("  GPU hung - possible infinite loop in shader or excessive workload");
                break;
            case DXGI_ERROR_DEVICE_REMOVED:
                RVX_RHI_ERROR("  GPU physically removed or disabled");
                break;
            case DXGI_ERROR_DEVICE_RESET:
                RVX_RHI_ERROR("  GPU reset by driver/OS (TDR triggered)");
                break;
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
                RVX_RHI_ERROR("  Driver internal error - update GPU drivers");
                break;
            case DXGI_ERROR_INVALID_CALL:
                RVX_RHI_ERROR("  Invalid API call");
                break;
            default:
                RVX_RHI_ERROR("  Unknown device removed reason");
                break;
        }

        RVX_RHI_ERROR("Last frame stats:");
        RVX_RHI_ERROR("  Draw calls: {}", m_stats.drawCalls.load());
        RVX_RHI_ERROR("  Dispatch calls: {}", m_stats.dispatchCalls.load());
        RVX_RHI_ERROR("  Errors/Warnings: {}/{}", m_stats.errorCount.load(), m_stats.warningCount.load());

        RVX_RHI_ERROR("=== END DIAGNOSTIC ===");
    }

    std::string DX11Debug::GetDeviceRemovedReason(ID3D11Device* device)
    {
        HRESULT reason = device->GetDeviceRemovedReason();
        return HRESULTToString(reason);
    }

    void DX11Debug::ReportLiveObjects()
    {
        if (m_d3dDebug)
        {
            m_d3dDebug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
        }
    }

} // namespace RVX
