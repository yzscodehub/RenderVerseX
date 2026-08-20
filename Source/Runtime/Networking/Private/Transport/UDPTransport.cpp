#include "Networking/Transport/UDPTransport.h"
#include "Core/Log.h"

#ifdef _WIN32
    #define _WIN32_WINNT 0x0601
#endif

#include <asio.hpp>

namespace RVX::Net
{
    // =========================================================================
    // Implementation Details
    // =========================================================================

    struct UDPTransport::Impl
    {
        asio::io_context ioContext;
        std::unique_ptr<asio::ip::udp::socket> socket;
        asio::ip::udp::endpoint localEndpoint;
        asio::ip::udp::endpoint remoteEndpoint; // For receiving
        
        std::array<uint8, RVX_NET_MAX_PACKET_SIZE> receiveBuffer;
        
        bool ipv6 = false;
        bool broadcastEnabled = false;
    };

    // =========================================================================
    // Construction
    // =========================================================================

    UDPTransport::UDPTransport()
        : m_impl(std::make_unique<Impl>())
    {
    }

    UDPTransport::~UDPTransport()
    {
        Shutdown();
    }

    // =========================================================================
    // ITransport Implementation
    // =========================================================================

    TransportResult UDPTransport::Initialize(const TransportConfig& config)
    {
        if (m_active)
        {
            RVX_CORE_WARN("UDPTransport already initialized");
            return TransportResult::Error;
        }

        try
        {
            m_impl->ipv6 = config.enableIPv6;
            m_impl->broadcastEnabled = config.enableBroadcast;

            // Create socket
            auto protocol = config.enableIPv6 ? 
                asio::ip::udp::v6() : asio::ip::udp::v4();
            
            m_impl->socket = std::make_unique<asio::ip::udp::socket>(
                m_impl->ioContext, protocol);

            // Set socket options
            if (config.reuseAddress)
            {
                m_impl->socket->set_option(asio::socket_base::reuse_address(true));
            }

            if (config.enableBroadcast)
            {
                m_impl->socket->set_option(asio::socket_base::broadcast(true));
            }

            if (config.receiveBufferSize > 0)
            {
                m_impl->socket->set_option(
                    asio::socket_base::receive_buffer_size(config.receiveBufferSize));
            }

            if (config.sendBufferSize > 0)
            {
                m_impl->socket->set_option(
                    asio::socket_base::send_buffer_size(config.sendBufferSize));
            }

            // Non-blocking mode
            m_impl->socket->non_blocking(config.nonBlocking);

            // Bind to address
            asio::ip::address bindAddr;
            if (!config.bindAddress.empty())
            {
                bindAddr = asio::ip::make_address(config.bindAddress);
            }
            else if (config.enableIPv6)
            {
                bindAddr = asio::ip::address_v6::any();
            }
            else
            {
                bindAddr = asio::ip::address_v4::any();
            }

            m_impl->localEndpoint = asio::ip::udp::endpoint(bindAddr, config.bindPort);
            m_impl->socket->bind(m_impl->localEndpoint);

            // Update local endpoint with actual bound port
            m_impl->localEndpoint = m_impl->socket->local_endpoint();

            m_active = true;

            RVX_CORE_INFO("UDPTransport initialized on {}:{}",
                          m_impl->localEndpoint.address().to_string(),
                          m_impl->localEndpoint.port());

            // Start async receive
            StartReceive();

            return TransportResult::Success;
        }
        catch (const asio::system_error& e)
        {
            RVX_CORE_ERROR("Failed to initialize UDPTransport: {}", e.what());
            
            if (e.code() == asio::error::address_in_use)
                return TransportResult::BindFailed;
            
            return TransportResult::Error;
        }
    }

    void UDPTransport::Shutdown()
    {
        if (!m_active)
            return;

        m_active = false;

        if (m_impl->socket)
        {
            std::error_code ec;
            m_impl->socket->close(ec);
            m_impl->socket.reset();
        }

        // Clear receive queue
        std::lock_guard<std::mutex> lock(m_receiveMutex);
        while (!m_receiveQueue.empty())
            m_receiveQueue.pop();

        RVX_CORE_INFO("UDPTransport shutdown");
    }

    bool UDPTransport::IsActive() const
    {
        return m_active;
    }

    TransportResult UDPTransport::SendTo(
        const NetworkAddress& address,
        std::span<const uint8> data)
    {
        if (!m_active || !m_impl->socket)
            return TransportResult::Error;

        if (data.size() > m_mtu)
        {
            RVX_CORE_WARN("Packet size {} exceeds MTU {}", data.size(), m_mtu);
        }

        try
        {
            asio::ip::udp::resolver resolver(m_impl->ioContext);
            auto endpoints = resolver.resolve(
                address.isIPv6 ? asio::ip::udp::v6() : asio::ip::udp::v4(),
                address.host,
                std::to_string(address.port));

            if (endpoints.empty())
            {
                RVX_CORE_ERROR("Could not resolve address: {}", address.ToString());
                return TransportResult::InvalidAddress;
            }

            asio::ip::udp::endpoint destEndpoint = *endpoints.begin();

            std::error_code ec;
            m_impl->socket->send_to(
                asio::buffer(data.data(), data.size()),
                destEndpoint,
                0,
                ec);

            if (ec)
            {
                if (ec == asio::error::would_block)
                    return TransportResult::WouldBlock;
                
                RVX_CORE_ERROR("Send failed: {}", ec.message());
                return TransportResult::SendFailed;
            }

            return TransportResult::Success;
        }
        catch (const asio::system_error& e)
        {
            RVX_CORE_ERROR("Send exception: {}", e.what());
            return TransportResult::SendFailed;
        }
    }

    TransportResult UDPTransport::ReceiveFrom(ReceivedPacket& outPacket)
    {
        std::lock_guard<std::mutex> lock(m_receiveMutex);
        
        if (m_receiveQueue.empty())
            return TransportResult::WouldBlock;

        outPacket = std::move(m_receiveQueue.front());
        m_receiveQueue.pop();
        
        return TransportResult::Success;
    }

    uint32 UDPTransport::Poll(uint32 timeoutMs)
    {
        if (!m_active)
            return 0;

        try
        {
            // Run pending handlers
            if (timeoutMs == 0)
            {
                return static_cast<uint32>(m_impl->ioContext.poll());
            }
            else
            {
                m_impl->ioContext.run_for(std::chrono::milliseconds(timeoutMs));
                return 1; // Approximate
            }
        }
        catch (const asio::system_error& e)
        {
            RVX_CORE_ERROR("Poll error: {}", e.what());
            return 0;
        }
    }

    void UDPTransport::SetPacketCallback(PacketReceivedCallback callback)
    {
        m_packetCallback = std::move(callback);
    }

    NetworkAddress UDPTransport::GetLocalAddress() const
    {
        if (!m_impl->socket)
            return {};

        NetworkAddress addr;
        addr.host = m_impl->localEndpoint.address().to_string();
        addr.port = m_impl->localEndpoint.port();
        addr.isIPv6 = m_impl->localEndpoint.address().is_v6();
        return addr;
    }

    TransportResult UDPTransport::Broadcast(uint16 port, std::span<const uint8> data)
    {
        if (!m_active || !m_impl->socket || !m_impl->broadcastEnabled)
            return TransportResult::Error;

        try
        {
            asio::ip::udp::endpoint broadcastEndpoint(
                asio::ip::address_v4::broadcast(), port);

            std::error_code ec;
            m_impl->socket->send_to(
                asio::buffer(data.data(), data.size()),
                broadcastEndpoint,
                0,
                ec);

            if (ec)
            {
                RVX_CORE_ERROR("Broadcast failed: {}", ec.message());
                return TransportResult::SendFailed;
            }

            return TransportResult::Success;
        }
        catch (const asio::system_error& e)
        {
            RVX_CORE_ERROR("Broadcast exception: {}", e.what());
            return TransportResult::SendFailed;
        }
    }

    uint32 UDPTransport::GetPendingSendCount() const
    {
        return 0; // UDP is connectionless, no send queue
    }

    uint32 UDPTransport::GetPendingReceiveCount() const
    {
        std::lock_guard<std::mutex> lock(m_receiveMutex);
        return static_cast<uint32>(m_receiveQueue.size());
    }

    void UDPTransport::StartReceive()
    {
        if (!m_active || !m_impl->socket)
            return;

        m_impl->socket->async_receive_from(
            asio::buffer(m_impl->receiveBuffer),
            m_impl->remoteEndpoint,
            [this](const std::error_code& error, std::size_t bytesReceived)
            {
                HandleReceive(error, bytesReceived);
            });
    }

    void UDPTransport::HandleReceive(const std::error_code& error, std::size_t bytesReceived)
    {
        if (!m_active)
            return;

        if (!error && bytesReceived > 0)
        {
            ReceivedPacket packet;
            packet.source.host = m_impl->remoteEndpoint.address().to_string();
            packet.source.port = m_impl->remoteEndpoint.port();
            packet.source.isIPv6 = m_impl->remoteEndpoint.address().is_v6();
            packet.receiveTime = std::chrono::steady_clock::now();
            packet.data.assign(
                m_impl->receiveBuffer.begin(),
                m_impl->receiveBuffer.begin() + bytesReceived);

            // Queue the packet
            {
                std::lock_guard<std::mutex> lock(m_receiveMutex);
                m_receiveQueue.push(std::move(packet));
            }

            // Invoke callback if set
            if (m_packetCallback)
            {
                std::lock_guard<std::mutex> lock(m_receiveMutex);
                if (!m_receiveQueue.empty())
                {
                    m_packetCallback(m_receiveQueue.back());
                }
            }
        }
        else if (error && error != asio::error::operation_aborted)
        {
            RVX_CORE_WARN("Receive error: {}", error.message());
        }

        // Continue receiving
        StartReceive();
    }

    // =========================================================================
    // Factory Function
    // =========================================================================

    TransportPtr CreateUDPTransport()
    {
        return std::make_shared<UDPTransport>();
    }

} // namespace RVX::Net
