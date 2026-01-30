#ifdef _WIN32

#include "TrackingIncludeHandler.h"
#include "Core/Log.h"
#include <fstream>
#include <sstream>

namespace RVX
{
    TrackingIncludeHandler::TrackingIncludeHandler(
        ComPtr<IDxcUtils> utils,
        const std::filesystem::path& baseDir)
        : m_utils(std::move(utils))
        , m_baseDir(baseDir)
    {
        // Add base directory as first include path
        if (!baseDir.empty())
        {
            m_includeDirs.push_back(baseDir);
        }
    }

    HRESULT STDMETHODCALLTYPE TrackingIncludeHandler::QueryInterface(
        REFIID riid,
        void** ppvObject)
    {
        if (!ppvObject)
        {
            return E_POINTER;
        }

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDxcIncludeHandler))
        {
            *ppvObject = static_cast<IDxcIncludeHandler*>(this);
            AddRef();
            return S_OK;
        }

        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE TrackingIncludeHandler::AddRef()
    {
        return ++m_refCount;
    }

    ULONG STDMETHODCALLTYPE TrackingIncludeHandler::Release()
    {
        ULONG count = --m_refCount;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE TrackingIncludeHandler::LoadSource(
        LPCWSTR pFilename,
        IDxcBlob** ppIncludeSource)
    {
        if (!pFilename || !ppIncludeSource)
        {
            return E_POINTER;
        }

        *ppIncludeSource = nullptr;

        // Resolve the include path
        std::filesystem::path resolvedPath = ResolveInclude(pFilename);
        if (resolvedPath.empty())
        {
            RVX_CORE_ERROR("TrackingIncludeHandler: Failed to resolve include: {}",
                WideToNarrow(pFilename));
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }

        std::string pathStr = resolvedPath.string();

        // Check if already processed (avoid cycles)
        if (m_processedIncludes.find(pathStr) != m_processedIncludes.end())
        {
            // Already loaded - return empty blob to avoid reprocessing
            // DXC will handle the duplicate include
        }
        else
        {
            m_processedIncludes.insert(pathStr);
        }

        // Read file content
        std::ifstream file(resolvedPath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            RVX_CORE_ERROR("TrackingIncludeHandler: Failed to open file: {}", pathStr);
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<char> content(static_cast<size_t>(size));
        if (!file.read(content.data(), size))
        {
            return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
        }

        // Compute file hash and add to tracking
        uint64 fileHash = ShaderSourceInfo::ComputeFileHash(resolvedPath);
        m_sourceInfo.AddInclude(pathStr, fileHash);

        RVX_CORE_DEBUG("TrackingIncludeHandler: Loaded include: {} (hash: {:016X})",
            pathStr, fileHash);

        // Create DXC blob from content
        ComPtr<IDxcBlobEncoding> blob;
        HRESULT hr = m_utils->CreateBlob(
            content.data(),
            static_cast<UINT32>(content.size()),
            DXC_CP_UTF8,
            &blob);

        if (FAILED(hr))
        {
            return hr;
        }

        *ppIncludeSource = blob.Detach();
        return S_OK;
    }

    void TrackingIncludeHandler::SetMainFile(const std::string& path, uint64 hash)
    {
        m_sourceInfo.mainFile = path;
        m_sourceInfo.fileHashes[path] = hash;
        m_processedIncludes.insert(path);

        // Set base directory from main file if not already set
        if (m_baseDir.empty())
        {
            m_baseDir = std::filesystem::path(path).parent_path();
            if (!m_baseDir.empty())
            {
                m_includeDirs.insert(m_includeDirs.begin(), m_baseDir);
            }
        }
    }

    void TrackingIncludeHandler::AddIncludeDirectory(const std::filesystem::path& dir)
    {
        // Avoid duplicates
        for (const auto& existing : m_includeDirs)
        {
            if (std::filesystem::equivalent(existing, dir))
            {
                return;
            }
        }
        m_includeDirs.push_back(dir);
    }

    void TrackingIncludeHandler::Reset()
    {
        m_sourceInfo.Clear();
        m_processedIncludes.clear();
    }

    std::filesystem::path TrackingIncludeHandler::ResolveInclude(const std::wstring& filename) const
    {
        std::filesystem::path includePath(filename);

        // If absolute path, use directly
        if (includePath.is_absolute())
        {
            if (std::filesystem::exists(includePath))
            {
                return includePath;
            }
            return {};
        }

        // Search in include directories
        for (const auto& dir : m_includeDirs)
        {
            std::filesystem::path fullPath = dir / includePath;
            std::error_code ec;
            if (std::filesystem::exists(fullPath, ec))
            {
                return std::filesystem::canonical(fullPath, ec);
            }
        }

        // Try relative to current directory
        if (std::filesystem::exists(includePath))
        {
            std::error_code ec;
            return std::filesystem::canonical(includePath, ec);
        }

        return {};
    }

    std::string TrackingIncludeHandler::WideToNarrow(const std::wstring& wide) const
    {
        if (wide.empty())
        {
            return {};
        }

        int sizeNeeded = WideCharToMultiByte(
            CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);

        std::string narrow(sizeNeeded, '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, wide.c_str(), -1, narrow.data(), sizeNeeded, nullptr, nullptr);

        if (!narrow.empty() && narrow.back() == '\0')
        {
            narrow.pop_back();
        }

        return narrow;
    }

} // namespace RVX

#endif // _WIN32
