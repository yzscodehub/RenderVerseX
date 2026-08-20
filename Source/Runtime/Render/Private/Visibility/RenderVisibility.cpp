#include "Render/Visibility/RenderVisibility.h"

#include <algorithm>
#include <cmath>

namespace RVX
{
namespace
{
    [[nodiscard]] bool IsFiniteVec3(const Vec3& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    [[nodiscard]] Vec4 GetRow(const Mat4& matrix, uint32 row) noexcept
    {
        return Vec4(matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]);
    }

    [[nodiscard]] size_t GetPassIndex(RenderPassKind pass) noexcept
    {
        return static_cast<size_t>(pass);
    }

    void InitializePassResults(RenderVisibilityResult& result)
    {
        for (size_t index = 0; index < result.passes.size(); ++index)
        {
            result.passes[index] = {};
            result.passes[index].pass = static_cast<RenderPassKind>(index);
        }
    }
} // namespace

RenderVisibilityFrustum RenderVisibilityFrustum::FromViewProjection(
    const Mat4& viewProjection) noexcept
{
    // Column-vector convention: clip = (projection * view) * world.
    // The backend upload Y flip is intentionally excluded from visibility.
    const Vec4 row0 = GetRow(viewProjection, 0);
    const Vec4 row1 = GetRow(viewProjection, 1);
    const Vec4 row2 = GetRow(viewProjection, 2);
    const Vec4 row3 = GetRow(viewProjection, 3);
    RenderVisibilityFrustum result;
    result.planes = {
        row3 + row0, row3 - row0,
        row3 + row1, row3 - row1,
        row2,        row3 - row2,
    };
    return result;
}

bool IsFiniteRenderVisibilityBounds(const AABB& bounds) noexcept
{
    return bounds.IsValid() && IsFiniteVec3(bounds.GetMin()) &&
           IsFiniteVec3(bounds.GetMax());
}

RenderVisibilityGPUInput MakeRenderVisibilityGPUInput(const AABB& bounds) noexcept
{
    RenderVisibilityGPUInput result;
    if (!IsFiniteRenderVisibilityBounds(bounds))
    {
        result.forceVisible = 1;
        return result;
    }

    result.aabbMin = Vec4(bounds.GetMin(), 0.0f);
    result.aabbMax = Vec4(bounds.GetMax(), 0.0f);
    return result;
}

bool IsRenderVisibilityAABBVisible(
    const AABB& bounds,
    const RenderVisibilityFrustum& frustum,
    bool* outInvalidBounds) noexcept
{
    const bool valid = IsFiniteRenderVisibilityBounds(bounds);
    if (outInvalidBounds != nullptr)
    {
        *outInvalidBounds = !valid;
    }
    if (!valid)
    {
        return true;
    }

    const Vec3 center = bounds.GetCenter();
    const Vec3 extent = bounds.GetExtent();
    for (const Vec4& plane : frustum.planes)
    {
        const Vec3 normal{plane.x, plane.y, plane.z};
        const float normalLengthSquared = dot(normal, normal);
        // Infinite/reverse-Z projections can yield a degenerate far plane.
        if (!std::isfinite(normalLengthSquared) || normalLengthSquared <= 1.0e-12f)
        {
            continue;
        }
        const float distance = dot(normal, center) + plane.w;
        const float projectedRadius = dot(glm::abs(normal), extent);
        // Equality remains visible so boundary-touching objects never flicker.
        if (distance < -projectedRadius)
        {
            return false;
        }
    }
    return true;
}

void RenderCandidateSet::Clear()
{
    m_candidates.clear();
    for (std::vector<uint32>& mapping : m_sourceToCandidate)
    {
        mapping.clear();
    }
    m_valid = true;
}

uint32 RenderCandidateSet::Add(RenderVisibilityCandidate candidate)
{
    const size_t passIndex = GetPassIndex(candidate.pass);
    const bool sceneCandidate = candidate.pass == RenderPassKind::None;
    const bool sourceIdentityValid = sceneCandidate
        ? candidate.sourcePacketIndex == RVX_INVALID_INDEX
        : passIndex < m_sourceToCandidate.size() &&
              candidate.sourcePacketIndex != RVX_INVALID_INDEX;
    if (!m_valid || candidate.objectIndex == RVX_INVALID_INDEX ||
        !sourceIdentityValid ||
        m_candidates.size() >= static_cast<size_t>(RVX_INVALID_INDEX))
    {
        m_valid = false;
        return RVX_INVALID_INDEX;
    }

    candidate.candidateIndex = static_cast<uint32>(m_candidates.size());
    const uint32 index = candidate.candidateIndex;
    if (!sceneCandidate)
    {
        std::vector<uint32>& mapping = m_sourceToCandidate[passIndex];
        if (mapping.size() <= candidate.sourcePacketIndex)
        {
            mapping.resize(static_cast<size_t>(candidate.sourcePacketIndex) + 1,
                           RVX_INVALID_INDEX);
        }
        else if (mapping[candidate.sourcePacketIndex] != RVX_INVALID_INDEX)
        {
            // A pass/source packet is a total identity key. Accepting an
            // overwrite would make Direct filtering order-dependent.
            m_valid = false;
            return RVX_INVALID_INDEX;
        }
        mapping[candidate.sourcePacketIndex] = index;
    }
    m_candidates.push_back(candidate);
    return index;
}

const RenderVisibilityCandidate* RenderCandidateSet::Find(
    RenderPassKind pass,
    uint32 sourcePacketIndex) const noexcept
{
    const size_t passIndex = GetPassIndex(pass);
    if (pass == RenderPassKind::None || passIndex >= m_sourceToCandidate.size())
    {
        return nullptr;
    }
    const std::vector<uint32>& mapping = m_sourceToCandidate[passIndex];
    if (sourcePacketIndex >= mapping.size() ||
        mapping[sourcePacketIndex] == RVX_INVALID_INDEX ||
        mapping[sourcePacketIndex] >= m_candidates.size())
    {
        return nullptr;
    }
    return &m_candidates[mapping[sourcePacketIndex]];
}

bool RenderVisibilityPassResult::IsSourcePacketVisible(uint32 sourcePacketIndex) const noexcept
{
    return sourcePacketIndex < cpuVisibleBySourcePacket.size() &&
           cpuVisibleBySourcePacket[sourcePacketIndex] != 0;
}

bool RenderVisibilityPassResult::HasSourcePacket(uint32 sourcePacketIndex) const noexcept
{
    return sourcePacketIndex < sourcePacketKnown.size() &&
           sourcePacketKnown[sourcePacketIndex] != 0;
}

bool RenderVisibilityResult::IsDirectPacketVisible(
    RenderPassKind pass,
    uint32 sourcePacketIndex) const noexcept
{
    const size_t passIndex = GetPassIndex(pass);
    return structurallyValid && passIndex < passes.size() &&
           passes[passIndex].IsSourcePacketVisible(sourcePacketIndex);
}

bool RenderVisibilityResult::HasDirectPacketSource(
    RenderPassKind pass,
    uint32 sourcePacketIndex) const noexcept
{
    const size_t passIndex = GetPassIndex(pass);
    return structurallyValid && passIndex < passes.size() &&
           passes[passIndex].HasSourcePacket(sourcePacketIndex);
}

void CPUVisibilityProvider::Evaluate(const RenderCandidateSet& candidates,
                                     const RenderVisibilityRequest& request,
                                     RenderVisibilityResult& outResult) const
{
    outResult = {};
    InitializePassResults(outResult);
    outResult.structurallyValid = candidates.IsValid();
    if (!outResult.structurallyValid)
    {
        return;
    }
    const RenderVisibilityFrustum frustum =
        RenderVisibilityFrustum::FromViewProjection(request.viewProjection);

    for (const RenderVisibilityCandidate& candidate : candidates.GetCandidates())
    {
        if (!candidate.objectVisible || !candidate.drawable)
        {
            continue;
        }

        bool invalidBounds = false;
        bool cpuVisible = IsRenderVisibilityAABBVisible(
            candidate.worldBounds, frustum, &invalidBounds);
        if (invalidBounds && candidate.pass == RenderPassKind::None)
        {
            ++outResult.diagnostics.invalidBoundsCount;
        }
        if (cpuVisible && !invalidBounds &&
            request.enableDistanceCulling && request.maxDrawDistance > 0.0f)
        {
            const float radius = length(candidate.worldBounds.GetExtent());
            cpuVisible = length(candidate.worldBounds.GetCenter() -
                                request.cameraPosition) - radius <=
                         request.maxDrawDistance;
        }

        if (candidate.pass == RenderPassKind::None)
        {
            ++outResult.diagnostics.sceneObjectCandidateCount;
            if (outResult.coarseVisibleObjectMask.size() <= candidate.objectIndex)
            {
                outResult.coarseVisibleObjectMask.resize(
                    static_cast<size_t>(candidate.objectIndex) + 1, 0);
                outResult.cpuVisibleObjectMask.resize(
                    static_cast<size_t>(candidate.objectIndex) + 1, 0);
            }
            outResult.coarseVisibleObjectMask[candidate.objectIndex] = 1;
            outResult.coarseVisibleObjectIndices.push_back(candidate.objectIndex);
            if (cpuVisible)
            {
                ++outResult.diagnostics.cpuVisibleObjectCount;
                outResult.cpuVisibleObjectMask[candidate.objectIndex] = 1;
                outResult.cpuVisibleObjectIndices.push_back(candidate.objectIndex);
            }
            continue;
        }

        const size_t passIndex = GetPassIndex(candidate.pass);
        if (passIndex >= outResult.passes.size() ||
            candidate.sourcePacketIndex == RVX_INVALID_INDEX)
        {
            continue;
        }

        RenderVisibilityPassResult& passResult = outResult.passes[passIndex];
        ++outResult.diagnostics.passCandidateCount;
        passResult.coarseCandidateIndices.push_back(candidate.candidateIndex);
        if (passResult.cpuVisibleBySourcePacket.size() <= candidate.sourcePacketIndex)
        {
            passResult.cpuVisibleBySourcePacket.resize(
                static_cast<size_t>(candidate.sourcePacketIndex) + 1, 0);
        }
        if (passResult.sourcePacketKnown.size() <= candidate.sourcePacketIndex)
        {
            passResult.sourcePacketKnown.resize(
                static_cast<size_t>(candidate.sourcePacketIndex) + 1, 0);
        }
        passResult.sourcePacketKnown[candidate.sourcePacketIndex] = 1;
        passResult.cpuVisibleBySourcePacket[candidate.sourcePacketIndex] =
            cpuVisible ? 1 : 0;
    }
}

void GPUVisibilityProvider::Evaluate(const RenderCandidateSet& candidates,
                                     const RenderVisibilityRequest& request,
                                     RenderVisibilityResult& outResult) const
{
    CPUVisibilityProvider cpu;
    cpu.Evaluate(candidates, request, outResult);
    if (outResult.structurallyValid)
    {
        outResult.diagnostics.gpuDeferredCandidateCount = static_cast<uint32>(
            outResult.passes[GetPassIndex(RenderPassKind::Depth)]
                .coarseCandidateIndices.size() +
            outResult.passes[GetPassIndex(RenderPassKind::Opaque)]
                .coarseCandidateIndices.size());
    }
    outResult.diagnostics.gpuReadbackPerformed = false;
    // HZB is explicitly unavailable in Task 8. A config request may be
    // recorded by diagnostics but cannot enable an unimplemented path.
    outResult.diagnostics.occlusionAvailable = false;
}

void GPUVisibilityProvider::AppendPassCandidates(
    const RenderCandidateSet& candidates,
    uint32 firstCandidateIndex,
    RenderVisibilityResult& inOutResult) const
{
    const std::vector<RenderVisibilityCandidate>& allCandidates =
        candidates.GetCandidates();
    if (!inOutResult.structurallyValid || !candidates.IsValid() ||
        firstCandidateIndex > allCandidates.size())
    {
        inOutResult.structurallyValid = false;
        InitializePassResults(inOutResult);
        return;
    }

    for (size_t index = firstCandidateIndex;
         index < allCandidates.size();
         ++index)
    {
        const RenderVisibilityCandidate& candidate = allCandidates[index];
        const size_t passIndex = GetPassIndex(candidate.pass);
        if (candidate.candidateIndex != index ||
            candidate.pass == RenderPassKind::None ||
            passIndex >= inOutResult.passes.size() ||
            candidate.sourcePacketIndex == RVX_INVALID_INDEX)
        {
            inOutResult.structurallyValid = false;
            InitializePassResults(inOutResult);
            return;
        }
        if (!candidate.objectVisible || !candidate.drawable)
        {
            continue;
        }

        RenderVisibilityPassResult& passResult =
            inOutResult.passes[passIndex];
        ++inOutResult.diagnostics.passCandidateCount;
        passResult.coarseCandidateIndices.push_back(candidate.candidateIndex);
        if (passResult.cpuVisibleBySourcePacket.size() <=
            candidate.sourcePacketIndex)
        {
            passResult.cpuVisibleBySourcePacket.resize(
                static_cast<size_t>(candidate.sourcePacketIndex) + 1, 0);
            passResult.sourcePacketKnown.resize(
                static_cast<size_t>(candidate.sourcePacketIndex) + 1, 0);
        }
        passResult.sourcePacketKnown[candidate.sourcePacketIndex] = 1;
        const bool objectVisible =
            candidate.objectIndex < inOutResult.cpuVisibleObjectMask.size() &&
            inOutResult.cpuVisibleObjectMask[candidate.objectIndex] != 0;
        passResult.cpuVisibleBySourcePacket[candidate.sourcePacketIndex] =
            objectVisible ? 1 : 0;

        if (candidate.pass == RenderPassKind::Depth ||
            candidate.pass == RenderPassKind::Opaque)
        {
            ++inOutResult.diagnostics.gpuDeferredCandidateCount;
        }
    }
    inOutResult.diagnostics.gpuReadbackPerformed = false;
    inOutResult.diagnostics.occlusionAvailable = false;
}
} // namespace RVX
