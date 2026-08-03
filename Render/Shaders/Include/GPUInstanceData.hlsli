#ifndef RVX_GPU_INSTANCE_DATA_HLSLI
#define RVX_GPU_INSTANCE_DATA_HLSLI

// Mirrors RVX::GPUInstanceData. Keep C++ offsetof assertions in
// Render/GPUDriven/GPUCulling.h synchronized with this declaration.
struct GPUInstanceData
{
    float4x4 worldMatrix;
    float4x4 normalMatrix;
    float4 boundingSphere;
    float4 aabbMin;
    float4 aabbMax;
    uint meshId;
    uint materialId;
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint sourceIndex;
    uint drawGroupIndex;
    uint drawGroupCommandOffset;
    uint candidateIndex;
    uint forceVisible;
};

#endif
