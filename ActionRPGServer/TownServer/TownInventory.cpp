#include "TownInstance.h"
#include "PlayerSession.h"
#include <set>

namespace TownServer::Domain
{
    using Json = ActionRPG::Items::Json;
    using Rules = ActionRPG::PlayerSkills::Catalog;

    Json TownInstance::RuntimeState(const PlayerEntry& inEntry) const
    {
        auto equipment = Json::array();
        const auto inventory = inEntry.inventory.ToJson();
        for (const auto& row : inventory.at("items"))
            if (row.at("container") == ActionRPG::Items::EQUIPPED_CONTAINER) equipment.push_back(row);
        return {{"characterId", std::to_string(inEntry.persistentCharacterId)},
            {"ownerGeneration", std::to_string(inEntry.ownerGeneration)},
            {"revision", std::to_string(inEntry.revision)}, {"progression", inEntry.progression.ToJson()},
            {"equipment", std::move(equipment)}};
    }

    void TownInstance::SendInventoryState(PlayerEntry& inEntry, const std::string& inRequestId)
    {
        std::set<std::string> referenced;
        for (const auto& item : inEntry.inventory.items) referenced.insert(item.definitionId);
        std::vector<Json> batches;
        for (const auto& id : referenced)
        {
            if (batches.empty()) batches.push_back(Json::array());
            auto candidate = batches.back(); candidate.push_back(itemsCatalog.presentations.at(id));
            if (candidate.dump().size() > 14000)
                batches.push_back(Json::array({itemsCatalog.presentations.at(id)}));
            else batches.back() = std::move(candidate);
        }
        Json key{{"requestId", inRequestId}, {"result", "Succeeded"},
            {"characterId", std::to_string(inEntry.persistentCharacterId)},
            {"revision", std::to_string(inEntry.revision)}};
        auto state = key; state["inventory"] = inEntry.inventory.ToJson();
        state["cooldownsComplete"] = inEntry.cooldownsComplete;
        state["cooldowns"] = inEntry.cooldownsComplete ? inEntry.itemCooldowns : Json(nullptr);
        state["definitionBatchCount"] = batches.size();
        inEntry.session->Send(TownProtocol::Encode(TownProtocol::InventoryStateResponse{state.dump()}));
        for (std::size_t index = 0; index < batches.size(); ++index)
        {
            auto batch = key; batch["batchIndex"] = index; batch["batchCount"] = batches.size();
            batch["definitions"] = std::move(batches[index]);
            if (batch.dump().size() > 16384) throw std::logic_error("Item definition batch exceeds its contract.");
            inEntry.session->Send(TownProtocol::Encode(TownProtocol::ItemDefinitionsResponse{batch.dump()}));
        }
    }

    /** Serialize mutations on the town strand; only a committed, ownership-bound DB snapshot replaces memory. */
    void TownInstance::SaveCharacter(PlayerId inPlayerId, std::string inRequestId, std::uint64_t inExpectedRevision,
        ActionRPG::PlayerSkills::CharacterProgression inProgression, ActionRPG::Items::Inventory inInventory,
        Json inOperation, std::function<void(std::string)> inHandler)
    {
        const auto found = players.find(inPlayerId);
        if (found == players.end() || found->second.session->GetAccountId() != found->second.accountId)
        { inHandler("Disconnected"); return; }
        auto& entry = found->second;
        if (entry.saving || entry.itemUsePending) { inHandler("Busy"); return; }
        Persistence::Request request;
        request.operation = Persistence::Operation::Save; request.accountId = entry.accountId;
        request.characterId = entry.persistentCharacterId; request.ownerToken = entry.ownerToken;
        request.generation = entry.ownerGeneration; request.revision = inExpectedRevision;
        request.requestId = std::move(inRequestId); request.operationJson = inOperation.dump();
        request.progression = inProgression.ToJson().dump(); request.inventory = inInventory.ToJson().dump();
        if (!ActionRPG::Items::IsHexId(request.requestId, 64) || request.operationJson.size() > 2048
            || request.progression.size() > 32768 || request.inventory.size() > 32768)
        { inHandler("InvalidRequest"); return; }
        entry.saving = true;
        const auto sessionId = entry.session->GetSessionId();
        ++characterWork[sessionId];
        auto procedure = std::make_unique<Persistence::CharacterStoreProcedure>(); procedure->req = request;
        RunStoreProcedure(std::move(procedure), [inPlayerId, sessionId, request,
            handler = std::move(inHandler)](TownInstance& town, auto result) mutable
        {
            const auto found = town.players.find(inPlayerId);
            std::string status = "Disconnected";
            std::shared_ptr<Network::PlayerSession> failedSession;
            if (found != town.players.end() && found->second.session->GetSessionId() == sessionId
                && found->second.ownerToken == request.ownerToken
                && found->second.ownerGeneration == request.generation
                && found->second.session->GetAccountId() == request.accountId)
            {
                auto& entry = found->second;
                entry.saving = false;
                status = "DatabaseUnavailable";
                if (!result.error && result.response)
                {
                    const auto& state = result.response->states.front();
                    status = Persistence::ResultName(state.result);
                    if (state.result == 0)
                    {
                        const auto previousGrowth = entry.progression.ToJson();
                        if (!town.ApplyCharacterState(entry, state)) status = "InvalidStoredState";
                        else
                        {
                            town.NotifyProgression(inPlayerId, entry);
                            if (previousGrowth != entry.progression.ToJson()) town.SendSkillState(entry, "State");
                        }
                    }
                    else if (state.result == 3 && state.characterId == entry.persistentCharacterId
                        && state.generation == entry.ownerGeneration && state.revision > entry.revision)
                    {
                        auto latest = state; latest.result = 0;
                        if (!town.ApplyCharacterState(entry, latest)) status = "InvalidStoredState";
                        else town.NotifyProgression(inPlayerId, entry);
                    }
                    if (state.result == 5 || state.result == 1) failedSession = entry.session;
                }
                if (status == "DatabaseUnavailable" || status == "InvalidStoredState") failedSession = entry.session;
            }
            handler(status);
            if (failedSession) failedSession->Stop();
            town.FinishCharacterWork(sessionId);
        });
    }

    void TownInstance::InventoryRequest(std::shared_ptr<Network::PlayerSession> inSession,
        TownProtocol::PacketType inType, std::string inJson)
    {
        const auto self = shared_from_this();
        asio::post(strand, [self, session = std::move(inSession), inType, data = std::move(inJson)]()
        {
            const auto playerId = session->GetPlayerId();
            const auto found = self->players.find(playerId);
            if (found == self->players.end() && session->GetAccountId() != 0
                && inType == TownProtocol::PacketType::InventoryOperationRequest
                && self->itemUseServices.contains(session->GetSessionId()))
            {
                try
                {
                    const auto body = Json::parse(data);
                    Rules::Keys(body, {"action", "requestId"});
                    const auto id = body.at("requestId").get<std::string>();
                    Rules::Require(body.at("action") == "UseStatus" && ActionRPG::Items::IsHexId(id, 64), "Invalid pending query.");
                    self->itemUseServices.at(session->GetSessionId())->Query(id); return;
                }
                catch (...) { session->Stop(); return; }
            }
            if (!self->running || found == self->players.end() || found->second.session != session
                || session->GetAccountId() != found->second.accountId) { session->Stop(); return; }
            auto& entry = found->second;
            std::string requestId;
            Json operation;
            std::uint64_t revision{};
            try
            {
                operation = Json::parse(data); requestId = operation.at("requestId").get<std::string>();
                Rules::Require(ActionRPG::Items::IsHexId(requestId, 64), "Invalid inventory request ID.");
                if (inType == TownProtocol::PacketType::InventoryStateRequest)
                {
                    Rules::Keys(operation, {"requestId"}); self->GetItemUseService(entry)->Query(requestId, true); return;
                }
                if (operation.at("action") == "UseStatus")
                {
                    Rules::Keys(operation, {"requestId", "action"});
                    self->GetItemUseService(entry)->Query(requestId); return;
                }
                Rules::Keys(operation, {"requestId", "revision", "action", "instanceId", "count"}, {"facingLeft"});
                revision = ActionRPG::Items::DecimalId(operation.at("revision"), true);
                Rules::Require(ActionRPG::Items::IsHexId(operation.at("instanceId").get<std::string>(), 32),
                    "Invalid inventory instance ID.");
                Rules::Integer(operation.at("count"), 1, std::numeric_limits<std::uint32_t>::max());
                const auto action = operation.at("action").get<std::string>();
                Rules::Require(action == "Equip" || action == "Unequip" || action == "Discard"
                    || action == "Use" || action == "Sell", "Invalid inventory action.");
                if (action == "Equip" || action == "Unequip")
                    Rules::Require(operation.at("count") == 1, "Equipment quantity must be one.");
                if (operation.contains("facingLeft")) Rules::Require(action == "Use" && operation.at("facingLeft").is_boolean(),
                    "Invalid throw facing.");
                if (action == "Use")
                {
                    Rules::Require(operation.at("count") == 1, "Item use quantity must be one.");
                    self->GetItemUseService(entry)->Use(operation); return;
                }
            }
            catch (...) { session->Stop(); return; }
            const auto reply = [weakTown = self->weak_from_this(), weakSession = std::weak_ptr<Network::PlayerSession>(session),
                playerId, requestId](const std::string& status)
            {
                const auto town = weakTown.lock(); const auto session = weakSession.lock();
                if (!town || !session) return;
                const auto found = town->players.find(playerId);
                if (found == town->players.end() || found->second.session != session
                    || session->GetAccountId() != found->second.accountId) return;
                auto& entry = found->second;
                Json body{{"requestId", requestId}, {"result", status},
                    {"characterId", std::to_string(entry.persistentCharacterId)},
                    {"revision", std::to_string(entry.revision)}};
                if (status == "Succeeded") body["inventory"] = entry.inventory.ToJson();
                session->Send(TownProtocol::Encode(TownProtocol::InventoryOperationResponse{body.dump()}));
                if (status == "Succeeded" || status == "RevisionConflict") town->SendInventoryState(entry, requestId);
            };
            if (entry.saving || entry.itemUsePending) { reply("Busy"); return; }
            const auto action = operation.at("action").get<std::string>();
            if (action == "Sell") { reply("NotImplemented"); return; }
            auto proposed = entry.inventory;
            std::string status = "Succeeded";
            // A stale revision goes to the persistent receipt check before any domain calculation.
            if (revision == entry.revision)
            {
                const auto id = operation.at("instanceId").get<std::string>();
                if (action == "Equip") status = proposed.Equip(self->itemsCatalog, id,
                    entry.player.GetCharacterId(), entry.progression.level);
                else if (action == "Unequip") status = proposed.Unequip(id);
                else status = proposed.Remove(id, operation.at("count").get<std::uint32_t>());
            }
            if (status != "Succeeded") { reply(status); return; }
            operation.erase("requestId"); operation.erase("revision");
            self->SaveCharacter(playerId, requestId, revision, entry.progression, std::move(proposed), operation, reply);
        });
    }

    void TownInstance::AcquireItem(PlayerId inPlayerId, std::string inRequestId, std::uint64_t inExpectedRevision,
        std::string inDefinitionId, std::uint32_t inCount, std::function<void(std::string)> inHandler)
    {
        const auto self = shared_from_this();
        asio::post(strand, [self, inPlayerId, requestId = std::move(inRequestId), inExpectedRevision,
            definitionId = std::move(inDefinitionId), inCount, handler = std::move(inHandler)]() mutable
        {
            const auto found = self->players.find(inPlayerId);
            if (found == self->players.end()) { handler("Disconnected"); return; }
            auto& entry = found->second; auto inventory = entry.inventory;
            if (entry.saving || entry.itemUsePending) { handler("Busy"); return; }
            std::string status = "Succeeded";
            try
            {
                if (inExpectedRevision == entry.revision)
                    status = inventory.Acquire(self->itemsCatalog, definitionId, inCount,
                        [] { return Persistence::RandomHex(16); });
            }
            catch (...) { handler("IdentityUnavailable"); return; }
            if (status != "Succeeded") { handler(status); return; }
            self->SaveCharacter(inPlayerId, requestId, inExpectedRevision, entry.progression, std::move(inventory),
                {{"action", "Acquire"}, {"definitionId", definitionId}, {"count", inCount}},
                [weakTown = self->weak_from_this(), inPlayerId, requestId, handler = std::move(handler)](std::string status)
            {
                if (const auto town = weakTown.lock(); town && status == "Succeeded")
                    if (const auto found = town->players.find(inPlayerId); found != town->players.end())
                        town->SendInventoryState(found->second, requestId);
                handler(std::move(status));
            });
        });
    }

    void TownInstance::ConsumeItem(PlayerId inPlayerId, std::string inRequestId, std::uint64_t inExpectedRevision,
        std::string inDefinitionId, std::uint32_t inCount, std::function<void(std::string)> inHandler)
    {
        const auto self = shared_from_this();
        asio::post(strand, [self, inPlayerId, requestId = std::move(inRequestId), inExpectedRevision,
            definitionId = std::move(inDefinitionId), inCount, handler = std::move(inHandler)]() mutable
        {
            const auto found = self->players.find(inPlayerId);
            if (found == self->players.end()) { handler("Disconnected"); return; }
            auto& entry = found->second; auto inventory = entry.inventory;
            if (entry.saving) { handler("Busy"); return; }
            std::string status = "Succeeded";
            if (inExpectedRevision == entry.revision) status = inventory.Consume(definitionId, inCount);
            if (status != "Succeeded") { handler(status); return; }
            self->SaveCharacter(inPlayerId, requestId, inExpectedRevision, entry.progression, std::move(inventory),
                {{"action", "Consume"}, {"definitionId", definitionId}, {"count", inCount}},
                [weakTown = self->weak_from_this(), inPlayerId, requestId, handler = std::move(handler)](std::string status)
            {
                if (const auto town = weakTown.lock(); town && status == "Succeeded")
                    if (const auto found = town->players.find(inPlayerId); found != town->players.end())
                        town->SendInventoryState(found->second, requestId);
                handler(std::move(status));
            });
        });
    }
}
