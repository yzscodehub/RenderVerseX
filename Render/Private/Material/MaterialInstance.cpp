/**
 * @file MaterialInstance.cpp
 * @brief MaterialInstance implementation
 */

#include "Render/Material/MaterialInstance.h"
#include <cstring>
#include <cmath>

namespace RVX
{

MaterialInstance::MaterialInstance(MaterialTemplate::ConstPtr materialTemplate)
    : m_template(materialTemplate)
{
}

void MaterialInstance::SetTemplate(MaterialTemplate::ConstPtr materialTemplate)
{
    m_template = materialTemplate;
    m_overrides.clear();
    m_dirty = true;
}

void MaterialInstance::SetFloat(const std::string& name, float value)
{
    SetParameterValue(name, value);
}

float MaterialInstance::GetFloat(const std::string& name) const
{
    auto value = GetParameterValue(name);
    if (std::holds_alternative<float>(value))
    {
        return std::get<float>(value);
    }
    return 0.0f;
}

void MaterialInstance::SetVector2(const std::string& name, const Vec2& value)
{
    SetParameterValue(name, value);
}

Vec2 MaterialInstance::GetVector2(const std::string& name) const
{
    auto value = GetParameterValue(name);
    if (std::holds_alternative<Vec2>(value))
    {
        return std::get<Vec2>(value);
    }
    return Vec2(0.0f);
}

void MaterialInstance::SetVector3(const std::string& name, const Vec3& value)
{
    SetParameterValue(name, value);
}

Vec3 MaterialInstance::GetVector3(const std::string& name) const
{
    auto value = GetParameterValue(name);
    if (std::holds_alternative<Vec3>(value))
    {
        return std::get<Vec3>(value);
    }
    return Vec3(0.0f);
}

void MaterialInstance::SetVector4(const std::string& name, const Vec4& value)
{
    SetParameterValue(name, value);
}

Vec4 MaterialInstance::GetVector4(const std::string& name) const
{
    auto value = GetParameterValue(name);
    if (std::holds_alternative<Vec4>(value))
    {
        return std::get<Vec4>(value);
    }
    return Vec4(0.0f);
}

void MaterialInstance::SetInt(const std::string& name, int32 value)
{
    SetParameterValue(name, value);
}

int32 MaterialInstance::GetInt(const std::string& name) const
{
    auto value = GetParameterValue(name);
    if (std::holds_alternative<int32>(value))
    {
        return std::get<int32>(value);
    }
    return 0;
}

void MaterialInstance::SetBool(const std::string& name, bool value)
{
    SetParameterValue(name, value);
}

bool MaterialInstance::GetBool(const std::string& name) const
{
    auto value = GetParameterValue(name);
    if (std::holds_alternative<bool>(value))
    {
        return std::get<bool>(value);
    }
    return false;
}

void MaterialInstance::SetTexture(const std::string& name, uint64 textureId)
{
    SetParameterValue(name, textureId);
}

uint64 MaterialInstance::GetTexture(const std::string& name) const
{
    auto value = GetParameterValue(name);
    if (std::holds_alternative<uint64>(value))
    {
        return std::get<uint64>(value);
    }
    return 0;
}

bool MaterialInstance::HasOverride(const std::string& name) const
{
    return m_overrides.find(name) != m_overrides.end();
}

void MaterialInstance::ClearOverride(const std::string& name)
{
    m_overrides.erase(name);
    m_dirty = true;
}

void MaterialInstance::ClearAllOverrides()
{
    m_overrides.clear();
    m_dirty = true;
}

void MaterialInstance::GetConstantBufferData(void* outData) const
{
    if (!m_template || !outData)
        return;

    uint8* data = static_cast<uint8*>(outData);
    std::memset(data, 0, m_template->GetConstantBufferSize());

    for (const auto& param : m_template->GetParameters())
    {
        // Skip textures
        if (param.type == MaterialParamType::Texture2D ||
            param.type == MaterialParamType::TextureCube ||
            param.type == MaterialParamType::Sampler)
        {
            continue;
        }

        auto value = GetParameterValue(param.name);

        switch (param.type)
        {
            case MaterialParamType::Float:
                if (std::holds_alternative<float>(value))
                {
                    float f = std::get<float>(value);
                    std::memcpy(data + param.offset, &f, sizeof(float));
                }
                break;
            case MaterialParamType::Float2:
                if (std::holds_alternative<Vec2>(value))
                {
                    Vec2 v = std::get<Vec2>(value);
                    std::memcpy(data + param.offset, &v, sizeof(Vec2));
                }
                break;
            case MaterialParamType::Float3:
                if (std::holds_alternative<Vec3>(value))
                {
                    Vec3 v = std::get<Vec3>(value);
                    std::memcpy(data + param.offset, &v, sizeof(Vec3));
                }
                break;
            case MaterialParamType::Float4:
                if (std::holds_alternative<Vec4>(value))
                {
                    Vec4 v = std::get<Vec4>(value);
                    std::memcpy(data + param.offset, &v, sizeof(Vec4));
                }
                break;
            case MaterialParamType::Int:
                if (std::holds_alternative<int32>(value))
                {
                    int32 i = std::get<int32>(value);
                    std::memcpy(data + param.offset, &i, sizeof(int32));
                }
                break;
            case MaterialParamType::Bool:
                if (std::holds_alternative<bool>(value))
                {
                    int32 b = std::get<bool>(value) ? 1 : 0;
                    std::memcpy(data + param.offset, &b, sizeof(int32));
                }
                break;
            default:
                break;
        }
    }
}

uint32 MaterialInstance::GetConstantBufferSize() const
{
    return m_template ? m_template->GetConstantBufferSize() : 0;
}

std::vector<MaterialInstance::TextureBinding> MaterialInstance::GetTextureBindings() const
{
    std::vector<TextureBinding> bindings;

    if (!m_template)
        return bindings;

    for (const auto& param : m_template->GetParameters())
    {
        if (param.type == MaterialParamType::Texture2D ||
            param.type == MaterialParamType::TextureCube)
        {
            TextureBinding binding;
            binding.binding = param.binding;
            binding.textureId = GetTexture(param.name);
            bindings.push_back(binding);
        }
    }

    return bindings;
}

MaterialParamValue MaterialInstance::GetParameterValue(const std::string& name) const
{
    // Check overrides first
    auto it = m_overrides.find(name);
    if (it != m_overrides.end())
    {
        return it->second;
    }

    // Check parent
    if (m_parent)
    {
        return m_parent->GetParameterValue(name);
    }

    // Fall back to template default
    if (m_template)
    {
        const MaterialParameterDef* param = m_template->FindParameter(name);
        if (param)
        {
            return param->defaultValue;
        }
    }

    return 0.0f;
}

void MaterialInstance::SetParameterValue(const std::string& name, MaterialParamValue value)
{
    m_overrides[name] = value;
    m_dirty = true;
}

// ============================================================================
// DynamicMaterialInstance
// ============================================================================

void DynamicMaterialInstance::LerpFloat(const std::string& name, float target, float t)
{
    float current = GetFloat(name);
    SetFloat(name, current + (target - current) * t);
}

void DynamicMaterialInstance::LerpVector4(const std::string& name, const Vec4& target, float t)
{
    Vec4 current = GetVector4(name);
    SetVector4(name, mix(current, target, t));
}

void DynamicMaterialInstance::PulseFloat(const std::string& name, float amplitude, float frequency, float time)
{
    float value = amplitude * std::sin(frequency * time * 2.0f * 3.14159265f);
    SetFloat(name, value);
}

} // namespace RVX
