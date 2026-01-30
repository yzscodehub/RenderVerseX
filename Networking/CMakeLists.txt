# =============================================================================
# Networking Module - Network communication and replication
# =============================================================================
add_library(RVX_Networking STATIC)

target_sources(RVX_Networking PRIVATE
    # Core
    Private/NetworkManager.cpp
    Private/NetworkSubsystem.cpp
    Private/Connection.cpp
    
    # Serialization
    Private/Serialization/BitStream.cpp
    Private/Serialization/NetworkSerializer.cpp
    
    # Transport
    Private/Transport/UDPTransport.cpp
    Private/Transport/ReliableUDP.cpp
    
    # Replication
    Private/Replication/ReplicatedObject.cpp
    Private/Replication/PropertyReplication.cpp
)

target_include_directories(RVX_Networking PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

# Find ASIO (standalone, header-only)
find_package(asio CONFIG REQUIRED)

target_link_libraries(RVX_Networking PUBLIC
    RVX::Core
)

target_link_libraries(RVX_Networking PRIVATE
    asio::asio
)

# Platform-specific network libraries
if(WIN32)
    # Windows Sockets
    target_link_libraries(RVX_Networking PRIVATE
        ws2_32
        wsock32
    )
elseif(UNIX)
    # Linux/macOS: pthread for async operations
    find_package(Threads REQUIRED)
    target_link_libraries(RVX_Networking PRIVATE
        Threads::Threads
    )
endif()

add_library(RVX::Networking ALIAS RVX_Networking)
