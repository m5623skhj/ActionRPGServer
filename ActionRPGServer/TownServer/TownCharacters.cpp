#include "TownInstance.h"
#include "PlayerSession.h"
#include <openssl/rand.h>

namespace TownServer::Domain
{
    namespace
    {
        using Json = ActionRPG::Items::Json;
        using Rules = ActionRPG::PlayerSkills::Catalog;

        std::string OwnerToken()
        {
            std::array<unsigned char, 32> bytes{};
            if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
                throw std::runtime_error("Unable to create character ownership token.");
            constexpr char HEX[] = "0123456789abcdef";
            std::string result;
            for (const auto value : bytes) { result += HEX[value >> 4]; result += HEX[value & 15]; }
            return result;
        }

        void SendCharacterResult(const std::shared_ptr<Network::PlayerSession>& inSession,
            TownProtocol::PacketType inType, Json inValue)
        {
            const auto data = inValue.dump();
            if (inType == TownProtocol::PacketType::CharacterListRequest)
                inSession->Send(TownProtocol::Encode(TownProtocol::CharacterListResponse{data}));
            else if (inType == TownProtocol::PacketType::CharacterCreateRequest)
                inSession->Send(TownProtocol::Encode(TownProtocol::CharacterCreateResponse{data}));
            else inSession->Send(TownProtocol::Encode(TownProtocol::CharacterSelectResponse{data}));
        }
    }

    void TownInstance::CharacterRequest(std::shared_ptr<Network::PlayerSession> inSession,
        TownProtocol::PacketType inType, std::string inJson)
    {
        const auto self = shared_from_this();
        asio::post(strand, [self, session = std::move(inSession), inType, data = std::move(inJson)]()
        {
            const auto accountId = session->GetAccountId();
            const auto sessionId = session->GetSessionId();
            if (!self->running || accountId == 0 || session->GetPlayerId() != 0) { session->Stop(); return; }
            std::string requestId;
            Persistence::Request request;
            try
            {
                const auto body = Json::parse(data);
                requestId = body.at("requestId").get<std::string>();
                Rules::Require(ActionRPG::Items::IsHexId(requestId, 64), "Invalid character request ID.");
                request.accountId = accountId; request.requestId = requestId;
                if (inType == TownProtocol::PacketType::CharacterListRequest)
                {
                    Rules::Keys(body, {"requestId"}); request.operation = Persistence::Operation::List;
                }
                else if (inType == TownProtocol::PacketType::CharacterCreateRequest)
                {
                    Rules::Keys(body, {"requestId", "name", "characterDefinitionId"});
                    request.operation = Persistence::Operation::Create;
                    request.name = body.at("name").get<std::string>();
                    request.definitionId = Rules::Integer(body.at("characterDefinitionId"), 1, 1000000);
                    Rules::Require(!request.name.empty() && request.name.size() <= 32
                        && request.name.find('\0') == std::string::npos
                        && self->playerSkills.characterIds.contains("Character" + std::to_string(request.definitionId)),
                        "Invalid character definition/name.");
                    (void)Persistence::Wide(request.name);
                    request.initialLevel = self->progressionPolicy.initialLevel;
                    request.initialSp = self->progressionPolicy.initialSkillPoints;
                }
                else
                {
                    Rules::Keys(body, {"requestId", "characterId", "expectedOwnerGeneration"});
                    request.operation = Persistence::Operation::Claim;
                    request.characterId = ActionRPG::Items::DecimalId(body.at("characterId"));
                    request.generation = ActionRPG::Items::DecimalId(body.at("expectedOwnerGeneration"), true);
                    request.ownerToken = OwnerToken();
                }
            }
            catch (...) { session->Stop(); return; }
            if (!self->characterRequests.emplace(sessionId).second)
            {
                SendCharacterResult(session, inType, {{"requestId", requestId}, {"result", "Busy"},
                    {"characterId", "0"}, {"revision", "0"}});
                return;
            }
            ++self->characterWork[sessionId];
            auto procedure = std::make_unique<Persistence::CharacterStoreProcedure>(); procedure->req = request;
            self->RunStoreProcedure(std::move(procedure),
                [weakSession = std::weak_ptr<Network::PlayerSession>(session), sessionId, accountId, requestId,
                    request, inType](TownInstance& town, auto result)
            {
                const auto session = weakSession.lock();
                const bool current = town.running && session && session->GetAccountId() == accountId
                    && session->GetPlayerId() == 0;
                if (request.operation == Persistence::Operation::Claim && !result.error && result.response
                    && (result.response->states.front().result == 0 || result.response->states.front().result == 7))
                {
                    const auto& state = result.response->states.front();
                    bool entered = false;
                    if (current)
                    {
                        Persistence::ItemUseRequest identity;
                        identity.accountId = accountId; identity.characterId = state.characterId;
                        identity.generation = state.generation; identity.ownerToken = request.ownerToken;
                        auto service = std::make_shared<ItemUseService>(town.shared_from_this(), session, identity, *result.response);
                        town.itemUseServices[sessionId] = service;
                        if (result.response->use->state == 1 || !result.response->use->pendingRequestId.empty())
                        {
                            auto release = request; release.operation = Persistence::Operation::Release;
                            release.generation = state.generation;
                            town.characterReleases[sessionId] = std::move(release);
                            service->RecoverClaim(requestId); entered = true;
                        }
                        else
                        {
                            town.EnterOnStrand(session, state, request.ownerToken, requestId);
                            entered = session->GetPlayerId() != 0;
                            if (entered)
                            {
                                auto& entry = town.players.at(session->GetPlayerId());
                                entry.cooldownsComplete = true; entry.itemCooldowns = Persistence::CooldownsJson(*result.response);
                                // Recreate with the entered runtime PlayerId.
                                town.itemUseServices[sessionId] = std::make_shared<ItemUseService>(town.shared_from_this(), session,
                                    identity, *result.response);
                            }
                        }
                    }
                    if (!entered)
                    {
                        auto release = request; release.operation = Persistence::Operation::Release;
                        release.generation = state.generation;
                        town.characterReleases[sessionId] = std::move(release);
                        if (session) session->Stop();
                    }
                }
                else if (current)
                {
                    Json reply{{"requestId", requestId}, {"result", "DatabaseUnavailable"},
                        {"characterId", "0"}, {"revision", "0"}};
                    if (result.response && !result.error)
                    {
                        const auto& state = result.response->states.front();
                        reply["result"] = Persistence::ResultName(state.result);
                        reply["characterId"] = std::to_string(state.characterId);
                        reply["revision"] = std::to_string(state.revision);
                        if (request.operation == Persistence::Operation::List && state.result == 0)
                        {
                            std::vector<Json> batches{Json::array()};
                            try
                            {
                                for (const auto& row : result.response->states)
                                {
                                    if (row.characterId == 0) continue;
                                    const auto progression = ActionRPG::PlayerSkills::CharacterProgression::Parse(
                                        Json::parse(row.progression));
                                    if (batches.back().size() == 64) batches.push_back(Json::array());
                                    batches.back().push_back({{"characterId", std::to_string(row.characterId)},
                                        {"name", row.name}, {"characterDefinitionId", row.definitionId},
                                        {"level", progression.level}, {"ownerGeneration", std::to_string(row.generation)},
                                        {"revision", std::to_string(row.revision)}});
                                }
                                for (std::size_t index = 0; index < batches.size(); ++index)
                                {
                                    reply["characters"] = batches[index]; reply["batchIndex"] = index;
                                    reply["batchCount"] = batches.size(); SendCharacterResult(session, inType, reply);
                                }
                            }
                            catch (...) { session->Stop(); }
                            town.FinishCharacterWork(sessionId);
                            return;
                        }
                    }
                    SendCharacterResult(session, inType, std::move(reply));
                }
                if (request.operation == Persistence::Operation::Claim && result.error
                    && result.error->executionMayHaveOccurred
                    && request.generation < std::numeric_limits<std::uint64_t>::max())
                {
                    auto release = request; release.operation = Persistence::Operation::Release;
                    ++release.generation;
                    town.characterReleases[sessionId] = std::move(release);
                    if (session) session->Stop();
                }
                town.FinishCharacterWork(sessionId);
            });
        });
    }

    bool TownInstance::ApplyCharacterState(PlayerEntry& inEntry, const Persistence::CharacterState& inState)
    {
        try
        {
            if (inState.result != 0 || inState.characterId != inEntry.persistentCharacterId
                || inState.generation != inEntry.ownerGeneration
                || inState.definitionId != inEntry.player.GetCharacterId() || inState.name != inEntry.player.GetName()
                || inState.revision < inEntry.revision) return false;
            auto progression = ActionRPG::PlayerSkills::CharacterProgression::Parse(Json::parse(inState.progression));
            skillTrees.ValidateProgression(playerSkills, progression, inState.definitionId);
            auto inventory = ActionRPG::Items::Inventory::Parse(Json::parse(inState.inventory), itemsCatalog,
                inState.definitionId, progression.level);
            inEntry.progression = std::move(progression); inEntry.inventory = std::move(inventory);
            inEntry.revision = inState.revision;
            return true;
        }
        catch (...) { return false; }
    }

    void TownInstance::FinishCharacterWork(std::uint64_t inSessionId)
    {
        characterRequests.erase(inSessionId);
        const auto found = characterWork.find(inSessionId);
        if (found != characterWork.end() && --found->second == 0) characterWork.erase(found);
        DrainCharacterRelease(inSessionId);
    }

    void TownInstance::DrainCharacterRelease(std::uint64_t inSessionId)
    {
        if (!deferredAdmissionReleases.contains(inSessionId) || characterWork.contains(inSessionId)) return;
        const auto retry = characterReleaseRetry.find(inSessionId);
        if (retry != characterReleaseRetry.end() && retry->second > std::chrono::steady_clock::now()) return;
        const auto use = itemUseServices.find(inSessionId);
        if (use != itemUseServices.end() && use->second->IsPending())
        {
            characterReleaseRetry[inSessionId] = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            use->second->ReconcileRelease();
            return; // Do not release Auth or DB ownership while a prepare/reserve response is unresolved.
        }
        const auto release = characterReleases.find(inSessionId);
        if (release == characterReleases.end())
        { deferredAdmissionReleases.erase(inSessionId); ReleaseAdmission(inSessionId); return; }
        ++characterWork[inSessionId];
        auto procedure = std::make_unique<Persistence::CharacterStoreProcedure>(); procedure->req = release->second;
        RunStoreProcedure(std::move(procedure), [inSessionId](TownInstance& town, auto result)
        {
            town.characterWork.erase(inSessionId);
            if (result.error || !result.response)
            {
                // Release is fenced/idempotent. Retry it from the existing admission timer, never replay a save.
                town.characterReleaseRetry[inSessionId] = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                return;
            }
            const auto code = result.response->states.front().result;
            if (code == 7 && town.itemUseServices.contains(inSessionId))
                town.itemUseServices.at(inSessionId)->ReconcileRelease();
            if (code != 0 && code != 5)
            { town.characterReleaseRetry[inSessionId] = std::chrono::steady_clock::now() + std::chrono::seconds(5); return; }
            town.characterReleaseRetry.erase(inSessionId);
            town.characterReleases.erase(inSessionId); town.deferredAdmissionReleases.erase(inSessionId);
            town.itemUseServices.erase(inSessionId);
            town.ReleaseAdmission(inSessionId);
        });
    }
}
