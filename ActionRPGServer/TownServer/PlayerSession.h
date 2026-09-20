#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace TownServer::Domain
{
    class TownInstance;
}

namespace TownServer::Network
{
    class TcpSession;

    class PlayerSession final : public std::enable_shared_from_this<PlayerSession>
    {
    public:
        PlayerSession(std::shared_ptr<TcpSession> inTcpSession,
            std::weak_ptr<Domain::TownInstance> inTownInstance);

        void Start();
        void Stop();
        void Disconnect();
        void Send(std::vector<std::uint8_t> inPacket);

        [[nodiscard]] std::uint64_t GetSessionId() const noexcept;

    private:
        void HandlePacket(std::vector<std::uint8_t> inPacket);

        std::shared_ptr<TcpSession> tcpSession;
        std::weak_ptr<Domain::TownInstance> townInstance;
        bool enterRequested = false;
    };
}
