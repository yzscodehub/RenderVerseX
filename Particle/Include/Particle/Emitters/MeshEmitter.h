#pragma once

/**
 * @file MeshEmitter.h
 * @brief Mesh emitter - emits particles from a mesh surface
 */

#include "Particle/Emitters/IEmitter.h"
#include <string>

namespace RVX::Particle
{
    /**
     * @brief Emission mode for mesh emitter
     */
    enum class MeshEmissionMode : uint8
    {
        Vertex,     ///< Emit from vertices
        Edge,       ///< Emit from edges
        Triangle,   ///< Emit from triangle surfaces
        Volume      ///< Emit from mesh volume (requires closed mesh)
    };

    /**
     * @brief Emits particles from a mesh surface
     * 
     * Note: Mesh emission requires pre-processing of mesh data
     * and is typically done on CPU with results uploaded to GPU.
     */
    class MeshEmitter : public IEmitter
    {
    public:
        /// Path to mesh resource
        std::string meshPath;

        /// Emission mode
        MeshEmissionMode emissionMode = MeshEmissionMode::Triangle;

        /// Use mesh normals for velocity direction
        bool useNormals = true;

        /// Normal offset (emit slightly above surface)
        float normalOffset = 0.0f;

        const char* GetTypeName() const override { return "MeshEmitter"; }
        EmitterShape GetShape() const override { return EmitterShape::Mesh; }

        void GetEmitParams(EmitterGPUData& outData) const override
        {
            FillCommonParams(outData);
            outData.emitterShape = static_cast<uint32>(EmitterShape::Mesh);
            outData.shapeParams = Vec4(
                static_cast<float>(emissionMode),
                useNormals ? 1.0f : 0.0f,
                normalOffset,
                0.0f
            );
            // Note: Actual mesh data (vertex positions, triangles) must be 
            // uploaded separately via mesh-specific buffers
        }
    };

} // namespace RVX::Particle
