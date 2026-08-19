#pragma once

/**
 * @file Heightmap.h
 * @brief Terrain heightmap data management
 * 
 * Heightmap provides storage and sampling of terrain height data.
 * Supports loading from image files and procedural generation.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    /**
     * @brief Heightmap data format
     */
    enum class HeightmapFormat : uint8
    {
        Float32,    ///< 32-bit float per sample
        UInt16,     ///< 16-bit unsigned integer per sample
        UInt8       ///< 8-bit unsigned integer per sample
    };

    /**
     * @brief Heightmap creation descriptor
     */
    struct HeightmapDesc
    {
        uint32 width = 0;               ///< Width in samples
        uint32 height = 0;              ///< Height in samples
        HeightmapFormat format = HeightmapFormat::Float32;
        float minHeight = 0.0f;         ///< Minimum height value
        float maxHeight = 100.0f;       ///< Maximum height value
        const void* initialData = nullptr;  ///< Optional initial data
    };

    /**
     * @brief Terrain heightmap
     * 
     * Stores and provides access to terrain height data.
     * Supports bilinear interpolation for smooth height queries.
     * 
     * Usage:
     * @code
     * HeightmapDesc desc;
     * desc.width = 1025;
     * desc.height = 1025;
     * desc.minHeight = 0.0f;
     * desc.maxHeight = 500.0f;
     * 
     * auto heightmap = std::make_shared<Heightmap>();
     * heightmap->Create(desc);
     * heightmap->LoadFromFile("terrain.raw");
     * 
     * float h = heightmap->SampleHeight(0.5f, 0.5f);  // Sample at center
     * @endcode
     */
    class Heightmap
    {
    public:
        using Ptr = std::shared_ptr<Heightmap>;

        Heightmap() = default;
        ~Heightmap();

        // Non-copyable
        Heightmap(const Heightmap&) = delete;
        Heightmap& operator=(const Heightmap&) = delete;

        // =====================================================================
        // Creation
        // =====================================================================

        /**
         * @brief Create an empty heightmap
         * @param desc Heightmap descriptor
         * @return true if creation succeeded
         */
        bool Create(const HeightmapDesc& desc);

        /**
         * @brief Load heightmap from a RAW file
         * @param filename Path to RAW file
         * @param width Expected width
         * @param height Expected height
         * @param format Data format
         * @return true if loading succeeded
         */
        bool LoadFromRAW(const std::string& filename, uint32 width, uint32 height, 
                         HeightmapFormat format = HeightmapFormat::UInt16);

        /**
         * @brief Load heightmap from an image file (PNG, BMP, etc.)
         * @param filename Path to image file
         * @return true if loading succeeded
         */
        bool LoadFromImage(const std::string& filename);

        /**
         * @brief Generate heightmap using Perlin noise
         * @param width Heightmap width
         * @param height Heightmap height
         * @param scale Noise scale
         * @param octaves Number of octaves
         * @param persistence Persistence value
         * @param seed Random seed
         */
        void GeneratePerlinNoise(uint32 width, uint32 height, float scale = 1.0f,
                                  int octaves = 6, float persistence = 0.5f, int seed = 0);

        // =====================================================================
        // Sampling
        // =====================================================================

        /**
         * @brief Sample height at normalized UV coordinates
         * @param u Horizontal position [0, 1]
         * @param v Vertical position [0, 1]
         * @return Interpolated height value
         */
        float SampleHeight(float u, float v) const;

        /**
         * @brief Sample height at integer coordinates
         * @param x Horizontal index
         * @param y Vertical index
         * @return Height value at the sample
         */
        float GetHeight(uint32 x, uint32 y) const;

        /**
         * @brief Set height at integer coordinates
         * @param x Horizontal index
         * @param y Vertical index
         * @param height Height value
         */
        void SetHeight(uint32 x, uint32 y, float height);

        /**
         * @brief Calculate normal at UV coordinates
         * @param u Horizontal position [0, 1]
         * @param v Vertical position [0, 1]
         * @param scale Terrain scale for proper normal calculation
         * @return Surface normal vector
         */
        Vec3 SampleNormal(float u, float v, const Vec3& scale) const;

        // =====================================================================
        // Properties
        // =====================================================================

        uint32 GetWidth() const { return m_width; }
        uint32 GetHeight() const { return m_height; }
        float GetMinHeight() const { return m_minHeight; }
        float GetMaxHeight() const { return m_maxHeight; }
        HeightmapFormat GetFormat() const { return m_format; }

        /**
         * @brief Get raw height data
         * @return Pointer to height data array
         */
        const float* GetData() const { return m_data.data(); }
        float* GetData() { return m_data.data(); }

        /**
         * @brief Check if heightmap is valid
         */
        bool IsValid() const { return !m_data.empty() && m_width > 0 && m_height > 0; }

        // =====================================================================
        // Render Upload Diagnostics
        // =====================================================================

        /**
         * @brief Report whether Terrain can create a feature-owned height texture.
         */
        bool CreateGPUTexture();

        /**
         * @brief Check whether height data has been uploaded by a render-owned path.
         */
        bool IsGPUTextureDataUploaded() const { return m_gpuTextureDataUploaded; }

        /**
         * @brief Diagnostic for the last height texture upload attempt or stale state
         */
        const std::string& GetGPUTextureDiagnostic() const { return m_gpuTextureDiagnostic; }

        /**
         * @brief Report whether Terrain can create a feature-owned normal map texture.
         * @param scale Terrain scale for proper normal calculation
         */
        bool GenerateNormalMap(const Vec3& scale);

        /**
         * @brief Check whether normal map texture data has been uploaded
         */
        bool IsNormalMapDataUploaded() const { return m_normalMapDataUploaded; }

        /**
         * @brief Diagnostic for the last normal map upload attempt or stale state
         */
        const std::string& GetNormalMapDiagnostic() const { return m_normalMapDiagnostic; }

    private:
        void MarkGPUDataStale(const char* reason);

        std::vector<float> m_data;      ///< Height data (always stored as float internally)
        uint32 m_width = 0;
        uint32 m_height = 0;
        float m_minHeight = 0.0f;
        float m_maxHeight = 100.0f;
        HeightmapFormat m_format = HeightmapFormat::Float32;

        bool m_gpuTextureDataUploaded = false;
        bool m_normalMapDataUploaded = false;
        std::string m_gpuTextureDiagnostic = "Heightmap render texture has not been created.";
        std::string m_normalMapDiagnostic = "Heightmap render normal map has not been generated.";
    };

} // namespace RVX
