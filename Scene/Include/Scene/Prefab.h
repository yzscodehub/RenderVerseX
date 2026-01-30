#pragma once

/**
 * @file Prefab.h
 * @brief Prefab system for reusable entity templates
 * 
 * Prefabs allow saving and instantiating entity hierarchies
 * with component data, supporting overrides on instances.
 */

#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "Resource/IResource.h"
#include "Core/MathTypes.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace RVX
{

/**
 * @brief Property value type for overrides
 */
using PropertyValue = std::variant<
    bool,
    int32_t,
    float,
    std::string,
    Vec2,
    Vec3,
    Vec4,
    Quat
>;

/**
 * @brief Property override for prefab instances
 */
struct PropertyOverride
{
    std::string componentType;  // Component type name
    std::string propertyPath;   // Property path (e.g., "position.x")
    PropertyValue value;        // Override value
};

/**
 * @brief Prefab entity data for serialization
 */
struct PrefabEntityData
{
    std::string name;
    int32_t parentIndex = -1;  // -1 = root
    
    // Transform
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
    
    // Components (serialized data per component type)
    std::unordered_map<std::string, std::string> componentData;
    
    // Layer mask
    uint32_t layerMask = ~0u;
    
    // Active state
    bool isActive = true;
};

/**
 * @brief Prefab resource for entity templates
 * 
 * Features:
 * - Hierarchical entity templates
 * - Component data serialization
 * - Instance override support
 * - Nested prefab support
 * 
 * Usage:
 * @code
 * // Create prefab from existing entity
 * auto prefab = Prefab::CreateFromEntity(entity);
 * 
 * // Instantiate prefab
 * SceneEntity* instance = prefab->Instantiate(scene);
 * 
 * // Instantiate with position
 * SceneEntity* instance2 = prefab->Instantiate(scene, Vec3(10, 0, 0));
 * @endcode
 */
class Prefab : public Resource::IResource, public std::enable_shared_from_this<Prefab>
{
public:
    using Ptr = std::shared_ptr<Prefab>;
    using ConstPtr = std::shared_ptr<const Prefab>;

    Prefab() = default;
    ~Prefab() override = default;

    // =========================================================================
    // IResource Interface
    // =========================================================================

    const char* GetTypeName() const override { return "Prefab"; }
    size_t GetMemoryUsage() const override;

    // =========================================================================
    // Creation
    // =========================================================================

    /// Create an empty prefab
    static Ptr Create();

    /// Create prefab from an existing entity hierarchy
    static Ptr CreateFromEntity(const SceneEntity* rootEntity);

    /// Create prefab from serialized data
    static Ptr CreateFromData(std::vector<PrefabEntityData> entityData);

    // =========================================================================
    // Instantiation
    // =========================================================================

    /// Instantiate the prefab in a scene
    SceneEntity* Instantiate(SceneManager& sceneManager) const;

    /// Instantiate at a specific position
    SceneEntity* Instantiate(SceneManager& sceneManager, const Vec3& position) const;

    /// Instantiate with position and rotation
    SceneEntity* Instantiate(SceneManager& sceneManager, const Vec3& position, const Quat& rotation) const;

    /// Instantiate as child of another entity
    SceneEntity* InstantiateAsChild(SceneEntity* parent) const;

    // =========================================================================
    // Prefab Data
    // =========================================================================

    /// Get the number of entities in the prefab
    size_t GetEntityCount() const { return m_entities.size(); }

    /// Get entity data by index
    const PrefabEntityData* GetEntityData(size_t index) const;

    /// Get the root entity data
    const PrefabEntityData* GetRootData() const;

    /// Add entity data to the prefab
    void AddEntityData(PrefabEntityData data);

    /// Clear all entity data
    void Clear();

    // =========================================================================
    // Prefab Properties
    // =========================================================================

    /// Prefab name (for debugging)
    const std::string& GetName() const { return m_name; }
    void SetName(const std::string& name) { m_name = name; }

    /// Source asset path (if loaded from file)
    const std::string& GetSourcePath() const { return m_sourcePath; }
    void SetSourcePath(const std::string& path) { m_sourcePath = path; }

private:
    SceneEntity* InstantiateInternal(SceneManager& sceneManager, const Vec3& position, 
                                      const Quat& rotation, SceneEntity* parent) const;
    void SerializeEntity(const SceneEntity* entity, int32_t parentIndex);
    void CreateComponents(SceneEntity* entity, const PrefabEntityData& data) const;

    std::string m_name;
    std::string m_sourcePath;
    std::vector<PrefabEntityData> m_entities;
};

/**
 * @brief Prefab instance component
 * 
 * Attached to entities that were instantiated from a prefab.
 * Tracks overrides and allows reverting to prefab values.
 */
class PrefabInstance : public Component
{
public:
    PrefabInstance() = default;
    ~PrefabInstance() override = default;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "PrefabInstance"; }

    void OnAttach() override;
    void OnDetach() override;

    // =========================================================================
    // Prefab Reference
    // =========================================================================

    /// Get source prefab
    Prefab::Ptr GetPrefab() const { return m_prefab; }
    void SetPrefab(Prefab::Ptr prefab) { m_prefab = prefab; }

    // =========================================================================
    // Overrides
    // =========================================================================

    /// Add a property override
    void AddOverride(const PropertyOverride& override);

    /// Remove an override
    void RemoveOverride(const std::string& componentType, const std::string& propertyPath);

    /// Get all overrides
    const std::vector<PropertyOverride>& GetOverrides() const { return m_overrides; }

    /// Clear all overrides
    void ClearOverrides() { m_overrides.clear(); }

    /// Check if a property is overridden
    bool IsOverridden(const std::string& componentType, const std::string& propertyPath) const;

    // =========================================================================
    // Prefab Operations
    // =========================================================================

    /// Revert all overrides to prefab values
    void RevertAll();

    /// Revert a specific property to prefab value
    void RevertProperty(const std::string& componentType, const std::string& propertyPath);

    /// Apply current values back to prefab (requires write access)
    void ApplyToPrefab();

    /// Unpack instance (remove prefab connection, keep current values)
    void Unpack();

private:
    Prefab::Ptr m_prefab;
    std::vector<PropertyOverride> m_overrides;
};

} // namespace RVX
