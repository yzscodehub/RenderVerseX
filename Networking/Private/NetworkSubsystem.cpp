#include "Networking/NetworkSubsystem.h"
#include "Core/Log.h"

namespace RVX::Net
{
    NetworkSubsystem::NetworkSubsystem() = default;
    NetworkSubsystem::~NetworkSubsystem() = default;

    void NetworkSubsystem::Initialize()
    {
        RVX_CORE_INFO("NetworkSubsystem initializing...");
        
        // Replication manager will be initialized when network starts
        RVX_CORE_INFO("NetworkSubsystem initialized");
    }

    void NetworkSubsystem::Deinitialize()
    {
        RVX_CORE_INFO("NetworkSubsystem shutting down...");
        
        m_replicationManager.Shutdown();
        m_networkManager.Stop();
        
        RVX_CORE_INFO("NetworkSubsystem shutdown complete");
    }

    void NetworkSubsystem::Tick(float deltaTime)
    {
        if (m_networkManager.IsActive())
        {
            m_networkManager.Update(deltaTime);
            m_replicationManager.Update(deltaTime);
        }
    }

    bool NetworkSubsystem::StartServer(uint16 port)
    {
        if (!m_networkManager.StartServer(port))
            return false;
        
        m_replicationManager.Initialize(&m_networkManager);
        return true;
    }

    bool NetworkSubsystem::Connect(const std::string& address, uint16 port)
    {
        if (!m_networkManager.Connect(address, port))
            return false;
        
        m_replicationManager.Initialize(&m_networkManager);
        return true;
    }

    bool NetworkSubsystem::StartHost(uint16 port)
    {
        if (!m_networkManager.StartHost(port))
            return false;
        
        m_replicationManager.Initialize(&m_networkManager);
        return true;
    }

    void NetworkSubsystem::Stop()
    {
        m_replicationManager.Shutdown();
        m_networkManager.Stop();
    }

} // namespace RVX::Net
