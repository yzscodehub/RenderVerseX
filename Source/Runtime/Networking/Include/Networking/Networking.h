#pragma once

/**
 * @file Networking.h
 * @brief Networking module unified header
 * 
 * Provides network communication and object replication.
 * Uses ASIO for cross-platform socket operations.
 * 
 * Features:
 * - Client/Server/Host networking modes
 * - Reliable and unreliable delivery
 * - Connection management with keep-alive
 * - Bit-level serialization for bandwidth efficiency
 * - Object replication with delta compression
 * - Property-level change tracking
 * - Fragmentation for large packets
 * 
 * Usage:
 * @code
 * #include "Networking/Networking.h"
 * using namespace RVX::Net;
 * 
 * // Server
 * NetworkManager server;
 * server.StartServer(7777);
 * server.SetOnConnect([](ConnectionPtr conn) {
 *     // Handle new connection
 * });
 * 
 * // Client
 * NetworkManager client;
 * client.Connect("127.0.0.1", 7777);
 * client.SetOnData([](ConnectionPtr conn, std::span<const uint8> data, ChannelId ch) {
 *     // Handle received data
 * });
 * 
 * // Update each frame
 * server.Update(deltaTime);
 * client.Update(deltaTime);
 * @endcode
 */

// Core types
#include "Networking/NetworkTypes.h"

// Serialization
#include "Networking/Serialization/BitStream.h"
#include "Networking/Serialization/NetworkSerializer.h"

// Transport
#include "Networking/Transport/ITransport.h"
#include "Networking/Transport/UDPTransport.h"
#include "Networking/Transport/ReliableUDP.h"

// Connection management
#include "Networking/Packet.h"
#include "Networking/Connection.h"
#include "Networking/NetworkManager.h"

// Replication
#include "Networking/Replication/ReplicatedObject.h"
#include "Networking/Replication/PropertyReplication.h"

// Subsystem
#include "Networking/NetworkSubsystem.h"

namespace RVX::Net
{

/**
 * @brief Networking module version info
 */
struct NetworkingInfo
{
    static constexpr const char* kModuleName = "RVX::Net";
    static constexpr int kMajorVersion = 1;
    static constexpr int kMinorVersion = 0;
    static constexpr int kPatchVersion = 0;
    
    static constexpr const char* GetVersionString()
    {
        return "1.0.0";
    }
    
    static constexpr uint32 GetProtocolVersion()
    {
        return RVX_NET_PROTOCOL_VERSION;
    }
};

} // namespace RVX::Net
