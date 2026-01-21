#include "Scene/Node.h"
#include <queue>
#include <atomic>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace RVX
{
    // =========================================================================
    // Transform Implementation
    // =========================================================================

    Transform::Transform(const Vec3& position, const Quat& rotation, const Vec3& scale)
        : m_position(position)
        , m_rotation(rotation)
        , m_scale(scale)
        , m_dirty(true)
        , m_dirtyTrs(false)
    {
    }

    Transform::Transform(const Mat4& matrix)
        : m_matrix(matrix)
        , m_dirty(false)
        , m_dirtyTrs(true)
    {
    }

    const Vec3& Transform::GetPosition() const
    {
        if (m_dirtyTrs) UpdateTRS();
        return m_position;
    }

    void Transform::SetPosition(const Vec3& position)
    {
        if (m_dirtyTrs) UpdateTRS();
        m_position = position;
        m_dirty = true;
        m_dirtyTrs = false;
    }

    void Transform::Translate(const Vec3& translation)
    {
        SetPosition(GetPosition() + translation);
    }

    const Quat& Transform::GetRotation() const
    {
        if (m_dirtyTrs) UpdateTRS();
        return m_rotation;
    }

    void Transform::SetRotation(const Quat& rotation)
    {
        if (m_dirtyTrs) UpdateTRS();
        m_rotation = rotation;
        m_dirty = true;
        m_dirtyTrs = false;
    }

    void Transform::SetRotation(const Vec3& euler)
    {
        SetRotation(Quat(euler));
    }

    void Transform::Rotate(const Quat& rotation)
    {
        SetRotation(rotation * GetRotation());
    }

    void Transform::Rotate(const Vec3& axis, float angle)
    {
        Rotate(glm::angleAxis(angle, normalize(axis)));
    }

    const Vec3& Transform::GetScale() const
    {
        if (m_dirtyTrs) UpdateTRS();
        return m_scale;
    }

    void Transform::SetScale(const Vec3& scale)
    {
        if (m_dirtyTrs) UpdateTRS();
        m_scale = scale;
        m_dirty = true;
        m_dirtyTrs = false;
    }

    void Transform::SetScale(float uniformScale)
    {
        SetScale(Vec3(uniformScale));
    }

    const Mat4& Transform::GetMatrix() const
    {
        if (m_dirty) UpdateMatrix();
        return m_matrix;
    }

    void Transform::SetMatrix(const Mat4& matrix)
    {
        m_matrix = matrix;
        m_dirty = false;
        m_dirtyTrs = true;
    }

    Vec3 Transform::GetForward() const
    {
        return normalize(GetRotation() * Vec3(0, 0, -1));
    }

    Vec3 Transform::GetRight() const
    {
        return normalize(GetRotation() * Vec3(1, 0, 0));
    }

    Vec3 Transform::GetUp() const
    {
        return normalize(GetRotation() * Vec3(0, 1, 0));
    }

    void Transform::LookAt(const Vec3& target, const Vec3& up)
    {
        Vec3 pos = GetPosition();
        Vec3 direction = normalize(target - pos);
        
        // Handle case where direction is parallel to up
        Vec3 right = normalize(cross(up, direction));
        if (length(right) < 0.001f)
        {
            right = Vec3(1, 0, 0);
        }
        Vec3 correctedUp = cross(direction, right);
        
        Mat4 lookMatrix;
        lookMatrix[0] = Vec4(right, 0);
        lookMatrix[1] = Vec4(correctedUp, 0);
        lookMatrix[2] = Vec4(-direction, 0);
        lookMatrix[3] = Vec4(0, 0, 0, 1);
        
        SetRotation(Mat4ToQuat(lookMatrix));
    }

    void Transform::UpdateMatrix() const
    {
        Mat4 T = translate(Mat4(1.0f), m_position);
        Mat4 R = QuatToMat4(m_rotation);
        Mat4 S = scale(Mat4(1.0f), m_scale);
        m_matrix = T * R * S;
        m_dirty = false;
    }

    void Transform::UpdateTRS() const
    {
        DecomposeMatrix(m_matrix, m_position, m_rotation, m_scale);
        m_dirtyTrs = false;
    }

    void Transform::DecomposeMatrix(const Mat4& matrix, Vec3& position,
                                    Quat& rotation, Vec3& scale)
    {
        Vec3 skew;
        Vec4 perspective;
        glm::decompose(matrix, scale, rotation, position, skew, perspective);
    }

    // =========================================================================
    // MeshComponent Implementation
    // =========================================================================

    MeshComponent::MeshComponent(Mesh::Ptr mesh)
        : m_mesh(std::move(mesh))
    {
    }

    // =========================================================================
    // Node Implementation
    // =========================================================================

    static std::atomic<uint32_t> s_nextNodeId{1};

    uint32_t Node::GenerateId()
    {
        return s_nextNodeId.fetch_add(1, std::memory_order_relaxed);
    }

    Node::Node(const std::string& name)
        : m_name(name)
        , m_id(GenerateId())
    {
    }

    const Mat4& Node::GetWorldMatrix() const
    {
        if (m_worldMatrixDirty)
        {
            UpdateWorldMatrix();
        }
        return m_worldMatrix;
    }

    Vec3 Node::GetWorldPosition() const
    {
        return Vec3(GetWorldMatrix()[3]);
    }

    Quat Node::GetWorldRotation() const
    {
        Vec3 pos, scale;
        Quat rot;
        Vec3 skew;
        Vec4 perspective;
        glm::decompose(GetWorldMatrix(), scale, rot, pos, skew, perspective);
        return rot;
    }

    Vec3 Node::GetWorldScale() const
    {
        const Mat4& world = GetWorldMatrix();
        return Vec3(
            length(Vec3(world[0])),
            length(Vec3(world[1])),
            length(Vec3(world[2]))
        );
    }

    Node* Node::GetChild(size_t index) const
    {
        return index < m_children.size() ? m_children[index].get() : nullptr;
    }

    Node* Node::GetChild(const std::string& name) const
    {
        for (const auto& child : m_children)
        {
            if (child->GetName() == name)
            {
                return child.get();
            }
        }
        return nullptr;
    }

    void Node::AddChild(Ptr child)
    {
        if (!child) return;
        
        // Remove from previous parent
        if (child->m_parent)
        {
            child->RemoveFromParent();
        }
        
        child->m_parent = this;
        child->MarkWorldMatrixDirty();
        m_children.push_back(std::move(child));
    }

    bool Node::RemoveChild(Node* child)
    {
        if (!child) return false;
        
        for (auto it = m_children.begin(); it != m_children.end(); ++it)
        {
            if (it->get() == child)
            {
                child->m_parent = nullptr;
                child->MarkWorldMatrixDirty();
                m_children.erase(it);
                return true;
            }
        }
        return false;
    }

    void Node::RemoveFromParent()
    {
        if (m_parent)
        {
            m_parent->RemoveChild(this);
        }
    }

    void Node::SetMesh(Mesh::Ptr mesh)
    {
        if (auto* comp = GetComponent<MeshComponent>())
        {
            comp->SetMesh(std::move(mesh));
        }
        else
        {
            AddComponent<MeshComponent>(std::move(mesh));
        }
    }

    Mesh::Ptr Node::GetMesh() const
    {
        if (auto* comp = GetComponent<MeshComponent>())
        {
            return comp->GetMesh();
        }
        return nullptr;
    }

    void Node::SetBone(int boneIndex)
    {
        if (auto* bone = GetComponent<BoneComponent>())
        {
            bone->SetBoneIndex(boneIndex);
        }
        else
        {
            AddComponent<BoneComponent>(boneIndex);
        }
    }

    int Node::GetBoneIndex() const
    {
        if (auto* bone = GetComponent<BoneComponent>())
        {
            return bone->GetBoneIndex();
        }
        return -1;
    }

    void Node::SetMaterialIndex(size_t submeshIndex, int materialIndex)
    {
        if (submeshIndex >= m_materialIndices.size())
        {
            m_materialIndices.resize(submeshIndex + 1, -1);
        }
        m_materialIndices[submeshIndex] = materialIndex;
    }

    void Node::TraverseDepthFirst(const std::function<void(Node*)>& visitor)
    {
        visitor(this);
        for (auto& child : m_children)
        {
            child->TraverseDepthFirst(visitor);
        }
    }

    void Node::TraverseBreadthFirst(const std::function<void(Node*)>& visitor)
    {
        std::queue<Node*> queue;
        queue.push(this);
        
        while (!queue.empty())
        {
            Node* current = queue.front();
            queue.pop();
            
            visitor(current);
            
            for (auto& child : current->m_children)
            {
                queue.push(child.get());
            }
        }
    }

    Node* Node::FindChild(const std::string& name, bool recursive) const
    {
        for (const auto& child : m_children)
        {
            if (child->GetName() == name)
            {
                return child.get();
            }
            
            if (recursive)
            {
                if (Node* found = child->FindChild(name, true))
                {
                    return found;
                }
            }
        }
        return nullptr;
    }

    Node* Node::FindChildByPath(const std::string& path) const
    {
        if (path.empty()) return nullptr;
        
        size_t start = 0;
        if (path[0] == '/') start = 1;
        
        const Node* current = this;
        size_t pos = start;
        
        while (pos < path.size() && current)
        {
            size_t next = path.find('/', pos);
            std::string segment = path.substr(pos, next - pos);
            
            if (segment.empty() || segment == ".")
            {
                // Skip
            }
            else if (segment == "..")
            {
                current = current->m_parent;
            }
            else
            {
                current = current->GetChild(segment);
            }
            
            pos = (next == std::string::npos) ? path.size() : next + 1;
        }
        
        return const_cast<Node*>(current);
    }

    std::string Node::GetPath() const
    {
        std::string path = m_name;
        const Node* current = m_parent;
        
        while (current)
        {
            path = current->m_name + "/" + path;
            current = current->m_parent;
        }
        
        return "/" + path;
    }

    size_t Node::GetDepth() const
    {
        size_t depth = 0;
        const Node* current = m_parent;
        
        while (current)
        {
            ++depth;
            current = current->m_parent;
        }
        
        return depth;
    }

    bool Node::IsAncestorOf(const Node* node) const
    {
        if (!node) return false;
        
        const Node* current = node->m_parent;
        while (current)
        {
            if (current == this) return true;
            current = current->m_parent;
        }
        return false;
    }

    bool Node::IsDescendantOf(const Node* node) const
    {
        return node && node->IsAncestorOf(this);
    }

    std::optional<BoundingBox> Node::ComputeWorldBoundingBox() const
    {
        BoundingBox result;
        bool hasAny = false;
        
        // Include this node's mesh bounds
        if (auto mesh = GetMesh())
        {
            if (const auto& localBounds = mesh->GetBoundingBox())
            {
                const Mat4& world = GetWorldMatrix();
                
                // Transform all 8 corners
                const Vec3& min = localBounds->GetMin();
                const Vec3& max = localBounds->GetMax();
                
                Vec3 corners[8] = {
                    {min.x, min.y, min.z},
                    {max.x, min.y, min.z},
                    {min.x, max.y, min.z},
                    {max.x, max.y, min.z},
                    {min.x, min.y, max.z},
                    {max.x, min.y, max.z},
                    {min.x, max.y, max.z},
                    {max.x, max.y, max.z}
                };
                
                for (const auto& corner : corners)
                {
                    Vec4 worldCorner = world * Vec4(corner, 1.0f);
                    result.Expand(Vec3(worldCorner));
                    hasAny = true;
                }
            }
        }
        
        // Include children bounds
        for (const auto& child : m_children)
        {
            if (auto childBounds = child->ComputeWorldBoundingBox())
            {
                result.Expand(*childBounds);
                hasAny = true;
            }
        }
        
        if (hasAny)
        {
            return result;
        }
        return std::nullopt;
    }

    void Node::UpdateWorldMatrix() const
    {
        if (m_parent)
        {
            m_worldMatrix = m_parent->GetWorldMatrix() * m_localTransform.GetMatrix();
        }
        else
        {
            m_worldMatrix = m_localTransform.GetMatrix();
        }
        m_worldMatrixDirty = false;
    }

    void Node::MarkWorldMatrixDirty()
    {
        m_worldMatrixDirty = true;
        for (auto& child : m_children)
        {
            child->MarkWorldMatrixDirty();
        }
    }

} // namespace RVX
