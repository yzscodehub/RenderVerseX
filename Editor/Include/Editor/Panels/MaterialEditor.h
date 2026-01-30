/**
 * @file MaterialEditor.h
 * @brief Material editor panel with node-based editing
 */

#pragma once

#include "Editor/Panels/IEditorPanel.h"
#include "Core/MathTypes.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace RVX
{
    class Material;
}

namespace RVX::Editor
{

/**
 * @brief Node types for material graph
 */
enum class MaterialNodeType : uint8
{
    Output,
    Constant,
    Parameter,
    Texture,
    Add,
    Subtract,
    Multiply,
    Divide,
    Lerp,
    Dot,
    Cross,
    Normalize,
    Fresnel,
    Time,
    TexCoord,
    WorldPosition,
    WorldNormal,
    ViewDirection
};

/**
 * @brief Material node pin type
 */
enum class MaterialPinType : uint8
{
    Float,
    Vec2,
    Vec3,
    Vec4,
    Texture2D,
    TextureCube
};

/**
 * @brief Material node pin
 */
struct MaterialPin
{
    std::string name;
    MaterialPinType type;
    bool isOutput;
    int nodeId;
    int pinIndex;
};

/**
 * @brief Material graph node
 */
struct MaterialNode
{
    int id;
    MaterialNodeType type;
    std::string name;
    Vec2 position;
    std::vector<MaterialPin> inputs;
    std::vector<MaterialPin> outputs;

    // Node-specific data
    Vec4 constantValue{0.0f};
    std::string texturePath;
    std::string parameterName;
};

/**
 * @brief Connection between nodes
 */
struct MaterialLink
{
    int id;
    int sourceNodeId;
    int sourcePinIndex;
    int destNodeId;
    int destPinIndex;
};

/**
 * @brief Material editor panel with node-based material editing
 */
class MaterialEditorPanel : public IEditorPanel
{
public:
    MaterialEditorPanel();
    ~MaterialEditorPanel() override = default;

    // =========================================================================
    // IEditorPanel Interface
    // =========================================================================

    const char* GetName() const override { return "Material"; }
    const char* GetIcon() const override { return "material"; }
    void OnInit() override;
    void OnGUI() override;

    // =========================================================================
    // Material Management
    // =========================================================================

    void SetMaterial(std::shared_ptr<Material> material);
    std::shared_ptr<Material> GetMaterial() const { return m_material; }

    void NewMaterial();
    void SaveMaterial();
    void LoadMaterial(const std::string& path);

private:
    void DrawToolbar();
    void DrawNodeGraph();
    void DrawNode(MaterialNode& node);
    void DrawNodePin(const MaterialPin& pin, const Vec2& nodePos);
    void DrawLinks();
    void DrawContextMenu();
    void DrawPropertiesPanel();
    void DrawPreviewPanel();

    void HandleNodeInteraction();
    void HandleLinkCreation();
    void HandleNodeDeletion();

    MaterialNode* CreateNode(MaterialNodeType type, Vec2 position);
    void DeleteNode(int nodeId);
    void CreateLink(int sourceNode, int sourcePin, int destNode, int destPin);
    void DeleteLink(int linkId);

    Vec2 GetPinPosition(int nodeId, int pinIndex, bool isOutput) const;
    MaterialPinType GetPinType(int nodeId, int pinIndex, bool isOutput) const;
    const char* GetNodeTypeName(MaterialNodeType type) const;
    uint32 GetPinTypeColor(MaterialPinType type) const;

    // Material data
    std::shared_ptr<Material> m_material;
    std::vector<MaterialNode> m_nodes;
    std::vector<MaterialLink> m_links;
    int m_nextNodeId = 1;
    int m_nextLinkId = 1;

    // Editor state
    Vec2 m_canvasOffset{0.0f, 0.0f};
    float m_canvasZoom = 1.0f;
    int m_selectedNode = -1;
    int m_hoveredNode = -1;
    int m_hoveredPin = -1;
    bool m_isDraggingNode = false;
    bool m_isDraggingCanvas = false;
    bool m_isCreatingLink = false;
    int m_linkSourceNode = -1;
    int m_linkSourcePin = -1;
    bool m_linkSourceIsOutput = false;

    // Display options
    bool m_showGrid = true;
    bool m_showMinimap = true;
    bool m_showProperties = true;
    bool m_showPreview = true;
    float m_nodeWidth = 180.0f;
    float m_pinRadius = 6.0f;
};

} // namespace RVX::Editor
