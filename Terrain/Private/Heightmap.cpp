/**
 * @file Heightmap.cpp
 * @brief Implementation of terrain heightmap
 */

#include "Terrain/Heightmap.h"
#include "RHI/RHIDevice.h"
#include "Core/Log.h"

#include <fstream>
#include <cmath>
#include <algorithm>

namespace RVX
{

Heightmap::~Heightmap() = default;

bool Heightmap::Create(const HeightmapDesc& desc)
{
    if (desc.width == 0 || desc.height == 0)
    {
        RVX_CORE_ERROR("Heightmap: Invalid dimensions {}x{}", desc.width, desc.height);
        return false;
    }

    m_width = desc.width;
    m_height = desc.height;
    m_minHeight = desc.minHeight;
    m_maxHeight = desc.maxHeight;
    m_format = desc.format;

    m_data.resize(m_width * m_height, 0.0f);

    if (desc.initialData)
    {
        const float heightRange = m_maxHeight - m_minHeight;

        switch (desc.format)
        {
        case HeightmapFormat::Float32:
            std::memcpy(m_data.data(), desc.initialData, m_data.size() * sizeof(float));
            break;

        case HeightmapFormat::UInt16:
        {
            const uint16* src = static_cast<const uint16*>(desc.initialData);
            for (size_t i = 0; i < m_data.size(); ++i)
            {
                m_data[i] = m_minHeight + (static_cast<float>(src[i]) / 65535.0f) * heightRange;
            }
            break;
        }

        case HeightmapFormat::UInt8:
        {
            const uint8* src = static_cast<const uint8*>(desc.initialData);
            for (size_t i = 0; i < m_data.size(); ++i)
            {
                m_data[i] = m_minHeight + (static_cast<float>(src[i]) / 255.0f) * heightRange;
            }
            break;
        }
        }
    }

    RVX_CORE_INFO("Heightmap: Created {}x{} heightmap", m_width, m_height);
    return true;
}

bool Heightmap::LoadFromRAW(const std::string& filename, uint32 width, uint32 height,
                             HeightmapFormat format)
{
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        RVX_CORE_ERROR("Heightmap: Failed to open file '{}'", filename);
        return false;
    }

    const std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Calculate expected size based on format
    size_t expectedSize = width * height;
    switch (format)
    {
    case HeightmapFormat::Float32:
        expectedSize *= sizeof(float);
        break;
    case HeightmapFormat::UInt16:
        expectedSize *= sizeof(uint16);
        break;
    case HeightmapFormat::UInt8:
        expectedSize *= sizeof(uint8);
        break;
    }

    if (static_cast<size_t>(fileSize) != expectedSize)
    {
        RVX_CORE_ERROR("Heightmap: File size mismatch. Expected {}, got {}", 
                       expectedSize, fileSize);
        return false;
    }

    std::vector<uint8> buffer(expectedSize);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), expectedSize))
    {
        RVX_CORE_ERROR("Heightmap: Failed to read file data");
        return false;
    }

    HeightmapDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.initialData = buffer.data();

    return Create(desc);
}

bool Heightmap::LoadFromImage(const std::string& filename)
{
    // Note: This would typically use stb_image or similar library
    // For now, we'll just log and return false
    RVX_CORE_WARN("Heightmap: LoadFromImage not yet implemented for '{}'", filename);
    return false;
}

void Heightmap::GeneratePerlinNoise(uint32 width, uint32 height, float scale,
                                     int octaves, float persistence, int seed)
{
    m_width = width;
    m_height = height;
    m_minHeight = 0.0f;
    m_maxHeight = 1.0f;
    m_format = HeightmapFormat::Float32;

    m_data.resize(width * height);

    // Simple Perlin-like noise implementation
    auto fade = [](float t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    auto lerp = [](float a, float b, float t) { return a + t * (b - a); };

    auto hash = [seed](int x, int y) -> float {
        int n = x + y * 57 + seed * 131;
        n = (n << 13) ^ n;
        return 1.0f - static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) 
               / 1073741824.0f;
    };

    auto noise = [&](float x, float y) -> float {
        int xi = static_cast<int>(std::floor(x));
        int yi = static_cast<int>(std::floor(y));
        float xf = x - xi;
        float yf = y - yi;

        float u = fade(xf);
        float v = fade(yf);

        float n00 = hash(xi, yi);
        float n10 = hash(xi + 1, yi);
        float n01 = hash(xi, yi + 1);
        float n11 = hash(xi + 1, yi + 1);

        float x0 = lerp(n00, n10, u);
        float x1 = lerp(n01, n11, u);

        return lerp(x0, x1, v);
    };

    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            float value = 0.0f;
            float amplitude = 1.0f;
            float frequency = scale;
            float maxValue = 0.0f;

            for (int o = 0; o < octaves; ++o)
            {
                value += noise(x * frequency / width, y * frequency / height) * amplitude;
                maxValue += amplitude;
                amplitude *= persistence;
                frequency *= 2.0f;
            }

            m_data[y * width + x] = (value / maxValue + 1.0f) * 0.5f;
        }
    }

    RVX_CORE_INFO("Heightmap: Generated {}x{} Perlin noise", width, height);
}

float Heightmap::SampleHeight(float u, float v) const
{
    if (m_data.empty()) return 0.0f;

    // Clamp UV to valid range
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);

    // Convert to sample coordinates
    float fx = u * (m_width - 1);
    float fy = v * (m_height - 1);

    uint32 x0 = static_cast<uint32>(fx);
    uint32 y0 = static_cast<uint32>(fy);
    uint32 x1 = std::min(x0 + 1, m_width - 1);
    uint32 y1 = std::min(y0 + 1, m_height - 1);

    float tx = fx - x0;
    float ty = fy - y0;

    // Bilinear interpolation
    float h00 = m_data[y0 * m_width + x0];
    float h10 = m_data[y0 * m_width + x1];
    float h01 = m_data[y1 * m_width + x0];
    float h11 = m_data[y1 * m_width + x1];

    float h0 = h00 + tx * (h10 - h00);
    float h1 = h01 + tx * (h11 - h01);

    return h0 + ty * (h1 - h0);
}

float Heightmap::GetHeight(uint32 x, uint32 y) const
{
    if (x >= m_width || y >= m_height) return 0.0f;
    return m_data[y * m_width + x];
}

void Heightmap::SetHeight(uint32 x, uint32 y, float height)
{
    if (x >= m_width || y >= m_height) return;
    m_data[y * m_width + x] = height;
}

Vec3 Heightmap::SampleNormal(float u, float v, const Vec3& scale) const
{
    if (m_data.empty()) return Vec3(0, 1, 0);

    const float du = 1.0f / (m_width - 1);
    const float dv = 1.0f / (m_height - 1);

    // Sample neighboring heights
    float hL = SampleHeight(u - du, v);
    float hR = SampleHeight(u + du, v);
    float hD = SampleHeight(u, v - dv);
    float hU = SampleHeight(u, v + dv);

    // Calculate tangent vectors
    Vec3 tangentU(2.0f * du * scale.x, (hR - hL) * scale.y, 0.0f);
    Vec3 tangentV(0.0f, (hU - hD) * scale.y, 2.0f * dv * scale.z);

    // Normal is cross product of tangents
    Vec3 normal = normalize(cross(tangentV, tangentU));

    return normal;
}

bool Heightmap::CreateGPUTexture(IRHIDevice* device)
{
    if (!device || m_data.empty())
    {
        RVX_CORE_ERROR("Heightmap: Cannot create GPU texture - invalid state");
        return false;
    }

    RHITextureDesc desc;
    desc.width = m_width;
    desc.height = m_height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = RHIFormat::R32_FLOAT;
    desc.usage = RHITextureUsage::ShaderResource;
    desc.dimension = RHITextureDimension::Texture2D;
    desc.debugName = "Heightmap";

    m_gpuTexture = device->CreateTexture(desc);
    if (!m_gpuTexture)
    {
        RVX_CORE_ERROR("Heightmap: Failed to create GPU texture");
        return false;
    }

    // Upload data
    // Note: Actual upload would be done through upload buffer
    // This is a simplified placeholder

    return true;
}

bool Heightmap::GenerateNormalMap(IRHIDevice* device, const Vec3& scale)
{
    if (!device || m_data.empty())
    {
        RVX_CORE_ERROR("Heightmap: Cannot generate normal map - invalid state");
        return false;
    }

    // Generate normal data
    std::vector<Vec4> normalData(m_width * m_height);

    for (uint32 y = 0; y < m_height; ++y)
    {
        for (uint32 x = 0; x < m_width; ++x)
        {
            float u = static_cast<float>(x) / (m_width - 1);
            float v = static_cast<float>(y) / (m_height - 1);

            Vec3 normal = SampleNormal(u, v, scale);

            // Pack normal into texture (normal map format: [0,1] range)
            normalData[y * m_width + x] = Vec4(
                normal.x * 0.5f + 0.5f,
                normal.y * 0.5f + 0.5f,
                normal.z * 0.5f + 0.5f,
                1.0f
            );
        }
    }

    RHITextureDesc desc;
    desc.width = m_width;
    desc.height = m_height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = RHIFormat::RGBA8_UNORM;
    desc.usage = RHITextureUsage::ShaderResource;
    desc.dimension = RHITextureDimension::Texture2D;
    desc.debugName = "HeightmapNormalMap";

    m_normalMapTexture = device->CreateTexture(desc);
    if (!m_normalMapTexture)
    {
        RVX_CORE_ERROR("Heightmap: Failed to create normal map texture");
        return false;
    }

    return true;
}

} // namespace RVX
