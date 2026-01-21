/**
 * @file Geometry.h
 * @brief Unified header for Geometry module
 * 
 * This header includes all geometric types and algorithms.
 * For lighter includes, use individual headers directly.
 */

#pragma once

// ============================================================================
// Core Types (from Core/Math - re-exported for convenience)
// ============================================================================
#include "Core/Math/Geometry.h"  // AABB, Sphere, Ray, Plane, Frustum, Intersection

// ============================================================================
// Forward Declarations and Common Types
// ============================================================================
#include "Geometry/GeometryFwd.h"
#include "Geometry/Constants.h"

// ============================================================================
// Primitives
// ============================================================================
#include "Geometry/Primitives/Line.h"
#include "Geometry/Primitives/Triangle.h"
#include "Geometry/Primitives/OBB.h"
#include "Geometry/Primitives/Capsule.h"
#include "Geometry/Primitives/Cylinder.h"
#include "Geometry/Primitives/Cone.h"

// ============================================================================
// Query Functions
// ============================================================================
#include "Geometry/Query/Intersection.h"
#include "Geometry/Query/Distance.h"

// ============================================================================
// SIMD Batch Processing
// ============================================================================
#include "Geometry/Batch/Batch.h"

// ============================================================================
// Collision Detection
// ============================================================================
#include "Geometry/Collision/Collision.h"

// ============================================================================
// Curves
// ============================================================================
#include "Geometry/Curves/Curves.h"

// ============================================================================
// Mesh Operations
// ============================================================================
#include "Geometry/Mesh/Mesh.h"

// ============================================================================
// CSG (Constructive Solid Geometry)
// ============================================================================
#include "Geometry/CSG/CSG.h"

// ============================================================================
// Convenience Aliases in RVX namespace
// ============================================================================
namespace RVX
{
    // Promote commonly used types to RVX namespace
    using Geometry::Line;
    using Geometry::Segment;
    using Geometry::Triangle;
    using Geometry::OBB;
    using Geometry::Capsule;
    using Geometry::Cylinder;
    using Geometry::Cone;

    // Query result types
    using Geometry::HitResult;
    using Geometry::DistanceResult;
    using Geometry::ShapeType;

    // Constants
    using Geometry::EPSILON;
    using Geometry::PI;

} // namespace RVX
