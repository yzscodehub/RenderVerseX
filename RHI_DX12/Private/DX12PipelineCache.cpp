#include "DX12PipelineCache.h"
#include "DX12Device.h"
#include <fstream>
#include <filesystem>

namespace RVX
{
    DX12PipelineCache::~DX12PipelineCache()
    {
        Shutdown();
    }

    bool DX12PipelineCache::Initialize(DX12Device* device, const std::string& cachePath)
    {
        m_device = device;
        m_cachePath = cachePath;
        
        auto d3dDevice = device->GetD3DDevice();
        
        // Try to load existing cache from disk
        if (!cachePath.empty() && LoadFromFile())
        {
            // CreatePipelineLibrary requires ID3D12Device1
            ComPtr<ID3D12Device1> device1;
            HRESULT hr = d3dDevice->QueryInterface(IID_PPV_ARGS(&device1));
            if (FAILED(hr))
            {
                RVX_RHI_WARN("ID3D12Device1 not available, PSO caching disabled");
                m_cacheData.clear();
            }
            else
            {
                // Create pipeline library from loaded data
                hr = device1->CreatePipelineLibrary(
                    m_cacheData.data(),
                    m_cacheData.size(),
                    IID_PPV_ARGS(&m_pipelineLibrary));
            
                if (SUCCEEDED(hr))
                {
                    RVX_RHI_INFO("Loaded PSO cache from: {} ({} bytes)", cachePath, m_cacheData.size());
                    return true;
                }
                else
                {
                    // Cache may be stale or corrupted, start fresh
                    RVX_RHI_WARN("Failed to load PSO cache (0x{:08X}), creating new", static_cast<uint32>(hr));
                    m_cacheData.clear();
                }
            }
        }
        
        // Create empty pipeline library (requires ID3D12Device1)
        ComPtr<ID3D12Device1> device1;
        HRESULT hr = d3dDevice->QueryInterface(IID_PPV_ARGS(&device1));
        if (FAILED(hr))
        {
            RVX_RHI_WARN("ID3D12Device1 not available, using memory-only cache");
            return true;
        }
        
        hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&m_pipelineLibrary));
        if (FAILED(hr))
        {
            RVX_RHI_WARN("ID3D12PipelineLibrary not supported (0x{:08X}), using memory-only cache", 
                static_cast<uint32>(hr));
            // Fall back to memory-only cache (m_psoCache)
            return true;
        }
        
        RVX_RHI_INFO("PSO cache initialized (empty)");
        return true;
    }

    void DX12PipelineCache::Shutdown()
    {
        // Save cache before shutdown
        if (m_dirty && !m_cachePath.empty())
        {
            SaveToFile();
        }
        
        m_psoCache.clear();
        m_pipelineLibrary.Reset();
        m_cacheData.clear();
    }

    bool DX12PipelineCache::LoadFromFile()
    {
        if (m_cachePath.empty())
            return false;
        
        try
        {
            if (!std::filesystem::exists(m_cachePath))
                return false;
            
            std::ifstream file(m_cachePath, std::ios::binary | std::ios::ate);
            if (!file.is_open())
                return false;
            
            size_t fileSize = static_cast<size_t>(file.tellg());
            if (fileSize == 0)
                return false;
            
            m_cacheData.resize(fileSize);
            file.seekg(0, std::ios::beg);
            file.read(reinterpret_cast<char*>(m_cacheData.data()), fileSize);
            
            return true;
        }
        catch (const std::exception& e)
        {
            RVX_RHI_WARN("Failed to load PSO cache file: {}", e.what());
            return false;
        }
    }

    bool DX12PipelineCache::SaveToFile()
    {
        if (m_cachePath.empty() || !m_pipelineLibrary)
            return false;
        
        try
        {
            // Get serialized size
            SIZE_T serializedSize = m_pipelineLibrary->GetSerializedSize();
            if (serializedSize == 0)
            {
                RVX_RHI_DEBUG("PSO cache is empty, nothing to save");
                return true;
            }
            
            // Serialize to buffer
            m_cacheData.resize(serializedSize);
            HRESULT hr = m_pipelineLibrary->Serialize(m_cacheData.data(), serializedSize);
            if (FAILED(hr))
            {
                RVX_RHI_ERROR("Failed to serialize PSO cache: 0x{:08X}", static_cast<uint32>(hr));
                return false;
            }
            
            // Create directory if needed
            std::filesystem::path cachePath(m_cachePath);
            if (cachePath.has_parent_path())
            {
                std::filesystem::create_directories(cachePath.parent_path());
            }
            
            // Write to file
            std::ofstream file(m_cachePath, std::ios::binary);
            if (!file.is_open())
            {
                RVX_RHI_ERROR("Failed to open PSO cache file for writing: {}", m_cachePath);
                return false;
            }
            
            file.write(reinterpret_cast<const char*>(m_cacheData.data()), serializedSize);
            file.close();
            
            m_dirty = false;
            RVX_RHI_INFO("Saved PSO cache to: {} ({} bytes, {} PSOs)", 
                m_cachePath, serializedSize, m_stats.totalPSOs);
            
            return true;
        }
        catch (const std::exception& e)
        {
            RVX_RHI_ERROR("Failed to save PSO cache: {}", e.what());
            return false;
        }
    }

    ComPtr<ID3D12PipelineState> DX12PipelineCache::GetOrCreateGraphicsPSO(
        const std::wstring& name,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        
        // Check in-memory cache first
        auto it = m_psoCache.find(name);
        if (it != m_psoCache.end())
        {
            m_stats.hitCount++;
            return it->second;
        }
        
        ComPtr<ID3D12PipelineState> pso;
        auto d3dDevice = m_device->GetD3DDevice();
        
        // Try to load from pipeline library
        if (m_pipelineLibrary)
        {
            HRESULT hr = m_pipelineLibrary->LoadGraphicsPipeline(
                name.c_str(),
                &desc,
                IID_PPV_ARGS(&pso));
            
            if (SUCCEEDED(hr))
            {
                m_stats.hitCount++;
                m_psoCache[name] = pso;
                return pso;
            }
            
            // Not in library, need to create
            if (hr != E_INVALIDARG)  // E_INVALIDARG means not found
            {
                RVX_RHI_DEBUG("LoadGraphicsPipeline failed: 0x{:08X}", static_cast<uint32>(hr));
            }
        }
        
        // Create new PSO
        m_stats.missCount++;
        HRESULT hr = d3dDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create graphics PSO: 0x{:08X}", static_cast<uint32>(hr));
            return nullptr;
        }
        
        // Store in pipeline library
        if (m_pipelineLibrary)
        {
            hr = m_pipelineLibrary->StorePipeline(name.c_str(), pso.Get());
            if (SUCCEEDED(hr))
            {
                m_dirty = true;
            }
        }
        
        m_psoCache[name] = pso;
        m_stats.totalPSOs++;
        
        return pso;
    }

    ComPtr<ID3D12PipelineState> DX12PipelineCache::GetOrCreateComputePSO(
        const std::wstring& name,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc)
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        
        // Check in-memory cache first
        auto it = m_psoCache.find(name);
        if (it != m_psoCache.end())
        {
            m_stats.hitCount++;
            return it->second;
        }
        
        ComPtr<ID3D12PipelineState> pso;
        auto d3dDevice = m_device->GetD3DDevice();
        
        // Try to load from pipeline library
        if (m_pipelineLibrary)
        {
            HRESULT hr = m_pipelineLibrary->LoadComputePipeline(
                name.c_str(),
                &desc,
                IID_PPV_ARGS(&pso));
            
            if (SUCCEEDED(hr))
            {
                m_stats.hitCount++;
                m_psoCache[name] = pso;
                return pso;
            }
        }
        
        // Create new PSO
        m_stats.missCount++;
        HRESULT hr = d3dDevice->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create compute PSO: 0x{:08X}", static_cast<uint32>(hr));
            return nullptr;
        }
        
        // Store in pipeline library
        if (m_pipelineLibrary)
        {
            hr = m_pipelineLibrary->StorePipeline(name.c_str(), pso.Get());
            if (SUCCEEDED(hr))
            {
                m_dirty = true;
            }
        }
        
        m_psoCache[name] = pso;
        m_stats.totalPSOs++;
        
        return pso;
    }

    std::wstring DX12PipelineCache::GeneratePSOName(const char* prefix, size_t hash)
    {
        wchar_t name[128];
        swprintf_s(name, L"%hs_%016llx", prefix, static_cast<unsigned long long>(hash));
        return name;
    }

} // namespace RVX
