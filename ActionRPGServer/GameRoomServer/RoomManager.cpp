#include "RoomManager.h"

#include "DungeonSession.h"
#include "GameRoom.h"

#include <chrono>
#include <utility>

namespace GameRoomServer
{
    RoomManager::RoomManager(
        asio::io_context& inIoContext,
        const Protocol::RoomServerId inRoomServerId,
        const std::uint32_t inMaxRoomCount,
        std::string inSessionBrokerAddress,
        const std::uint16_t inSessionBrokerPort,
        const std::chrono::milliseconds inEnterTimeout,
        std::unordered_map<std::uint32_t, std::shared_ptr<const DungeonDefinition>> inDefinitions,
        std::shared_ptr<const CombatDefinition> inCombatDefinition)
        : strand(asio::make_strand(inIoContext)),
          ioContext(inIoContext),
          definitions(std::move(inDefinitions)),
          combatDefinition(std::move(inCombatDefinition)),
          maxRoomCount(inMaxRoomCount),
          roomServerId(inRoomServerId),
          sessionBrokerAddress(std::move(inSessionBrokerAddress)),
          sessionBrokerPort(inSessionBrokerPort),
          enterTimeout(inEnterTimeout),
          randomState(static_cast<std::uint64_t>(
              std::chrono::steady_clock::now().time_since_epoch().count()))
    {
        rooms.reserve(maxRoomCount);
    }

    void RoomManager::SetSendHandler(SendHandler inSendHandler)
    {
        const std::shared_ptr<RoomManager> self = shared_from_this();
        asio::dispatch(strand, [self, sendHandler = std::move(inSendHandler)]() mutable
        {
            self->sendHandler = std::move(sendHandler);
        });
    }

    void RoomManager::CreateRoom(
        Protocol::CreateRoom inRequest,
        CreateResultHandler inResultHandler, const Protocol::RoomId inReplacingRoomId)
    {
        const std::shared_ptr<RoomManager> self = shared_from_this();
        asio::dispatch(strand,
            [self, request = std::move(inRequest), resultHandler = std::move(inResultHandler), inReplacingRoomId]() mutable
            {
                Protocol::CreateRoomResult result;
                result.requestId = request.requestId;
                const auto definition = self->definitions.find(request.dungeonId);
                const std::unordered_set<Protocol::PlayerId> participantIds(
                    request.participantPlayerIds.begin(), request.participantPlayerIds.end());
                if ((self->rooms.size() >= self->maxRoomCount && !self->rooms.contains(inReplacingRoomId)) || definition == self->definitions.end()
                    || request.participantPlayerIds.empty()
                    || participantIds.contains(0) || participantIds.size() != request.participantPlayerIds.size()
                    || request.participantPlayerIds.size() > definition->second->maxPlayers)
                {
                    if (resultHandler)
                    {
                        resultHandler(std::move(result));
                    }
                    return;
                }

                const Protocol::RoomId roomId = (self->roomServerId << 32) | self->nextRoomId++;
                const std::uint64_t combatSeed = self->GenerateSeed();
                const std::weak_ptr<RoomManager> weakSelf = self;
                std::shared_ptr<GameRoom> room = std::make_shared<GameRoom>(
                    self->ioContext,
                    roomId,
                    request.dungeonId,
                    combatSeed,
                    std::move(request.participantPlayerIds),
                    self->enterTimeout,
                    [weakSelf](const Protocol::RoomId inRoomId)
                    {
                        if (const std::shared_ptr<RoomManager> manager = weakSelf.lock())
                        {
                            manager->RemoveRoom(inRoomId, true);
                        }
                    }, definition->second, self->combatDefinition,
                    [weakSelf](const Protocol::RoomId inRoomId)
                    {
                        if (const auto manager = weakSelf.lock()) manager->CompleteDungeon(inRoomId);
                    });
                self->rooms.emplace(roomId, room);
                room->Start();

                result.succeeded = true;
                result.roomId = roomId;
                result.combatSeed = combatSeed;
                result.sessionBrokerAddress = self->sessionBrokerAddress;
                result.sessionBrokerPort = self->sessionBrokerPort;
                if (resultHandler)
                {
                    resultHandler(std::move(result));
                }
            });
    }

    // Keep the cleared room until fresh-room creation succeeds. All manager mutations use its strand.
    void RoomManager::FinishRoom(Protocol::FinishRoom inRequest,
        std::function<void(Protocol::FinishRoomResult)> inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, request = std::move(inRequest), handler = std::move(inHandler)]() mutable
        {
            const auto found = self->rooms.find(request.roomId);
            if (found == self->rooms.end() || !self->endedRooms.contains(request.roomId))
            {
                handler(Protocol::FinishRoomResult{request.requestId, request.roomId});
                return;
            }
            const auto room = found->second;
            const auto participants = request.participantPlayerIds;
            room->ValidateCompletion(participants,
                [self, room, request = std::move(request), handler = std::move(handler)](const bool inValid) mutable
                {
                    asio::dispatch(self->strand,
                        [self, room, request = std::move(request), handler = std::move(handler), inValid]() mutable
                        {
                            if (!inValid || !self->rooms.contains(request.roomId)
                                || self->rooms.at(request.roomId) != room || !self->endedRooms.contains(request.roomId))
                            {
                                handler(Protocol::FinishRoomResult{request.requestId, request.roomId});
                                return;
                            }
                            auto complete = [self, requestId = request.requestId, oldRoomId = request.roomId,
                                handler = std::move(handler)](Protocol::CreateRoomResult inResult) mutable
                            {
                                if (inResult.succeeded) self->RemoveRoom(oldRoomId, false);
                                handler(Protocol::FinishRoomResult{requestId, oldRoomId, inResult.succeeded,
                                    inResult.roomId, inResult.combatSeed, std::move(inResult.sessionBrokerAddress),
                                    inResult.sessionBrokerPort});
                            };
                            if (request.retry)
                                self->CreateRoom(Protocol::CreateRoom{request.requestId, room->GetDungeonId(),
                                    std::move(request.participantPlayerIds)}, std::move(complete), request.roomId);
                            else
                            {
                                Protocol::CreateRoomResult result;
                                result.succeeded = true;
                                complete(std::move(result));
                            }
                        });
                });
        });
    }

    void RoomManager::RegisterChallenge(
        const std::uint64_t inChallenge,
        DungeonSession* const inSession,
        const std::uint32_t inSessionGeneration)
    {
        const std::shared_ptr<RoomManager> self = shared_from_this();
        asio::dispatch(strand, [self, inChallenge, inSession, inSessionGeneration]()
        {
            if (inChallenge != 0 && inSession != nullptr)
            {
                self->pendingSessions[inChallenge] = PendingSession{ inSession, inSessionGeneration };
            }
        });
    }

    void RoomManager::ConfirmJoin(Protocol::ConfirmJoin inRequest)
    {
        const std::shared_ptr<RoomManager> self = shared_from_this();
        asio::dispatch(strand, [self, request = std::move(inRequest)]()
        {
            const auto pendingIterator = self->pendingSessions.find(request.challenge);
            const auto roomIterator = self->rooms.find(request.roomId);
            if (pendingIterator == self->pendingSessions.end() || roomIterator == self->rooms.end())
            {
                return;
            }

            DungeonSession* const session = pendingIterator->second.session;
            const std::uint32_t generation = pendingIterator->second.generation;
            self->pendingSessions.erase(pendingIterator);
            const std::weak_ptr<RoomManager> weakSelf = self;
            roomIterator->second->TryEnter(request.playerId, request.characterId,
                [weakSelf, request, session, generation](const bool inAccepted)
                {
                    if (const std::shared_ptr<RoomManager> manager = weakSelf.lock())
                    {
                        manager->CompleteJoin(request.roomId, request.playerId, request.challenge,
                            session, generation, inAccepted);
                    }
                });
        });
    }

    void RoomManager::SessionDisconnected(
        const std::uint64_t inChallenge,
        DungeonSession* const inSession,
        const std::uint32_t inSessionGeneration,
        const Protocol::RoomId inRoomId,
        const Protocol::PlayerId inPlayerId)
    {
        const std::shared_ptr<RoomManager> self = shared_from_this();
        asio::dispatch(strand,
            [self, inChallenge, inSession, inSessionGeneration, inRoomId, inPlayerId]()
            {
                const auto pendingIterator = self->pendingSessions.find(inChallenge);
                if (pendingIterator != self->pendingSessions.end()
                    && pendingIterator->second.session == inSession
                    && pendingIterator->second.generation == inSessionGeneration)
                {
                    self->pendingSessions.erase(pendingIterator);
                }

                if (inRoomId == 0 || inPlayerId == 0)
                {
                    return;
                }
                const auto roomIterator = self->rooms.find(inRoomId);
                if (roomIterator == self->rooms.end())
                {
                    return;
                }
                const std::weak_ptr<RoomManager> weakSelf = self;
                roomIterator->second->Leave(inPlayerId,
                    [weakSelf, inRoomId, inPlayerId](const bool inRemoved, const bool inRoomEmpty)
                    {
                        if (const std::shared_ptr<RoomManager> manager = weakSelf.lock())
                        {
                            asio::dispatch(manager->strand,
                                [manager, inRoomId, inPlayerId, inRemoved, inRoomEmpty]()
                                {
                                    if (inRemoved)
                                    {
                                        manager->Send(Protocol::Encode(
                                            Protocol::LeaveRoom{ inRoomId, inPlayerId }));
                                    }
                                    if (inRoomEmpty)
                                    {
                                        manager->RemoveRoom(inRoomId, true);
                                    }
                                });
                        }
                    });
            });
    }

    void RoomManager::CompleteDungeon(const Protocol::RoomId inRoomId)
    {
        const std::shared_ptr<RoomManager> self = shared_from_this();
        asio::dispatch(strand, [self, inRoomId]()
        {
            const auto iterator = self->rooms.find(inRoomId);
            if (iterator == self->rooms.end())
            {
                return;
            }
            const std::weak_ptr<RoomManager> weakSelf = self;
            iterator->second->CompleteDungeon([weakSelf, inRoomId](std::vector<Protocol::PlayerId> inPlayers)
            {
                if (const std::shared_ptr<RoomManager> manager = weakSelf.lock())
                {
                    asio::dispatch(manager->strand,
                        [manager, inRoomId, players = std::move(inPlayers)]() mutable
                        {
                            manager->endedRooms.insert(inRoomId);
                            manager->Send(Protocol::Encode(Protocol::RoomEnded{
                                inRoomId, Protocol::RoomEndReason::Cleared, std::move(players)
                            }));
                        });
                }
            });
        });
    }

    void RoomManager::Stop()
    {
        const std::shared_ptr<RoomManager> self = shared_from_this();
        asio::dispatch(strand, [self]()
        {
            for (const auto& [roomId, room] : self->rooms)
            {
                room->Stop();
            }
            self->rooms.clear();
            self->endedRooms.clear();
            self->pendingSessions.clear();
        });
    }

    void RoomManager::CompleteJoin(
        const Protocol::RoomId inRoomId,
        const Protocol::PlayerId inPlayerId,
        const std::uint64_t inChallenge,
        DungeonSession* const inSession,
        const std::uint32_t inGeneration,
        const bool inRoomAccepted)
    {
        const std::shared_ptr<RoomManager> self = shared_from_this();
        asio::dispatch(strand,
            [self, inRoomId, inPlayerId, inChallenge, inSession, inGeneration, inRoomAccepted]()
            {
                const auto roomIterator = self->rooms.find(inRoomId);
                if (!inRoomAccepted || roomIterator == self->rooms.end()
                    || !inSession->ConfirmAuthentication(
                        inRoomId, inPlayerId, inChallenge, inGeneration, roomIterator->second))
                {
                    if (roomIterator != self->rooms.end() && inRoomAccepted)
                    {
                        const std::weak_ptr<RoomManager> weakSelf = self;
                        roomIterator->second->RemoveUnannouncedPlayer(inPlayerId,
                            [weakSelf, inRoomId](const bool inRoomEmpty)
                            {
                                if (inRoomEmpty)
                                {
                                    if (const std::shared_ptr<RoomManager> manager = weakSelf.lock())
                                    {
                                        manager->RemoveRoom(inRoomId, true);
                                    }
                                }
                            });
                    }
                    return;
                }
                self->Send(Protocol::Encode(Protocol::EnterRoom{ inRoomId, inPlayerId }));
            });
    }

    void RoomManager::RemoveRoom(const Protocol::RoomId inRoomId, const bool inAborted)
    {
        const std::shared_ptr<RoomManager> self = shared_from_this();
        asio::dispatch(strand, [self, inRoomId, inAborted]()
        {
            const auto iterator = self->rooms.find(inRoomId);
            if (iterator == self->rooms.end())
            {
                return;
            }
            iterator->second->Stop();
            self->rooms.erase(iterator);
            const bool wasEnded = self->endedRooms.erase(inRoomId) > 0;
            if (inAborted && !wasEnded)
            {
                self->Send(Protocol::Encode(Protocol::RoomEnded{
                    inRoomId, Protocol::RoomEndReason::Aborted, {}
                }));
            }
        });
    }

    void RoomManager::Send(std::vector<std::uint8_t> inPacket)
    {
        if (sendHandler)
        {
            sendHandler(std::move(inPacket));
        }
    }

    std::uint64_t RoomManager::GenerateSeed()
    {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 7;
        randomState ^= randomState << 17;
        return randomState == 0 ? 1 : randomState;
    }
}
