#include "Resource/ResourceManager.h"
#include "ModelTextureStreamingService.h"
#include "Resource/HotReloadManager.h"
#include "Resource/Loader/AnimationLoader.h"
#include "Resource/Loader/EnvironmentLoader.h"
#include "Resource/Loader/MeshLoader.h"
#include "Resource/Loader/ModelLoader.h"
#include "Resource/Loader/ShaderLoader.h"
#include "Resource/Loader/TextureLoader.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MaterialInstanceResource.h"
#include "Resource/Types/ModelResource.h"
#include "Resource/Types/TextureResource.h"
#include "Core/Assert.h"
#include "Core/Diagnostics/JsonWriter.h"
#include "Core/Log.h"
#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

// Logging macros
#ifndef RVX_RESOURCE_INFO
#define RVX_RESOURCE_INFO(...)  RVX_CORE_INFO(__VA_ARGS__)
#define RVX_RESOURCE_WARN(...)  RVX_CORE_WARN(__VA_ARGS__)
#define RVX_RESOURCE_ERROR(...) RVX_CORE_ERROR(__VA_ARGS__)
#define RVX_RESOURCE_DEBUG(...) RVX_CORE_DEBUG(__VA_ARGS__)
#endif

namespace RVX::Resource
{

class AssetResidencyLeaseControl final
{
public:
    struct Snapshot
    {
        uint64 activeLeases = 0;
        uint64 protectedResources = 0;
        uint64 queuedUnloads = 0;
        uint64 blockedEvictions = 0;
    };

    using DeferredUnloadCallback =
        std::function<bool(const AssetKey&, uint64)>;

    explicit AssetResidencyLeaseControl(DeferredUnloadCallback onDeferredUnload)
        : m_onDeferredUnload(std::move(onDeferredUnload))
    {
    }

    bool BeginAcquire(const AssetKey& assetKey, ResourceId rootResourceId)
    {
        if (!assetKey.IsValid() || rootResourceId == InvalidResourceId)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_acceptingLeases)
        {
            return false;
        }

        Entry& entry = m_entries[assetKey];
        if (entry.activeLeaseCount == 0 && entry.pendingAcquireCount == 0)
        {
            entry.unloadQueued = false;
            entry.closureHold = false;
            entry.exactUnloadInProgress = false;
            entry.exactUnloadThread = {};
        }
        else if (entry.rootResourceId != rootResourceId)
        {
            return false;
        }
        entry.rootResourceId = rootResourceId;
        ++entry.pendingAcquireCount;
        return true;
    }

    bool PromoteAcquire(const AssetKey& assetKey,
                        ResourceId rootResourceId,
                        std::vector<ResourceId> dependencyClosure,
                        uint64& outGeneration)
    {
        if (!assetKey.IsValid() || rootResourceId == InvalidResourceId ||
            dependencyClosure.empty())
        {
            return false;
        }

        std::sort(dependencyClosure.begin(), dependencyClosure.end());
        dependencyClosure.erase(
            std::unique(dependencyClosure.begin(), dependencyClosure.end()),
            dependencyClosure.end());
        if (dependencyClosure.front() == InvalidResourceId)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        if (!m_acceptingLeases || found == m_entries.end() ||
            found->second.rootResourceId != rootResourceId ||
            found->second.pendingAcquireCount == 0)
        {
            return false;
        }

        Entry& entry = found->second;
        --entry.pendingAcquireCount;
        if (entry.activeLeaseCount == 0)
        {
            entry.generation = AllocateGenerationLocked();
            entry.dependencyClosure = std::move(dependencyClosure);
            entry.unloadQueued = false;
            entry.closureHold = false;
            entry.exactUnloadInProgress = false;
            entry.exactUnloadThread = {};
        }
        else if (entry.dependencyClosure != dependencyClosure)
        {
            ++entry.pendingAcquireCount;
            return false;
        }

        ++entry.activeLeaseCount;
        ++m_activeLeaseCount;
        outGeneration = entry.generation;
        return true;
    }

    void AbortAcquire(const AssetKey& assetKey, ResourceId rootResourceId) noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        if (found == m_entries.end() || found->second.rootResourceId != rootResourceId ||
            found->second.pendingAcquireCount == 0)
        {
            return;
        }

        --found->second.pendingAcquireCount;
        if (found->second.pendingAcquireCount == 0 &&
            found->second.activeLeaseCount == 0 &&
            !found->second.unloadQueued)
        {
            m_entries.erase(found);
        }
    }

    AssetResidencyReleaseResult RequestExactUnload(const AssetKey& assetKey)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        if (found == m_entries.end())
        {
            return AssetResidencyReleaseResult::Unloaded;
        }

        Entry& entry = found->second;
        if (entry.activeLeaseCount == 0 && entry.pendingAcquireCount == 0)
        {
            if (!entry.closureHold)
            {
                return AssetResidencyReleaseResult::Unloaded;
            }
            if (entry.exactUnloadInProgress || entry.unloadQueued)
            {
                return AssetResidencyReleaseResult::Queued;
            }

            // A previous precise retirement failed closed. Re-queueing is
            // explicit and keeps the completion-owned residency hold intact
            // until the exact closure transaction succeeds. The manager's
            // callback has no route back into this control; invoking it under
            // the lock avoids a throwing std::function copy in this boundary.
            entry.unloadQueued = true;
            if (m_onDeferredUnload)
            {
                try
                {
                    if (!m_onDeferredUnload(assetKey, entry.generation))
                    {
                        entry.unloadQueued = false;
                        return AssetResidencyReleaseResult::RetainedFailure;
                    }
                }
                catch (...)
                {
                    entry.unloadQueued = false;
                    return AssetResidencyReleaseResult::RetainedFailure;
                }
            }
        }
        else
        {
            entry.unloadQueued = true;
        }
        return AssetResidencyReleaseResult::Queued;
    }

    void CancelExactUnload(const AssetKey& assetKey) noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        if (found != m_entries.end())
        {
            // A cache hit/republication reclaims the root ownership record.
            // Do not let an older lease generation complete that superseded
            // explicit-release request after the root became active again.
            found->second.unloadQueued = false;
            found->second.closureHold = false;
            found->second.exactUnloadInProgress = false;
            found->second.exactUnloadThread = {};
            if (found->second.activeLeaseCount == 0 &&
                found->second.pendingAcquireCount == 0)
            {
                m_entries.erase(found);
            }
        }
    }

    bool HasLiveLeaseForResource(ResourceId resourceId,
                                 bool countBlockedEviction)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [assetKey, entry] : m_entries)
        {
            (void)assetKey;
            const bool exactTransactionOwnsRemoval =
                entry.exactUnloadInProgress &&
                entry.exactUnloadThread == std::this_thread::get_id();
            if (((entry.activeLeaseCount != 0 ||
                  (entry.closureHold && !exactTransactionOwnsRemoval)) &&
                 std::binary_search(entry.dependencyClosure.begin(),
                                    entry.dependencyClosure.end(),
                                    resourceId)) ||
                (entry.pendingAcquireCount != 0 &&
                 entry.rootResourceId == resourceId))
            {
                if (countBlockedEviction)
                {
                    ++m_blockedEvictionCount;
                }
                return true;
            }
        }
        return false;
    }

    bool IsSoleActiveLease(const AssetKey& assetKey,
                           uint64 generation) const noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        return found != m_entries.end() && generation != 0 &&
               found->second.generation == generation &&
               found->second.activeLeaseCount == 1 &&
               found->second.pendingAcquireCount == 0;
    }

    bool TryBeginQueuedUnload(const AssetKey& assetKey,
                              uint64 generation,
                              ResourceId rootResourceId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        if (found == m_entries.end() || found->second.generation != generation ||
            found->second.activeLeaseCount != 0 ||
            found->second.pendingAcquireCount != 0 ||
            !found->second.unloadQueued || !found->second.closureHold ||
            found->second.exactUnloadInProgress)
        {
            return false;
        }

        for (const auto& [otherKey, other] : m_entries)
        {
            (void)otherKey;
            if (other.activeLeaseCount != 0 &&
                std::binary_search(other.dependencyClosure.begin(),
                                   other.dependencyClosure.end(),
                                   rootResourceId))
            {
                return false;
            }
        }

        found->second.unloadQueued = false;
        found->second.exactUnloadInProgress = true;
        found->second.exactUnloadThread = std::this_thread::get_id();
        return true;
    }

    bool IsQueuedUnload(const AssetKey& assetKey, uint64 generation) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        return found != m_entries.end() && found->second.generation == generation &&
               found->second.unloadQueued;
    }

    void RequeueUnload(const AssetKey& assetKey, uint64 generation) noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        if (found != m_entries.end() && found->second.generation == generation &&
            found->second.activeLeaseCount == 0 &&
            found->second.pendingAcquireCount == 0)
        {
            found->second.unloadQueued = true;
            found->second.closureHold = true;
            found->second.exactUnloadInProgress = false;
            found->second.exactUnloadThread = {};
        }
    }

    void RetainFailedUnload(const AssetKey& assetKey,
                            uint64 generation) noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        if (found != m_entries.end() &&
            found->second.generation == generation &&
            found->second.activeLeaseCount == 0 &&
            found->second.pendingAcquireCount == 0)
        {
            found->second.unloadQueued = false;
            found->second.closureHold = true;
            found->second.exactUnloadInProgress = false;
            found->second.exactUnloadThread = {};
        }
    }

    void CompleteUnload(const AssetKey& assetKey, uint64 generation)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        if (found != m_entries.end() &&
            (generation == 0 || found->second.generation == generation) &&
            found->second.activeLeaseCount == 0 &&
            found->second.pendingAcquireCount == 0 &&
            !found->second.unloadQueued)
        {
            m_entries.erase(found);
        }
    }

    void Release(const AssetKey& assetKey, uint64 generation) noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        if (found == m_entries.end() || found->second.generation != generation ||
            found->second.activeLeaseCount == 0)
        {
            return;
        }

        --found->second.activeLeaseCount;
        --m_activeLeaseCount;
        const bool queueUnload = found->second.activeLeaseCount == 0 &&
                                 found->second.unloadQueued;
        if (queueUnload)
        {
            found->second.closureHold = true;
            if (m_onDeferredUnload)
            {
                try
                {
                    if (!m_onDeferredUnload(assetKey, generation))
                    {
                        found->second.unloadQueued = false;
                    }
                }
                catch (...)
                {
                    // Lease destruction remains noexcept and the closure hold
                    // prevents eviction until an explicit retry is admitted.
                    found->second.unloadQueued = false;
                }
            }
        }
    }

    /**
     * @brief Consume a lease on behalf of the Scene closure facade.
     *
     * Unlike the destructor path this reports whether this caller was the
     * final consumer. The callback is still invoked outside the control lock,
     * so it cannot observe a partially-mutated lease entry.
     */
    bool ReleaseForSceneClosure(const AssetKey& assetKey,
                                uint64 generation,
                                bool& outFinalQueued) noexcept
    {
        outFinalQueued = false;
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(assetKey);
        if (found == m_entries.end() ||
            found->second.generation != generation ||
            found->second.activeLeaseCount == 0)
        {
            return false;
        }

        --found->second.activeLeaseCount;
        --m_activeLeaseCount;
        outFinalQueued = found->second.activeLeaseCount == 0 &&
                         found->second.unloadQueued;
        if (outFinalQueued)
        {
            found->second.closureHold = true;
            if (m_onDeferredUnload)
            {
                try
                {
                    if (!m_onDeferredUnload(assetKey, generation))
                    {
                        found->second.unloadQueued = false;
                        found->second.closureHold = false;
                        ++found->second.activeLeaseCount;
                        ++m_activeLeaseCount;
                        outFinalQueued = false;
                        return false;
                    }
                }
                catch (...)
                {
                    found->second.unloadQueued = false;
                    found->second.closureHold = false;
                    ++found->second.activeLeaseCount;
                    ++m_activeLeaseCount;
                    outFinalQueued = false;
                    return false;
                }
            }
        }
        return true;
    }

    Snapshot GetSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Snapshot snapshot;
        snapshot.activeLeases = m_activeLeaseCount;
        snapshot.blockedEvictions = m_blockedEvictionCount;
        std::unordered_set<ResourceId> protectedResources;
        for (const auto& [assetKey, entry] : m_entries)
        {
            (void)assetKey;
            if (entry.activeLeaseCount != 0 || entry.closureHold)
            {
                protectedResources.insert(entry.dependencyClosure.begin(),
                                          entry.dependencyClosure.end());
            }
            else if (entry.pendingAcquireCount != 0)
            {
                protectedResources.insert(entry.rootResourceId);
            }
            if (entry.unloadQueued)
            {
                ++snapshot.queuedUnloads;
            }
        }
        snapshot.protectedResources = protectedResources.size();
        return snapshot;
    }

    void StopAcceptingLeases() noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_acceptingLeases = false;
        m_onDeferredUnload = {};
    }

private:
    struct Entry
    {
        uint64 generation = 0;
        uint64 activeLeaseCount = 0;
        uint64 pendingAcquireCount = 0;
        ResourceId rootResourceId = InvalidResourceId;
        std::vector<ResourceId> dependencyClosure;
        bool unloadQueued = false;
        // The last Scene/lease consumer transfers ownership to this hold.
        // It remains active through queueing, exact CPU closure commit, and a
        // fail-closed Render retirement admission. Only the precise owner
        // transaction may temporarily bypass cache removal protection.
        bool closureHold = false;
        bool exactUnloadInProgress = false;
        std::thread::id exactUnloadThread{};
    };

    uint64 AllocateGenerationLocked()
    {
        const uint64 generation = m_nextGeneration++;
        if (m_nextGeneration == 0)
        {
            m_nextGeneration = 1;
        }
        return generation == 0 ? AllocateGenerationLocked() : generation;
    }

    mutable std::mutex m_mutex;
    std::unordered_map<AssetKey, Entry, AssetKeyHash> m_entries;
    DeferredUnloadCallback m_onDeferredUnload;
    uint64 m_nextGeneration = 1;
    uint64 m_activeLeaseCount = 0;
    uint64 m_blockedEvictionCount = 0;
    bool m_acceptingLeases = true;
};

AssetResidencyLease::AssetResidencyLease(
    std::weak_ptr<AssetResidencyLeaseControl> control,
    AssetKey assetKey,
    uint64 generation,
    std::vector<ResourceId> dependencyClosure) noexcept
    : m_control(std::move(control))
    , m_assetKey(std::move(assetKey))
    , m_generation(generation)
    , m_dependencyClosure(std::move(dependencyClosure))
{
}

AssetResidencyLease::~AssetResidencyLease()
{
    Reset();
}

AssetResidencyLease::AssetResidencyLease(AssetResidencyLease&& other) noexcept
    : m_control(std::move(other.m_control))
    , m_assetKey(std::move(other.m_assetKey))
    , m_generation(other.m_generation)
    , m_dependencyClosure(std::move(other.m_dependencyClosure))
{
    other.m_generation = 0;
}

AssetResidencyLease& AssetResidencyLease::operator=(
    AssetResidencyLease&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        m_control = std::move(other.m_control);
        m_assetKey = std::move(other.m_assetKey);
        m_generation = other.m_generation;
        m_dependencyClosure = std::move(other.m_dependencyClosure);
        other.m_generation = 0;
    }
    return *this;
}

bool AssetResidencyLease::IsValid() const noexcept
{
    return m_generation != 0 && m_assetKey.IsValid() && !m_control.expired();
}

const AssetKey& AssetResidencyLease::GetAssetKey() const noexcept
{
    return m_assetKey;
}

uint64 AssetResidencyLease::GetGeneration() const noexcept
{
    return m_generation;
}

const std::vector<ResourceId>& AssetResidencyLease::GetDependencyClosure() const noexcept
{
    return m_dependencyClosure;
}

void AssetResidencyLease::Reset() noexcept
{
    if (m_generation != 0)
    {
        if (const std::shared_ptr<AssetResidencyLeaseControl> control =
                m_control.lock())
        {
            control->Release(m_assetKey, m_generation);
        }
    }
    m_control.reset();
    m_assetKey = {};
    m_generation = 0;
    m_dependencyClosure.clear();
}

static ResourceManager* s_instance = nullptr;

namespace
{
    using Diagnostics::JsonBool;
    using Diagnostics::JsonString;

    void AppendJsonString(std::ostringstream& json,
                          std::string_view name,
                          std::string_view value,
                          bool trailingComma = true)
    {
        json << "  \"" << name << "\": " << JsonString(value);
        json << (trailingComma ? ",\n" : "\n");
    }

    void AppendJsonBool(std::ostringstream& json,
                        std::string_view name,
                        bool value,
                        bool trailingComma = true)
    {
        json << "  \"" << name << "\": " << JsonBool(value);
        json << (trailingComma ? ",\n" : "\n");
    }

    void AppendJsonUInt64(std::ostringstream& json,
                          std::string_view name,
                          uint64 value,
                          bool trailingComma = true)
    {
        json << "  \"" << name << "\": " << value;
        json << (trailingComma ? ",\n" : "\n");
    }

    ResourceType DetectCookedArtifactType(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return ResourceType::Unknown;
        }

        std::string firstLine;
        std::getline(file, firstLine);

        if (firstLine == "RVX_TEXTURE_PREBAKE_V1")
        {
            return ResourceType::Texture;
        }
        if (firstLine == "RVX_MESH_PREBAKE_V1")
        {
            return ResourceType::Mesh;
        }
        if (firstLine == "RVX_MODEL_PREBAKE_V1" ||
            firstLine == "RVX_MODEL_PREBAKE_V2")
        {
            return ResourceType::Model;
        }
        if (firstLine == "RVX_SHADER_PREBAKE_V1")
        {
            return ResourceType::Shader;
        }

        return ResourceType::Unknown;
    }

    ResourceLoadDiagnostic MakeLoadDiagnostic(const ResourcePathResolution& resolution,
                                              bool success,
                                              ResourceLoadFailureCode failure,
                                              const std::string& message)
    {
        ResourceLoadDiagnostic diagnostic;
        diagnostic.attempted = true;
        diagnostic.success = success;
        diagnostic.domain = resolution.domain;
        diagnostic.failure = failure;
        diagnostic.requestedPath = resolution.requestedPath;
        diagnostic.resolvedPath = resolution.resolvedPath;
        diagnostic.packageName = resolution.packageName;
        diagnostic.packageMountPriority = resolution.packageMountPriority;
        diagnostic.packageLogicalPath = resolution.packageLogicalPath;
        diagnostic.packageArtifactPath = resolution.packageArtifactPath;
        diagnostic.packageExpectedContentHash = resolution.packageExpectedContentHash;
        diagnostic.packageActualContentHash = resolution.packageActualContentHash;
        diagnostic.message = message;
        diagnostic.sourceAssetRead = resolution.sourceAssetRead;
        diagnostic.cookedArtifactRead = resolution.cookedArtifactRead;
        diagnostic.runtimePackageRead = resolution.runtimePackageRead;
        diagnostic.packageHashChecked = resolution.packageHashChecked;
        diagnostic.packageHashMatched = resolution.packageHashMatched;
        return diagnostic;
    }

    ResourceType DetectResourceType(const std::string& resolvedPath)
    {
        const std::string extension = std::filesystem::path(resolvedPath).extension().string();
        ResourceType type = ResourceManager::GetTypeFromExtension(extension);
        if (type != ResourceType::Unknown)
        {
            return type;
        }

        std::string lowerExtension = extension;
        std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(), ::tolower);
        return lowerExtension == ".rva" ? DetectCookedArtifactType(resolvedPath)
                                        : ResourceType::Unknown;
    }

    uint64 GetCurrentThreadToken()
    {
        return static_cast<uint64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    bool VerifyPreparedContentIdentity(
        const AssetKey& assetKey,
        const PreparedResourceBundle& bundle,
        ResourceContentVerificationReceipt& outReceipt,
        ResourceLoadError& outError)
    {
        outReceipt = {};
        outReceipt.expected = assetKey.expectedContentIdentity;
        outReceipt.observed = bundle.GetObservedContentIdentity();

        if (assetKey.expectedContentIdentity.IsEmpty())
        {
            if (outReceipt.observed.IsValid())
            {
                outReceipt.status = ResourceContentVerificationStatus::Observed;
            }
            return true;
        }

        // AssetKey admission rejects malformed expectations. Keep this check
        // defensive because third-party callers may construct an AssetKey
        // directly before giving it to ResourceLoadOperationOwner.
        if (!assetKey.expectedContentIdentity.IsValid())
        {
            outError = {ResourceLoadErrorCode::InvalidRequest,
                        "The requested resource content identity is malformed."};
            return false;
        }
        if (!outReceipt.observed.IsValid())
        {
            outError = {ResourceLoadErrorCode::ContentIdentityUnavailable,
                        "The loader did not observe a valid identity for the consumed asset bytes."};
            return false;
        }
        if (outReceipt.observed != assetKey.expectedContentIdentity)
        {
            outError = {ResourceLoadErrorCode::ContentIdentityMismatch,
                        "The bytes consumed by the loader do not match the requested content identity."};
            return false;
        }

        outReceipt.status = ResourceContentVerificationStatus::Verified;
        return true;
    }

    std::string BuildAssetResourceIdentity(const AssetKey& assetKey)
    {
        // Preserve the established path-only ResourceId for the default
        // import profile, so synchronous and asynchronous path loads share
        // exactly one cache entry. Non-default output-affecting inputs get a
        // deterministic suffix and therefore cannot overwrite that entry.
        if (assetKey.importOptionsHash == 0 &&
            assetKey.platformProfileHash == 0 &&
            assetKey.loaderSchemaVersion == 1 &&
            assetKey.expectedContentIdentity.IsEmpty())
        {
            return assetKey.canonicalPath;
        }

        std::string identity = assetKey.canonicalPath + "#rvx_asset_" +
               std::to_string(static_cast<uint32>(assetKey.resourceType)) + "_" +
               std::to_string(assetKey.importOptionsHash) + "_" +
               std::to_string(assetKey.platformProfileHash) + "_" +
               std::to_string(assetKey.loaderSchemaVersion);
        if (assetKey.expectedContentIdentity.IsEmpty())
        {
            return identity;
        }

        return identity + "_content_" +
               std::to_string(assetKey.expectedContentIdentity.schemaVersion) + "_" +
               std::to_string(static_cast<uint32>(assetKey.expectedContentIdentity.domain)) + "_" +
               std::to_string(static_cast<uint32>(assetKey.expectedContentIdentity.scope)) + "_" +
               std::to_string(static_cast<uint32>(assetKey.expectedContentIdentity.algorithm)) + "_" +
               assetKey.expectedContentIdentity.digest + "_" +
               std::to_string(assetKey.expectedContentIdentity.byteCount) + "_" +
               std::to_string(assetKey.expectedContentIdentity.fileCount);
    }

    ResourceId GetResourceIdForAssetKey(const AssetKey& assetKey)
    {
        return GenerateResourceId(BuildAssetResourceIdentity(assetKey));
    }

    ResourceId GetCanonicalResolvedResourceId(const ResourcePathResolution& resolution)
    {
        return GenerateResourceId(CanonicalizeAssetPath(resolution.resolvedPath));
    }
} // namespace

ResourceManager::ResourceManager() = default;

ResourceManager::~ResourceManager()
{
    RVX_ASSERT_MSG(!m_initialized || IsOwnerThread(),
                   "ResourceManager must be destroyed on its owner/update thread");
    Shutdown();
}

const char* GetResourceHotReloadStatusName(ResourceHotReloadStatus status)
{
    switch (status)
    {
        case ResourceHotReloadStatus::Disabled: return "Disabled";
        case ResourceHotReloadStatus::Active: return "Active";
        case ResourceHotReloadStatus::UnsupportedRuntimePolicy: return "UnsupportedRuntimePolicy";
        case ResourceHotReloadStatus::Uninitialized: return "Uninitialized";
        default: return "Unknown";
    }
}

std::string ExportResourceHotReloadDiagnosticJson(const ResourceHotReloadDiagnostic& diagnostic)
{
    std::ostringstream json;
    json << "{\n";
    AppendJsonUInt64(json, "schemaVersion", RVX_RESOURCE_HOT_RELOAD_DIAGNOSTIC_SCHEMA_VERSION);
    AppendJsonString(json, "schemaId", RVX_RESOURCE_HOT_RELOAD_DIAGNOSTIC_SCHEMA_ID);
    AppendJsonString(json, "id", "resourceHotReloadDiagnosticJson");
    AppendJsonString(json, "kind", "ResourceHotReloadDiagnosticJson");
    AppendJsonString(json, "contentType", "application/json");
    AppendJsonString(json, "contentHash", "");
    AppendJsonString(json, "relativePath", "");
    AppendJsonBool(json, "requested", diagnostic.requested);
    AppendJsonBool(json, "enabled", diagnostic.enabled);
    AppendJsonBool(json, "sourceAssetAccessRequired", diagnostic.sourceAssetAccessRequired);
    AppendJsonBool(json, "watcherInitialized", diagnostic.watcherInitialized);
    AppendJsonString(json, "status", GetResourceHotReloadStatusName(diagnostic.status));
    AppendJsonUInt64(json, "statusCode", static_cast<uint32>(diagnostic.status));
    AppendJsonString(json, "message", diagnostic.message);
    AppendJsonUInt64(json, "watchedFileCount", static_cast<uint64>(diagnostic.watchedFileCount));
    AppendJsonUInt64(json,
                     "registeredResourceCount",
                     static_cast<uint64>(diagnostic.registeredResourceCount));
    AppendJsonUInt64(json, "pendingReloadCount", static_cast<uint64>(diagnostic.pendingReloadCount));
    AppendJsonUInt64(json, "totalReloadCount", static_cast<uint64>(diagnostic.totalReloadCount));
    AppendJsonUInt64(json,
                     "successfulReloadCount",
                     static_cast<uint64>(diagnostic.successfulReloadCount));
    AppendJsonUInt64(json,
                     "failedReloadCount",
                     static_cast<uint64>(diagnostic.failedReloadCount),
                     false);
    json << "}\n";
    return json.str();
}

bool SaveResourceHotReloadDiagnosticJson(const char* filename,
                                         const ResourceHotReloadDiagnostic& diagnostic)
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return false;
    }

    file << ExportResourceHotReloadDiagnosticJson(diagnostic);
    return file.good();
}

ResourceManager& ResourceManager::Get()
{
    if (!s_instance)
    {
        s_instance = new ResourceManager();
    }
    return *s_instance;
}

void ResourceManager::Initialize(const ResourceManagerConfig& config)
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    if (m_initialized || m_shuttingDown)
    {
        RVX_RESOURCE_WARN("ResourceManager already initialized");
        return;
    }

    m_config = config;
    m_ownerThreadToken = GetCurrentThreadToken();
    m_cancelledLoadCount.store(0, std::memory_order_relaxed);
    m_closureUnloadRequestCount.store(0, std::memory_order_relaxed);
    m_closureUnloadedResourceCount.store(0, std::memory_order_relaxed);
    m_closureRetainedResourceCount.store(0, std::memory_order_relaxed);
    m_closureUnloadRejectedCount.store(0, std::memory_order_relaxed);
    Diagnostics::TraceSpan initializationSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "Resource.Manager.Initialize");
    m_registry = std::make_unique<ResourceRegistry>();
    m_cache = std::make_unique<ResourceCache>(config.cacheConfig);
    m_assetResidencyControl = std::make_shared<AssetResidencyLeaseControl>(
        [this](const AssetKey& assetKey, uint64 generation)
        {
            try
            {
                std::lock_guard<std::mutex> lock(m_pendingLeaseUnloadMutex);
                m_pendingLeaseUnloads.emplace_back(assetKey, generation);
                return true;
            }
            catch (...)
            {
                return false;
            }
        });
    const std::weak_ptr<AssetResidencyLeaseControl> residencyControl =
        m_assetResidencyControl;
    m_cache->SetCanRemoveCallback(
        [residencyControl](ResourceId resourceId)
        {
            const std::shared_ptr<AssetResidencyLeaseControl> control =
                residencyControl.lock();
            return !control || !control->HasLiveLeaseForResource(
                                   resourceId,
                                   true);
        });
    m_cache->SetBeforeRemoveCallback(
        [this](IResource* resource)
        {
            QueueLifecycleEvent(ResourceLifecycleEventType::BeforeUnload,
                                resource);
        });
    m_dependencyGraph = std::make_unique<DependencyGraph>();
    // Register default loaders
    RegisterDefaultLoaders();

    m_initialized = true;
    ConfigureHotReload(config.enableHotReload);
    StartAsyncWorkers();
    m_modelTextureStreaming =
        std::make_unique<ModelTextureStreamingService>(
            m_config.modelTextureDecodedByteBudget,
            m_config.modelTextureMaxConcurrentDecodes);
    // Publish admission only after every object observed by RequestAsync is
    // initialized. This release pairs with request-side acquire checks.
    m_acceptAsyncRequests.store(true, std::memory_order_release);
    initializationSpan.SetAttribute("result", "initialized");
    RVX_RESOURCE_INFO("ResourceManager initialized");
}

void ResourceManager::RegisterDefaultLoaders()
{
    // Register TextureLoader
    auto textureLoader = std::make_unique<TextureLoader>(this);
    RegisterLoader(ResourceType::Texture, std::move(textureLoader));

    // Register MeshLoader
    auto meshLoader = std::make_unique<MeshLoader>(this);
    RegisterLoader(ResourceType::Mesh, std::move(meshLoader));

    // Register ShaderLoader
    auto shaderLoader = std::make_unique<ShaderLoader>(this);
    RegisterLoader(ResourceType::Shader, std::move(shaderLoader));

    // Register ModelLoader
    auto modelLoader = std::make_unique<ModelLoader>(this);
    RegisterLoader(ResourceType::Model, std::move(modelLoader));

    auto animationLoader = std::make_unique<AnimationLoader>();
    RegisterLoader(ResourceType::Animation, std::move(animationLoader));

    auto environmentLoader = std::make_unique<EnvironmentLoader>();
    RegisterLoader(ResourceType::Environment, std::move(environmentLoader));

    RVX_RESOURCE_INFO("Registered default resource loaders");
}

void ResourceManager::Shutdown()
{
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        if (!m_initialized || m_shuttingDown)
        {
            return;
        }
        if (!IsOwnerThread())
        {
            RVX_RESOURCE_ERROR("ResourceManager::Shutdown must run on its owner/update thread");
            return;
        }

        // Block loaded-resource acquisition before worker cancellation and
        // serialize the final cache teardown with m_loadMutex below.
        m_shuttingDown = true;
        m_initialized = false;
    }

    // Stop accepting new subscriptions before waiting.  Existing workers retain
    // only loader snapshots and prepared data; they cannot reach manager-owned
    // cache/registry state.
    m_acceptAsyncRequests.store(false, std::memory_order_release);

    // Waiting before signalling cancellation can deadlock a cooperative
    // loader blocked in IO/decode. Request cancellation for every operation
    // first; workers retain their loader snapshots and will stage only a
    // cancelled completion.
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        for (auto& [assetKey, operation] : m_inFlightLoads)
        {
            (void)assetKey;
            operation.CompleteCancelled("ResourceManager is shutting down.");
        }
    }

    if (m_hotReloadCallbackId != 0 &&
        HotReloadManager::Get().IsInitialized())
    {
        HotReloadManager::Get().RemoveReloadCallback(m_hotReloadCallbackId);
        m_hotReloadCallbackId = 0;
    }
    if (m_hotReloadInitializedByManager && HotReloadManager::Get().IsInitialized())
    {
        HotReloadManager::Get().Shutdown();
    }
    {
        std::lock_guard<std::mutex> lock(m_hotReloadMutex);
        m_hotReloadWatchIds.clear();
        m_hotReloadInitializedByManager = false;
    }
    SetHotReloadDiagnostic(ResourceHotReloadStatus::Uninitialized,
                           m_config.enableHotReload,
                           false,
                           "ResourceManager is shut down.");

    if (m_modelTextureStreaming)
        m_modelTextureStreaming->Stop();
    StopAsyncWorkers();
    DrainPreparedLoadCompletions(true);
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        m_inFlightLoads.clear();
    }
    DrainLegacyAsyncWaiters();
    ProcessCompletedLoads();

    std::lock_guard<std::recursive_mutex> shutdownLock(m_loadMutex);
    m_loaders.clear();
    m_cache->ClearInternal(true);
    m_registry->Clear();
    m_dependencyGraph->Clear();
    m_publishedAssetKeys.clear();
    m_modelTextureStreaming.reset();
    if (m_assetResidencyControl)
    {
        m_assetResidencyControl->StopAcceptingLeases();
        m_assetResidencyControl.reset();
    }
    {
        std::lock_guard<std::mutex> pendingLeaseLock(m_pendingLeaseUnloadMutex);
        m_pendingLeaseUnloads.clear();
    }

    m_shuttingDown = false;
    DrainLifecycleEvents();
    SetLifecycleEventCallback({});
    SetClosureRetirementCallback({});
    m_reloadCallback = {};
    RVX_RESOURCE_INFO("ResourceManager shutdown");
}

bool ResourceManager::IsInitialized() const
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    return m_initialized && !m_shuttingDown;
}

IResource* ResourceManager::LoadResource(const std::string& path)
{
    return LoadResource(path, ResourceType::Unknown);
}

IResource* ResourceManager::LoadResource(const std::string& path,
                                         ResourceType requestedType)
{
    if (!m_initialized)
    {
        RVX_RESOURCE_ERROR("ResourceManager not initialized");
        return nullptr;
    }

    Diagnostics::TraceSpan resolveSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "AssetResolve",
        {{"requestedPath", path}});
    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(m_config.runtimePolicy, m_config.basePath, path);
    resolveSpan.SetAttribute("resolvedPath", resolution.resolvedPath);
    resolveSpan.SetAttribute("allowed", resolution.allowed);
    if (!resolution.allowed)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution, false, resolution.failure, resolution.diagnosticMessage));
        RVX_RESOURCE_ERROR("Resource load denied for '{}': {} ({})",
                           path,
                           resolution.diagnosticMessage,
                           GetResourceLoadFailureCodeName(resolution.failure));
        return nullptr;
    }

    const ResourceType detectedType = DetectResourceType(resolution.resolvedPath);
    const ResourceType type = requestedType != ResourceType::Unknown
                                  ? requestedType
                                  : detectedType;

    // Snapshot the loader before deriving the identity. Stateful loaders must
    // contribute their immutable import profile to synchronous cache lookup as
    // well, otherwise sync and prepared paths would create two root meanings.
    std::shared_ptr<IResourceLoader> loader;
    uint64 canonicalImportOptionsHash = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        const auto loaderIt = m_loaders.find(type);
        if (loaderIt != m_loaders.end())
        {
            loader = loaderIt->second;
        }
    }
    if (!loader || !loader->CanLoad(resolution.resolvedPath))
    {
        RVX_RESOURCE_ERROR("No compatible loader registered for resource type: {}",
                           GetResourceTypeName(type));
        return nullptr;
    }

    ResourceLoadPreparationStateRef loaderState;
    ResourceLoadError captureError;
    if (!loader->CapturePreparationState(0,
                                         loaderState,
                                         canonicalImportOptionsHash,
                                         captureError))
    {
        RVX_RESOURCE_ERROR("Resource loader rejected the active import profile for '{}': {}",
                           path,
                           captureError.message);
        return nullptr;
    }
    const AssetKey assetKey = MakeAssetKey(resolution.resolvedPath,
                                           type,
                                           canonicalImportOptionsHash,
                                           0,
                                           1);
    const ResourceId id = GetResourceIdForAssetKey(assetKey);

    // Check cache first
    IResource* cached = m_cache->Get(id);
    Diagnostics::RecordTraceInstant(
        m_config.startupTraceContext,
        "CacheLookup",
        {{"requestedPath", path},
         {"resourceId", id},
         {"cacheHit", cached != nullptr}});
    if (cached != nullptr)
    {
        bool reactivationFailed = false;
        {
            std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
            const auto published = m_publishedAssetKeys.find(id);
            reactivationFailed = published != m_publishedAssetKeys.end() &&
                                 !ReactivatePublishedRoot(id, assetKey);
        }
        if (reactivationFailed)
        {
            SetLastLoadDiagnostic(
                MakeLoadDiagnostic(resolution,
                                   false,
                                   ResourceLoadFailureCode::LoaderFailed,
                                   "Could not reactivate the exact root ownership claim."));
            return nullptr;
        }
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               true,
                               ResourceLoadFailureCode::None,
                               "Resource returned from cache."));
        Diagnostics::RecordTraceInstant(
            m_config.startupTraceContext,
            "CacheHit",
            {{"requestedPath", path}, {"resourceId", id}});
        return cached;
    }

    if (loader->SupportsPreparedLoading())
    {
        return LoadPreparedSynchronously(resolution,
                                         path,
                                         assetKey,
                                         std::move(loader),
                                         std::move(loaderState));
    }

    return LoadInternal(path, resolution, type, id, std::move(loader));
}

IResource* ResourceManager::LoadResource(ResourceId id)
{
    if (!m_initialized) return nullptr;

    Diagnostics::TraceSpan resolveSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "AssetResolve",
        {{"resourceId", id}});

    // Check cache first
    IResource* cached = m_cache->Get(id);
    Diagnostics::RecordTraceInstant(
        m_config.startupTraceContext,
        "CacheLookup",
        {{"resourceId", id}, {"cacheHit", cached != nullptr}});
    if (cached != nullptr)
    {
        Diagnostics::RecordTraceInstant(
            m_config.startupTraceContext,
            "CacheHit",
            {{"resourceId", id}});
        return cached;
    }

    // Look up path in registry
    auto metadata = m_registry->FindById(id);
    if (!metadata)
    {
        RVX_RESOURCE_ERROR("Resource not found in registry: {}", id);
        return nullptr;
    }

    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(m_config.runtimePolicy, m_config.basePath, metadata->path);
    resolveSpan.SetAttribute("requestedPath", metadata->path);
    resolveSpan.SetAttribute("resolvedPath", resolution.resolvedPath);
    resolveSpan.SetAttribute("allowed", resolution.allowed);
    if (!resolution.allowed)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution, false, resolution.failure, resolution.diagnosticMessage));
        RVX_RESOURCE_ERROR("Resource load denied for '{}': {} ({})",
                           metadata->path,
                           resolution.diagnosticMessage,
                           GetResourceLoadFailureCodeName(resolution.failure));
        return nullptr;
    }

    // ResourceId identifies an exact AssetKey variant, not merely a source
    // path. Registry metadata from older schemas does not retain the immutable
    // loader payload for arbitrary import/platform profiles. Recreate the
    // currently active loader identity and fail closed when it cannot produce
    // the requested id; silently applying today's options to an old variant id
    // would corrupt the cache's identity contract.
    std::shared_ptr<IResourceLoader> loader;
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        const auto loaderIt = m_loaders.find(metadata->type);
        if (loaderIt != m_loaders.end())
        {
            loader = loaderIt->second;
        }
    }
    if (!loader || !loader->CanLoad(resolution.resolvedPath))
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderUnavailable,
                               "No compatible loader is available for the registered resource."));
        return nullptr;
    }

    ResourceLoadPreparationStateRef loaderState;
    uint64 canonicalImportOptionsHash = 0;
    ResourceLoadError captureError;
    if (!loader->CapturePreparationState(0,
                                         loaderState,
                                         canonicalImportOptionsHash,
                                         captureError))
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::AssetIdentityMismatch,
                               "The registered resource import profile cannot be reconstructed: " +
                                   captureError.message));
        return nullptr;
    }

    const AssetKey currentAssetKey = MakeAssetKey(resolution.resolvedPath,
                                                   metadata->type,
                                                   canonicalImportOptionsHash,
                                                   0,
                                                   1);
    if (GetResourceIdForAssetKey(currentAssetKey) != id)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::AssetIdentityMismatch,
                               "The registered ResourceId belongs to an import/profile variant "
                               "that is not reproducible by the active loader configuration."));
        RVX_RESOURCE_ERROR("Refusing to reload resource {} from '{}' with a different AssetKey profile",
                           id,
                           metadata->path);
        return nullptr;
    }

    if (loader->SupportsPreparedLoading())
    {
        return LoadPreparedSynchronously(resolution,
                                         metadata->path,
                                         currentAssetKey,
                                         std::move(loader),
                                         std::move(loaderState));
    }

    return LoadInternal(metadata->path,
                        resolution,
                        metadata->type,
                        id,
                        std::move(loader));
}

IResource* ResourceManager::LoadPreparedSynchronously(
    const ResourcePathResolution& resolution,
    const std::string& requestedPath,
    const AssetKey& assetKey,
    std::shared_ptr<IResourceLoader> loader,
    ResourceLoadPreparationStateRef loaderState)
{
    if (!IsOwnerThread())
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderFailed,
                               "Prepared synchronous publication must run on the ResourceManager owner thread."));
        RVX_RESOURCE_ERROR("Prepared synchronous load for '{}' was requested off the owner thread",
                           requestedPath);
        return nullptr;
    }

    ResourceLoadPreparationContext context;
    context.assetKey = assetKey;
    context.resourceIdentityPath = BuildAssetResourceIdentity(assetKey);
    context.requestedPath = requestedPath;
    context.resolvedPath = resolution.resolvedPath;
    context.rootResourceId = GetResourceIdForAssetKey(assetKey);
    context.traceContext = m_config.startupTraceContext;
    context.loaderState = std::move(loaderState);

    PreparedResourceBundle bundle;
    ResourceLoadError error;
    if (!loader || !loader->Prepare(context, bundle, error))
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderFailed,
                               error.message.empty()
                                   ? "The loader failed to prepare the resource."
                                   : error.message));
        return nullptr;
    }

    const ResourceHandle<IResource> root = bundle.GetRoot();
    if (!root || !PublishPreparedBundle(resolution,
                                        requestedPath,
                                        assetKey,
                                        bundle,
                                        error))
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderFailed,
                               error.message.empty()
                                   ? "The prepared resource could not be published."
                                   : error.message));
        return nullptr;
    }

    return root.Get();
}

IResource* ResourceManager::LoadInternal(const std::string& path,
                                         const ResourcePathResolution& resolution,
                                         ResourceType type,
                                         ResourceId managerId,
                                         std::shared_ptr<IResourceLoader> loader)
{
    Diagnostics::TraceSpan loadSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "ResourceLoad",
        {{"requestedPath", path},
         {"resolvedPath", resolution.resolvedPath},
         {"resourceType", GetResourceTypeName(type)}});
    const std::string& resolvedPath = resolution.resolvedPath;

    // Snapshot the loader under the manager lock, then perform IO/parse/decode
    // after releasing it. The shared ownership keeps the loader alive through
    // synchronous work even if shutdown or replacement begins elsewhere.
    if (!loader)
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        const auto loaderIt = m_loaders.find(type);
        if (loaderIt != m_loaders.end())
        {
            loader = loaderIt->second;
        }
    }
    if (!loader)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderUnavailable,
                               "No loader registered for resource type."));
        RVX_RESOURCE_ERROR("No loader registered for resource type: {}", GetResourceTypeName(type));
        loadSpan.SetAttribute("result", "loader-unavailable");
        return nullptr;
    }

    // Load the resource
    IResource* resource = loader->Load(resolvedPath);
    if (!resource)
    {
        SetLastLoadDiagnostic(
            MakeLoadDiagnostic(resolution,
                               false,
                               ResourceLoadFailureCode::LoaderFailed,
                               "Loader failed to create the resource."));
        RVX_RESOURCE_ERROR("Failed to load resource: {}", path);
        loadSpan.SetAttribute("result", "loader-failed");
        return nullptr;
    }

    Diagnostics::TraceSpan publishSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "CPUPublish",
        {{"requestedPath", path},
         {"resourceType", GetResourceTypeName(type)}});

    // Some loaders cache specialized variants internally while loading. The
    // ResourceManager path API owns the generic path identity, so remove any
    // loader-assigned cache entry before rewriting the ID to avoid one object
    // being stored under two keys.
    const ResourceId loaderAssignedId = resource->GetId();
    bool releaseLoaderCacheRetainAfterStore = false;
    if (loaderAssignedId != InvalidResourceId && loaderAssignedId != managerId &&
        m_cache->Contains(loaderAssignedId))
    {
        resource->AddRef();
        static_cast<void>(m_cache->Remove(loaderAssignedId));
        releaseLoaderCacheRetainAfterStore = true;
    }

    resource->SetId(managerId);
    resource->SetPath(path);
    resource->SetName(std::filesystem::path(path).stem().string());

    // Load dependencies
    LoadDependencies(resource);

    // Register in registry
    ResourceMetadata metadata;
    metadata.id = resource->GetId();
    metadata.path = path;
    metadata.name = resource->GetName();
    metadata.type = type;
    metadata.dependencies = resource->GetAllDependencies();
    m_registry->Register(metadata);

    // Add to dependency graph
    m_dependencyGraph->AddResource(resource->GetId(), metadata.dependencies);

    // Store in cache
    m_cache->Store(resource);
    if (releaseLoaderCacheRetainAfterStore)
    {
        resource->Release();
    }

    // Mark as loaded
    resource->NotifyLoaded();
    QueueLifecycleEvent(ResourceLifecycleEventType::Ready, resource);
    publishSpan.SetAttribute("resourceId", resource->GetId());
    publishSpan.SetAttribute("result", "published");

    SetLastLoadDiagnostic(
        MakeLoadDiagnostic(resolution,
                           true,
                           ResourceLoadFailureCode::None,
                           "Resource loaded successfully."));
    RegisterHotReloadResource(resource, resolution);

    loadSpan.SetAttribute("result", "loaded");
    Diagnostics::RecordTraceInstant(
        m_config.startupTraceContext,
        "CPUReady",
        {{"requestedPath", path}, {"resourceId", resource->GetId()}});
    RVX_RESOURCE_DEBUG("Loaded resource: {} (type: {})", path, GetResourceTypeName(type));
    return resource;
}

ResourceLoadDiagnostic ResourceManager::GetLastLoadDiagnostic() const
{
    std::lock_guard<std::mutex> lock(m_diagnosticMutex);
    return m_lastLoadDiagnostic;
}

std::string ResourceManager::ExportLastLoadDiagnosticJson() const
{
    return ExportResourceLoadDiagnosticJson(GetLastLoadDiagnostic());
}

bool ResourceManager::SaveLastLoadDiagnosticJson(const char* filename) const
{
    return SaveResourceLoadDiagnosticJson(filename, GetLastLoadDiagnostic());
}

void ResourceManager::SetLastLoadDiagnostic(const ResourceLoadDiagnostic& diagnostic)
{
    std::lock_guard<std::mutex> lock(m_diagnosticMutex);
    m_lastLoadDiagnostic = diagnostic;
}

bool ResourceManager::IsHotReloadSupportedByPolicy() const
{
    return m_config.runtimePolicy.allowSourceAssetReads &&
           !m_config.runtimePolicy.requireCookedArtifacts &&
           !m_config.runtimePolicy.requireRuntimePackage;
}

void ResourceManager::SetHotReloadDiagnostic(ResourceHotReloadStatus status,
                                             bool requested,
                                             bool enabled,
                                             const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_hotReloadMutex);

    m_hotReloadDiagnostic.requested = requested;
    m_hotReloadDiagnostic.enabled = enabled;
    m_hotReloadDiagnostic.sourceAssetAccessRequired = true;
    m_hotReloadDiagnostic.status = status;
    m_hotReloadDiagnostic.message = message;
    m_hotReloadDiagnostic.watcherInitialized = HotReloadManager::Get().IsInitialized();

    if (HotReloadManager::Get().IsInitialized())
    {
        const HotReloadManager::ReloadStats stats = HotReloadManager::Get().GetStats();
        m_hotReloadDiagnostic.watchedFileCount = stats.watchedFiles;
        m_hotReloadDiagnostic.registeredResourceCount = stats.registeredResources;
        m_hotReloadDiagnostic.pendingReloadCount = HotReloadManager::Get().GetPendingReloadCount();
        m_hotReloadDiagnostic.totalReloadCount = stats.totalReloads;
        m_hotReloadDiagnostic.successfulReloadCount = stats.successfulReloads;
        m_hotReloadDiagnostic.failedReloadCount = stats.failedReloads;
    }
    else
    {
        m_hotReloadDiagnostic.watchedFileCount = 0;
        m_hotReloadDiagnostic.registeredResourceCount = 0;
        m_hotReloadDiagnostic.pendingReloadCount = 0;
        m_hotReloadDiagnostic.totalReloadCount = 0;
        m_hotReloadDiagnostic.successfulReloadCount = 0;
        m_hotReloadDiagnostic.failedReloadCount = 0;
    }
}

void ResourceManager::ConfigureHotReload(bool enable)
{
    const bool supported = IsHotReloadSupportedByPolicy();
    m_config.enableHotReload = enable && supported;

    if (!enable)
    {
        if (HotReloadManager::Get().IsInitialized())
        {
            HotReloadManager::Get().SetEnabled(false);
        }
        SetHotReloadDiagnostic(ResourceHotReloadStatus::Disabled,
                               false,
                               false,
                               "Hot reload is disabled by configuration.");
        return;
    }

    if (!supported)
    {
        if (HotReloadManager::Get().IsInitialized())
        {
            HotReloadManager::Get().SetEnabled(false);
        }
        SetHotReloadDiagnostic(ResourceHotReloadStatus::UnsupportedRuntimePolicy,
                               true,
                               false,
                               "Hot reload requires source asset reads and is disabled for cooked/package runtime policies.");
        RVX_RESOURCE_WARN("ResourceManager: hot reload requested but rejected by runtime resource policy");
        return;
    }

    HotReloadConfig hotReloadConfig;
    hotReloadConfig.enabled = true;
    if (!HotReloadManager::Get().IsInitialized())
    {
        HotReloadManager::Get().Initialize(this, hotReloadConfig);
        m_hotReloadInitializedByManager = true;
    }
    else
    {
        HotReloadManager::Get().SetEnabled(true);
    }

    if (m_hotReloadCallbackId == 0)
    {
        m_hotReloadCallbackId = HotReloadManager::Get().OnReload(
            [this](const ReloadEvent& event)
            {
                if (event.success && event.newResource != nullptr)
                {
                    QueueLifecycleEvent(ResourceLifecycleEventType::Reloaded,
                                        event.newResource);
                }
            });
    }

    SetHotReloadDiagnostic(ResourceHotReloadStatus::Active,
                           true,
                           true,
                           "Hot reload is active for source asset resources.");
}

void ResourceManager::RegisterHotReloadResource(IResource* resource, const ResourcePathResolution& resolution)
{
    if (!resource || !m_config.enableHotReload || resolution.domain != ResourceLoadDomain::SourceAsset ||
        !HotReloadManager::Get().IsInitialized())
    {
        return;
    }

    // An expected consumed-byte identity pins this publication to the exact
    // verified artifact. Hot reload's unload-first replacement semantics
    // cannot preserve that contract, so this root is deliberately untracked.
    if (!resource->GetContentVerificationReceipt().expected.IsEmpty())
    {
        return;
    }

    HotReloadManager& hotReload = HotReloadManager::Get();
    hotReload.RegisterResource(resource, resolution.resolvedPath);

    const ResourceId id = resource->GetId();
    {
        std::lock_guard<std::mutex> lock(m_hotReloadMutex);
        if (m_hotReloadWatchIds.find(id) != m_hotReloadWatchIds.end())
        {
            return;
        }
    }

    const uint32_t watchId = hotReload.WatchFile(resolution.resolvedPath);
    {
        std::lock_guard<std::mutex> lock(m_hotReloadMutex);
        if (watchId != 0)
        {
            m_hotReloadWatchIds[id] = watchId;
        }
    }

    SetHotReloadDiagnostic(ResourceHotReloadStatus::Active,
                           true,
                           true,
                           watchId == 0
                               ? "Hot reload is active, but watcher registration failed for the resource path."
                               : "Hot reload is active and tracking loaded source assets.");
}

void ResourceManager::LoadDependencies(IResource* resource)
{
    auto dependencies = resource->GetRequiredDependencies();
    for (ResourceId depId : dependencies)
    {
        // Load each dependency (will be cached)
        LoadResource(depId);
    }
}

void ResourceManager::LoadBatch(const std::vector<std::string>& paths,
                                 std::function<void(float)> onProgress,
                                 std::function<void()> onComplete)
{
    size_t total = paths.size();
    size_t loaded = 0;

    for (const auto& path : paths)
    {
        LoadResource(path);
        loaded++;

        if (onProgress)
        {
            onProgress(static_cast<float>(loaded) / static_cast<float>(total));
        }
    }

    if (onComplete)
    {
        onComplete();
    }
}

bool ResourceManager::IsLoaded(const std::string& path) const
{
    if (!m_initialized || !m_cache)
    {
        return false;
    }

    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(m_config.runtimePolicy, m_config.basePath, path);
    return resolution.allowed &&
           m_cache->ContainsLoaded(GetCanonicalResolvedResourceId(resolution));
}

bool ResourceManager::IsLoaded(const AssetKey& assetKey) const
{
    if (!m_initialized || !m_cache || !assetKey.IsValid())
    {
        return false;
    }

    const ResourceId resourceId = GetResourceIdForAssetKey(assetKey);
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    return IsPublishedRootActive(resourceId, assetKey) &&
           m_cache->ContainsLoaded(resourceId);
}

bool ResourceManager::IsLoaded(ResourceId id) const
{
    return m_cache && m_cache->ContainsLoaded(id);
}

MaterialInstanceCreateResult ResourceManager::CreateMaterialInstance(
    const ResourceHandle<MaterialResource>& parent,
    const std::string& runtimeKey)
{
    MaterialInstanceCreateResult result;
    const auto fail = [&result](MaterialInstanceMutationCode code,
                                ResourceId resourceId,
                                std::string message)
    {
        result.receipt.code = code;
        result.receipt.resourceId = resourceId;
        result.receipt.message = std::move(message);
        return result;
    };

    if (!m_initialized || !m_cache || !m_registry || !m_dependencyGraph)
    {
        return fail(MaterialInstanceMutationCode::ManagerNotInitialized,
                    InvalidResourceId,
                    "ResourceManager must be initialized before creating a material instance.");
    }
    if (!IsOwnerThread())
    {
        return fail(MaterialInstanceMutationCode::OwnerThreadRequired,
                    InvalidResourceId,
                    "Runtime material instances can only be created on the ResourceManager owner/update thread.");
    }
    if (!parent || !parent.IsLoaded() || runtimeKey.empty())
    {
        return fail(MaterialInstanceMutationCode::InvalidParent,
                    InvalidResourceId,
                    "A loaded parent material and non-empty runtime key are required.");
    }

    const AssetKey runtimeAssetKey = MakeAssetKey(
        "runtime://material-instance/" + runtimeKey,
        ResourceType::Material);
    const ResourceId resourceId = GetResourceIdForAssetKey(runtimeAssetKey);
    result.assetKey = runtimeAssetKey;

    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    IResource* cachedParent = m_cache->Get(parent.GetId());
    if (cachedParent != parent.Get() || !m_cache->ContainsLoaded(parent.GetId()) ||
        !m_registry->Contains(parent.GetId()) || !m_dependencyGraph->Contains(parent.GetId()))
    {
        return fail(MaterialInstanceMutationCode::InvalidParent,
                    resourceId,
                    "The parent material is not the ResourceManager-published material identity.");
    }
    if (m_cache->Contains(resourceId) || m_registry->Contains(resourceId) ||
        m_dependencyGraph->Contains(resourceId) || m_publishedAssetKeys.contains(resourceId))
    {
        return fail(MaterialInstanceMutationCode::RuntimeKeyConflict,
                    resourceId,
                    "The runtime material-instance key already resolves to a live resource identity.");
    }

    auto* rawInstance = new MaterialInstanceResource(parent, runtimeKey);
    MaterialInstanceHandle instance(rawInstance);
    rawInstance->SetId(resourceId);
    rawInstance->SetPath(runtimeAssetKey.canonicalPath);
    rawInstance->SetName(runtimeKey);
    if (!rawInstance->InitializeFromParent())
    {
        return fail(MaterialInstanceMutationCode::InvalidParent,
                    resourceId,
                    "The immutable parent material could not initialize an instance state.");
    }

    const std::vector<ResourceId> dependencies = rawInstance->GetRequiredDependencies();
    if (dependencies.empty())
    {
        return fail(MaterialInstanceMutationCode::InvalidParent,
                    resourceId,
                    "A material instance must retain its immutable parent as a dependency.");
    }
    for (ResourceId dependencyId : dependencies)
    {
        if (!m_cache->ContainsLoaded(dependencyId))
        {
            return fail(MaterialInstanceMutationCode::DependencyUnavailable,
                        resourceId,
                        "A parent material dependency is not published and loaded.");
        }
    }

    ResourceMetadata metadata;
    metadata.id = resourceId;
    metadata.path = runtimeAssetKey.canonicalPath;
    metadata.name = runtimeKey;
    metadata.type = ResourceType::Material;
    metadata.dependencies = dependencies;

    // Identity and releaseable ownership share one record. Reserve before any
    // owner-database mutation so a runtime root cannot publish without its
    // initial Active claim.
    try
    {
        m_publishedAssetKeys.reserve(m_publishedAssetKeys.size() + 1);
    }
    catch (const std::exception& error)
    {
        return fail(MaterialInstanceMutationCode::TransactionFailed,
                    resourceId,
                    error.what());
    }
    catch (...)
    {
        return fail(MaterialInstanceMutationCode::TransactionFailed,
                    resourceId,
                    "Could not reserve runtime material-instance ownership state.");
    }

    bool assetKeyInserted = false;
    bool registryInserted = false;
    bool graphInserted = false;
    try
    {
        assetKeyInserted = m_publishedAssetKeys.emplace(
            resourceId,
            PublishedRootRecord{runtimeAssetKey, RootOwnershipState::Active}).second;
        if (!assetKeyInserted)
        {
            throw std::runtime_error("Runtime material-instance AssetKey collision.");
        }
        // Mark owner-table rollback eligibility before each call. Registry and
        // dependency-graph implementations may allocate after their first
        // insertion, so an exception does not prove that no partial mutation
        // occurred.
        registryInserted = true;
        m_registry->Register(metadata);
        graphInserted = true;
        m_dependencyGraph->AddResource(resourceId, dependencies);
        if (!m_cache->StorePreparedBatch({rawInstance}))
        {
            throw std::runtime_error("Resource cache rejected the material-instance publication.");
        }
    }
    catch (const std::exception& error)
    {
        if (graphInserted)
        {
            m_dependencyGraph->RemoveResource(resourceId);
        }
        if (registryInserted)
        {
            m_registry->Unregister(resourceId);
        }
        if (assetKeyInserted)
        {
            m_publishedAssetKeys.erase(resourceId);
        }
        return fail(MaterialInstanceMutationCode::TransactionFailed,
                    resourceId,
                    error.what());
    }
    catch (...)
    {
        if (graphInserted)
        {
            m_dependencyGraph->RemoveResource(resourceId);
        }
        if (registryInserted)
        {
            m_registry->Unregister(resourceId);
        }
        if (assetKeyInserted)
        {
            m_publishedAssetKeys.erase(resourceId);
        }
        return fail(MaterialInstanceMutationCode::TransactionFailed,
                    resourceId,
                    "An unknown failure interrupted material-instance publication.");
    }

    QueueLifecycleEvent(ResourceLifecycleEventType::Ready, rawInstance);
    result.instance = std::move(instance);
    result.receipt.code = MaterialInstanceMutationCode::Applied;
    result.receipt.resourceId = resourceId;
    result.receipt.revision = rawInstance->GetRevision();
    result.receipt.message = "Runtime material instance published.";
    return result;
}

MaterialInstanceMutationReceipt ResourceManager::UpdateMaterialInstance(
    const MaterialInstanceHandle& instance,
    const MaterialInstancePatch& patch)
{
    MaterialInstanceMutationReceipt receipt;
    receipt.resourceId = instance ? instance.GetId() : InvalidResourceId;
    if (!m_initialized || !m_cache || !m_registry || !m_dependencyGraph)
    {
        receipt.code = MaterialInstanceMutationCode::ManagerNotInitialized;
        receipt.message = "ResourceManager must be initialized before updating a material instance.";
        return receipt;
    }
    if (!IsOwnerThread())
    {
        receipt.code = MaterialInstanceMutationCode::OwnerThreadRequired;
        receipt.message = "Runtime material instances can only be updated on the ResourceManager owner/update thread.";
        return receipt;
    }
    if (!instance)
    {
        receipt.code = MaterialInstanceMutationCode::InvalidInstance;
        receipt.message = "The material-instance handle is invalid.";
        return receipt;
    }

    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    IResource* cached = m_cache->Get(instance.GetId());
    if (cached != instance.Get() || !instance.IsLoaded() ||
        !m_registry->Contains(instance.GetId()) ||
        !m_dependencyGraph->Contains(instance.GetId()))
    {
        receipt.code = MaterialInstanceMutationCode::InvalidInstance;
        receipt.message = "The material-instance handle is not the current ResourceManager-published identity.";
        return receipt;
    }

    MaterialInstanceResource::PreparedState prepared;
    std::string message;
    const MaterialInstanceMutationCode buildCode =
        instance->BuildPatchedState(patch, prepared, message);
    if (buildCode != MaterialInstanceMutationCode::Applied)
    {
        receipt.code = buildCode;
        receipt.revision = instance->GetRevision();
        receipt.message = std::move(message);
        return receipt;
    }

    if (instance->HasSameState(prepared))
    {
        receipt.code = MaterialInstanceMutationCode::NoChange;
        receipt.revision = instance->GetRevision();
        receipt.message = "The patch is already reflected by the published material instance.";
        return receipt;
    }

    for (const auto& [slot, texture] : prepared.textures)
    {
        (void)slot;
        if (!texture || !texture.IsLoaded() ||
            m_cache->Get(texture.GetId()) != texture.Get())
        {
            receipt.code = MaterialInstanceMutationCode::DependencyUnavailable;
            receipt.revision = instance->GetRevision();
            receipt.message =
                "A texture override is not the canonical ResourceManager-published identity.";
            return receipt;
        }
    }
    if (prepared.shader &&
        (!prepared.shader.IsLoaded() ||
         m_cache->Get(prepared.shader.GetId()) != prepared.shader.Get()))
    {
        receipt.code = MaterialInstanceMutationCode::DependencyUnavailable;
        receipt.revision = instance->GetRevision();
        receipt.message =
            "The material shader is not the canonical ResourceManager-published identity.";
        return receipt;
    }

    for (ResourceId dependencyId : prepared.dependencies)
    {
        if (!m_cache->ContainsLoaded(dependencyId))
        {
            receipt.code = MaterialInstanceMutationCode::DependencyUnavailable;
            receipt.revision = instance->GetRevision();
            receipt.message = "A patch dependency is not published and loaded.";
            return receipt;
        }
    }

    const std::vector<ResourceId> previousDependencies =
        m_dependencyGraph->GetDependencies(instance.GetId());
    const bool dependenciesChanged = previousDependencies != prepared.dependencies;
    if (dependenciesChanged && m_assetResidencyControl &&
        m_assetResidencyControl->HasLiveLeaseForResource(instance.GetId(), false))
    {
        receipt.code = MaterialInstanceMutationCode::NeedsResidencyRefresh;
        receipt.revision = instance->GetRevision();
        receipt.message = "The material instance has a live residency lease; texture dependency changes require a closure refresh contract.";
        return receipt;
    }

    const std::optional<ResourceMetadata> previousMetadata =
        m_registry->FindById(instance.GetId());
    if (!previousMetadata)
    {
        receipt.code = MaterialInstanceMutationCode::TransactionFailed;
        receipt.revision = instance->GetRevision();
        receipt.message = "The published material instance has no registry metadata to update.";
        return receipt;
    }
    ResourceMetadata updatedMetadata = *previousMetadata;
    updatedMetadata.dependencies = prepared.dependencies;

    try
    {
        // Both manager and graph boundaries remain serialized for the full
        // mutation. No Scene/Render observer can see the new material state
        // until its dependency metadata has been replaced.
        if (dependenciesChanged)
        {
            m_registry->Update(updatedMetadata);
            m_dependencyGraph->UpdateDependencies(instance.GetId(), prepared.dependencies);
        }
        instance->CommitPatchedState(std::move(prepared));
    }
    catch (const std::exception& error)
    {
        // CommitPatchedState is noexcept and is intentionally last. A failure
        // before it leaves the material unchanged; restore metadata best-effort
        // in case an allocator failed inside one of the owner databases.
        if (dependenciesChanged)
        {
            try
            {
                m_registry->Update(*previousMetadata);
                m_dependencyGraph->UpdateDependencies(instance.GetId(), previousDependencies);
            }
            catch (...)
            {
                RVX_RESOURCE_ERROR("Material-instance dependency rollback failed for {}", instance.GetId());
            }
        }
        receipt.code = MaterialInstanceMutationCode::TransactionFailed;
        receipt.revision = instance->GetRevision();
        receipt.message = error.what();
        return receipt;
    }
    catch (...)
    {
        if (dependenciesChanged)
        {
            try
            {
                m_registry->Update(*previousMetadata);
                m_dependencyGraph->UpdateDependencies(instance.GetId(), previousDependencies);
            }
            catch (...)
            {
                RVX_RESOURCE_ERROR("Material-instance dependency rollback failed for {}", instance.GetId());
            }
        }
        receipt.code = MaterialInstanceMutationCode::TransactionFailed;
        receipt.revision = instance->GetRevision();
        receipt.message = "An unknown failure interrupted the material-instance transaction.";
        return receipt;
    }

    receipt.code = MaterialInstanceMutationCode::Applied;
    receipt.revision = instance->GetRevision();
    receipt.message = "Runtime material instance updated.";
    QueueLifecycleEvent(ResourceLifecycleEventType::Reloaded, instance.Get());
    return receipt;
}

AssetResidencyReleaseResult ResourceManager::Unload(const std::string& path)
{
    if (!m_initialized)
    {
        return AssetResidencyReleaseResult::NotFound;
    }

    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(m_config.runtimePolicy, m_config.basePath, path);
    if (resolution.allowed)
    {
        return Unload(GetCanonicalResolvedResourceId(resolution));
    }
    return AssetResidencyReleaseResult::NotFound;
}

AssetResidencyReleaseResult ResourceManager::Unload(ResourceId id)
{
    if (id == InvalidResourceId)
    {
        return AssetResidencyReleaseResult::NotFound;
    }
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    const auto publishedRoot = m_publishedAssetKeys.find(id);
    if (publishedRoot != m_publishedAssetKeys.end())
    {
        // Route published roots through their exact AssetKey path. A
        // ReleasePending record represents a retryable explicit request, not
        // an absent identity, and this avoids a second state machine for id
        // callers.
        const AssetKey publishedAssetKey = publishedRoot->second.assetKey;
        return Unload(publishedAssetKey);
    }
    if (m_assetResidencyControl &&
        m_assetResidencyControl->HasLiveLeaseForResource(id, false))
    {
        return AssetResidencyReleaseResult::BlockedByLease;
    }
    return UnloadResourceNow(id);
}

AssetResidencyReleaseResult ResourceManager::Unload(const AssetKey& assetKey)
{
    if (!assetKey.IsValid())
    {
        return AssetResidencyReleaseResult::NotFound;
    }

    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    const ResourceId resourceId = GetResourceIdForAssetKey(assetKey);
    const auto publishedRoot = m_publishedAssetKeys.find(resourceId);
    if (publishedRoot == m_publishedAssetKeys.end() ||
        publishedRoot->second.assetKey != assetKey)
    {
        return AssetResidencyReleaseResult::NotFound;
    }

    // State transitions are stored in the existing root record rather than a
    // separate set, so consuming an explicit release claim and retrying it
    // never allocate. Both immediate and deferred failures remain
    // ReleasePending until a cache hit explicitly reactivates the root or the
    // final closure deletion removes its identity record.
    if (publishedRoot->second.ownership == RootOwnershipState::Active)
    {
        publishedRoot->second.ownership = RootOwnershipState::ReleasePending;
        publishedRoot->second.pendingClosureGeneration =
            AllocateClosureGeneration();
    }
    if (m_assetResidencyControl)
    {
        const AssetResidencyReleaseResult admission =
            m_assetResidencyControl->RequestExactUnload(assetKey);
        if (admission == AssetResidencyReleaseResult::Queued)
        {
            return admission;
        }
    }

    const AssetResidencyReleaseResult result = UnloadClosureNow(resourceId);
    if (m_assetResidencyControl &&
        result == AssetResidencyReleaseResult::Unloaded)
    {
        // No live lease is associated with this request. Keeping no dormant
        // generation lets a future publish/acquire begin a fresh lease epoch.
        m_assetResidencyControl->CompleteUnload(assetKey, 0);
    }
    return result;
}

AssetResidencyReleaseResult ResourceManager::Evict(const AssetKey& assetKey)
{
    if (!assetKey.IsValid())
    {
        return AssetResidencyReleaseResult::NotFound;
    }

    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    const ResourceId resourceId = GetResourceIdForAssetKey(assetKey);
    if (!IsPublishedRootActive(resourceId, assetKey))
    {
        return AssetResidencyReleaseResult::NotFound;
    }
    if (m_assetResidencyControl &&
        m_assetResidencyControl->HasLiveLeaseForResource(resourceId, false))
    {
        return AssetResidencyReleaseResult::BlockedByLease;
    }
    return EvictResourceNow(resourceId);
}

AssetResidencyLease ResourceManager::AcquireAssetResidencyLease(
    const AssetKey& assetKey)
{
    if (!m_initialized || !assetKey.IsValid() || !m_cache ||
        !m_dependencyGraph || !m_assetResidencyControl)
    {
        return {};
    }

    const ResourceId rootResourceId = GetResourceIdForAssetKey(assetKey);
    std::vector<ResourceId> dependencyClosure;
    std::lock_guard<std::recursive_mutex> loadLock(m_loadMutex);
    std::unique_lock<std::shared_mutex> removalBarrier =
        m_cache->LockRemovalsForResidency();
    if (!IsPublishedRootActive(rootResourceId, assetKey))
    {
        return {};
    }
    if (!m_assetResidencyControl->BeginAcquire(assetKey, rootResourceId))
    {
        return {};
    }

    const auto abortAcquire = [this, &assetKey, rootResourceId]() noexcept
    {
        m_assetResidencyControl->AbortAcquire(assetKey, rootResourceId);
    };

    try
    {
        if (!m_cache->ContainsLoadedUnderResidencyBarrier(rootResourceId))
        {
            abortAcquire();
            return {};
        }
        dependencyClosure = m_dependencyGraph->GetAllDependencies(rootResourceId);
        dependencyClosure.push_back(rootResourceId);
        std::sort(dependencyClosure.begin(), dependencyClosure.end());
        dependencyClosure.erase(
            std::unique(dependencyClosure.begin(), dependencyClosure.end()),
            dependencyClosure.end());
        for (ResourceId dependencyId : dependencyClosure)
        {
            if (dependencyId == InvalidResourceId ||
                !m_cache->ContainsLoadedUnderResidencyBarrier(dependencyId))
            {
                abortAcquire();
                return {};
            }
        }
    }
    catch (...)
    {
        abortAcquire();
        return {};
    }

    uint64 generation = 0;
    if (!m_assetResidencyControl->PromoteAcquire(assetKey,
                                                 rootResourceId,
                                                 dependencyClosure,
                                                 generation))
    {
        abortAcquire();
        return {};
    }
    return AssetResidencyLease(
        m_assetResidencyControl,
        assetKey,
        generation,
        std::move(dependencyClosure));
}

bool ResourceManager::IsSoleAssetResidencyConsumer(
    const AssetResidencyLease& lease) const noexcept
{
    if (!lease.IsValid() || !m_assetResidencyControl)
    {
        return false;
    }
    return m_assetResidencyControl->IsSoleActiveLease(
        lease.m_assetKey,
        lease.m_generation);
}

std::optional<AssetKey> ResourceManager::FindPublishedAssetKey(
    ResourceId resourceId) const
{
    if (resourceId == InvalidResourceId)
    {
        return std::nullopt;
    }

    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    const auto found = m_publishedAssetKeys.find(resourceId);
    return found != m_publishedAssetKeys.end()
               ? std::optional<AssetKey>(found->second.assetKey)
               : std::nullopt;
}

AssetResidencyLeaseConsumeResult
ResourceManager::ConsumeSceneAssetResidencyLease(AssetResidencyLease& lease)
{
    AssetResidencyLeaseConsumeResult result;
    if (!m_initialized || !lease.IsValid() || !m_assetResidencyControl)
    {
        return result;
    }

    const std::shared_ptr<AssetResidencyLeaseControl> leaseControl =
        lease.m_control.lock();
    if (leaseControl != m_assetResidencyControl)
    {
        return result;
    }

    const AssetKey assetKey = lease.m_assetKey;
    const uint64 leaseGeneration = lease.m_generation;
    const ResourceId rootResourceId = GetResourceIdForAssetKey(assetKey);
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    const auto root = m_publishedAssetKeys.find(rootResourceId);
    if (root == m_publishedAssetKeys.end() ||
        root->second.assetKey != assetKey)
    {
        result.code = AssetResidencyLeaseConsumeCode::NotFound;
        return result;
    }

    // Scene consumption may encounter an earlier explicit Unload() request
    // for the same root. Only an Active -> ReleasePending transition belongs
    // to this attempt; an existing ReleasePending record is independent
    // caller intent and must survive shared lease consumption.
    const RootOwnershipState ownershipBeforeAdmission =
        root->second.ownership;
    const uint64 closureGenerationBeforeAdmission =
        root->second.pendingClosureGeneration;
    const bool ownsProvisionalAdmission =
        ownershipBeforeAdmission == RootOwnershipState::Active;
    if (ownsProvisionalAdmission)
    {
        root->second.ownership = RootOwnershipState::ReleasePending;
        root->second.pendingClosureGeneration =
            AllocateClosureGeneration();
    }
    if (root->second.pendingClosureGeneration == 0)
    {
        result.code = AssetResidencyLeaseConsumeCode::RetainedFailure;
        return result;
    }

    const auto rollbackProvisionalAdmission = [&]() noexcept
    {
        if (!ownsProvisionalAdmission)
        {
            return;
        }

        root->second.ownership = ownershipBeforeAdmission;
        root->second.pendingClosureGeneration =
            closureGenerationBeforeAdmission;
        m_assetResidencyControl->CancelExactUnload(assetKey);
    };

    const AssetResidencyReleaseResult requested =
        m_assetResidencyControl->RequestExactUnload(assetKey);
    if (requested != AssetResidencyReleaseResult::Queued)
    {
        rollbackProvisionalAdmission();
        result.code = AssetResidencyLeaseConsumeCode::RetainedFailure;
        return result;
    }

    bool finalQueued = false;
    if (!m_assetResidencyControl->ReleaseForSceneClosure(
            assetKey, leaseGeneration, finalQueued))
    {
        rollbackProvisionalAdmission();
        result.code = AssetResidencyLeaseConsumeCode::RetainedFailure;
        return result;
    }

    // A non-final Scene consumer relinquishes only its own lease. When this
    // call created the provisional exact-unload request, the root and its
    // remaining consumers stay active, so that request must not survive as a
    // permanently queued future unload. Keep the allocated generation as
    // value-only receipt provenance, but restore only this call's root/control
    // state before returning ReleasedShared.
    const uint64 closureGeneration =
        root->second.pendingClosureGeneration;
    if (!finalQueued)
    {
        rollbackProvisionalAdmission();
    }

    // The control has consumed this exact generation. Do not invoke Reset(),
    // which would try to decrement the same lease a second time.
    lease.m_control.reset();
    lease.m_assetKey = {};
    lease.m_generation = 0;
    lease.m_dependencyClosure.clear();

    result.rootAssetId = AssetId{rootResourceId};
    result.leaseGeneration = leaseGeneration;
    result.closureGeneration = closureGeneration;
    result.code = finalQueued
                      ? AssetResidencyLeaseConsumeCode::QueuedForClosure
                      : AssetResidencyLeaseConsumeCode::ReleasedShared;
    return result;
}

bool ResourceManager::IsPublishedRootActive(
    ResourceId resourceId,
    const AssetKey& assetKey) const
{
    const auto found = m_publishedAssetKeys.find(resourceId);
    return found != m_publishedAssetKeys.end() &&
           found->second.assetKey == assetKey &&
           found->second.ownership == RootOwnershipState::Active;
}

bool ResourceManager::ReactivatePublishedRoot(
    ResourceId resourceId,
    const AssetKey& assetKey)
{
    const auto found = m_publishedAssetKeys.find(resourceId);
    if (found == m_publishedAssetKeys.end() ||
        found->second.assetKey != assetKey)
    {
        return false;
    }
    found->second.ownership = RootOwnershipState::Active;
    found->second.pendingClosureGeneration = 0;
    if (m_assetResidencyControl)
    {
        m_assetResidencyControl->CancelExactUnload(assetKey);
    }
    return true;
}

uint64 ResourceManager::AllocateClosureGeneration() noexcept
{
    const uint64 generation = m_nextClosureGeneration++;
    if (m_nextClosureGeneration == 0)
    {
        m_nextClosureGeneration = 1;
    }
    return generation == 0 ? AllocateClosureGeneration() : generation;
}

std::optional<ResourceClosureRetirementOutcome>
ResourceManager::MakeClosureRetirementOutcome(
    ResourceId rootResourceId,
    const ClosureUnloadPlan& plan) const
{
    const auto root = m_publishedAssetKeys.find(rootResourceId);
    if (root == m_publishedAssetKeys.end() ||
        root->second.ownership != RootOwnershipState::ReleasePending ||
        root->second.pendingClosureGeneration == 0)
    {
        return std::nullopt;
    }

    ResourceClosureRetirementOutcome outcome;
    outcome.rootAssetId = AssetId{rootResourceId};
    outcome.closureGeneration = root->second.pendingClosureGeneration;
    outcome.reason = ResourceClosureRetirementReason::SceneTeardown;
    outcome.removedResourceIds = plan.removalOrder;
    outcome.sharedRetainedResourceIds = plan.sharedRetainedResourceIds;
    return outcome.IsStructurallyValid()
               ? std::optional<ResourceClosureRetirementOutcome>(
                     std::move(outcome))
               : std::nullopt;
}

AssetResidencyReleaseResult ResourceManager::UnloadResourceNow(ResourceId id)
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    if (!m_cache || !m_cache->Contains(id))
    {
        return AssetResidencyReleaseResult::NotFound;
    }
    if (!m_cache->Remove(id))
    {
        return AssetResidencyReleaseResult::BlockedByLease;
    }
    if (m_registry)
    {
        m_registry->Unregister(id);
    }
    if (m_dependencyGraph)
    {
        m_dependencyGraph->RemoveResource(id);
    }
    m_publishedAssetKeys.erase(id);
    return AssetResidencyReleaseResult::Unloaded;
}

std::optional<ResourceManager::ClosureUnloadPlan>
ResourceManager::BuildClosureUnloadPlan(ResourceId rootResourceId) const
{
    if (!m_cache || !m_dependencyGraph || rootResourceId == InvalidResourceId ||
        !m_dependencyGraph->Contains(rootResourceId) ||
        m_dependencyGraph->HasCircularDependency(rootResourceId))
    {
        return std::nullopt;
    }

    std::vector<ResourceId> closure =
        m_dependencyGraph->GetAllDependencies(rootResourceId);
    closure.push_back(rootResourceId);
    std::sort(closure.begin(), closure.end());
    closure.erase(std::unique(closure.begin(), closure.end()), closure.end());

    std::unordered_set<ResourceId> closureIds(closure.begin(), closure.end());
    std::unordered_set<ResourceId> retainedIds;

    // A resource can be both a dependency of the requested root and an
    // independently published root in its own right. Runtime material
    // instances are the important case: their immutable parent material (and
    // then the parent's textures) must survive instance teardown. Seed those
    // independently-owned roots before propagating retention to descendants.
    for (ResourceId resourceId : closure)
    {
        const auto published = m_publishedAssetKeys.find(resourceId);
        if (resourceId != rootResourceId &&
            published != m_publishedAssetKeys.end() &&
            published->second.ownership == RootOwnershipState::Active)
        {
            retainedIds.insert(resourceId);
        }
    }

    // A resource remains owned when a still-live dependent outside the
    // candidate release set needs it. Retention propagates down the closure:
    // if a shared material remains, its textures must remain too. Iterate to a
    // fixed point over immutable graph snapshots before the first removal.
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (ResourceId candidate : closure)
        {
            if (retainedIds.contains(candidate))
            {
                continue;
            }

            const std::vector<ResourceId> dependents =
                m_dependencyGraph->GetDependents(candidate);
            for (ResourceId dependent : dependents)
            {
                const bool dependentWillRemain =
                    !closureIds.contains(dependent) || retainedIds.contains(dependent);
                // Cache residency is deliberately not ownership. Evicting an
                // active root drops only its physical cache record while its
                // registry, dependency graph and exact root identity remain
                // authoritative for a later reload. Such an evicted external
                // dependent must continue to pin this candidate, otherwise a
                // different root can delete shared graph dependencies and
                // leave the active root structurally broken.
                if (dependentWillRemain &&
                    m_dependencyGraph->Contains(dependent))
                {
                    retainedIds.insert(candidate);
                    changed = true;
                    break;
                }
            }
        }
    }

    std::unordered_set<ResourceId> releaseIds;
    releaseIds.reserve(closure.size());
    for (ResourceId resourceId : closure)
    {
        if (!retainedIds.contains(resourceId))
        {
            releaseIds.insert(resourceId);
        }
    }

    ClosureUnloadPlan plan;
    plan.sharedRetainedResourceIds.assign(retainedIds.begin(),
                                          retainedIds.end());
    std::sort(plan.sharedRetainedResourceIds.begin(),
              plan.sharedRetainedResourceIds.end());
    plan.removalOrder.reserve(releaseIds.size());

    // Remove outer consumers first. Choosing the smallest ready ResourceId
    // gives a deterministic release order without relying on unordered-map
    // iteration inside DependencyGraph.
    while (!releaseIds.empty())
    {
        ResourceId next = InvalidResourceId;
        for (ResourceId candidate : releaseIds)
        {
            bool hasReleasableDependent = false;
            for (ResourceId dependent : m_dependencyGraph->GetDependents(candidate))
            {
                if (releaseIds.contains(dependent))
                {
                    hasReleasableDependent = true;
                    break;
                }
            }
            if (!hasReleasableDependent &&
                (next == InvalidResourceId || candidate < next))
            {
                next = candidate;
            }
        }

        // The graph was cycle-free when the plan was built. Reaching here
        // means a concurrent graph mutation escaped manager serialization;
        // fail closed before any irreversible cache release.
        if (next == InvalidResourceId)
        {
            return std::nullopt;
        }
        plan.removalOrder.push_back(next);
        releaseIds.erase(next);
    }
    return plan;
}

AssetResidencyReleaseResult ResourceManager::UnloadClosureNow(
    ResourceId rootResourceId)
{
    // Queued lease releases enter here outside their caller's manager lock.
    // Keep plan construction, root-ownership inspection and owner-database
    // mutation in the same recursive critical section as immediate unloads.
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    m_closureUnloadRequestCount.fetch_add(1, std::memory_order_relaxed);
    const std::optional<ClosureUnloadPlan> plan =
        BuildClosureUnloadPlan(rootResourceId);
    if (!plan)
    {
        m_closureUnloadRejectedCount.fetch_add(1, std::memory_order_relaxed);
        return m_dependencyGraph && m_dependencyGraph->Contains(rootResourceId)
                   ? AssetResidencyReleaseResult::Rejected
                   : AssetResidencyReleaseResult::NotFound;
    }

    // Preflight every physical removal while the manager's owner lock is held.
    // Lease acquisition and graph publication use the same lock, making the
    // following irreversible cache/registry release a single owner-thread
    // transaction rather than a best-effort recursive deletion.
    if (m_assetResidencyControl)
    {
        for (ResourceId resourceId : plan->removalOrder)
        {
            if (m_assetResidencyControl->HasLiveLeaseForResource(resourceId, false))
            {
                m_closureUnloadRejectedCount.fetch_add(1,
                                                        std::memory_order_relaxed);
                return AssetResidencyReleaseResult::BlockedByLease;
            }
        }
    }

    // Copy the callback before entering ResourceCache's pre-commit gate. Its
    // allocation, if any, must never follow a Render RequestRelease side
    // effect. A missing callback deliberately preserves the established
    // legacy lifecycle path for standalone ResourceManager use.
    ResourceClosureRetirementCallback retirementCallback;
    {
        std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
        retirementCallback = m_closureRetirementCallback;
    }
    const std::optional<ResourceClosureRetirementOutcome> retirementOutcome =
        retirementCallback ? MakeClosureRetirementOutcome(rootResourceId, *plan)
                           : std::nullopt;
    if (retirementCallback && !retirementOutcome)
    {
        m_closureUnloadRejectedCount.fetch_add(1, std::memory_order_relaxed);
        return AssetResidencyReleaseResult::RetainedFailure;
    }

    bool lifecyclePreCommitRejected = false;
    bool lifecycleCommitted = false;
    bool retirementFailedRetained = false;
    const auto commitBeforeUnloadEvents =
        [this,
         &retirementCallback,
         &retirementOutcome,
         &lifecyclePreCommitRejected,
         &lifecycleCommitted,
         &retirementFailedRetained](
            const ResourceCacheBatchSnapshot& resources) noexcept -> bool
    {
        // ResourceCache constructs this retained snapshot while it owns its
        // mutex and after all guards have passed. Therefore this complete,
        // value-owned lifecycle batch describes exactly the resources about
        // to be released; no direct cache removal can interleave between
        // staging and mutation.
        try
        {
            std::vector<ResourceLifecycleEvent> beforeUnloadEvents;
            beforeUnloadEvents.reserve(resources.size());
            for (const auto& [resourceId, resource] : resources)
            {
                ResourceLifecycleEvent event;
                event.type = ResourceLifecycleEventType::BeforeUnload;
                event.resourceId = resourceId;
                event.resource = resource;
                beforeUnloadEvents.push_back(std::move(event));
            }

            // The pre-commit gate runs cache -> lifecycle, matching direct
            // cache callbacks. All allocation precedes the first Render
            // RequestRelease side effect.
            std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
            m_lifecycleEvents.reserve(m_lifecycleEvents.size() +
                                      beforeUnloadEvents.size());

            if (retirementCallback)
            {
                const ResourceClosureRetirementAdmission admission =
                    retirementCallback(*retirementOutcome);
                if (admission.code !=
                    ResourceClosureRetirementAdmissionCode::CommitCpuClosure)
                {
                    retirementFailedRetained = admission.code ==
                                               ResourceClosureRetirementAdmissionCode::FailedRetained;
                    return false;
                }
                for (ResourceLifecycleEvent& event : beforeUnloadEvents)
                {
                    event.closureRetirementToken = admission.receipt.token;
                }
            }
            static_assert(std::is_nothrow_move_constructible_v<ResourceLifecycleEvent>);
            for (ResourceLifecycleEvent& event : beforeUnloadEvents)
            {
                m_lifecycleEvents.push_back(std::move(event));
            }
            lifecycleCommitted = true;
            return true;
        }
        catch (...)
        {
            lifecyclePreCommitRejected = true;
            return false;
        }
    };

    // The cache owns the resource lifetime boundary. Its batch transaction
    // holds the residency barrier continuously, rechecks every ownership
    // guard, and performs no mutation if any member is now protected. Only
    // after that succeeds may the owner databases retire their matching
    // registry/graph records.
    if (!m_cache->RemoveBatch(plan->removalOrder, commitBeforeUnloadEvents))
    {
        m_closureUnloadRejectedCount.fetch_add(1, std::memory_order_relaxed);
        if (retirementFailedRetained)
        {
            return AssetResidencyReleaseResult::RetainedFailure;
        }
        return lifecyclePreCommitRejected
                   ? AssetResidencyReleaseResult::Rejected
                   : AssetResidencyReleaseResult::BlockedByLease;
    }
    RVX_DEBUG_ASSERT(lifecycleCommitted || plan->removalOrder.empty());

    // A cache entry already evicted before RemoveBatch acquired its barrier is
    // intentionally a no-op in that batch. Its registry/graph record still
    // belongs to this explicitly released closure and is retired here.
    for (ResourceId resourceId : plan->removalOrder)
    {
        if (m_registry)
        {
            m_registry->Unregister(resourceId);
        }
        if (m_dependencyGraph)
        {
            m_dependencyGraph->RemoveResource(resourceId);
        }
        m_publishedAssetKeys.erase(resourceId);
    }

    m_closureUnloadedResourceCount.fetch_add(plan->removalOrder.size(),
                                               std::memory_order_relaxed);
    m_closureRetainedResourceCount.fetch_add(
        plan->sharedRetainedResourceIds.size(),
                                               std::memory_order_relaxed);
    return AssetResidencyReleaseResult::Unloaded;
}

AssetResidencyReleaseResult ResourceManager::EvictResourceNow(ResourceId id)
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    if (!m_cache || !m_cache->Contains(id))
    {
        return AssetResidencyReleaseResult::NotFound;
    }
    // Runtime material instances have no loader capable of reconstructing a
    // cache-only eviction. Treat eviction as a complete owner-database unload
    // so their runtime AssetKey can be reused and no registry/graph tombstone
    // survives after the object becomes unreachable.
    if (dynamic_cast<MaterialInstanceResource*>(m_cache->Get(id)) != nullptr)
    {
        return UnloadResourceNow(id);
    }
    return m_cache->Remove(id)
               ? AssetResidencyReleaseResult::Unloaded
               : AssetResidencyReleaseResult::BlockedByLease;
}

void ResourceManager::ProcessPendingLeaseUnloads()
{
    if (!m_assetResidencyControl || !IsOwnerThread())
    {
        return;
    }

    std::vector<std::pair<AssetKey, uint64>> pending;
    {
        std::lock_guard<std::mutex> lock(m_pendingLeaseUnloadMutex);
        pending.swap(m_pendingLeaseUnloads);
    }

    const auto pendingLess = [](const auto& left, const auto& right)
    {
        const AssetKey& leftKey = left.first;
        const AssetKey& rightKey = right.first;
        if (leftKey.canonicalPath != rightKey.canonicalPath)
        {
            return leftKey.canonicalPath < rightKey.canonicalPath;
        }
        if (leftKey.resourceType != rightKey.resourceType)
        {
            return static_cast<uint32>(leftKey.resourceType) <
                   static_cast<uint32>(rightKey.resourceType);
        }
        if (leftKey.importOptionsHash != rightKey.importOptionsHash)
        {
            return leftKey.importOptionsHash < rightKey.importOptionsHash;
        }
        if (leftKey.platformProfileHash != rightKey.platformProfileHash)
        {
            return leftKey.platformProfileHash < rightKey.platformProfileHash;
        }
        if (leftKey.loaderSchemaVersion != rightKey.loaderSchemaVersion)
        {
            return leftKey.loaderSchemaVersion < rightKey.loaderSchemaVersion;
        }
        return left.second < right.second;
    };
    std::sort(pending.begin(), pending.end(), pendingLess);
    pending.erase(
        std::unique(
            pending.begin(),
            pending.end(),
            [](const auto& left, const auto& right)
            {
                return left.second == right.second && left.first == right.first;
            }),
        pending.end());

    std::vector<std::pair<AssetKey, uint64>> retry;
    retry.reserve(pending.size());
    for (const auto& [assetKey, generation] : pending)
    {
        // Serialize reactivation/publication against authorization and the
        // precise closure transaction. Without this owner lock, a cache hit
        // could cancel the hold after TryBeginQueuedUnload and before
        // UnloadClosureNow acquires m_loadMutex.
        std::lock_guard<std::recursive_mutex> unloadLock(m_loadMutex);
        const ResourceId rootResourceId = GetResourceIdForAssetKey(assetKey);
        if (!m_assetResidencyControl->TryBeginQueuedUnload(
                assetKey,
                generation,
                rootResourceId))
        {
            if (m_assetResidencyControl->IsQueuedUnload(assetKey, generation))
            {
                retry.emplace_back(assetKey, generation);
            }
            continue;
        }

        const AssetResidencyReleaseResult result =
            UnloadClosureNow(rootResourceId);
        bool verifiedNotFound = false;
        if (result == AssetResidencyReleaseResult::NotFound)
        {
            std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
            verifiedNotFound =
                (!m_cache || !m_cache->Contains(rootResourceId)) &&
                (!m_dependencyGraph ||
                 !m_dependencyGraph->Contains(rootResourceId));
        }

        if (result != AssetResidencyReleaseResult::Unloaded &&
            !(result == AssetResidencyReleaseResult::NotFound &&
              verifiedNotFound))
        {
            if (result == AssetResidencyReleaseResult::RetainedFailure)
            {
                // The Scene/Render facade retained a terminal receipt with
                // CPU ownership still intact. Do not hammer the same closure
                // generation on every owner tick; explicit retry or a cache
                // reactivation is required to establish a new generation.
                m_assetResidencyControl->RetainFailedUnload(assetKey,
                                                            generation);
                continue;
            }
            // A rejected plan is not a completed unload. Preserve the exact
            // lease request for diagnostics/retry rather than silently
            // dropping it after TryBeginQueuedUnload cleared the flag.
            m_assetResidencyControl->RequeueUnload(assetKey, generation);
            retry.emplace_back(assetKey, generation);
            continue;
        }
        m_assetResidencyControl->CompleteUnload(assetKey, generation);
    }

    if (!retry.empty())
    {
        std::lock_guard<std::mutex> lock(m_pendingLeaseUnloadMutex);
        for (const auto& candidate : retry)
        {
            const bool alreadyQueued = std::any_of(
                m_pendingLeaseUnloads.begin(),
                m_pendingLeaseUnloads.end(),
                [&candidate](const auto& existing)
                {
                    return existing.second == candidate.second &&
                           existing.first == candidate.first;
                });
            if (!alreadyQueued)
            {
                m_pendingLeaseUnloads.push_back(candidate);
            }
        }
    }
}

void ResourceManager::UnloadUnused()
{
    if (m_cache)
    {
        m_cache->EvictUnused();
    }
}

void ResourceManager::Clear()
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);

    // Admission, closure planning and cache removal all serialize through
    // m_loadMutex. Inspect lease ownership only after taking that lock, then
    // use the cache's all-or-nothing transaction before clearing matching
    // registry/graph/identity state.
    if (m_assetResidencyControl &&
        m_assetResidencyControl->GetSnapshot().activeLeases != 0)
    {
        return;
    }
    if (m_cache)
    {
        bool lifecyclePreCommitRejected = false;
        bool lifecycleCommitted = false;
        const auto commitBeforeUnloadEvents =
            [this,
             &lifecyclePreCommitRejected,
             &lifecycleCommitted](
                const ResourceCacheBatchSnapshot& resources) noexcept -> bool
        {
            // Clear is a complete owner-database transaction. Build every
            // retained BeforeUnload value while the cache holds its mutex,
            // then reserve the update-side queue before the first cache
            // retain is released. This is the same cache -> lifecycle lock
            // ordering used by closure removal and direct cache callbacks.
            try
            {
                std::vector<ResourceLifecycleEvent> beforeUnloadEvents;
                beforeUnloadEvents.reserve(resources.size());
                for (const auto& [resourceId, resource] : resources)
                {
                    ResourceLifecycleEvent event;
                    event.type = ResourceLifecycleEventType::BeforeUnload;
                    event.resourceId = resourceId;
                    event.resource = resource;
                    beforeUnloadEvents.push_back(std::move(event));
                }

                std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
                m_lifecycleEvents.reserve(m_lifecycleEvents.size() +
                                          beforeUnloadEvents.size());
                static_assert(
                    std::is_nothrow_move_constructible_v<ResourceLifecycleEvent>);
                for (ResourceLifecycleEvent& event : beforeUnloadEvents)
                {
                    m_lifecycleEvents.push_back(std::move(event));
                }
                lifecycleCommitted = true;
                return true;
            }
            catch (...)
            {
                lifecyclePreCommitRejected = true;
                return false;
            }
        };

        if (!m_cache->ClearAllOrNothing(commitBeforeUnloadEvents))
        {
            // No cache, registry, graph or root-ownership mutation occurred.
            // A failed lifecycle reservation is a fail-closed Clear rather
            // than a best-effort resource release with missing retirement.
            (void)lifecyclePreCommitRejected;
            return;
        }
        RVX_DEBUG_ASSERT(lifecycleCommitted);
    }

    if (m_registry) m_registry->Clear();
    if (m_dependencyGraph) m_dependencyGraph->Clear();
    m_publishedAssetKeys.clear();
}

void ResourceManager::EnableHotReload(bool enable)
{
    ConfigureHotReload(enable);
}

bool ResourceManager::IsHotReloadEnabled() const
{
    return m_config.enableHotReload &&
           HotReloadManager::Get().IsInitialized() &&
           HotReloadManager::Get().IsEnabled();
}

ResourceHotReloadDiagnostic ResourceManager::GetHotReloadDiagnostic() const
{
    ResourceHotReloadDiagnostic diagnostic;
    {
        std::lock_guard<std::mutex> lock(m_hotReloadMutex);
        diagnostic = m_hotReloadDiagnostic;
    }

    diagnostic.watcherInitialized = HotReloadManager::Get().IsInitialized();
    if (HotReloadManager::Get().IsInitialized())
    {
        const HotReloadManager::ReloadStats stats = HotReloadManager::Get().GetStats();
        diagnostic.watchedFileCount = stats.watchedFiles;
        diagnostic.registeredResourceCount = stats.registeredResources;
        diagnostic.pendingReloadCount = HotReloadManager::Get().GetPendingReloadCount();
        diagnostic.totalReloadCount = stats.totalReloads;
        diagnostic.successfulReloadCount = stats.successfulReloads;
        diagnostic.failedReloadCount = stats.failedReloads;
    }

    return diagnostic;
}

std::string ResourceManager::ExportHotReloadDiagnosticJson() const
{
    return ExportResourceHotReloadDiagnosticJson(GetHotReloadDiagnostic());
}

bool ResourceManager::SaveHotReloadDiagnosticJson(const char* filename) const
{
    return SaveResourceHotReloadDiagnosticJson(filename, GetHotReloadDiagnostic());
}

void ResourceManager::CheckForChanges()
{
    if (!IsHotReloadEnabled())
    {
        return;
    }

    HotReloadManager::Get().Update();
    SetHotReloadDiagnostic(ResourceHotReloadStatus::Active,
                           true,
                           true,
                           "Hot reload update processed.");
}

void ResourceManager::OnResourceReloaded(std::function<void(ResourceId, IResource*)> callback)
{
    m_reloadCallback = std::move(callback);
}

void ResourceManager::SetCacheLimit(size_t bytes)
{
    if (m_cache)
    {
        m_cache->SetMemoryLimit(bytes);
    }
}

void ResourceManager::ClearCache()
{
    if (m_cache)
    {
        m_cache->Clear();
    }
}

void ResourceManager::RegisterLoader(ResourceType type, std::unique_ptr<IResourceLoader> loader)
{
    if (!loader)
    {
        RVX_RESOURCE_WARN("ResourceManager rejected null loader registration for {}",
                          GetResourceTypeName(type));
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    m_loaders[type] = std::shared_ptr<IResourceLoader>(std::move(loader));
}

IResourceLoader* ResourceManager::GetLoader(ResourceType type)
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    auto it = m_loaders.find(type);
    return it != m_loaders.end() ? it->second.get() : nullptr;
}

void ResourceManager::ProcessCompletedLoads()
{
    if (m_initialized && !IsOwnerThread())
    {
        RVX_RESOURCE_ERROR("ResourceManager::ProcessCompletedLoads must run on its owner/update thread");
        return;
    }
    JobSystem::Get().ProcessMainThreadCompletions();
    DrainPreparedLoadCompletions();
    ProcessPendingLeaseUnloads();
    if (m_modelTextureStreaming)
    {
        m_modelTextureStreaming->DrainCompletions(
            [this](TextureResource* texture)
            {
                if (texture && texture->IsLoaded())
                {
                    QueueLifecycleEvent(
                        ResourceLifecycleEventType::Reloaded,
                        texture);
                    return true;
                }
                return false;
            });
    }
    DrainLegacyAsyncWaiters();
    DrainLifecycleEvents();
}

bool ResourceManager::BeginModelTextureStreaming(
    ResourceHandle<ModelResource> model)
{
    if (!m_initialized || !IsOwnerThread() || !m_modelTextureStreaming ||
        !model || !model->IsLoaded())
    {
        return false;
    }
    return m_modelTextureStreaming->Start(std::move(model));
}

std::vector<ResourceHandle<TextureResource>>
ResourceManager::GetPendingModelTexturePublications() const
{
    return m_modelTextureStreaming
               ? m_modelTextureStreaming->GetPendingPublications()
               : std::vector<ResourceHandle<TextureResource>>{};
}

bool ResourceManager::CompleteModelTexturePublication(
    ResourceId textureId,
    bool succeeded,
    std::string error)
{
    if (!m_initialized || !IsOwnerThread() || !m_modelTextureStreaming)
        return false;
    return m_modelTextureStreaming->CompletePublication(
        textureId,
        succeeded,
        std::move(error));
}

ModelTextureStreamingCancellationResult
ResourceManager::CancelModelTextureStreaming(ResourceId modelId)
{
    if (!m_initialized || !IsOwnerThread() || !m_modelTextureStreaming ||
        modelId == InvalidResourceId)
    {
        return {};
    }
    return m_modelTextureStreaming->CancelModel(modelId);
}

ModelTextureStreamingStats
ResourceManager::GetModelTextureStreamingStats() const
{
    return m_modelTextureStreaming
               ? m_modelTextureStreaming->GetStats()
               : ModelTextureStreamingStats{};
}

ResourceLoadOperationOwner ResourceManager::RequestPreparedLoad(
    const std::string& path,
    ResourceLoadOptions options,
    ResourceType requestedType,
    ResourceLoadPreparationStateRef preparationState)
{
    if (!m_acceptAsyncRequests.load(std::memory_order_acquire))
    {
        return {};
    }

    if (!options.expectedContentIdentity.IsEmpty() &&
        !options.expectedContentIdentity.IsValid())
    {
        RVX_RESOURCE_WARN("ResourceManager rejected async request '{}' with a malformed content identity", path);
        return {};
    }

    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(m_config.runtimePolicy, m_config.basePath, path);
    if (!resolution.allowed)
    {
        RVX_RESOURCE_WARN("ResourceManager rejected async request '{}': {}",
                          path,
                          resolution.diagnosticMessage);
        return {};
    }

    const ResourceType detectedType = DetectResourceType(resolution.resolvedPath);
    const ResourceType resourceType = requestedType != ResourceType::Unknown
                                          ? requestedType
                                          : detectedType;
    if (!options.traceContext.IsEnabled())
    {
        options.traceContext = m_config.startupTraceContext;
    }

    AssetKey assetKey;
    std::string resourceIdentityPath;
    ResourceId rootResourceId = InvalidResourceId;
    std::shared_ptr<IResourceLoader> loader;
    ResourceLoadPreparationStateRef loaderState;
    ResourceLoadOperationOwner operation;
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        if (!m_initialized || !m_acceptAsyncRequests.load(std::memory_order_acquire) ||
            !m_cache)
        {
            return {};
        }

        const auto loaderIt = m_loaders.find(resourceType);
        if (loaderIt == m_loaders.end() || !loaderIt->second)
        {
            return {};
        }
        loader = loaderIt->second;
        if (!loader->CanLoad(resolution.resolvedPath))
        {
            RVX_RESOURCE_WARN("Resource loader for {} rejected path '{}'",
                              GetResourceTypeName(resourceType),
                              resolution.resolvedPath);
            return {};
        }

        uint64 canonicalImportOptionsHash = options.importOptionsHash;
        ResourceLoadError captureError;
        try
        {
            const bool captured = preparationState
                                      ? loader->ValidatePreparationState(
                                            options.importOptionsHash,
                                            std::move(preparationState),
                                            loaderState,
                                            canonicalImportOptionsHash,
                                            captureError)
                                      : loader->CapturePreparationState(
                                            options.importOptionsHash,
                                            loaderState,
                                            canonicalImportOptionsHash,
                                            captureError);
            if (!captured)
            {
                RVX_RESOURCE_WARN("ResourceManager rejected async import profile for '{}': {}",
                                  path,
                                  captureError.message);
                return {};
            }
        }
        catch (const std::exception& error)
        {
            RVX_RESOURCE_ERROR("Resource loader failed to capture import state for '{}': {}",
                               path,
                               error.what());
            return {};
        }
        catch (...)
        {
            RVX_RESOURCE_ERROR("Resource loader failed to capture import state for '{}'",
                               path);
            return {};
        }
        options.importOptionsHash = canonicalImportOptionsHash;
        assetKey = MakeAssetKey(resolution.resolvedPath,
                                resourceType,
                                options.importOptionsHash,
                                options.platformProfileHash,
                                options.loaderSchemaVersion,
                                options.expectedContentIdentity);
        if (!assetKey.IsValid())
        {
            RVX_RESOURCE_WARN("ResourceManager cannot create an async asset key for '{}'", path);
            return {};
        }
        resourceIdentityPath = BuildAssetResourceIdentity(assetKey);
        rootResourceId = GetResourceIdForAssetKey(assetKey);

        // Prefer the operation until its owner has completed publication and
        // removed it. This keeps every concurrent subscriber on one request id
        // even during the short cache-visible/AwaitingPublish interval.
        const auto inFlight = m_inFlightLoads.find(assetKey);
        if (inFlight != m_inFlightLoads.end())
        {
            return inFlight->second;
        }

        // Cache and in-flight lookup share the same admission critical section
        // as owner publication, so a request cannot miss both meanings.
        if (options.allowCacheLookup)
        {
            if (IResource* cached = m_cache->Get(rootResourceId))
            {
                const auto publishedRoot = m_publishedAssetKeys.find(rootResourceId);
                if (publishedRoot == m_publishedAssetKeys.end() ||
                    publishedRoot->second.assetKey != assetKey ||
                    cached->GetId() != rootResourceId ||
                    cached->GetType() != assetKey.resourceType)
                {
                    RVX_RESOURCE_ERROR(
                        "ResourceManager rejected cached async resource {} because its structured root identity does not match the request",
                        rootResourceId);
                    return {};
                }
                if (!assetKey.expectedContentIdentity.IsEmpty())
                {
                    const ResourceContentVerificationReceipt& receipt =
                        cached->GetContentVerificationReceipt();
                    if (!receipt.IsVerified() ||
                        receipt.expected != assetKey.expectedContentIdentity ||
                        receipt.observed != assetKey.expectedContentIdentity)
                    {
                        RVX_RESOURCE_ERROR(
                            "ResourceManager rejected cached async resource {} because its content verification receipt is not exact",
                            rootResourceId);
                        return {};
                    }
                }
                if (!ReactivatePublishedRoot(rootResourceId, assetKey))
                {
                    RVX_RESOURCE_ERROR(
                        "ResourceManager rejected cached async resource {} because its root ownership claim could not be reactivated",
                        rootResourceId);
                    return {};
                }
                ResourceLoadOperationOwner cachedOperation =
                    ResourceLoadOperationOwner::Create(assetKey, std::move(options));
                if (!cachedOperation || !cachedOperation.BeginLoading() ||
                    !cachedOperation.BeginAwaitingPublish() ||
                    !cachedOperation.CompleteReady(ResourceHandle<IResource>(cached)))
                {
                    RVX_RESOURCE_ERROR(
                        "ResourceManager rejected cached async resource {} because its ready operation could not be completed",
                        rootResourceId);
                    return {};
                }
                return cachedOperation;
            }
        }

        operation = ResourceLoadOperationOwner::Create(assetKey, std::move(options));
        if (!operation || !operation.BeginLoading())
        {
            return {};
        }
        m_inFlightLoads.emplace(assetKey, operation);
    }

    ResourceLoadPreparationContext context;
    context.assetKey = assetKey;
    context.resourceIdentityPath = resourceIdentityPath;
    context.requestedPath = path;
    context.resolvedPath = resolution.resolvedPath;
    context.rootResourceId = rootResourceId;
    context.traceContext = operation.GetSnapshot().options.traceContext;
    context.loaderState = std::move(loaderState);
    context.isCancellationRequested = [operation]()
    {
        return operation.IsCancellationRequested();
    };

    m_pendingAsyncJobCount.fetch_add(1, std::memory_order_relaxed);
    JobSubmissionDesc desc;
    desc.category = "Resource.LoadAsync";
    desc.priority = operation.GetSnapshot().options.priority == ResourceLoadPriority::High
                        ? JobPriority::High
                        : JobPriority::Normal;

    // JobSystem intentionally executes inline when no worker system exists.
    // Even then ExecutePreparedLoad only stages a completion; publication
    // remains owned by ProcessCompletedLoads().
    {
        std::lock_guard<std::mutex> lock(m_preparedJobMutex);
        if (!m_acceptAsyncRequests.load(std::memory_order_acquire))
        {
            m_pendingAsyncJobCount.fetch_sub(1, std::memory_order_relaxed);
            operation.CompleteCancelled("ResourceManager stopped accepting requests.");
            RemoveInFlightLoad(assetKey, operation.GetRequestId());
            return operation;
        }
        JobHandle job = JobSystem::Get().Submit(
            [this,
             operation,
             loader = std::move(loader),
             context = std::move(context),
             resolution]() mutable
            {
                ExecutePreparedLoad(std::move(operation),
                                    std::move(loader),
                                    std::move(context),
                                    resolution);
            },
            desc);
        m_preparedJobs.push_back(std::move(job));
    }
    return operation;
}

bool IResourceLoader::ValidatePreparationState(
    uint64 requestedImportOptionsHash,
    ResourceLoadPreparationStateRef suppliedState,
    ResourceLoadPreparationStateRef& outState,
    uint64& outCanonicalImportOptionsHash,
    ResourceLoadError& outError) const
{
    outState.reset();
    outCanonicalImportOptionsHash = requestedImportOptionsHash;
    if (suppliedState)
    {
        outError = {
            ResourceLoadErrorCode::InvalidRequest,
            "This resource loader does not accept caller-supplied preparation state."};
        return false;
    }
    return CapturePreparationState(requestedImportOptionsHash,
                                   outState,
                                   outCanonicalImportOptionsHash,
                                   outError);
}

void ResourceManager::ExecutePreparedLoad(ResourceLoadOperationOwner operation,
                                          std::shared_ptr<IResourceLoader> loader,
                                          ResourceLoadPreparationContext context,
                                          ResourcePathResolution resolution)
{
    struct PendingJobGuard
    {
        std::atomic<size_t>& counter;
        ~PendingJobGuard()
        {
            counter.fetch_sub(1, std::memory_order_relaxed);
        }
    } guard{m_pendingAsyncJobCount};

    PreparedLoadCompletion completion;
    completion.assetKey = context.assetKey;
    completion.operation = operation;
    completion.resolution = std::move(resolution);
    completion.requestedPath = context.requestedPath;

    if (!operation || operation.IsCancellationRequested())
    {
        completion.cancelled = true;
    }
    else if (!loader)
    {
        completion.error = {ResourceLoadErrorCode::LoaderUnavailable,
                            "The resource loader was unavailable before preparation began."};
    }
    else
    {
        Diagnostics::TraceSpan prepareSpan = Diagnostics::BeginTraceSpan(
            context.traceContext,
            "ResourcePrepare",
            {{"requestedPath", context.requestedPath},
             {"resolvedPath", context.resolvedPath},
             {"resourceType", GetResourceTypeName(context.assetKey.resourceType)}});

        try
        {
            operation.UpdateProgress(ResourceLoadStage::Resolve, 1.0f);
            const bool prepared = loader->Prepare(context,
                                                   completion.bundle,
                                                   completion.error);
            if (!prepared && !completion.error.HasError())
            {
                completion.error = {ResourceLoadErrorCode::LoaderFailure,
                                    "The loader rejected the resource preparation request."};
            }
            if (operation.IsCancellationRequested() || context.IsCancellationRequested())
            {
                completion.bundle = {};
                completion.cancelled = true;
            }
            else if (prepared)
            {
                operation.UpdateProgress(ResourceLoadStage::Finalize, 1.0f);
                operation.BeginAwaitingPublish();
                prepareSpan.SetAttribute("result", "prepared");
            }
            else
            {
                prepareSpan.SetAttribute("result", "failed");
            }
        }
        catch (const std::exception& error)
        {
            completion.bundle = {};
            completion.error = {ResourceLoadErrorCode::LoaderFailure, error.what()};
            prepareSpan.SetAttribute("result", "exception");
        }
        catch (...)
        {
            completion.bundle = {};
            completion.error = {ResourceLoadErrorCode::LoaderFailure,
                                "The loader raised an unknown exception while preparing the resource."};
            prepareSpan.SetAttribute("result", "unknown-exception");
        }
    }

    m_pendingAsyncCompletionCount.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_preparedCompletionMutex);
        m_preparedCompletions.push_back(std::move(completion));
    }
}

void ResourceManager::DrainPreparedLoadCompletions(bool shutdownCancellation)
{
    std::vector<PreparedLoadCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(m_preparedCompletionMutex);
        completions.swap(m_preparedCompletions);
    }

    for (PreparedLoadCompletion& completion : completions)
    {
        m_pendingAsyncCompletionCount.fetch_sub(1, std::memory_order_relaxed);
        try
        {
            if (shutdownCancellation || completion.cancelled ||
                completion.error.code == ResourceLoadErrorCode::Cancelled ||
                completion.operation.IsCancellationRequested())
            {
                m_cancelledLoadCount.fetch_add(1, std::memory_order_relaxed);
                completion.operation.CompleteCancelled(
                    shutdownCancellation ? "ResourceManager is shutting down."
                                          : "All resource-load subscribers cancelled their request.");
            }
            else if (completion.error.HasError())
            {
                completion.operation.CompleteFailed(std::move(completion.error));
            }
            else if (!completion.operation.BeginOwnerPublication())
            {
                completion.operation.CompleteCancelled(
                    "Resource publication was cancelled before owner commit.");
            }
            else
            {
                ResourceLoadError publishError;
                if (PublishPreparedBundle(completion.resolution,
                                          completion.requestedPath,
                                          completion.assetKey,
                                          completion.bundle,
                                          publishError))
                {
                    completion.operation.CompleteReady(completion.bundle.GetRoot());
                }
                else
                {
                    if (!publishError.HasError())
                    {
                        publishError = {ResourceLoadErrorCode::PublishFailure,
                                        "Prepared resource publication failed without a diagnostic."};
                    }
                    completion.operation.CompleteFailed(std::move(publishError));
                }
            }
        }
        catch (const std::exception& error)
        {
            RVX_RESOURCE_ERROR("Prepared resource owner publication failed: {}", error.what());
            completion.operation.CompleteFailed(
                {ResourceLoadErrorCode::PublishFailure, error.what()});
        }
        catch (...)
        {
            RVX_RESOURCE_ERROR("Prepared resource owner publication failed with an unknown exception");
            completion.operation.CompleteFailed(
                {ResourceLoadErrorCode::PublishFailure,
                 "An unknown exception interrupted owner-thread publication."});
        }
        RemoveInFlightLoad(completion.assetKey, completion.operation.GetRequestId());
    }

    {
        std::lock_guard<std::mutex> lock(m_preparedJobMutex);
        std::erase_if(m_preparedJobs,
                      [](const JobHandle& job)
                      {
                          return job.IsComplete();
                      });
    }
}

void ResourceManager::DrainLegacyAsyncWaiters()
{
    std::vector<LegacyAsyncWaiter> waiters;
    {
        std::lock_guard<std::mutex> lock(m_legacyAsyncWaiterMutex);
        waiters.swap(m_legacyAsyncWaiters);
    }

    std::vector<LegacyAsyncWaiter> pending;
    pending.reserve(waiters.size());
    for (LegacyAsyncWaiter& waiter : waiters)
    {
        const ResourceLoadSnapshot snapshot = waiter.subscription.GetSnapshot();
        if (!waiter.readyWithoutSubscription && !snapshot.IsTerminal())
        {
            pending.push_back(std::move(waiter));
            continue;
        }

        ResourceHandle<IResource> resource = waiter.subscription.TryGet();
        if (waiter.completion)
        {
            waiter.completion(std::move(resource));
        }
        waiter.subscription.Cancel();
    }

    if (!pending.empty())
    {
        std::lock_guard<std::mutex> lock(m_legacyAsyncWaiterMutex);
        m_legacyAsyncWaiters.insert(m_legacyAsyncWaiters.end(),
                                    std::make_move_iterator(pending.begin()),
                                    std::make_move_iterator(pending.end()));
    }
}

bool ResourceManager::ValidatePreparedBundle(const PreparedResourceBundle& bundle,
                                             ResourceLoadError& outError) const
{
    if (!bundle.IsValid())
    {
        outError = {ResourceLoadErrorCode::PublishFailure, bundle.GetValidationError()};
        return false;
    }

    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        if (!entry.resource || entry.resource->IsLoaded())
        {
            outError = {ResourceLoadErrorCode::PublishFailure,
                        "A prepared bundle contains an already-published or null resource."};
            return false;
        }
    }
    return true;
}

bool ResourceManager::PublishPreparedBundle(const ResourcePathResolution& resolution,
                                            const std::string& requestedPath,
                                            const AssetKey& assetKey,
                                            PreparedResourceBundle& bundle,
                                            ResourceLoadError& outError)
{
    Diagnostics::TraceSpan publishSpan;
    try
    {
        publishSpan = Diagnostics::BeginTraceSpan(
            m_config.startupTraceContext,
            "CPUPublish",
            {{"requestedPath", requestedPath},
             {"resourceType", GetResourceTypeName(assetKey.resourceType)}});
    }
    catch (const std::exception& error)
    {
        outError = {ResourceLoadErrorCode::PublishFailure, error.what()};
        return false;
    }
    catch (...)
    {
        outError = {ResourceLoadErrorCode::PublishFailure,
                    "Could not begin prepared resource publication tracing."};
        return false;
    }

    // Publication is short, owner-thread work, but it must be serialized with
    // cache/in-flight admission so concurrent RequestAsync callers cannot miss
    // both the operation and its newly published cache object.
    std::unique_lock<std::recursive_mutex> publicationLock(m_loadMutex);
    bool publicationCommitted = false;
    try
    {
        if (!m_initialized || !ValidatePreparedBundle(bundle, outError))
        {
            return false;
        }

        ResourceContentVerificationReceipt contentVerificationReceipt;
        if (!VerifyPreparedContentIdentity(assetKey,
                                           bundle,
                                           contentVerificationReceipt,
                                           outError))
        {
            return false;
        }

        ResourceHandle<IResource> root = bundle.GetRoot();
        if (!root || root->GetType() != assetKey.resourceType)
        {
            outError = {ResourceLoadErrorCode::PublishFailure,
                        "Prepared bundle root does not match the requested resource type."};
            return false;
        }

    // The AssetKey is the authority for a coalesced root identity.  A loader
    // may use an absolute source path internally, but aliases must resolve to
    // the same cache record before owner-thread publication.
    root->SetId(GetResourceIdForAssetKey(assetKey));
    root->SetPath(requestedPath);
    root->SetName(std::filesystem::path(requestedPath).stem().string());

    // Loader bundle bookkeeping was created before the manager asserted the
    // canonical root identity. Revalidate effective ids after that rebind so a
    // malicious or stale dependency cannot alias the root at publication.
    std::unordered_set<ResourceId> effectiveResourceIds;
    effectiveResourceIds.reserve(bundle.GetEntries().size());
    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        IResource* effective = entry.isRoot ? root.Get() : entry.resource.Get();
        if (effective == nullptr || effective->GetId() == InvalidResourceId ||
            !effectiveResourceIds.insert(effective->GetId()).second)
        {
            outError = {ResourceLoadErrorCode::PublishFailure,
                        "Prepared bundle contains duplicate or invalid effective resource ids."};
            return false;
        }
    }

    // Resolve any dependency that was already published to its canonical
    // cache object before registering this transaction. A prepared worker owns
    // duplicate temporary objects by design; allowing Model/Material to retain
    // them would create two live meanings for one ResourceId.
    std::unordered_map<ResourceId, ResourceHandle<IResource>> cachedResources;
    cachedResources.reserve(bundle.GetEntries().size());
    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        IResource* prepared = entry.isRoot ? root.Get() : entry.resource.Get();
        IResource* cached = m_cache->Get(prepared->GetId());
        const auto publishedRoot = m_publishedAssetKeys.find(prepared->GetId());
        if (!entry.isRoot && publishedRoot != m_publishedAssetKeys.end())
        {
            if (cached == nullptr)
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared dependency collides with a reserved published root whose cache entry is absent."};
                return false;
            }

            const std::optional<ResourceMetadata> metadata =
                m_registry->FindById(prepared->GetId());
            if (cached->GetId() != prepared->GetId() ||
                cached->GetType() != publishedRoot->second.assetKey.resourceType ||
                !metadata || metadata->id != prepared->GetId() ||
                metadata->type != publishedRoot->second.assetKey.resourceType ||
                !m_dependencyGraph->Contains(prepared->GetId()))
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared dependency collides with a published root whose structured identity is incomplete."};
                return false;
            }

            const ResourceContentIdentity& expectedIdentity =
                publishedRoot->second.assetKey.expectedContentIdentity;
            if (!expectedIdentity.IsEmpty())
            {
                const ResourceContentVerificationReceipt& receipt =
                    cached->GetContentVerificationReceipt();
                if (!receipt.IsVerified() || receipt.expected != expectedIdentity ||
                    receipt.observed != expectedIdentity)
                {
                    outError = {ResourceLoadErrorCode::PublishFailure,
                                "A prepared dependency collides with a published root lacking an exact verified content receipt."};
                    return false;
                }
            }
        }

        if (cached != nullptr)
        {
            if (cached->GetType() != prepared->GetType())
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared resource id collides with a cached resource of another type."};
                return false;
            }
            if (entry.isRoot)
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared root collides with an existing cache entry; its AssetKey identity is stale."};
                return false;
            }
            cachedResources.emplace(prepared->GetId(), ResourceHandle<IResource>(cached));
        }
    }

    auto lookupCached = [&cachedResources](ResourceId resourceId,
                                            ResourceType expectedType,
                                            ResourceHandle<IResource>& outResource) -> bool
    {
        const auto cached = cachedResources.find(resourceId);
        if (cached == cachedResources.end())
        {
            return true;
        }
        if (!cached->second || cached->second->GetType() != expectedType)
        {
            return false;
        }
        outResource = cached->second;
        return true;
    };

    // Materials first: Model resources may refer to a Material which itself
    // refers to a cached Texture. Copy slots before SetTexture because the
    // setter can update the backing unordered map.
    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        auto* material = dynamic_cast<MaterialResource*>(entry.resource.Get());
        if (!material || cachedResources.contains(material->GetId()))
        {
            continue;
        }

        std::vector<std::pair<std::string, ResourceHandle<TextureResource>>> textures;
        textures.reserve(material->GetTextures().size());
        for (const auto& [slot, texture] : material->GetTextures())
        {
            textures.emplace_back(slot, texture);
        }
        for (const auto& [slot, texture] : textures)
        {
            if (!texture)
            {
                continue;
            }
            ResourceHandle<IResource> canonical;
            if (!lookupCached(texture.GetId(), ResourceType::Texture, canonical))
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared material texture does not match its cached ResourceId type."};
                return false;
            }
            if (canonical)
            {
                material->SetTexture(slot,
                                     ResourceHandle<TextureResource>(
                                         static_cast<TextureResource*>(canonical.Get())));
            }
        }
    }

    // Environment roots own strong handles to their four IBL textures just as
    // materials own texture handles.  If those dependencies survived an
    // earlier root eviction, rebind the newly prepared root to the canonical
    // cache objects before publication.  Otherwise the root would retain
    // duplicate, never-published TextureResource instances with the same ids.
    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        auto* environment = dynamic_cast<EnvironmentResource*>(
            entry.isRoot ? root.Get() : entry.resource.Get());
        if (!environment || cachedResources.contains(environment->GetId()))
        {
            continue;
        }

        EnvironmentResourceData data = environment->GetData();
        auto rebindTexture = [&lookupCached](TextureHandle& texture) -> bool
        {
            if (!texture)
            {
                return false;
            }

            ResourceHandle<IResource> canonical;
            if (!lookupCached(texture.GetId(), ResourceType::Texture, canonical))
            {
                return false;
            }
            if (canonical)
            {
                texture = TextureHandle(static_cast<TextureResource*>(canonical.Get()));
            }
            return true;
        };

        if (!rebindTexture(data.environment) ||
            !rebindTexture(data.irradiance) ||
            !rebindTexture(data.prefiltered) ||
            !rebindTexture(data.brdfLUT) ||
            !environment->SetPreparedData(std::move(data)))
        {
            outError = {ResourceLoadErrorCode::PublishFailure,
                        "A prepared environment could not bind its canonical texture dependencies."};
            return false;
        }
    }

    for (const PreparedResourceEntry& entry : bundle.GetEntries())
    {
        auto* model = dynamic_cast<ModelResource*>(entry.isRoot ? root.Get() : entry.resource.Get());
        if (!model || cachedResources.contains(model->GetId()))
        {
            continue;
        }

        std::vector<ResourceHandle<MeshResource>> meshes = model->GetMeshes();
        for (ResourceHandle<MeshResource>& mesh : meshes)
        {
            if (!mesh)
            {
                continue;
            }
            ResourceHandle<IResource> canonical;
            if (!lookupCached(mesh.GetId(), ResourceType::Mesh, canonical))
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared model mesh does not match its cached ResourceId type."};
                return false;
            }
            if (canonical)
            {
                mesh = ResourceHandle<MeshResource>(static_cast<MeshResource*>(canonical.Get()));
            }
        }
        model->SetMeshes(std::move(meshes));

        std::vector<ResourceHandle<MaterialResource>> materials = model->GetMaterials();
        for (ResourceHandle<MaterialResource>& material : materials)
        {
            if (!material)
            {
                continue;
            }
            ResourceHandle<IResource> canonical;
            if (!lookupCached(material.GetId(), ResourceType::Material, canonical))
            {
                outError = {ResourceLoadErrorCode::PublishFailure,
                            "A prepared model material does not match its cached ResourceId type."};
                return false;
            }
            if (canonical)
            {
                material = ResourceHandle<MaterialResource>(
                    static_cast<MaterialResource*>(canonical.Get()));
            }
        }
        model->SetMaterials(std::move(materials));
        for (const auto& [resourceId, canonical] : cachedResources)
        {
            if (canonical && canonical->GetType() == ResourceType::Texture)
            {
                model->RebindTextureStreamingDependency(
                    resourceId,
                    ResourceHandle<TextureResource>(
                        static_cast<TextureResource*>(canonical.Get())));
            }
        }
    }

    struct StagedPublication
    {
        ResourceHandle<IResource> resource;
        ResourceMetadata metadata;
        std::optional<ResourceMetadata> previousMetadata;
        std::vector<ResourceId> previousDependencies;
        bool graphExisted = false;
        bool registryChanged = false;
        bool graphChanged = false;
    };

    // Complete every fallible loader-defined metadata query before changing
    // owner state. In particular, GetAllDependencies() may be implemented by
    // third-party resources and is not allowed inside the commit boundary.
    std::vector<StagedPublication> staged;
    try
    {
        staged.reserve(bundle.GetEntries().size());
        for (const PreparedResourceEntry& entry : bundle.GetEntries())
        {
            IResource* resource = entry.resource.Get();
            if (entry.isRoot)
            {
                resource = root.Get();
            }
            if (cachedResources.contains(resource->GetId()))
            {
                continue;
            }

            StagedPublication publication;
            publication.resource = ResourceHandle<IResource>(resource);
            publication.metadata.id = resource->GetId();
            publication.metadata.path = resource->GetPath();
            publication.metadata.name = resource->GetName();
            publication.metadata.type = resource->GetType();
            publication.metadata.dependencies = resource->GetAllDependencies();
            for (ResourceId dependencyId : publication.metadata.dependencies)
            {
                if (dependencyId == InvalidResourceId ||
                    (!effectiveResourceIds.contains(dependencyId) &&
                     !m_cache->Contains(dependencyId)))
                {
                    outError = {ResourceLoadErrorCode::PublishFailure,
                                "Prepared resource dependency closure is incomplete."};
                    return false;
                }
            }
            publication.previousMetadata = m_registry->FindById(publication.metadata.id);
            publication.graphExisted = m_dependencyGraph->Contains(publication.metadata.id);
            if (publication.graphExisted)
            {
                publication.previousDependencies =
                    m_dependencyGraph->GetDependencies(publication.metadata.id);
            }
            staged.push_back(std::move(publication));
        }
    }
    catch (const std::exception& error)
    {
        outError = {ResourceLoadErrorCode::PublishFailure, error.what()};
        return false;
    }
    catch (...)
    {
        outError = {ResourceLoadErrorCode::PublishFailure,
                    "An unknown error interrupted prepared resource validation."};
        return false;
    }

    auto rollbackMetadata = [&]() noexcept
    {
        for (auto it = staged.rbegin(); it != staged.rend(); ++it)
        {
            try
            {
                if (it->graphChanged)
                {
                    if (it->graphExisted)
                    {
                        m_dependencyGraph->UpdateDependencies(it->metadata.id,
                                                              it->previousDependencies);
                    }
                    else
                    {
                        m_dependencyGraph->RemoveResource(it->metadata.id);
                    }
                }
                if (it->registryChanged)
                {
                    if (it->previousMetadata)
                    {
                        m_registry->Update(*it->previousMetadata);
                    }
                    else
                    {
                        m_registry->Unregister(it->metadata.id);
                    }
                }
            }
            catch (...)
            {
                RVX_RESOURCE_ERROR("Prepared resource metadata rollback failed for id {}",
                                   it->metadata.id);
            }
        }
    };

    // Exact identity and root ownership state form one owner-side publication
    // transaction. Keeping the identity after a later explicit release is
    // intentional, but a new publication must never become cache-visible if
    // recording or reactivating its root claim fails.
    const ResourceId rootResourceId = root.GetId();
    bool rootIdentityInserted = false;
    bool rootOwnershipChanged = false;
    RootOwnershipState previousRootOwnership = RootOwnershipState::Active;
    try
    {
        const auto existing = m_publishedAssetKeys.find(rootResourceId);
        if (existing != m_publishedAssetKeys.end() &&
            existing->second.assetKey != assetKey)
        {
            outError = {ResourceLoadErrorCode::PublishFailure,
                        "Prepared root collides with another AssetKey identity."};
            return false;
        }
        m_publishedAssetKeys.reserve(m_publishedAssetKeys.size() + 1);
    }
    catch (const std::exception& error)
    {
        outError = {ResourceLoadErrorCode::PublishFailure, error.what()};
        return false;
    }
    catch (...)
    {
        outError = {ResourceLoadErrorCode::PublishFailure,
                    "Could not reserve prepared root ownership state."};
        return false;
    }

    const auto rollbackRootOwnership = [&]() noexcept
    {
        if (rootIdentityInserted)
        {
            m_publishedAssetKeys.erase(rootResourceId);
        }
        else if (rootOwnershipChanged)
        {
            const auto found = m_publishedAssetKeys.find(rootResourceId);
            if (found != m_publishedAssetKeys.end())
            {
                found->second.ownership = previousRootOwnership;
            }
        }
    };

    try
    {
        const auto [identity, insertedIdentity] =
            m_publishedAssetKeys.emplace(
                rootResourceId,
                PublishedRootRecord{assetKey, RootOwnershipState::Active});
        if (!insertedIdentity && identity->second.assetKey != assetKey)
        {
            throw std::runtime_error("Prepared root AssetKey identity changed during publication.");
        }
        rootIdentityInserted = insertedIdentity;
        if (!insertedIdentity)
        {
            previousRootOwnership = identity->second.ownership;
            rootOwnershipChanged =
                previousRootOwnership != RootOwnershipState::Active;
            identity->second.ownership = RootOwnershipState::Active;
        }

        std::vector<IResource*> cacheBatch;
        cacheBatch.reserve(staged.size());
        for (StagedPublication& publication : staged)
        {
            m_registry->Register(publication.metadata);
            publication.registryChanged = true;
            if (publication.graphExisted)
            {
                m_dependencyGraph->UpdateDependencies(publication.metadata.id,
                                                      publication.metadata.dependencies);
            }
            else
            {
                m_dependencyGraph->AddResource(publication.metadata.id,
                                               publication.metadata.dependencies);
            }
            publication.graphChanged = true;
            cacheBatch.push_back(publication.resource.Get());
        }
        if (!m_cache->StorePreparedBatch(cacheBatch))
        {
            rollbackMetadata();
            rollbackRootOwnership();
            outError = {ResourceLoadErrorCode::PublishFailure,
                        "Resource cache rejected the prepared publication batch."};
            return false;
        }

        // The receipt becomes visible only after every publication owner has
        // committed successfully. A failed identity check above therefore
        // cannot leave a partially marked root in the registry or cache.
        root->SetContentVerificationReceipt(std::move(contentVerificationReceipt));
        // StorePreparedBatch() plus receipt installation is the owner-side
        // commit point. No later observer or tracing error may fail the load.
        publicationCommitted = true;
    }
    catch (const std::exception& error)
    {
        rollbackMetadata();
        rollbackRootOwnership();
        outError = {ResourceLoadErrorCode::PublishFailure, error.what()};
        return false;
    }
    catch (...)
    {
        rollbackMetadata();
        rollbackRootOwnership();
        outError = {ResourceLoadErrorCode::PublishFailure,
                    "An unknown error interrupted the prepared resource transaction."};
        return false;
    }

    if (!publicationCommitted)
    {
        outError = {ResourceLoadErrorCode::PublishFailure,
                    "Prepared resource publication did not reach its commit point."};
        return false;
    }

    if (m_assetResidencyControl)
    {
        try
        {
            m_assetResidencyControl->CancelExactUnload(assetKey);
        }
        catch (...)
        {
            RVX_RESOURCE_ERROR("Committed resource {} could not cancel a stale residency unload",
                               root.GetId());
        }
    }

    // No rollback-capable work remains. Release admission serialization before
    // calling arbitrary observers so callbacks may safely enqueue another load
    // without deadlocking on m_loadMutex.
    publicationLock.unlock();

    // The owner databases and cache are now committed. Resource callbacks are
    // observers, not transaction participants: isolate exceptions so a bad
    // callback cannot roll back visible resources or leak contradictory Ready
    // events. Dependencies are still announced before the root.
    auto notifyCommitted = [this](const ResourceHandle<IResource>& resource)
    {
        if (!resource)
            return;
        try
        {
            resource->NotifyLoadedObserver();
        }
        catch (const std::exception& error)
        {
            RVX_RESOURCE_ERROR("Resource {} on-loaded observer failed: {}",
                               resource.GetId(),
                               error.what());
        }
        catch (...)
        {
            RVX_RESOURCE_ERROR("Resource {} on-loaded observer failed with an unknown exception",
                               resource.GetId());
        }

        try
        {
            QueueLifecycleEvent(ResourceLifecycleEventType::Ready, resource.Get());
        }
        catch (...)
        {
            RVX_RESOURCE_ERROR("Resource {} Ready lifecycle enqueue failed",
                               resource.GetId());
        }
    };
    for (const StagedPublication& publication : staged)
    {
        if (publication.resource.Get() != root.Get())
        {
            notifyCommitted(publication.resource);
        }
    }
    notifyCommitted(root);

    try
    {
        SetLastLoadDiagnostic(MakeLoadDiagnostic(
            resolution,
            true,
            ResourceLoadFailureCode::None,
            "Prepared resource bundle published on the owner thread."));
        RegisterHotReloadResource(root.Get(), resolution);
        Diagnostics::RecordTraceInstant(
            m_config.startupTraceContext,
            "CPUPublishCommitted",
            {{"requestedPath", requestedPath}, {"resourceId", root.GetId()}});
    }
    catch (const std::exception& error)
    {
        RVX_RESOURCE_ERROR("Committed resource {} post-publish observer failed: {}",
                           root.GetId(),
                           error.what());
    }
    catch (...)
    {
        RVX_RESOURCE_ERROR("Committed resource {} post-publish observer failed with an unknown exception",
                           root.GetId());
    }
    try
    {
        publishSpan.SetAttribute("resourceId", root.GetId());
        publishSpan.SetAttribute("result", "published");
    }
    catch (const std::exception& error)
    {
        RVX_RESOURCE_ERROR("Committed resource {} publication trace failed: {}",
                           root.GetId(),
                           error.what());
    }
    catch (...)
    {
        RVX_RESOURCE_ERROR("Committed resource {} publication trace failed with an unknown exception",
                           root.GetId());
    }
    return true;
    }
    catch (const std::exception& error)
    {
        if (publicationCommitted)
        {
            RVX_RESOURCE_ERROR("Committed resource publication observer failed: {}", error.what());
            return true;
        }
        outError = {ResourceLoadErrorCode::PublishFailure, error.what()};
        return false;
    }
    catch (...)
    {
        if (publicationCommitted)
        {
            RVX_RESOURCE_ERROR("Committed resource publication observer failed with an unknown exception");
            return true;
        }
        outError = {ResourceLoadErrorCode::PublishFailure,
                    "An unknown exception interrupted prepared resource publication."};
        return false;
    }
}

void ResourceManager::RemoveInFlightLoad(const AssetKey& assetKey,
                                         ResourceLoadRequestId requestId)
{
    std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
    const auto it = m_inFlightLoads.find(assetKey);
    if (it != m_inFlightLoads.end() && it->second.GetRequestId() == requestId)
    {
        m_inFlightLoads.erase(it);
    }
}

bool ResourceManager::IsOwnerThread() const
{
    return m_ownerThreadToken != 0 && m_ownerThreadToken == GetCurrentThreadToken();
}

void ResourceManager::SetLifecycleEventCallback(
    ResourceLifecycleEventCallback callback)
{
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    m_lifecycleEventCallback = std::move(callback);
}

void ResourceManager::SetClosureRetirementCallback(
    ResourceClosureRetirementCallback callback)
{
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    m_closureRetirementCallback = std::move(callback);
}

void ResourceManager::QueueLifecycleEvent(ResourceLifecycleEventType type,
                                          IResource* resource)
{
    if (resource == nullptr || resource->GetId() == InvalidResourceId)
        return;

    ResourceLifecycleEvent event;
    event.type = type;
    event.resourceId = resource->GetId();
    event.resource = ResourceHandle<IResource>(resource);
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    m_lifecycleEvents.push_back(std::move(event));
}

void ResourceManager::DrainLifecycleEvents()
{
    std::vector<ResourceLifecycleEvent> events;
    ResourceLifecycleEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        events.swap(m_lifecycleEvents);
        callback = m_lifecycleEventCallback;
    }

    std::unordered_set<IResource*> reloadedResources;
    reloadedResources.reserve(events.size());
    for (const ResourceLifecycleEvent& event : events)
    {
        if (event.type == ResourceLifecycleEventType::Reloaded)
        {
            reloadedResources.insert(event.resource.Get());
        }
    }

    for (const ResourceLifecycleEvent& event : events)
    {
        if (event.type == ResourceLifecycleEventType::Ready &&
            reloadedResources.contains(event.resource.Get()))
        {
            continue;
        }
        if (callback)
        {
            try
            {
                callback(event);
            }
            catch (const std::exception& error)
            {
                RVX_RESOURCE_ERROR("Resource lifecycle observer failed for {}: {}",
                                   event.resourceId,
                                   error.what());
            }
            catch (...)
            {
                RVX_RESOURCE_ERROR("Resource lifecycle observer failed for {} with an unknown exception",
                                   event.resourceId);
            }
        }
        if (event.type == ResourceLifecycleEventType::Reloaded &&
            m_reloadCallback)
        {
            try
            {
                m_reloadCallback(event.resourceId, event.resource.Get());
            }
            catch (const std::exception& error)
            {
                RVX_RESOURCE_ERROR("Resource reload observer failed for {}: {}",
                                   event.resourceId,
                                   error.what());
            }
            catch (...)
            {
                RVX_RESOURCE_ERROR("Resource reload observer failed for {} with an unknown exception",
                                   event.resourceId);
            }
        }
    }
}

ResourceManager::Stats ResourceManager::GetStats() const
{
    std::lock_guard<std::recursive_mutex> loadLock(m_loadMutex);

    Stats stats;

    if (m_registry)
    {
        stats.totalResources = m_registry->GetCount();
    }

    if (m_cache)
    {
        auto cacheStats = m_cache->GetStats();
        stats.loadedCount = cacheStats.totalResources;
        stats.cpuMemory = cacheStats.memoryUsage;
        stats.gpuMemory = cacheStats.gpuMemoryUsage;
    }

    stats.pendingLoads += m_pendingAsyncJobCount.load(std::memory_order_relaxed);
    stats.pendingLoads += m_pendingAsyncCompletionCount.load(std::memory_order_relaxed);
    if (m_modelTextureStreaming)
    {
        const ModelTextureStreamingStats streaming =
            m_modelTextureStreaming->GetStats();
        stats.pendingLoads += streaming.queuedDecodes;
        stats.pendingLoads += streaming.activeDecodes;
        stats.pendingLoads += streaming.pendingPublications;
    }

    return stats;
}

ResourceDiagnosticsSnapshot ResourceManager::GetDiagnosticsSnapshot() const
{
    ResourceDiagnosticsSnapshot snapshot;
    {
        std::lock_guard<std::recursive_mutex> loadLock(m_loadMutex);
        snapshot.activeOperations = m_inFlightLoads.size();
        for (const auto& [assetKey, operation] : m_inFlightLoads)
        {
            (void)assetKey;
            snapshot.activeSubscribers += operation.GetSnapshot().subscriberCount;
        }

        if (m_cache)
        {
            const ResourceCache::Stats cache = m_cache->GetStats();
            snapshot.cacheEntryCount = cache.totalResources;
            snapshot.cacheCPUBytes = cache.memoryUsage;
            snapshot.cacheGPUBytes = cache.gpuMemoryUsage;
            snapshot.cacheHits = cache.hitCount;
            snapshot.cacheMisses = cache.missCount;
        }
    }

    snapshot.pendingAsyncJobs =
        m_pendingAsyncJobCount.load(std::memory_order_relaxed);
    snapshot.pendingAsyncCompletions =
        m_pendingAsyncCompletionCount.load(std::memory_order_relaxed);
    snapshot.cancelledLoadCount =
        m_cancelledLoadCount.load(std::memory_order_relaxed);
    snapshot.closureUnloadRequestCount =
        m_closureUnloadRequestCount.load(std::memory_order_relaxed);
    snapshot.closureUnloadedResourceCount =
        m_closureUnloadedResourceCount.load(std::memory_order_relaxed);
    snapshot.closureRetainedResourceCount =
        m_closureRetainedResourceCount.load(std::memory_order_relaxed);
    snapshot.closureUnloadRejectedCount =
        m_closureUnloadRejectedCount.load(std::memory_order_relaxed);

    if (m_modelTextureStreaming)
    {
        const ModelTextureStreamingStats streaming =
            m_modelTextureStreaming->GetStats();
        snapshot.decodeQueuedCount = streaming.queuedDecodes;
        snapshot.decodeActiveCount = streaming.activeDecodes;
        snapshot.decodeCompletedCount = streaming.completedDecodes;
        snapshot.decodeFailedCount = streaming.failedDecodes;
        snapshot.decodeCancelledCount = streaming.cancelledDecodes;
        snapshot.decodeCompletedBytes = streaming.completedDecodedBytes;
        snapshot.decodeReservedBytes = streaming.reservedDecodedBytes;
        snapshot.decodePeakReservedBytes = streaming.peakReservedDecodedBytes;
        snapshot.decodeBudgetBytes = streaming.decodedByteBudget;
        snapshot.pendingPublicationCount = streaming.pendingPublications;
    }

    if (m_assetResidencyControl)
    {
        const AssetResidencyLeaseControl::Snapshot residency =
            m_assetResidencyControl->GetSnapshot();
        snapshot.activeLeaseCount = residency.activeLeases;
        snapshot.protectedResourceCount = residency.protectedResources;
        snapshot.queuedLeaseUnloadCount = residency.queuedUnloads;
        snapshot.leaseBlockedEvictionCount = residency.blockedEvictions;
    }
    return snapshot;
}

ResourceType ResourceManager::GetTypeFromExtension(const std::string& extension)
{
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Model formats (3D files with scene hierarchy)
    if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".model")
        return ResourceType::Model;

    // Mesh formats (standalone mesh files)
    if (ext == ".obj")
        return ResourceType::Mesh;

    // Texture formats
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds" ||
        ext == ".tga" || ext == ".bmp" || ext == ".hdr" || ext == ".gif")
        return ResourceType::Texture;

    // Material
    if (ext == ".mat" || ext == ".material")
        return ResourceType::Material;

    // Shader
    if (ext == ".hlsl" || ext == ".glsl" || ext == ".shader")
        return ResourceType::Shader;

    // Animation
    if (ext == ".anim" || ext == ".animation" || ext == ".rvxanim")
        return ResourceType::Animation;

    // Audio
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg")
        return ResourceType::Audio;

    // Scene
    if (ext == ".scene")
        return ResourceType::Scene;

    return ResourceType::Unknown;
}

std::string ResourceManager::ResolvePath(const std::string& path) const
{
    if (m_config.basePath.empty())
    {
        return path;
    }

    std::filesystem::path basePath(m_config.basePath);
    std::filesystem::path resourcePath(path);

    if (resourcePath.is_absolute())
    {
        return path;
    }

    return (basePath / resourcePath).string();
}

void ResourceManager::StartAsyncWorkers()
{
    const int requestedCount = m_config.asyncThreadCount;
    if (requestedCount <= 0)
    {
        RVX_RESOURCE_INFO("ResourceManager async loading uses inline fallback because asyncThreadCount is disabled");
        return;
    }

    JobSystem& jobSystem = JobSystem::Get();
    if (jobSystem.IsInitialized())
    {
        RVX_RESOURCE_INFO("ResourceManager async loading using existing Core JobSystem with {} worker(s)",
                          jobSystem.GetWorkerCount());
        return;
    }

    jobSystem.Initialize(static_cast<size_t>(requestedCount));
    m_jobSystemInitializedByManager = true;
    RVX_RESOURCE_INFO("ResourceManager initialized Core JobSystem with {} worker(s) for async loading",
                      jobSystem.GetWorkerCount());
}

void ResourceManager::StopAsyncWorkers()
{
    std::vector<JobHandle> preparedJobs;
    {
        std::lock_guard<std::mutex> lock(m_preparedJobMutex);
        preparedJobs.swap(m_preparedJobs);
    }
    for (const JobHandle& job : preparedJobs)
    {
        job.Wait();
    }

    JobSystem& jobSystem = JobSystem::Get();
    if (m_jobSystemInitializedByManager)
    {
        jobSystem.Shutdown();
        m_jobSystemInitializedByManager = false;
    }
}

// IResourceLoader implementation
bool IResourceLoader::Prepare(const ResourceLoadPreparationContext& context,
                              PreparedResourceBundle& outBundle,
                              ResourceLoadError& outError)
{
    (void)context;
    (void)outBundle;
    outError = {ResourceLoadErrorCode::LoaderFailure,
                "This resource loader does not implement allocation-free prepared loading."};
    return false;
}

bool IResourceLoader::CapturePreparationState(
    uint64 requestedImportOptionsHash,
    ResourceLoadPreparationStateRef& outState,
    uint64& outCanonicalImportOptionsHash,
    ResourceLoadError& outError) const
{
    outState.reset();
    outCanonicalImportOptionsHash = requestedImportOptionsHash;
    outError = {};
    return true;
}

bool IResourceLoader::CanLoad(const std::string& path) const
{
    std::filesystem::path fsPath(path);
    std::string ext = fsPath.extension().string();

    auto supported = GetSupportedExtensions();
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

} // namespace RVX::Resource
