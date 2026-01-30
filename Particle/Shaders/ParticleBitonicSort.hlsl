/**
 * @file ParticleBitonicSort.hlsl
 * @brief GPU bitonic sort for particle depth sorting
 */

// Sort key: distance + index
struct SortKey
{
    float distance;
    uint index;
};

RWStructuredBuffer<SortKey> g_SortKeys : register(u0);

cbuffer SortParams : register(b0)
{
    uint g_K;           // Current bitonic block size
    uint g_J;           // Current comparison distance
    uint g_Count;       // Number of particles
    uint g_Pad;
};

// Compare and swap
void CompareAndSwap(uint i, uint j, bool ascending)
{
    SortKey a = g_SortKeys[i];
    SortKey b = g_SortKeys[j];
    
    bool swap = ascending ? (a.distance > b.distance) : (a.distance < b.distance);
    
    if (swap)
    {
        g_SortKeys[i] = b;
        g_SortKeys[j] = a;
    }
}

[numthreads(256, 1, 1)]
void CSBitonicSort(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;
    
    // Calculate partner index
    uint ixj = i ^ g_J;
    
    if (ixj > i && ixj < g_Count && i < g_Count)
    {
        // Determine sort direction
        bool ascending = ((i & g_K) == 0);
        CompareAndSwap(i, ixj, ascending);
    }
}

[numthreads(256, 1, 1)]
void CSBitonicMerge(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;
    uint ixj = i ^ g_J;
    
    if (ixj > i && ixj < g_Count && i < g_Count)
    {
        // Always ascending for merge phase
        bool ascending = ((i & g_K) == 0);
        CompareAndSwap(i, ixj, ascending);
    }
}

// Generate sort keys from particle distances
StructuredBuffer<GPUParticle> g_Particles : register(t0);
RWStructuredBuffer<SortKey> g_SortKeysOut : register(u1);

cbuffer KeyGenParams : register(b1)
{
    float3 g_CameraPosition;
    uint g_ParticleCount;
};

[numthreads(256, 1, 1)]
void CSGenerateSortKeys(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;
    
    if (i >= g_ParticleCount)
        return;
    
    GPUParticle p = g_Particles[i];
    
    SortKey key;
    key.distance = length(p.position - g_CameraPosition);
    key.index = i;
    
    g_SortKeysOut[i] = key;
}
