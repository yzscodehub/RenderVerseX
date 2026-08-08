/**
 * @file RenderDrawPacketCache.cpp
 * @brief Value-only retained draw packet template cache.
 */

#include "Render/Renderer/RenderDrawPacketCache.h"

namespace RVX
{
namespace
{
    uint64 MixIdentity(uint64 value) noexcept
    {
        value ^= value >> 33U;
        value *= 0xff51afd7ed558ccdULL;
        value ^= value >> 33U;
        value *= 0xc4ceb9fe1a85ec53ULL;
        value ^= value >> 33U;
        return value;
    }

} // namespace

uint64 RenderDrawPacketCacheStats::GetInvalidationCount(
    RenderDrawPacketCacheInvalidationReason reason) const noexcept
{
    const size_t index = static_cast<size_t>(reason);
    return index < invalidationCounts.size() ? invalidationCounts[index] : 0;
}

void RenderDrawPacketCache::BeginAcceptedPublication(bool pruneUnobserved) noexcept
{
    ++m_publicationGeneration;
    if (m_publicationGeneration == 0)
    {
        ++m_publicationGeneration;
    }
    m_publicationActive = true;
    m_pruneUnobserved = pruneUnobserved;
}

void RenderDrawPacketCache::EndAcceptedPublication()
{
    if (!m_publicationActive)
    {
        return;
    }

    if (m_pruneUnobserved)
    {
        for (auto entry = m_entries.begin(); entry != m_entries.end();)
        {
            if (entry->second.lastPublishedGeneration == m_publicationGeneration)
            {
                ++entry;
                continue;
            }

            entry = m_entries.erase(entry);
            RecordInvalidation(RenderDrawPacketCacheInvalidationReason::ObjectRemoved);
        }
    }
    m_publicationActive = false;
    m_pruneUnobserved = true;
}

bool RenderDrawPacketCache::Remove(RenderObjectId objectId,
                                   uint32 submeshIndex) noexcept
{
    const size_t erased = m_entries.erase(EntryKey{objectId, submeshIndex});
    if (erased != 0)
        RecordInvalidation(RenderDrawPacketCacheInvalidationReason::ObjectRemoved);
    return erased != 0;
}

void RenderDrawPacketCache::Clear()
{
    m_entries.clear();
    m_publicationActive = false;
    m_pruneUnobserved = true;
    ++m_stats.clearCount;
}

RenderDrawPacketCacheResolveResult RenderDrawPacketCache::Find(
    const MeshBatch& batch,
    const RenderDrawPacketCacheVersions& versions,
    RenderDrawPacket& outTemplate) const noexcept
{
    return FindExact(batch, versions, outTemplate);
}

RenderDrawPacketCacheResolveResult RenderDrawPacketCache::Resolve(
    const MeshBatch& batch,
    const RenderDrawPacketCacheVersions& versions,
    bool dynamicBypass,
    RenderDrawPacket& outTemplate)
{
    if (!m_publicationActive)
    {
        return {RenderDrawPacketCacheResolveCode::PublicationInactive,
                RenderDrawPacketCacheInvalidationReason::None};
    }

    ++m_stats.resolveCount;
    const EntryKey key{batch.objectId, batch.submeshIndex};
    if (dynamicBypass)
    {
        ++m_stats.dynamicBypassCount;
        const auto entry = m_entries.find(key);
        if (entry != m_entries.end())
        {
            m_entries.erase(entry);
            RecordInvalidation(
                RenderDrawPacketCacheInvalidationReason::DynamicBypass);
        }
        return {RenderDrawPacketCacheResolveCode::DynamicBypass,
                RenderDrawPacketCacheInvalidationReason::DynamicBypass};
    }

    const RenderDrawPacketStaticSignature signature =
        MakeSignature(batch, versions);
    const auto existing = m_entries.find(key);
    if (existing != m_entries.end() && existing->second.signature == signature)
    {
        outTemplate = existing->second.packetTemplate;
        if (m_publicationActive)
        {
            existing->second.lastPublishedGeneration = m_publicationGeneration;
        }
        ++m_stats.hitCount;
        return {RenderDrawPacketCacheResolveCode::Hit,
                RenderDrawPacketCacheInvalidationReason::None};
    }

    const RenderDrawPacketCacheInvalidationReason invalidationReason =
        existing == m_entries.end()
            ? RenderDrawPacketCacheInvalidationReason::None
            : ClassifyMismatch(existing->second.signature, signature);
    ++m_stats.missCount;
    RecordInvalidation(invalidationReason);

    if (m_publicationActive)
    {
        RenderDrawPacket packetTemplate = BuildLegacyMaterialDrawPacket(batch);
        packetTemplate.objectId = batch.objectId;
        packetTemplate.primitiveData = RVX_INVALID_PRIMITIVE_DATA_INDEX;
        packetTemplate.submeshIndex = batch.submeshIndex;
        const auto [entry, inserted] = m_entries.insert_or_assign(
            key,
            Entry{signature, packetTemplate, m_publicationGeneration});
        outTemplate = entry->second.packetTemplate;
        if (inserted)
        {
            ++m_stats.entryCreationCount;
        }
        ++m_stats.packetBuildCount;
    }
    return {RenderDrawPacketCacheResolveCode::Miss, invalidationReason};
}

RenderDrawPacketCacheStats RenderDrawPacketCache::GetStats() const noexcept
{
    RenderDrawPacketCacheStats stats = m_stats;
    stats.entryCount = m_entries.size();
    return stats;
}

RenderDrawPacketStaticSignature RenderDrawPacketCache::MakeSignature(
    const MeshBatch& batch,
    const RenderDrawPacketCacheVersions& versions) noexcept
{
    RenderDrawPacketStaticSignature signature;
    signature.mesh = batch.mesh;
    signature.material = batch.material;
    signature.indexType = batch.indexType;
    signature.indexOffset = batch.geometry.indexOffset;
    signature.indexCount = batch.geometry.indexCount;
    signature.baseVertex = batch.geometry.baseVertex;
    signature.topology = batch.geometry.topology;
    signature.materialMode = batch.materialMode;
    signature.flags = batch.flags;
    signature.objectRevision = batch.objectRevision;
    signature.versions = versions;
    return signature;
}

size_t RenderDrawPacketCache::EntryKeyHasher::operator()(
    const EntryKey& key) const noexcept
{
    return static_cast<size_t>(MixIdentity(
        key.objectId ^ (static_cast<uint64>(key.submeshIndex) << 32U)));
}

RenderDrawPacketCacheResolveResult RenderDrawPacketCache::FindExact(
    const MeshBatch& batch,
    const RenderDrawPacketCacheVersions& versions,
    RenderDrawPacket& outTemplate) const noexcept
{
    const EntryKey key{batch.objectId, batch.submeshIndex};
    const auto entry = m_entries.find(key);
    if (entry == m_entries.end())
    {
        return {RenderDrawPacketCacheResolveCode::Miss,
                RenderDrawPacketCacheInvalidationReason::None};
    }

    const RenderDrawPacketStaticSignature requested =
        MakeSignature(batch, versions);
    if (!(entry->second.signature == requested))
    {
        return {RenderDrawPacketCacheResolveCode::Miss,
                ClassifyMismatch(entry->second.signature, requested)};
    }

    outTemplate = entry->second.packetTemplate;
    return {RenderDrawPacketCacheResolveCode::Hit,
            RenderDrawPacketCacheInvalidationReason::None};
}

RenderDrawPacketCacheInvalidationReason
RenderDrawPacketCache::ClassifyMismatch(
    const RenderDrawPacketStaticSignature& existing,
    const RenderDrawPacketStaticSignature& requested) noexcept
{
    if (existing.versions.passContractVersion !=
        requested.versions.passContractVersion)
    {
        return RenderDrawPacketCacheInvalidationReason::PassContractVersionChanged;
    }
    if (existing.versions.shaderLayoutVersion !=
        requested.versions.shaderLayoutVersion)
    {
        return RenderDrawPacketCacheInvalidationReason::ShaderLayoutVersionChanged;
    }
    if (existing.mesh.slot == requested.mesh.slot &&
        existing.mesh.generation != requested.mesh.generation)
    {
        return RenderDrawPacketCacheInvalidationReason::MeshGenerationChanged;
    }
    if (existing.material.slot == requested.material.slot &&
        existing.material.generation != requested.material.generation)
    {
        return RenderDrawPacketCacheInvalidationReason::MaterialGenerationChanged;
    }
    if (existing.objectRevision != requested.objectRevision)
    {
        return RenderDrawPacketCacheInvalidationReason::ObjectRevisionChanged;
    }
    return RenderDrawPacketCacheInvalidationReason::StaticStateChanged;
}

void RenderDrawPacketCache::RecordInvalidation(
    RenderDrawPacketCacheInvalidationReason reason) noexcept
{
    const size_t index = static_cast<size_t>(reason);
    if (reason != RenderDrawPacketCacheInvalidationReason::None &&
        index < m_stats.invalidationCounts.size())
    {
        ++m_stats.invalidationCounts[index];
    }
}
} // namespace RVX
