#pragma once

#include <cstdint>
#include <atomic>
#include <chrono>
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
        [[nodiscard]] std::uint64_t GetAccountId() const noexcept;
        void SetPlayerId(std::uint64_t inPlayerId) noexcept;

    private:
        friend class Domain::TownInstance;
        enum class LoginState { Unauthenticated, ResolvingAccount, Authenticated, Closed };
        // Admission start/complete belong to the town strand; close invalidates them from any strand.
        std::uint64_t BeginAuthentication() noexcept;
        bool CompleteAuthentication(std::uint64_t inAttemptId, std::uint64_t inAccountId) noexcept;
        [[nodiscard]] bool MatchesAuthentication(std::uint64_t inAttemptId) const noexcept;
        void CloseAuthentication() noexcept;
        void SetAdmissionDeadline(std::chrono::steady_clock::time_point inDeadline) noexcept;
        void HandlePacket(std::vector<std::uint8_t> inPacket);

        std::shared_ptr<ActionRPG::Network::TcpSession> tcpSession;
        std::weak_ptr<Domain::TownInstance> townInstance;
        std::weak_ptr<RoomControlTcpServer> roomControlServer;
        std::atomic_uint64_t playerId{};
        std::atomic_uint32_t characterId{};
        std::atomic_uint64_t accountId{};
        std::atomic_uint64_t loginAttemptId{};
        std::atomic_int64_t admissionDeadline{};
        std::atomic<LoginState> loginState{ LoginState::Unauthenticated };
        std::atomic_bool enterRequested{ false };
    };
}
