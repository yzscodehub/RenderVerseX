/**
 * @file NoiseUtils.hlsli
 * @brief Noise functions for particle turbulence
 */

#ifndef NOISE_UTILS_HLSLI
#define NOISE_UTILS_HLSLI

// Simplex noise permutation table
static const int perm[256] = {
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
    140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
    247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
    57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
    74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
    60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
    65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
    200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
    52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
    207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
    119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
    129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
    218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
    81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
    184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
    222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
};

float fade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
float grad(int hash, float x, float y, float z)
{
    int h = hash & 15;
    float u = h < 8 ? x : y;
    float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

/**
 * 3D Perlin noise
 */
float PerlinNoise3D(float3 p)
{
    int X = int(floor(p.x)) & 255;
    int Y = int(floor(p.y)) & 255;
    int Z = int(floor(p.z)) & 255;
    
    float x = p.x - floor(p.x);
    float y = p.y - floor(p.y);
    float z = p.z - floor(p.z);
    
    float u = fade(x);
    float v = fade(y);
    float w = fade(z);
    
    int A = perm[X] + Y;
    int AA = perm[A & 255] + Z;
    int AB = perm[(A + 1) & 255] + Z;
    int B = perm[(X + 1) & 255] + Y;
    int BA = perm[B & 255] + Z;
    int BB = perm[(B + 1) & 255] + Z;
    
    return lerp(
        lerp(lerp(grad(perm[AA & 255], x, y, z),
                  grad(perm[BA & 255], x - 1, y, z), u),
             lerp(grad(perm[AB & 255], x, y - 1, z),
                  grad(perm[BB & 255], x - 1, y - 1, z), u), v),
        lerp(lerp(grad(perm[(AA + 1) & 255], x, y, z - 1),
                  grad(perm[(BA + 1) & 255], x - 1, y, z - 1), u),
             lerp(grad(perm[(AB + 1) & 255], x, y - 1, z - 1),
                  grad(perm[(BB + 1) & 255], x - 1, y - 1, z - 1), u), v), w);
}

/**
 * Fractal Brownian Motion (multi-octave noise)
 */
float FBM(float3 p, int octaves, float persistence)
{
    float total = 0.0;
    float frequency = 1.0;
    float amplitude = 1.0;
    float maxValue = 0.0;
    
    for (int i = 0; i < octaves; i++)
    {
        total += PerlinNoise3D(p * frequency) * amplitude;
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= 2.0;
    }
    
    return total / maxValue;
}

/**
 * 3D curl noise for divergence-free turbulence
 */
float3 CurlNoise3D(float3 p, float epsilon)
{
    float3 curl;
    
    float n1 = PerlinNoise3D(p + float3(0, epsilon, 0));
    float n2 = PerlinNoise3D(p - float3(0, epsilon, 0));
    float n3 = PerlinNoise3D(p + float3(0, 0, epsilon));
    float n4 = PerlinNoise3D(p - float3(0, 0, epsilon));
    float n5 = PerlinNoise3D(p + float3(epsilon, 0, 0));
    float n6 = PerlinNoise3D(p - float3(epsilon, 0, 0));
    
    curl.x = (n1 - n2) - (n3 - n4);
    curl.y = (n3 - n4) - (n5 - n6);
    curl.z = (n5 - n6) - (n1 - n2);
    
    return curl / (2.0 * epsilon);
}

#endif // NOISE_UTILS_HLSLI
