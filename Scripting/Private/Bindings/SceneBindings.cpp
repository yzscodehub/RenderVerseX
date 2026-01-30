#include "Scripting/Bindings/SceneBindings.h"
#include "Scripting/LuaState.h"
#include "Scene/SceneEntity.h"
#include "Scene/Component.h"
#include "Scene/SceneManager.h"
#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include "Core/Log.h"

#include <functional>

namespace RVX::Bindings
{
    void RegisterSceneBindings(LuaState& lua)
    {
        sol::state& state = lua.GetState();
        sol::table rvx = lua.GetOrCreateNamespace("RVX");

        // =====================================================================
        // EntityType enum
        // =====================================================================
        state.new_enum<EntityType>("EntityType",
            {
                {"Node", EntityType::Node},
                {"StaticMesh", EntityType::StaticMesh},
                {"SkeletalMesh", EntityType::SkeletalMesh},
                {"Light", EntityType::Light},
                {"Camera", EntityType::Camera},
                {"Probe", EntityType::Probe},
                {"Decal", EntityType::Decal},
                {"Custom", EntityType::Custom}
            }
        );

        // =====================================================================
        // Component (base class)
        // =====================================================================
        state.new_usertype<Component>("Component",
            sol::no_constructor,  // Abstract class

            // Type info
            "GetTypeName", &Component::GetTypeName,

            // State
            "GetOwner", &Component::GetOwner,
            "IsEnabled", &Component::IsEnabled,
            "SetEnabled", &Component::SetEnabled
        );

        // =====================================================================
        // SceneEntity
        // =====================================================================
        state.new_usertype<SceneEntity>("SceneEntity",
            sol::no_constructor,  // Created by SceneManager

            // =====================================================================
            // Basic Properties
            // =====================================================================
            "GetName", &SceneEntity::GetName,
            "SetName", &SceneEntity::SetName,
            "GetHandle", &SceneEntity::GetHandle,
            "GetEntityType", &SceneEntity::GetEntityType,
            
            "IsActive", &SceneEntity::IsActive,
            "SetActive", &SceneEntity::SetActive,

            // =====================================================================
            // Layer
            // =====================================================================
            "GetLayerMask", &SceneEntity::GetLayerMask,
            "SetLayerMask", &SceneEntity::SetLayerMask,
            "SetLayer", &SceneEntity::SetLayer,
            "AddLayer", &SceneEntity::AddLayer,
            "RemoveLayer", &SceneEntity::RemoveLayer,
            "IsInLayer", &SceneEntity::IsInLayer,

            // =====================================================================
            // Transform (Local)
            // =====================================================================
            "GetPosition", &SceneEntity::GetPosition,
            "SetPosition", &SceneEntity::SetPosition,
            "GetRotation", &SceneEntity::GetRotation,
            "SetRotation", &SceneEntity::SetRotation,
            "GetScale", &SceneEntity::GetScale,
            "SetScale", &SceneEntity::SetScale,
            
            "GetLocalMatrix", &SceneEntity::GetLocalMatrix,
            
            "Translate", &SceneEntity::Translate,
            "Rotate", &SceneEntity::Rotate,
            "RotateAround", &SceneEntity::RotateAround,

            // =====================================================================
            // Transform (World)
            // =====================================================================
            "GetWorldMatrix", &SceneEntity::GetWorldMatrix,
            "GetWorldPosition", &SceneEntity::GetWorldPosition,
            "GetWorldRotation", &SceneEntity::GetWorldRotation,
            "GetWorldScale", &SceneEntity::GetWorldScale,

            // =====================================================================
            // Bounds
            // =====================================================================
            "GetLocalBounds", &SceneEntity::GetLocalBounds,
            "SetLocalBounds", &SceneEntity::SetLocalBounds,
            "GetWorldBounds", &SceneEntity::GetWorldBounds,

            // =====================================================================
            // Hierarchy
            // =====================================================================
            "GetParent", &SceneEntity::GetParent,
            "SetParent", &SceneEntity::SetParent,
            "GetChildren", &SceneEntity::GetChildren,
            "GetChildCount", &SceneEntity::GetChildCount,
            "AddChild", &SceneEntity::AddChild,
            "RemoveChild", &SceneEntity::RemoveChild,
            "IsRoot", &SceneEntity::IsRoot,
            "IsAncestorOf", &SceneEntity::IsAncestorOf,
            "IsDescendantOf", &SceneEntity::IsDescendantOf,
            "GetRoot", sol::resolve<SceneEntity*()>(&SceneEntity::GetRoot),

            // =====================================================================
            // Component access (generic)
            // =====================================================================
            "GetComponentCount", &SceneEntity::GetComponentCount,
            "TickComponents", &SceneEntity::TickComponents,

            // =====================================================================
            // Convenience methods for Lua
            // =====================================================================
            
            // Move in local space
            "MoveLocal", [](SceneEntity& self, const Vec3& offset) {
                self.SetPosition(self.GetPosition() + offset);
            },
            
            // Move in world direction
            "MoveWorld", [](SceneEntity& self, const Vec3& worldOffset) {
                // Transform world offset to local space if has parent
                Vec3 localOffset = worldOffset;
                if (self.GetParent()) {
                    Mat4 parentWorldInv = glm::inverse(self.GetParent()->GetWorldMatrix());
                    localOffset = Vec3(parentWorldInv * Vec4(worldOffset, 0.0f));
                }
                self.SetPosition(self.GetPosition() + localOffset);
            },
            
            // Look at target
            "LookAt", [](SceneEntity& self, const Vec3& target, const Vec3& up) {
                Vec3 pos = self.GetWorldPosition();
                Vec3 forward = glm::normalize(target - pos);
                Mat4 lookMat = glm::lookAt(pos, target, up);
                Quat rot = glm::conjugate(glm::quat_cast(lookMat));
                
                // Convert to local rotation if has parent
                if (self.GetParent()) {
                    Quat parentRot = self.GetParent()->GetWorldRotation();
                    rot = glm::inverse(parentRot) * rot;
                }
                self.SetRotation(rot);
            },
            
            // Get forward/right/up vectors
            "GetForward", [](SceneEntity& self) {
                Mat4 worldMat = self.GetWorldMatrix();
                return -Vec3(worldMat[2]);  // -Z is forward
            },
            "GetRight", [](SceneEntity& self) {
                Mat4 worldMat = self.GetWorldMatrix();
                return Vec3(worldMat[0]);  // +X is right
            },
            "GetUp", [](SceneEntity& self) {
                Mat4 worldMat = self.GetWorldMatrix();
                return Vec3(worldMat[1]);  // +Y is up
            },

            // Find child by name
            "FindChild", [](SceneEntity& self, const std::string& name) -> SceneEntity* {
                for (SceneEntity* child : self.GetChildren()) {
                    if (child && child->GetName() == name) {
                        return child;
                    }
                }
                return nullptr;
            },

            // Find child recursively
            "FindChildRecursive", [](SceneEntity& self, const std::string& name) -> SceneEntity* {
                std::function<SceneEntity*(SceneEntity*)> search = [&](SceneEntity* entity) -> SceneEntity* {
                    if (!entity) return nullptr;
                    for (SceneEntity* child : entity->GetChildren()) {
                        if (child) {
                            if (child->GetName() == name) return child;
                            SceneEntity* found = search(child);
                            if (found) return found;
                        }
                    }
                    return nullptr;
                };
                return search(&self);
            }
        );

        // =====================================================================
        // AABB (for bounds)
        // =====================================================================
        state.new_usertype<AABB>("AABB",
            sol::constructors<
                AABB(),
                AABB(const Vec3&, const Vec3&)
            >(),
            
            // Properties via getters
            "GetMin", sol::resolve<const Vec3&() const>(&AABB::GetMin),
            "GetMax", sol::resolve<const Vec3&() const>(&AABB::GetMax),
            "SetMin", &AABB::SetMin,
            "SetMax", &AABB::SetMax,
            
            "GetCenter", &AABB::GetCenter,
            "GetExtent", &AABB::GetExtent,
            "GetSize", &AABB::GetSize,
            "IsValid", &AABB::IsValid,
            "Contains", sol::overload(
                static_cast<bool(AABB::*)(const Vec3&) const>(&AABB::Contains),
                static_cast<bool(AABB::*)(const AABB&) const>(&AABB::Contains)
            ),
            "Overlaps", &AABB::Overlaps,
            "Expand", sol::overload(
                static_cast<void(AABB::*)(const Vec3&)>(&AABB::Expand),
                static_cast<void(AABB::*)(const AABB&)>(&AABB::Expand)
            ),
            "Transformed", &AABB::Transformed,
            "Union", &AABB::Union,
            "Intersection", &AABB::Intersection,
            "SurfaceArea", &AABB::SurfaceArea,
            "Volume", &AABB::Volume,
            "Reset", &AABB::Reset
        );

        // =====================================================================
        // SceneManager access (read-only)
        // =====================================================================
        state.new_usertype<SceneManager>("SceneManager",
            sol::no_constructor,
            
            "GetEntityCount", &SceneManager::GetEntityCount,
            "GetEntity", sol::resolve<SceneEntity*(SceneEntity::Handle)>(&SceneManager::GetEntity),
            
            // ForEach wrapper for Lua iteration
            "ForEachEntity", [](SceneManager& self, sol::function callback) {
                self.ForEachEntity([&callback](SceneEntity* entity) {
                    callback(entity);
                });
            },
            "ForEachActiveEntity", [](SceneManager& self, sol::function callback) {
                self.ForEachActiveEntity([&callback](SceneEntity* entity) {
                    callback(entity);
                });
            }
        );

        // Make types available in RVX namespace
        rvx["EntityType"] = state["EntityType"];
        rvx["SceneEntity"] = state["SceneEntity"];
        rvx["Component"] = state["Component"];
        rvx["AABB"] = state["AABB"];
        rvx["SceneManager"] = state["SceneManager"];

        RVX_CORE_INFO("SceneBindings registered");
    }

} // namespace RVX::Bindings
