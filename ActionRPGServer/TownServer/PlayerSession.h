#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <vector>

namespace ActionRPG::Network
{
    class TcpSession;
}

namespace TownServer::Domain
{
    class TownInstance;
}

namespace TownServer::Network
{
    class RoomControlTcpServer;

    class PlayerSession final : public std::enable_shared_from_this<PlayerSession>
    {
    public:
        PlayerSession(std::shared_ptr<ActionRPG::Network::TcpSession> inTcpSession,
            std::weak_ptr<Domain::TownInstance> inTownInstance,
            std::weak_ptr<RoomControlTcpServer> inRoomControlServer);

        void Start();
        void Stop();
        void Disconnect();
        void Send(std::vector<std::uint8_t> inPacket);

        [[nodiscard]] std::uint64_t GetSessionId() const noexcept;
        [[nodiscard]] std::uint64_t GetPlayerId() const noexcept;
        void SetPlayerId(std::uint64_t inPlayerId) noexcept;

    private:
        void HandlePacket(std::vector<std::uint8_t> inPacket);

        std::shared_ptr<ActionRPG::Network::TcpSession> tcpSession;
        std::weak_ptr<Domain::TownInstance> townInstance;
        std::weak_ptr<RoomControlTcpServer> roomControlServer;
        std::atomic_uint64_t playerId{};
        std::atomic_uint32_t characterId{};
        bool enterRequested = false;
    };
}
