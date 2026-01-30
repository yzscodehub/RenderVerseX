#pragma once

#ifdef _WIN32

// Include COM interface definitions before Windows.h to ensure
// IUnknown and IStream are available even with WIN32_LEAN_AND_MEAN
#include <unknwn.h>
#include <objidl.h>

#include "ShaderCompiler/ShaderSourceInfo.h"
#include <Windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <unordered_set>

namespace RVX
{
    using Microsoft::WRL::ComPtr;

    // =========================================================================
    // Tracking Include Handler
    // Wraps IDxcIncludeHandler to track all included files and their hashes
    // =========================================================================
    class TrackingIncludeHandler : public IDxcIncludeHandler
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================
        TrackingIncludeHandler(
            ComPtr<IDxcUtils> utils,
            const std::filesystem::path& baseDir);

        virtual ~TrackingIncludeHandler() = default;

        // =====================================================================
        // IUnknown
        // =====================================================================
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;

        // =====================================================================
        // IDxcIncludeHandler
        // =====================================================================
        HRESULT STDMETHODCALLTYPE LoadSource(
            LPCWSTR pFilename,
            IDxcBlob** ppIncludeSource) override;

        // =====================================================================
        // Tracking API
        // =====================================================================

        /** @brief Get tracked source info (includes, hashes) */
        const ShaderSourceInfo& GetSourceInfo() const { return m_sourceInfo; }

        /** @brief Set main file info */
        void SetMainFile(const std::string& path, uint64 hash);

        /** @brief Add additional include directory */
        void AddIncludeDirectory(const std::filesystem::path& dir);

        /** @brief Reset tracking state */
        void Reset();

        /** @brief Get list of include directories */
        const std::vector<std::filesystem::path>& GetIncludeDirectories() const
        {
            return m_includeDirs;
        }

    private:
        // =====================================================================
        // Internal Implementation
        // =====================================================================
        std::filesystem::path ResolveInclude(const std::wstring& filename) const;
        std::string WideToNarrow(const std::wstring& wide) const;

        ComPtr<IDxcUtils> m_utils;
        std::filesystem::path m_baseDir;
        std::vector<std::filesystem::path> m_includeDirs;
        ShaderSourceInfo m_sourceInfo;
        std::unordered_set<std::string> m_processedIncludes;
        ULONG m_refCount = 1;
    };

} // namespace RVX

#endif // _WIN32
