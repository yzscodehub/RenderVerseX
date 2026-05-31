/**
 * @file MaterialTemplate.cpp
 * @brief MaterialTemplate implementation
 */

#include "Render/Material/MaterialTemplate.h"

namespace RVX
{

MaterialTemplate::MaterialTemplate(const std::string& name)
    : m_name(name)
{
}

void MaterialTemplate::AddFloatParameter(const std::string& name, float defaultValue,
                                          float minVal, float maxVal)
{
    MaterialParameterDef param;
    param.name = name;
    param.type = MaterialParamType::Float;
    param.defaultValue = defaultValue;
    param.minValue = minVal;
    param.maxValue = maxVal;
    param.displayName = name;

    m_parameterLookup[name] = m_parameters.size();
    m_parameters.push_back(param);
    CalculateParameterOffsets();
}

void MaterialTemplate::AddVectorParameter(const std::string& name, const Vec4& defaultValue)
{
    MaterialParameterDef param;
    param.name = name;
    param.type = MaterialParamType::Float4;
    param.defaultValue = defaultValue;
    param.displayName = name;

    m_parameterLookup[name] = m_parameters.size();
    m_parameters.push_back(param);
    CalculateParameterOffsets();
}

void MaterialTemplate::AddVector3Parameter(const std::string& name, const Vec3& defaultValue)
{
    MaterialParameterDef param;
    param.name = name;
    param.type = MaterialParamType::Float3;
    param.defaultValue = defaultValue;
    param.displayName = name;

    m_parameterLookup[name] = m_parameters.size();
    m_parameters.push_back(param);
    CalculateParameterOffsets();
}

void MaterialTemplate::AddVector2Parameter(const std::string& name, const Vec2& defaultValue)
{
    MaterialParameterDef param;
    param.name = name;
    param.type = MaterialParamType::Float2;
    param.defaultValue = defaultValue;
    param.displayName = name;

    m_parameterLookup[name] = m_parameters.size();
    m_parameters.push_back(param);
    CalculateParameterOffsets();
}

void MaterialTemplate::AddTextureParameter(const std::string& name, uint32 binding,
                                            uint64 defaultTextureId)
{
    MaterialParameterDef param;
    param.name = name;
    param.type = MaterialParamType::Texture2D;
    param.defaultValue = defaultTextureId;
    param.binding = binding;
    param.displayName = name;

    m_parameterLookup[name] = m_parameters.size();
    m_parameters.push_back(param);
}

const MaterialParameterDef* MaterialTemplate::FindParameter(const std::string& name) const
{
    auto it = m_parameterLookup.find(name);
    if (it != m_parameterLookup.end())
    {
        return &m_parameters[it->second];
    }
    return nullptr;
}

int32 MaterialTemplate::GetParameterIndex(const std::string& name) const
{
    auto it = m_parameterLookup.find(name);
    if (it != m_parameterLookup.end())
    {
        return static_cast<int32>(it->second);
    }
    return -1;
}

bool MaterialTemplate::Compile(IRHIDevice* device)
{
    m_compiled = false;
    m_pipeline.Reset();
    m_lastCompileError.clear();

    if (!device)
    {
        m_lastCompileError = "Cannot compile material template without an RHI device";
        return false;
    }

    m_lastCompileError =
        "MaterialTemplate::Compile is not implemented; no shader compilation or pipeline creation occurred";
    return false;
}

void MaterialTemplate::CalculateParameterOffsets()
{
    uint32 offset = 0;
    
    for (auto& param : m_parameters)
    {
        // Skip texture parameters (they use binding slots, not buffer offsets)
        if (param.type == MaterialParamType::Texture2D ||
            param.type == MaterialParamType::TextureCube ||
            param.type == MaterialParamType::Sampler)
        {
            continue;
        }

        // Align to 16 bytes for Vec4, 8 bytes for Vec2, 4 bytes otherwise
        uint32 alignment = 4;
        uint32 size = 4;
        
        switch (param.type)
        {
            case MaterialParamType::Float:
                alignment = 4; size = 4;
                break;
            case MaterialParamType::Float2:
                alignment = 8; size = 8;
                break;
            case MaterialParamType::Float3:
                alignment = 16; size = 12;  // Vec3 pads to 16
                break;
            case MaterialParamType::Float4:
                alignment = 16; size = 16;
                break;
            case MaterialParamType::Int:
            case MaterialParamType::Bool:
                alignment = 4; size = 4;
                break;
            default:
                break;
        }

        // Align offset
        offset = (offset + alignment - 1) & ~(alignment - 1);
        param.offset = offset;
        offset += size;
    }

    // Final size aligned to 16 bytes
    m_constantBufferSize = (offset + 15) & ~15;
}

} // namespace RVX
