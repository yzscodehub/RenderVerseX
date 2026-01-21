#include "Spatial/Index/BVHIndex.h"
#include "Spatial/Query/QueryFilter.h"
#include <algorithm>
#include <chrono>

namespace RVX::Spatial
{

BVHIndex::BVHIndex(const BVHConfig& config)
    : m_config(config)
{
}

BVHIndex::~BVHIndex() = default;

void BVHIndex::Build(std::span<ISpatialEntity*> entities)
{
    Clear();

    if (entities.empty()) return;

    auto startTime = std::chrono::high_resolution_clock::now();

    // Copy entities and create primitive indices
    m_entities.assign(entities.begin(), entities.end());
    m_primitiveIndices.resize(m_entities.size());
    
    for (size_t i = 0; i < m_entities.size(); ++i)
    {
        m_primitiveIndices[i] = static_cast<int>(i);
        m_entityIndex[m_entities[i]->GetHandle()] = i;
    }

    // Reserve nodes (worst case: 2n-1 nodes for n primitives)
    m_nodes.reserve(2 * m_entities.size());

    // Build recursively
    BuildRecursive(0, static_cast<int>(m_primitiveIndices.size()), 0);

    auto endTime = std::chrono::high_resolution_clock::now();
    float buildTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    // Store build time in stats (we'd need a member for this)
    (void)buildTime;
}

void BVHIndex::Clear()
{
    m_nodes.clear();
    m_entities.clear();
    m_primitiveIndices.clear();
    m_entityIndex.clear();
    m_pendingInserts.clear();
    m_pendingRemoves.clear();
    m_needsRebuild = false;
}

void BVHIndex::Insert(ISpatialEntity* entity)
{
    if (!entity) return;
    m_pendingInserts.push_back(entity);
    m_needsRebuild = true;
}

void BVHIndex::Remove(EntityHandle handle)
{
    m_pendingRemoves.push_back(handle);
    m_needsRebuild = true;
}

void BVHIndex::Update(ISpatialEntity* entity)
{
    if (!entity) return;
    // For now, mark for rebuild
    // TODO: Implement incremental update
    m_needsRebuild = true;
}

void BVHIndex::Commit()
{
    if (!m_needsRebuild) return;

    // Process removals
    for (auto handle : m_pendingRemoves)
    {
        auto it = m_entityIndex.find(handle);
        if (it != m_entityIndex.end())
        {
            size_t idx = it->second;
            m_entities[idx] = nullptr;
            m_entityIndex.erase(it);
        }
    }
    m_pendingRemoves.clear();

    // Compact and add insertions
    std::vector<ISpatialEntity*> newEntities;
    for (auto* entity : m_entities)
    {
        if (entity) newEntities.push_back(entity);
    }
    for (auto* entity : m_pendingInserts)
    {
        if (entity) newEntities.push_back(entity);
    }
    m_pendingInserts.clear();

    // Rebuild
    Build(newEntities);
    m_needsRebuild = false;
}

int BVHIndex::BuildRecursive(int start, int end, int depth)
{
    int nodeIdx = static_cast<int>(m_nodes.size());
    m_nodes.emplace_back();
    Node& node = m_nodes[nodeIdx];

    // Compute bounds
    node.bounds.Reset();
    for (int i = start; i < end; ++i)
    {
        node.bounds.Expand(m_entities[m_primitiveIndices[i]]->GetWorldBounds());
    }

    int primitiveCount = end - start;

    // Create leaf if few primitives
    if (primitiveCount <= m_config.maxLeafSize)
    {
        node.firstPrimitive = start;
        node.primitiveCount = primitiveCount;
        return nodeIdx;
    }

    // Find best split
    int bestAxis = 0;
    float bestPos = 0.0f;
    
    if (m_config.useSAH)
    {
        FindBestSplit(start, end, bestAxis, bestPos);
    }
    else
    {
        // Simple median split on longest axis
        Vec3 size = node.bounds.GetSize();
        if (size.y > size.x && size.y > size.z) bestAxis = 1;
        else if (size.z > size.x) bestAxis = 2;
        bestPos = node.bounds.GetCenter()[bestAxis];
    }

    // Partition primitives
    int mid = Partition(start, end, bestAxis, bestPos);

    // Handle degenerate case
    if (mid == start || mid == end)
    {
        mid = (start + end) / 2;
    }

    // Build children
    node.leftChild = BuildRecursive(start, mid, depth + 1);
    node.rightChild = BuildRecursive(mid, end, depth + 1);

    return nodeIdx;
}

int BVHIndex::FindBestSplit(int start, int end, int& outAxis, float& outPos)
{
    float bestCost = std::numeric_limits<float>::max();
    outAxis = 0;
    outPos = 0.0f;

    // Compute parent bounds
    AABB parentBounds;
    for (int i = start; i < end; ++i)
    {
        parentBounds.Expand(m_entities[m_primitiveIndices[i]]->GetWorldBounds());
    }

    float parentArea = parentBounds.SurfaceArea();
    if (parentArea <= 0.0f) return (start + end) / 2;

    int primitiveCount = end - start;

    // Try each axis
    for (int axis = 0; axis < 3; ++axis)
    {
        float axisMin = parentBounds.GetMin()[axis];
        float axisMax = parentBounds.GetMax()[axis];
        float step = (axisMax - axisMin) / m_config.sahBuckets;

        if (step <= 0.0f) continue;

        // Try each bucket boundary
        for (int b = 1; b < m_config.sahBuckets; ++b)
        {
            float splitPos = axisMin + b * step;

            AABB leftBounds, rightBounds;
            int leftCount = 0, rightCount = 0;

            for (int i = start; i < end; ++i)
            {
                AABB primBounds = m_entities[m_primitiveIndices[i]]->GetWorldBounds();
                float center = primBounds.GetCenter()[axis];

                if (center < splitPos)
                {
                    leftBounds.Expand(primBounds);
                    leftCount++;
                }
                else
                {
                    rightBounds.Expand(primBounds);
                    rightCount++;
                }
            }

            if (leftCount == 0 || rightCount == 0) continue;

            // SAH cost
            float cost = m_config.traversalCost +
                (leftBounds.SurfaceArea() * leftCount +
                 rightBounds.SurfaceArea() * rightCount) *
                m_config.intersectionCost / parentArea;

            if (cost < bestCost)
            {
                bestCost = cost;
                outAxis = axis;
                outPos = splitPos;
            }
        }
    }

    // Check if split is worthwhile
    float leafCost = primitiveCount * m_config.intersectionCost;
    if (bestCost >= leafCost)
    {
        // Not worth splitting, return mid point
        return (start + end) / 2;
    }

    return -1; // Use the found split
}

int BVHIndex::Partition(int start, int end, int axis, float splitPos)
{
    int left = start;
    int right = end - 1;

    while (left <= right)
    {
        float centerLeft = m_entities[m_primitiveIndices[left]]->GetWorldBounds().GetCenter()[axis];
        
        if (centerLeft < splitPos)
        {
            left++;
        }
        else
        {
            std::swap(m_primitiveIndices[left], m_primitiveIndices[right]);
            right--;
        }
    }

    return left;
}

void BVHIndex::QueryFrustum(
    const Frustum& frustum,
    const QueryFilter& filter,
    std::vector<QueryResult>& outResults) const
{
    if (m_nodes.empty()) return;
    QueryFrustumRecursive(0, frustum, filter, outResults);
}

void BVHIndex::QueryFrustumRecursive(
    int nodeIdx,
    const Frustum& frustum,
    const QueryFilter& filter,
    std::vector<QueryResult>& results) const
{
    const Node& node = m_nodes[nodeIdx];

    // Check frustum intersection
    auto intersection = frustum.Intersects(node.bounds);
    if (intersection == IntersectionResult::Outside) return;

    if (node.IsLeaf())
    {
        // Test individual primitives
        for (int i = 0; i < node.primitiveCount; ++i)
        {
            ISpatialEntity* entity = m_entities[m_primitiveIndices[node.firstPrimitive + i]];
            
            if (!filter.Accepts(entity)) continue;
            
            if (frustum.IsVisible(entity->GetWorldBounds()))
            {
                QueryResult result;
                result.handle = entity->GetHandle();
                result.userData = entity->GetUserData();
                results.push_back(result);
            }
        }
    }
    else
    {
        QueryFrustumRecursive(node.leftChild, frustum, filter, results);
        QueryFrustumRecursive(node.rightChild, frustum, filter, results);
    }
}

void BVHIndex::QueryBox(
    const BoundingBox& box,
    const QueryFilter& filter,
    std::vector<QueryResult>& outResults) const
{
    if (m_nodes.empty()) return;
    QueryBoxRecursive(0, box, filter, outResults);
}

void BVHIndex::QueryBoxRecursive(
    int nodeIdx,
    const BoundingBox& box,
    const QueryFilter& filter,
    std::vector<QueryResult>& results) const
{
    const Node& node = m_nodes[nodeIdx];

    if (!node.bounds.Overlaps(box)) return;

    if (node.IsLeaf())
    {
        for (int i = 0; i < node.primitiveCount; ++i)
        {
            ISpatialEntity* entity = m_entities[m_primitiveIndices[node.firstPrimitive + i]];
            
            if (!filter.Accepts(entity)) continue;
            
            if (entity->GetWorldBounds().Overlaps(box))
            {
                QueryResult result;
                result.handle = entity->GetHandle();
                result.userData = entity->GetUserData();
                results.push_back(result);
            }
        }
    }
    else
    {
        QueryBoxRecursive(node.leftChild, box, filter, results);
        QueryBoxRecursive(node.rightChild, box, filter, results);
    }
}

void BVHIndex::QuerySphere(
    const Vec3& center,
    float radius,
    const QueryFilter& filter,
    std::vector<QueryResult>& outResults) const
{
    if (m_nodes.empty()) return;
    QuerySphereRecursive(0, center, radius, filter, outResults);
}

void BVHIndex::QuerySphereRecursive(
    int nodeIdx,
    const Vec3& center,
    float radius,
    const QueryFilter& filter,
    std::vector<QueryResult>& results) const
{
    const Node& node = m_nodes[nodeIdx];

    // Check sphere-box intersection
    Sphere sphere(center, radius);
    if (!sphere.Overlaps(node.bounds)) return;

    if (node.IsLeaf())
    {
        for (int i = 0; i < node.primitiveCount; ++i)
        {
            ISpatialEntity* entity = m_entities[m_primitiveIndices[node.firstPrimitive + i]];
            
            if (!filter.Accepts(entity)) continue;
            
            if (sphere.Overlaps(entity->GetWorldBounds()))
            {
                QueryResult result;
                result.handle = entity->GetHandle();
                result.distance = glm::length(entity->GetWorldBounds().GetCenter() - center);
                result.sortKey = result.distance;
                result.userData = entity->GetUserData();
                results.push_back(result);
            }
        }
    }
    else
    {
        QuerySphereRecursive(node.leftChild, center, radius, filter, results);
        QuerySphereRecursive(node.rightChild, center, radius, filter, results);
    }
}

bool BVHIndex::QueryRay(
    const Ray& ray,
    const QueryFilter& filter,
    QueryResult& outResult) const
{
    if (m_nodes.empty()) return false;

    float closestT = ray.tMax;
    bool hit = QueryRayRecursive(0, ray, filter, closestT, outResult);
    
    if (hit)
    {
        outResult.distance = closestT;
        outResult.sortKey = closestT;
    }

    return hit;
}

bool BVHIndex::QueryRayRecursive(
    int nodeIdx,
    const Ray& ray,
    const QueryFilter& filter,
    float& closestT,
    QueryResult& result) const
{
    const Node& node = m_nodes[nodeIdx];

    float tMin, tMax;
    if (!IntersectRayBox(ray, node.bounds, tMin, tMax)) return false;
    if (tMin > closestT) return false;

    bool hit = false;

    if (node.IsLeaf())
    {
        for (int i = 0; i < node.primitiveCount; ++i)
        {
            ISpatialEntity* entity = m_entities[m_primitiveIndices[node.firstPrimitive + i]];
            
            if (!filter.Accepts(entity)) continue;

            float t1, t2;
            if (IntersectRayBox(ray, entity->GetWorldBounds(), t1, t2))
            {
                if (t1 < closestT && t1 >= ray.tMin)
                {
                    closestT = t1;
                    result.handle = entity->GetHandle();
                    result.userData = entity->GetUserData();
                    hit = true;
                }
            }
        }
    }
    else
    {
        // Visit closer child first
        int firstChild = node.leftChild;
        int secondChild = node.rightChild;

        float t1Left, t2Left, t1Right, t2Right;
        bool hitLeft = IntersectRayBox(ray, m_nodes[node.leftChild].bounds, t1Left, t2Left);
        bool hitRight = IntersectRayBox(ray, m_nodes[node.rightChild].bounds, t1Right, t2Right);

        if (hitLeft && hitRight && t1Right < t1Left)
        {
            std::swap(firstChild, secondChild);
        }

        if (QueryRayRecursive(firstChild, ray, filter, closestT, result)) hit = true;
        if (QueryRayRecursive(secondChild, ray, filter, closestT, result)) hit = true;
    }

    return hit;
}

void BVHIndex::QueryRayAll(
    const Ray& ray,
    const QueryFilter& filter,
    std::vector<QueryResult>& outResults) const
{
    if (m_nodes.empty()) return;
    QueryRayAllRecursive(0, ray, filter, outResults);
    
    // Sort by distance
    std::sort(outResults.begin(), outResults.end());
}

void BVHIndex::QueryRayAllRecursive(
    int nodeIdx,
    const Ray& ray,
    const QueryFilter& filter,
    std::vector<QueryResult>& results) const
{
    const Node& node = m_nodes[nodeIdx];

    float tMin, tMax;
    if (!IntersectRayBox(ray, node.bounds, tMin, tMax)) return;

    if (node.IsLeaf())
    {
        for (int i = 0; i < node.primitiveCount; ++i)
        {
            ISpatialEntity* entity = m_entities[m_primitiveIndices[node.firstPrimitive + i]];
            
            if (!filter.Accepts(entity)) continue;

            float t1, t2;
            if (IntersectRayBox(ray, entity->GetWorldBounds(), t1, t2))
            {
                if (t1 >= ray.tMin && t1 <= ray.tMax)
                {
                    QueryResult result;
                    result.handle = entity->GetHandle();
                    result.distance = t1;
                    result.sortKey = t1;
                    result.userData = entity->GetUserData();
                    results.push_back(result);
                }
            }
        }
    }
    else
    {
        QueryRayAllRecursive(node.leftChild, ray, filter, results);
        QueryRayAllRecursive(node.rightChild, ray, filter, results);
    }
}

bool BVHIndex::IntersectRayBox(const Ray& ray, const AABB& box, float& tMin, float& tMax) const
{
    Vec3 invDir = 1.0f / ray.direction;
    Vec3 t0 = (box.GetMin() - ray.origin) * invDir;
    Vec3 t1 = (box.GetMax() - ray.origin) * invDir;

    Vec3 tSmall = glm::min(t0, t1);
    Vec3 tBig = glm::max(t0, t1);

    tMin = glm::max(glm::max(tSmall.x, tSmall.y), tSmall.z);
    tMax = glm::min(glm::min(tBig.x, tBig.y), tBig.z);

    return tMax >= tMin && tMax >= 0.0f;
}

IndexStats BVHIndex::GetStats() const
{
    IndexStats stats;
    stats.entityCount = m_entities.size();
    stats.nodeCount = m_nodes.size();
    stats.memoryBytes = m_nodes.capacity() * sizeof(Node) +
                        m_entities.capacity() * sizeof(ISpatialEntity*) +
                        m_primitiveIndices.capacity() * sizeof(int);

    // Calculate max depth and avg entities per leaf
    int maxDepth = 0;
    int leafCount = 0;
    int totalLeafEntities = 0;

    std::function<void(int, int)> traverse = [&](int nodeIdx, int depth) {
        const Node& node = m_nodes[nodeIdx];
        maxDepth = std::max(maxDepth, depth);

        if (node.IsLeaf())
        {
            leafCount++;
            totalLeafEntities += node.primitiveCount;
        }
        else
        {
            traverse(node.leftChild, depth + 1);
            traverse(node.rightChild, depth + 1);
        }
    };

    if (!m_nodes.empty())
    {
        traverse(0, 0);
    }

    stats.maxDepth = maxDepth;
    stats.avgEntitiesPerLeaf = leafCount > 0 ? static_cast<float>(totalLeafEntities) / leafCount : 0.0f;

    return stats;
}

void BVHIndex::DebugDraw(IDebugRenderer* renderer, int maxDepth) const
{
    if (!renderer || m_nodes.empty()) return;

    std::function<void(int, int)> draw = [&](int nodeIdx, int depth) {
        if (maxDepth >= 0 && depth > maxDepth) return;

        const Node& node = m_nodes[nodeIdx];
        
        // Color based on depth
        float t = static_cast<float>(depth) / 10.0f;
        Vec4 color(1.0f - t, t, 0.5f, 0.5f);
        
        renderer->DrawBox(node.bounds, color);

        if (!node.IsLeaf())
        {
            draw(node.leftChild, depth + 1);
            draw(node.rightChild, depth + 1);
        }
    };

    draw(0, 0);
}

AABB BVHIndex::GetWorldBounds() const
{
    if (m_nodes.empty()) return AABB();
    return m_nodes[0].bounds;
}

// QueryFilter implementation
bool QueryFilter::Accepts(const ISpatialEntity* entity) const
{
    if (!entity) return false;

    // Check layer mask
    if ((entity->GetLayerMask() & layerMask) == 0) return false;

    // Check type mask
    if ((entity->GetTypeMask() & typeMask) == 0) return false;

    // Custom filter
    if (customFilter && !customFilter(entity)) return false;

    return true;
}

} // namespace RVX::Spatial
