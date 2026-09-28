#include "TownInstance.h"

#include "PlayerSession.h"
#include "Protocol.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <utility>

namespace
{
    constexpr float TICK_SECONDS = 1.0f / 20.0f;
    constexpr std::uint32_t SNAPSHOT_TICK_INTERVAL = 2;
}

namespace TownServer::Domain
{
    TownInstance::TownInstance(asio::io_context& inIoContext, TownMap inMap)
        : strand(asio::make_strand(inIoContext)),
          tickTimer(strand),
          map(std::move(inMap))
    {
    }

    void TownInstance::Start()
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self]()
        {
            if (self->running)
            {
                return;
            }
            self->running = true;
            self->ScheduleTick();
        });
    }

    void TownInstance::Stop()
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self]()
        {
            self->running = false;
            asio::error_code ignoredError;
            self->tickTimer.cancel(ignoredError);
        });
    }

    void TownInstance::Enter(std::shared_ptr<Network::PlayerSession> inSession, std::string inPlayerName)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, session = std::move(inSession), playerName = std::move(inPlayerName)]() mutable
        {
            self->EnterOnStrand(std::move(session), std::move(playerName));
        });
    }

    void TownInstance::Leave(const std::uint64_t inSessionId)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId]()
        {
            self->LeaveOnStrand(inSessionId);
        });
    }

    void TownInstance::ApplyMovementInput(const std::uint64_t inSessionId, const TownProtocol::MoveInput inInput)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, inInput]()
        {
            const auto sessionIterator = self->sessionToPlayer.find(inSessionId);
            if (sessionIterator == self->sessionToPlayer.end())
            {
                return;
            }

            const auto playerIterator = self->players.find(sessionIterator->second);
            if (playerIterator != self->players.end() && playerIterator->second.dungeonRoomId == 0)
            {
                playerIterator->second.player.SetMovementInput(inInput, std::chrono::steady_clock::now());
            }
        });
    }

    void TownInstance::EnterDungeon(
        const PlayerId inPlayerId,
        const ActionRPG::RoomControlProtocol::RoomId inRoomId)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, inRoomId]()
        {
            const auto iterator = self->players.find(inPlayerId);
            if (iterator == self->players.end() || iterator->second.dungeonRoomId != 0)
            {
                return;
            }
            self->HideFromTown(inPlayerId, iterator->second);
            iterator->second.dungeonRoomId = inRoomId;
            iterator->second.player.StopMovement();
        });
    }

    void TownInstance::LeaveDungeon(
        const PlayerId inPlayerId,
        const ActionRPG::RoomControlProtocol::RoomId inRoomId)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, inRoomId]()
        {
            const auto iterator = self->players.find(inPlayerId);
            if (iterator == self->players.end() || iterator->second.dungeonRoomId != inRoomId)
            {
                return;
            }
            iterator->second.dungeonRoomId = 0;
            iterator->second.sector = self->GetSector(iterator->second.player.GetPosition());
            self->AddToSector(inPlayerId, iterator->second.sector);
            self->RefreshVisibility(inPlayerId);
        });
    }

    void TownInstance::HandleRoomEnded(ActionRPG::RoomControlProtocol::RoomEnded inRoomEnded)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, roomEnded = std::move(inRoomEnded)]()
        {
            if (roomEnded.reason != ActionRPG::RoomControlProtocol::RoomEndReason::Cleared)
            {
                return;
            }
            for (const PlayerId playerId : roomEnded.rewardPlayerIds)
            {
                const auto iterator = self->players.find(playerId);
                if (iterator != self->players.end() && iterator->second.dungeonRoomId == roomEnded.roomId)
                {
                    std::cout << "Reward target confirmed: room=" << roomEnded.roomId
                        << ", player=" << playerId << '\n';
                }
            }
        });
    }

    std::size_t TownInstance::SectorHash::operator()(const SectorCoordinate& inCoordinate) const noexcept
    {
        const std::uint64_t x = static_cast<std::uint32_t>(inCoordinate.x);
        const std::uint64_t y = static_cast<std::uint32_t>(inCoordinate.y);
        return static_cast<std::size_t>((x << 32) ^ y);
    }

    void TownInstance::ScheduleTick()
    {
        tickTimer.expires_after(std::chrono::milliseconds(50));
        const std::shared_ptr<TownInstance> self = shared_from_this();
        tickTimer.async_wait([self](const asio::error_code& inError)
        {
            if (!inError && self->running)
            {
                self->Tick();
                self->ScheduleTick();
            }
        });
    }

    void TownInstance::Tick()
    {
        ++serverTick;
        const auto now = std::chrono::steady_clock::now();
        std::vector<PlayerId> changedSectorPlayers;
        const TownProtocol::MapInfo& mapInfo = map.GetInfo();

        for (auto& [playerId, entry] : players)
        {
            if (entry.dungeonRoomId != 0)
            {
                continue;
            }
            const TownProtocol::Vector2 previousPosition = entry.player.GetPosition();
            entry.player.Simulate(TICK_SECONDS, mapInfo.walkSpeed, now);
            const TownProtocol::Vector2 proposedPosition = entry.player.GetPosition();
            const TownProtocol::Vector2 constrainedPosition = map.ConstrainMovement(
                previousPosition, proposedPosition);
            entry.player.SetPosition(constrainedPosition);
            if (constrainedPosition.x != proposedPosition.x || constrainedPosition.y != proposedPosition.y)
            {
                entry.player.StopMovement();
            }

            const SectorCoordinate newSector = GetSector(entry.player.GetPosition());
            if (newSector != entry.sector)
            {
                RemoveFromSector(playerId, entry.sector);
                entry.sector = newSector;
                AddToSector(playerId, entry.sector);
                changedSectorPlayers.push_back(playerId);
            }
        }

        for (const PlayerId playerId : changedSectorPlayers)
        {
            RefreshVisibility(playerId);
        }

        if (serverTick % SNAPSHOT_TICK_INTERVAL == 0)
        {
            BroadcastMovement();
        }
    }

    void TownInstance::EnterOnStrand(std::shared_ptr<Network::PlayerSession> inSession, std::string inPlayerName)
    {
        const std::uint64_t sessionId = inSession->GetSessionId();
        if (sessionToPlayer.contains(sessionId))
        {
            return;
        }

        const PlayerId playerId = nextPlayerId++;
        const TownProtocol::MapInfo& mapInfo = map.GetInfo();
        const TownProtocol::Vector2 spawn{ mapInfo.spawnX, mapInfo.spawnY };
        const SectorCoordinate sector = GetSector(spawn);

        PlayerEntry entry{
            Player(playerId, std::move(inPlayerName), spawn),
            std::move(inSession),
            sector,
            {},
            spawn,
            false,
            0
        };
        players.emplace(playerId, std::move(entry));
        sessionToPlayer.emplace(sessionId, playerId);
        AddToSector(playerId, sector);

        PlayerEntry& playerEntry = players.at(playerId);
        playerEntry.session->SetPlayerId(playerId);
        playerEntry.session->Send(TownProtocol::Encode(TownProtocol::EnterTownResponse{ playerId, mapInfo }));
        RefreshVisibility(playerId);
    }

    void TownInstance::LeaveOnStrand(const std::uint64_t inSessionId)
    {
        const auto sessionIterator = sessionToPlayer.find(inSessionId);
        if (sessionIterator == sessionToPlayer.end())
        {
            return;
        }

        const PlayerId playerId = sessionIterator->second;
        auto playerIterator = players.find(playerId);
        if (playerIterator == players.end())
        {
            sessionToPlayer.erase(sessionIterator);
            return;
        }

        if (playerIterator->second.dungeonRoomId == 0)
        {
            HideFromTown(playerId, playerIterator->second);
        }
        players.erase(playerIterator);
        sessionToPlayer.erase(sessionIterator);
    }

    void TownInstance::HideFromTown(const PlayerId inPlayerId, PlayerEntry& inEntry)
    {
        const std::vector<PlayerId> visiblePlayers(
            inEntry.visiblePlayers.begin(), inEntry.visiblePlayers.end());
        for (const PlayerId visibleId : visiblePlayers)
        {
            const auto visibleIterator = players.find(visibleId);
            if (visibleIterator != players.end())
            {
                visibleIterator->second.visiblePlayers.erase(inPlayerId);
                SendDisappear(visibleIterator->second, inPlayerId);
            }
        }
        inEntry.visiblePlayers.clear();
        RemoveFromSector(inPlayerId, inEntry.sector);
    }

    void TownInstance::RefreshVisibility(const PlayerId inPlayerId)
    {
        auto playerIterator = players.find(inPlayerId);
        if (playerIterator == players.end())
        {
            return;
        }

        PlayerEntry& entry = playerIterator->second;
        const std::unordered_set<PlayerId> newVisiblePlayers = FindVisiblePlayers(inPlayerId);
        const std::vector<PlayerId> oldVisiblePlayers(entry.visiblePlayers.begin(), entry.visiblePlayers.end());

        for (const PlayerId previousId : oldVisiblePlayers)
        {
            if (newVisiblePlayers.contains(previousId))
            {
                continue;
            }

            entry.visiblePlayers.erase(previousId);
            SendDisappear(entry, previousId);
            if (auto previousIterator = players.find(previousId); previousIterator != players.end())
            {
                previousIterator->second.visiblePlayers.erase(inPlayerId);
                SendDisappear(previousIterator->second, inPlayerId);
            }
        }

        for (const PlayerId newId : newVisiblePlayers)
        {
            if (entry.visiblePlayers.contains(newId))
            {
                continue;
            }

            auto newIterator = players.find(newId);
            if (newIterator == players.end())
            {
                continue;
            }

            entry.visiblePlayers.insert(newId);
            newIterator->second.visiblePlayers.insert(inPlayerId);
            SendAppear(entry, newIterator->second);
            SendAppear(newIterator->second, entry);
        }
    }

    void TownInstance::BroadcastMovement()
    {
        for (auto& [playerId, entry] : players)
        {
            if (entry.dungeonRoomId != 0)
            {
                continue;
            }
            const TownProtocol::Vector2 position = entry.player.GetPosition();
            const TownProtocol::Vector2 velocity = entry.player.GetVelocity();
            const bool isMoving = velocity.x != 0.0f || velocity.y != 0.0f;
            const bool positionChanged = position.x != entry.lastBroadcastPosition.x
                || position.y != entry.lastBroadcastPosition.y;
            if (!isMoving && !entry.wasMovingOnLastBroadcast && !positionChanged)
            {
                continue;
            }

            const TownProtocol::PlayerMove movement{
                playerId,
                serverTick,
                entry.player.GetLastProcessedInput(),
                position,
                velocity
            };
            const std::vector<std::uint8_t> encoded = TownProtocol::Encode(movement);
            entry.session->Send(encoded);

            for (const PlayerId observerId : entry.visiblePlayers)
            {
                if (auto observerIterator = players.find(observerId); observerIterator != players.end())
                {
                    observerIterator->second.session->Send(encoded);
                }
            }

            entry.lastBroadcastPosition = position;
            entry.wasMovingOnLastBroadcast = isMoving;
        }
    }

    void TownInstance::AddToSector(const PlayerId inPlayerId, const SectorCoordinate inSector)
    {
        sectors[inSector].insert(inPlayerId);
    }

    void TownInstance::RemoveFromSector(const PlayerId inPlayerId, const SectorCoordinate inSector)
    {
        const auto iterator = sectors.find(inSector);
        if (iterator == sectors.end())
        {
            return;
        }
        iterator->second.erase(inPlayerId);
        if (iterator->second.empty())
        {
            sectors.erase(iterator);
        }
    }

    void TownInstance::SendAppear(PlayerEntry& inReceiver, const PlayerEntry& inSubject)
    {
        inReceiver.session->Send(TownProtocol::Encode(TownProtocol::PlayerAppear{
            inSubject.player.GetId(),
            inSubject.player.GetName(),
            inSubject.player.GetPosition(),
            inSubject.player.GetVelocity()
        }));
    }

    void TownInstance::SendDisappear(PlayerEntry& inReceiver, const PlayerId inSubjectId)
    {
        inReceiver.session->Send(TownProtocol::Encode(TownProtocol::PlayerDisappear{ inSubjectId }));
    }

    TownInstance::SectorCoordinate TownInstance::GetSector(const TownProtocol::Vector2 inPosition) const noexcept
    {
        const TownProtocol::MapInfo& mapInfo = map.GetInfo();
        return SectorCoordinate{
            static_cast<int>(std::floor((inPosition.x - mapInfo.worldLeft) / mapInfo.sectorWidth)),
            static_cast<int>(std::floor((inPosition.y - mapInfo.worldTop) / mapInfo.sectorHeight))
        };
    }

    std::unordered_set<PlayerId> TownInstance::FindVisiblePlayers(const PlayerId inPlayerId) const
    {
        std::unordered_set<PlayerId> result;
        const auto playerIterator = players.find(inPlayerId);
        if (playerIterator == players.end() || playerIterator->second.dungeonRoomId != 0)
        {
            return result;
        }

        const SectorCoordinate center = playerIterator->second.sector;
        for (int y = center.y - 1; y <= center.y + 1; ++y)
        {
            for (int x = center.x - 1; x <= center.x + 1; ++x)
            {
                const auto sectorIterator = sectors.find(SectorCoordinate{ x, y });
                if (sectorIterator == sectors.end())
                {
                    continue;
                }
                for (const PlayerId candidateId : sectorIterator->second)
                {
                    const auto candidate = players.find(candidateId);
                    if (candidate != players.end() && candidate->second.dungeonRoomId == 0)
                    {
                        result.insert(candidateId);
                    }
                }
            }
        }
        result.erase(inPlayerId);
        return result;
    }
}
