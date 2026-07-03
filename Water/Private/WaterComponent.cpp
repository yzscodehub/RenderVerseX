/**
 * @file WaterComponent.cpp
 * @brief Implementation of water scene component
 */

#include "Water/WaterComponent.h"
#include "Water/Caustics.h"
#include "Water/Underwater.h"
#include "Water/WaterSimulation.h"
#include "Water/WaterSurface.h"
#include "Scene/SceneEntity.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace RVX
{
namespace
{
    WaterRenderSnapshotSurfaceType ToSnapshotSurfaceType(WaterSurfaceType type)
    {
        switch (type)
        {
            case WaterSurfaceType::Ocean:
                return WaterRenderSnapshotSurfaceType::Ocean;
            case WaterSurfaceType::Lake:
                return WaterRenderSnapshotSurfaceType::Lake;
            case WaterSurfaceType::River:
                return WaterRenderSnapshotSurfaceType::River;
            case WaterSurfaceType::Pool:
                return WaterRenderSnapshotSurfaceType::Pool;
        }

        return WaterRenderSnapshotSurfaceType::Ocean;
    }

    WaterRenderSnapshotSimulationType ToSnapshotSimulationType(WaterSimulationType type)
    {
        switch (type)
        {
            case WaterSimulationType::Simple:
                return WaterRenderSnapshotSimulationType::Simple;
            case WaterSimulationType::Gerstner:
                return WaterRenderSnapshotSimulationType::Gerstner;
            case WaterSimulationType::FFT:
                return WaterRenderSnapshotSimulationType::FFT;
        }

        return WaterRenderSnapshotSimulationType::Gerstner;
    }
} // namespace

WaterComponent::WaterComponent() = default;

WaterComponent::~WaterComponent() = default;

void WaterComponent::OnAttach()
{
    RVX_CORE_INFO("WaterComponent: Attached to entity");

    m_surface = std::make_unique<WaterSurface>();
    m_simulation = std::make_unique<WaterSimulation>();
    m_caustics = std::make_unique<Caustics>();
    m_underwater = std::make_unique<Underwater>();

    // Apply default settings
    SetSettings(m_settings);
    UpdateBounds();
}

void WaterComponent::OnDetach()
{
    RVX_CORE_INFO("WaterComponent: Detached from entity");

    m_surface.reset();
    m_simulation.reset();
    m_caustics.reset();
    m_underwater.reset();
}

void WaterComponent::Tick(float deltaTime)
{
    if (m_simulation)
    {
        m_simulation->Update(deltaTime);
    }

    if (m_caustics && m_settings.enableCaustics)
    {
        m_caustics->Update(deltaTime, m_simulation.get());
    }

    if (m_underwater && m_settings.enableUnderwaterEffects)
    {
        m_underwater->Update(deltaTime);
    }
}

AABB WaterComponent::GetLocalBounds() const
{
    return m_localBounds;
}

void WaterComponent::SetSettings(const WaterSettings& settings)
{
    m_settings = settings;

    // Update surface
    if (m_surface)
    {
        WaterSurfaceDesc surfaceDesc;
        surfaceDesc.size = settings.size;
        surfaceDesc.resolution = settings.resolution;
        surfaceDesc.type = settings.surfaceType;
        m_surface->Create(surfaceDesc);
    }

    // Update simulation
    if (m_simulation)
    {
        WaterSimulationDesc simDesc;
        simDesc.type = settings.simulationType;
        simDesc.resolution = 256;
        simDesc.domainSize = std::max(settings.size.x, settings.size.y);

        // Add default Gerstner waves for ocean
        if (settings.simulationType == WaterSimulationType::Gerstner)
        {
            GerstnerWave wave1;
            wave1.direction = Vec2(1.0f, 0.3f);
            wave1.wavelength = 20.0f;
            wave1.amplitude = 0.5f;
            wave1.steepness = 0.5f;
            simDesc.gerstnerWaves.push_back(wave1);

            GerstnerWave wave2;
            wave2.direction = Vec2(0.5f, 1.0f);
            wave2.wavelength = 15.0f;
            wave2.amplitude = 0.3f;
            wave2.steepness = 0.4f;
            simDesc.gerstnerWaves.push_back(wave2);

            GerstnerWave wave3;
            wave3.direction = Vec2(-0.3f, 0.8f);
            wave3.wavelength = 8.0f;
            wave3.amplitude = 0.15f;
            wave3.steepness = 0.3f;
            simDesc.gerstnerWaves.push_back(wave3);
        }

        m_simulation->Initialize(simDesc);
    }

    // Update caustics
    if (m_caustics && settings.enableCaustics)
    {
        CausticsDesc causticsDesc;
        causticsDesc.quality = CausticsQuality::Medium;
        causticsDesc.maxDepth = settings.depth;
        m_caustics->Initialize(causticsDesc);
    }

    // Update underwater
    if (m_underwater && settings.enableUnderwaterEffects)
    {
        UnderwaterDesc underwaterDesc;
        underwaterDesc.quality = UnderwaterQuality::High;
        m_underwater->Initialize(underwaterDesc);
    }

    UpdateBounds();
    NotifyBoundsChanged();
}

void WaterComponent::SetWind(const Vec2& direction, float speed)
{
    if (m_simulation)
    {
        m_simulation->SetWind(direction, speed);
    }
}

float WaterComponent::GetWaterHeightAt(float worldX, float worldZ) const
{
    Vec3 terrainPos(0.0f);
    if (auto* owner = GetOwner())
    {
        terrainPos = owner->GetWorldPosition();
    }

    float baseHeight = terrainPos.y;

    if (m_simulation)
    {
        Vec2 local = WorldToLocal(worldX, worldZ);
        float waveHeight = m_simulation->SampleHeight(local.x, local.y);
        return baseHeight + waveHeight;
    }

    return baseHeight;
}

Vec3 WaterComponent::GetWaterNormalAt(float worldX, float worldZ) const
{
    if (m_simulation)
    {
        Vec2 local = WorldToLocal(worldX, worldZ);
        return m_simulation->SampleNormal(local.x, local.y);
    }

    return Vec3(0, 1, 0);
}

Vec3 WaterComponent::GetDisplacementAt(float worldX, float worldZ) const
{
    if (m_simulation)
    {
        Vec2 local = WorldToLocal(worldX, worldZ);
        return m_simulation->SampleDisplacement(local.x, local.y);
    }

    return Vec3(0);
}

bool WaterComponent::IsUnderwater(const Vec3& worldPos) const
{
    float waterHeight = GetWaterHeightAt(worldPos.x, worldPos.z);
    return worldPos.y < waterHeight;
}

float WaterComponent::GetDepthAt(float worldX, float worldZ, float terrainHeight) const
{
    float waterHeight = GetWaterHeightAt(worldX, worldZ);
    return waterHeight - terrainHeight;
}

Vec3 WaterComponent::CalculateBuoyancy(const Vec3& position, float volume, float objectDensity) const
{
    const float waterDensity = 1000.0f;  // kg/m³
    const float gravity = 9.81f;         // m/s²

    if (!IsUnderwater(position))
    {
        return Vec3(0);
    }

    // Calculate submerged fraction (simplified)
    float waterHeight = GetWaterHeightAt(position.x, position.z);
    float submergedDepth = waterHeight - position.y;
    float submergedFraction = std::clamp(submergedDepth / 2.0f, 0.0f, 1.0f);

    // Buoyancy force = water density * g * submerged volume
    float buoyancyForce = waterDensity * gravity * volume * submergedFraction;

    // Weight = object density * g * volume
    float weight = objectDensity * gravity * volume;

    // Net upward force
    float netForce = buoyancyForce - weight;

    return Vec3(0, netForce, 0);
}

bool WaterComponent::BuildRenderSnapshot(WaterRenderSnapshot& outSnapshot) const
{
    outSnapshot.BeginBuild(++m_nextRenderSnapshotSequence);

    WaterRenderSnapshotItem item;
    if (auto* owner = GetOwner())
    {
        item.componentId = owner->GetHandle();
        item.worldPosition = owner->GetWorldPosition();
        item.worldBounds = owner->GetWorldBounds();
    }
    else
    {
        item.worldBounds = m_localBounds;
    }

    const WaterVisualProperties visual = m_surface ? m_surface->GetVisualProperties() : WaterVisualProperties{};
    item.size = m_settings.size;
    item.depth = m_settings.depth;
    item.resolution = m_settings.resolution;
    item.surfaceType = ToSnapshotSurfaceType(m_settings.surfaceType);
    item.simulationType = ToSnapshotSimulationType(m_settings.simulationType);
    item.shallowColor = visual.shallowColor;
    item.deepColor = visual.deepColor;
    item.foamColor = visual.foamColor;
    item.transparency = visual.transparency;
    item.reflectionStrength = visual.reflectionStrength;
    item.refractionStrength = visual.refractionStrength;
    item.roughness = visual.roughness;
    item.foamIntensity = visual.foamIntensity;
    item.reflectionEnabled = m_settings.enableReflection;
    item.refractionEnabled = m_settings.enableRefraction;
    item.causticsEnabled = m_settings.enableCaustics;
    item.underwaterEffectsEnabled = m_settings.enableUnderwaterEffects;
    item.foamEnabled = m_settings.enableFoam;
    item.gpuInitialized = m_gpuInitialized;

    outSnapshot.items.push_back(item);
    outSnapshot.MarkComplete();
    return true;
}

void WaterComponent::UpdateBounds()
{
    Vec3 halfSize(m_settings.size.x * 0.5f, 
                   m_settings.depth,
                   m_settings.size.y * 0.5f);

    // Include wave amplitude in bounds
    float waveAmplitude = 5.0f;  // Estimate max wave height
    
    m_localBounds = AABB(
        Vec3(-halfSize.x, -halfSize.y, -halfSize.z),
        Vec3(halfSize.x, waveAmplitude, halfSize.z)
    );
}

Vec2 WaterComponent::WorldToLocal(float worldX, float worldZ) const
{
    Vec3 terrainPos(0.0f);
    if (auto* owner = GetOwner())
    {
        terrainPos = owner->GetWorldPosition();
    }

    return Vec2(worldX - terrainPos.x, worldZ - terrainPos.z);
}

} // namespace RVX
