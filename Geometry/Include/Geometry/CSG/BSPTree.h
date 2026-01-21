/**
 * @file BSPTree.h
 * @brief Binary Space Partitioning tree for CSG operations
 */

#pragma once

#include "Geometry/CSG/Polygon.h"
#include <memory>
#include <vector>

namespace RVX::Geometry
{

/**
 * @brief BSP tree node for CSG operations
 */
class BSPNode
{
public:
    Plane plane;
    std::vector<CSGPolygon> polygons;
    std::unique_ptr<BSPNode> front;
    std::unique_ptr<BSPNode> back;

    BSPNode() = default;

    explicit BSPNode(const std::vector<CSGPolygon>& polys)
    {
        Build(polys);
    }

    /// Build the BSP tree from a list of polygons
    void Build(const std::vector<CSGPolygon>& polys)
    {
        if (polys.empty()) return;

        // Use first polygon's plane as splitting plane
        plane = polys[0].plane;

        std::vector<CSGPolygon> frontPolys, backPolys;

        for (const auto& poly : polys)
        {
            std::vector<CSGPolygon> coplanarFront, coplanarBack, polyFront, polyBack;
            poly.SplitByPlane(plane, coplanarFront, coplanarBack, polyFront, polyBack);

            // Add coplanar polygons to this node
            for (auto& p : coplanarFront) polygons.push_back(std::move(p));
            for (auto& p : coplanarBack) polygons.push_back(std::move(p));

            // Collect front/back polygons
            for (auto& p : polyFront) frontPolys.push_back(std::move(p));
            for (auto& p : polyBack) backPolys.push_back(std::move(p));
        }

        if (!frontPolys.empty())
        {
            front = std::make_unique<BSPNode>();
            front->Build(frontPolys);
        }

        if (!backPolys.empty())
        {
            back = std::make_unique<BSPNode>();
            back->Build(backPolys);
        }
    }

    /// Invert the BSP tree (flip all polygons and swap front/back)
    void Invert()
    {
        for (auto& poly : polygons)
        {
            poly.Flip();
        }

        plane = Plane(-plane.normal, -plane.distance);
        std::swap(front, back);

        if (front) front->Invert();
        if (back) back->Invert();
    }

    /// Clip polygons to this BSP tree
    std::vector<CSGPolygon> ClipPolygons(const std::vector<CSGPolygon>& polys) const
    {
        if (polygons.empty() && !front && !back)
        {
            return polys;
        }

        std::vector<CSGPolygon> frontPolys, backPolys;

        for (const auto& poly : polys)
        {
            std::vector<CSGPolygon> coplanarFront, coplanarBack, polyFront, polyBack;
            poly.SplitByPlane(plane, coplanarFront, coplanarBack, polyFront, polyBack);

            // Front-side coplanar polygons go to front
            for (auto& p : coplanarFront) frontPolys.push_back(std::move(p));
            for (auto& p : polyFront) frontPolys.push_back(std::move(p));

            // Back-side coplanar polygons go to back
            for (auto& p : coplanarBack) backPolys.push_back(std::move(p));
            for (auto& p : polyBack) backPolys.push_back(std::move(p));
        }

        if (front)
            frontPolys = front->ClipPolygons(frontPolys);
        
        if (back)
            backPolys = back->ClipPolygons(backPolys);
        else
            backPolys.clear();  // Remove polygons behind solid

        frontPolys.insert(frontPolys.end(),
                         std::make_move_iterator(backPolys.begin()),
                         std::make_move_iterator(backPolys.end()));

        return frontPolys;
    }

    /// Clip this tree's polygons to another tree
    void ClipTo(const BSPNode& other)
    {
        polygons = other.ClipPolygons(polygons);

        if (front) front->ClipTo(other);
        if (back) back->ClipTo(other);
    }

    /// Get all polygons in the tree
    std::vector<CSGPolygon> GetAllPolygons() const
    {
        std::vector<CSGPolygon> result = polygons;

        if (front)
        {
            auto frontPolys = front->GetAllPolygons();
            result.insert(result.end(), frontPolys.begin(), frontPolys.end());
        }

        if (back)
        {
            auto backPolys = back->GetAllPolygons();
            result.insert(result.end(), backPolys.begin(), backPolys.end());
        }

        return result;
    }

    /// Clone the BSP tree
    std::unique_ptr<BSPNode> Clone() const
    {
        auto node = std::make_unique<BSPNode>();
        node->plane = plane;
        node->polygons = polygons;

        if (front) node->front = front->Clone();
        if (back) node->back = back->Clone();

        return node;
    }
};

/**
 * @brief BSP tree for CSG operations
 */
class BSPTree
{
public:
    BSPTree() = default;

    explicit BSPTree(const std::vector<CSGPolygon>& polygons)
    {
        Build(polygons);
    }

    /// Build from polygons
    void Build(const std::vector<CSGPolygon>& polygons)
    {
        if (!polygons.empty())
        {
            m_root = std::make_unique<BSPNode>(polygons);
        }
    }

    /// Check if tree is empty
    bool IsEmpty() const { return m_root == nullptr; }

    /// Invert the tree
    void Invert()
    {
        if (m_root) m_root->Invert();
    }

    /// Clip polygons to this tree
    std::vector<CSGPolygon> ClipPolygons(const std::vector<CSGPolygon>& polys) const
    {
        if (!m_root) return polys;
        return m_root->ClipPolygons(polys);
    }

    /// Clip this tree to another
    void ClipTo(const BSPTree& other)
    {
        if (m_root && other.m_root)
        {
            m_root->ClipTo(*other.m_root);
        }
    }

    /// Get all polygons
    std::vector<CSGPolygon> GetAllPolygons() const
    {
        if (!m_root) return {};
        return m_root->GetAllPolygons();
    }

    /// Clone the tree
    BSPTree Clone() const
    {
        BSPTree result;
        if (m_root)
        {
            result.m_root = m_root->Clone();
        }
        return result;
    }

private:
    std::unique_ptr<BSPNode> m_root;
};

} // namespace RVX::Geometry
