#pragma once

// =============================================================================
// DX11 Common Headers and Utilities
// =============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "Core/Types.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "RHI/RHIDefinitions.h"

#include <string>
#include <format>

namespace RVX
{
    using Microsoft::WRL::ComPtr;

    // =============================================================================
    // DX11 Constants
    // =============================================================================
    constexpr D3D_FEATURE_LEVEL DX11_MIN_FEATURE_LEVEL = D3D_FEATURE_LEVEL_11_0;
    constexpr UINT DX11_MAX_CONSTANT_BUFFER_SIZE = 65536;  // 64KB (DX11 limit)
    
    // Binding limits
    constexpr uint32 DX11_MAX_CBUFFER_SLOTS = 14;
    constexpr uint32 DX11_MAX_SRV_SLOTS = 128;
    constexpr uint32 DX11_MAX_SAMPLER_SLOTS = 16;
    constexpr uint32 DX11_MAX_UAV_SLOTS = 8;       // DX11.0 PS UAV limit
    constexpr uint32 DX11_MAX_UAV_SLOTS_11_1 = 64; // DX11.1 UAV limit
    constexpr uint32 DX11_MAX_VERTEX_BUFFERS = 16;
    constexpr uint32 DX11_MAX_RENDER_TARGETS = 8;

    // Frame constants
    constexpr uint32 DX11_MAX_FRAME_COUNT = 3;

    // =============================================================================
    // HRESULT to String
    // =============================================================================
    inline std::string HRESULTToString(HRESULT hr)
    {
        switch (hr)
        {
            case S_OK:                              return "S_OK";
            case S_FALSE:                           return "S_FALSE";
            case E_FAIL:                            return "E_FAIL";
            case E_INVALIDARG:                      return "E_INVALIDARG";
            case E_OUTOFMEMORY:                     return "E_OUTOFMEMORY";
            case E_NOTIMPL:                         return "E_NOTIMPL";
            case E_NOINTERFACE:                     return "E_NOINTERFACE";
            
            // DXGI errors
            case DXGI_ERROR_DEVICE_HUNG:            return "DXGI_ERROR_DEVICE_HUNG";
            case DXGI_ERROR_DEVICE_REMOVED:         return "DXGI_ERROR_DEVICE_REMOVED";
            case DXGI_ERROR_DEVICE_RESET:           return "DXGI_ERROR_DEVICE_RESET";
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR:  return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
            case DXGI_ERROR_INVALID_CALL:           return "DXGI_ERROR_INVALID_CALL";
            case DXGI_ERROR_UNSUPPORTED:            return "DXGI_ERROR_UNSUPPORTED";
            
            // D3D11 errors
            case D3D11_ERROR_FILE_NOT_FOUND:        return "D3D11_ERROR_FILE_NOT_FOUND";
            case D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS: 
                return "D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS";
            case D3D11_ERROR_TOO_MANY_UNIQUE_VIEW_OBJECTS:  
                return "D3D11_ERROR_TOO_MANY_UNIQUE_VIEW_OBJECTS";
            case D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD: 
                return "D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD";
            
            default:
                return std::format("Unknown HRESULT: 0x{:08X}", static_cast<uint32>(hr));
        }
    }

    // =============================================================================
    // DX11 Error Handling
    // =============================================================================
    inline bool DX11Succeeded(HRESULT hr)
    {
        return SUCCEEDED(hr);
    }

    inline void DX11Check(HRESULT hr, const char* operation, const char* file, int line)
    {
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX11 Error in '{}': {} at {}:{}", 
                operation, HRESULTToString(hr), file, line);
            
            #ifdef RVX_DEBUG
            if (IsDebuggerPresent())
            {
                __debugbreak();
            }
            #endif
        }
    }

    #define DX11_CHECK(hr) \
        DX11Check(hr, #hr, __FILE__, __LINE__)

    #define DX11_CHECK_RETURN(hr, retval) \
        do { \
            HRESULT _hr = (hr); \
            if (FAILED(_hr)) { \
                DX11Check(_hr, #hr, __FILE__, __LINE__); \
                return retval; \
            } \
        } while(0)

    #define DX11_CHECK_NULLPTR(hr) DX11_CHECK_RETURN(hr, nullptr)
    #define DX11_CHECK_FALSE(hr) DX11_CHECK_RETURN(hr, false)

    // =============================================================================
    // Debug Name Helper
    // =============================================================================
    inline void SetDX11DebugName(ID3D11DeviceChild* obj, const char* name)
    {
        if (obj && name && name[0])
        {
            obj->SetPrivateData(WKPDID_D3DDebugObjectName,
                static_cast<UINT>(strlen(name)), name);
        }
    }

    #ifdef RVX_DX11_DEBUG
        #define DX11_SET_DEBUG_NAME(obj, name) SetDX11DebugName(obj, name)
    #else
        #define DX11_SET_DEBUG_NAME(obj, name) ((void)0)
    #endif

} // namespace RVX
