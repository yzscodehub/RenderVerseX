#include "Particle/ParticlePool.h"
#include "Particle/ParticleSystemInstance.h"

namespace RVX::Particle
{

ParticleSystemInstance* ParticlePool::Acquire(ParticleSystem::Ptr system)
{
    if (!system)
        return nullptr;

    auto it = m_pools.find(system.get());
    if (it != m_pools.end() && !it->second.available.empty())
    {
        // Get from pool
        auto entry = std::move(it->second.available.back());
        it->second.available.pop_back();
        
        ParticleSystemInstance* instance = entry.instance.release();
        instance->Clear();
        return instance;
    }

    // Create new instance
    return new ParticleSystemInstance(system);
}

void ParticlePool::Release(ParticleSystemInstance* instance)
{
    if (!instance)
        return;

    auto system = instance->GetSystem();
    if (!system)
    {
        delete instance;
        return;
    }

    auto& pool = m_pools[system.get()];
    
    // Check if pool is full
    if (pool.available.size() >= pool.maxSize)
    {
        delete instance;
        return;
    }

    // Stop and clear the instance
    instance->Stop();
    instance->Clear();

    // Add to pool
    PoolEntry entry;
    entry.instance.reset(instance);
    entry.idleTime = 0.0f;
    pool.available.push_back(std::move(entry));
}

void ParticlePool::Prewarm(ParticleSystem::Ptr system, uint32 count)
{
    if (!system)
        return;

    auto& pool = m_pools[system.get()];
    
    for (uint32 i = 0; i < count && pool.available.size() < pool.maxSize; ++i)
    {
        PoolEntry entry;
        entry.instance = std::make_unique<ParticleSystemInstance>(system);
        entry.idleTime = 0.0f;
        pool.available.push_back(std::move(entry));
    }
}

void ParticlePool::SetPoolSize(ParticleSystem::Ptr system, uint32 size)
{
    if (!system)
        return;

    m_pools[system.get()].maxSize = size;
}

uint32 ParticlePool::GetPoolSize(ParticleSystem::Ptr system) const
{
    if (!system)
        return 0;

    auto it = m_pools.find(system.get());
    if (it != m_pools.end())
        return it->second.maxSize;
    return 0;
}

void ParticlePool::Cleanup(float maxIdleTime)
{
    for (auto& [system, pool] : m_pools)
    {
        auto it = pool.available.begin();
        while (it != pool.available.end())
        {
            it->idleTime += 1.0f / 60.0f;  // Assuming called once per frame at 60 FPS
            
            if (it->idleTime > maxIdleTime)
            {
                it = pool.available.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

void ParticlePool::Clear()
{
    m_pools.clear();
}

uint32 ParticlePool::GetTotalPooled() const
{
    uint32 total = 0;
    for (const auto& [system, pool] : m_pools)
    {
        total += static_cast<uint32>(pool.available.size());
    }
    return total;
}

uint32 ParticlePool::GetPooledSystemCount() const
{
    return static_cast<uint32>(m_pools.size());
}

} // namespace RVX::Particle
