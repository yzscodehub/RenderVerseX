#include "Spatial/Index/SpatialFactory.h"

namespace RVX::Spatial
{

SpatialIndexPtr SpatialFactory::Create(SpatialIndexType type)
{
    switch (type)
    {
        case SpatialIndexType::BVH:
            return CreateBVH();
        case SpatialIndexType::Octree:
            // TODO: Implement OctreeIndex
            return CreateBVH(); // Fallback to BVH
        case SpatialIndexType::Grid:
            // TODO: Implement GridIndex
            return CreateBVH(); // Fallback to BVH
        default:
            return CreateBVH();
    }
}

SpatialIndexPtr SpatialFactory::CreateBVH(const BVHConfig& config)
{
    return std::make_unique<BVHIndex>(config);
}

const char* SpatialFactory::GetTypeName(SpatialIndexType type)
{
    switch (type)
    {
        case SpatialIndexType::BVH:     return "BVH";
        case SpatialIndexType::Octree:  return "Octree";
        case SpatialIndexType::Grid:    return "Grid";
        default:                        return "Unknown";
    }
}

} // namespace RVX::Spatial
