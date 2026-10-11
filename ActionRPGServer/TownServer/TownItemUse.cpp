#include "TownInstance.h"
#include "PlayerSession.h"
#include <cmath>

namespace TownServer::Domain
{
    using Json = ActionRPG::Items::Json;
    using UseOperation = Persistence::ItemUseOperation;

    void TownInstance::SetRoomItemUseHandler(RoomItemUseHandler inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, handler = std::move(inHandler)]() mutable
        { self->roomItemUseHandler = std::move(handler); });
    }
    std::shared_ptr<ItemUseService> TownInstance::GetItemUseService(PlayerEntry& inEntry)
    {
        const auto id = inEntry.session->GetSessionId();
        const auto found = itemUseServices.find(id);
        if (found != itemUseServices.end()) return found->second;
        Persistence::ItemUseRequest identity;
        identity.accountId = inEntry.accountId; identity.characterId = inEntry.persistentCharacterId;
        identity.ownerToken = inEntry.ownerToken; identity.generation = inEntry.ownerGeneration;
        auto service = std::make_shared<ItemUseService>(shared_from_this(), inEntry.session, std::move(identity));
        itemUseServices.emplace(id, service); return service;
    }
    ItemUseService::ItemUseService(std::shared_ptr<TownInstance> inTown,
        std::shared_ptr<Network::PlayerSession> inSession, Persistence::ItemUseRequest inIdentity,
        Persistence::Response inSnapshot)
        : town(inTown), session(inSession), sessionId(inSession->GetSessionId()), playerId(inSession->GetPlayerId()),
          identity(std::move(inIdentity)), snapshot(std::move(inSnapshot))
    {
        pending = snapshot.use && (snapshot.use->state == 1 || !snapshot.use->pendingRequestId.empty());
    }
    void ItemUseService::Call(Persistence::ItemUseRequest inRequest,
        std::function<void(std::optional<Persistence::Response>)> inHandler)
    {
        const auto owner = town.lock(); if (!owner) return;
        ++owner->characterWork[sessionId];
        auto procedure = std::make_unique<Persistence::ItemUseStoreProcedure>(); procedure->req = std::move(inRequest);
        const auto self = shared_from_this();
        owner->RunStoreProcedure(std::move(procedure), [self, handler = std::move(inHandler)](TownInstance& town, auto result) mutable
        {
            if (result.IsSuccess() && result.response->states.front().result == 5)
            {
                // The DB proved that this token/generation no longer owns the character. The new owner reconciles its ledger.
                self->ownershipLost = true; self->pending = false; self->inFlight = false;
                self->stagedRequestId.clear(); self->ClientDisconnected();
                if (const auto session = self->session.lock()) session->Stop();
                town.FinishCharacterWork(self->sessionId); return;
            }
            try { handler(result.IsSuccess() ? std::move(result.response) : std::nullopt); }
            catch (const std::exception&) { self->pending = true; self->Publish("Pending"); }
            town.FinishCharacterWork(self->sessionId);
        });
    }
    void ItemUseService::Room(Json inRequest, std::function<void(Json)> inHandler)
    {
        const auto owner = town.lock(); if (!owner) return;
        if (!owner->roomItemUseHandler) { inHandler({{"status", "Unknown"}}); return; }
        const auto self = shared_from_this();
        owner->roomItemUseHandler(identity.roomId, playerId, inRequest.dump(),
            [self, handler = std::move(inHandler)](std::string json) mutable
            {
                const auto owner = self->town.lock(); if (!owner) return;
                asio::post(owner->strand, [self, handler = std::move(handler), json = std::move(json)]() mutable
                {
                    auto result = Json::parse(json, nullptr, false);
                    if (!result.is_object()) result = {{"status", "Unknown"}};
                    try { handler(std::move(result)); }
                    catch (const std::exception&) { self->pending = true; self->Publish("Pending"); }
                });
            });
    }
    bool ItemUseService::ApplySnapshot(Persistence::Response inSnapshot)
    {
        const auto owner = town.lock(); if (!owner || inSnapshot.states.size() != 1 || !inSnapshot.use) return false;
        const auto& state = inSnapshot.states.front();
        if (state.characterId != identity.characterId || state.generation != identity.generation) return false;
        snapshot = std::move(inSnapshot);
        pending = snapshot.use->state == 1 || !snapshot.use->pendingRequestId.empty();
        const auto found = owner->players.find(playerId);
        if (found != owner->players.end() && found->second.session->GetSessionId() == sessionId
            && found->second.ownerToken == identity.ownerToken && found->second.ownerGeneration == identity.generation)
        {
            auto state = snapshot.states.front(); state.result = 0;
            if (!owner->ApplyCharacterState(found->second, state)) return false;
            found->second.itemUsePending = pending;
            found->second.cooldownsComplete = true;
            found->second.itemCooldowns = Persistence::CooldownsJson(snapshot);
            owner->NotifyProgression(playerId, found->second);
        }
        return true;
    }
    void ItemUseService::Use(Json inCommand)
    {
        if (ownershipLost) { if (const auto current = session.lock()) current->Stop(); return; }
        if (inFlight) { SendBusy(inCommand.at("requestId"), "Use"); return; }
        command = std::move(inCommand); requestId = command.at("requestId"); replyRequestId = requestId;
        action = "Use"; stateQuery = false;
        const auto owner = town.lock(); if (!owner) return;
        const auto found = owner->players.find(playerId);
        if (found == owner->players.end() || found->second.saving) { Publish("Busy"); return; }
        inFlight = true;
        auto request = identity; request.operation = UseOperation::Get; request.requestId = requestId;
        const auto self = shared_from_this();
        Call(request, [self](auto result)
        {
            if (!result || !self->ApplySnapshot(std::move(*result))) { self->Publish("DatabaseUnavailable"); return; }
            const auto& row = *self->snapshot.use;
            if (row.state != 0)
            {
                auto original = self->command; original.erase("requestId"); original.erase("revision");
                if (row.expectedRevision != ActionRPG::Items::DecimalId(self->command.at("revision"), true)
                    || Json::parse(row.operationJson, nullptr, false) != original)
                { self->Publish("RequestConflict"); return; }
                if (row.state == 1) self->Resolve(false); else self->SettleTerminal();
                return;
            }
            if (self->pending || !self->stagedRequestId.empty()) { self->pending = true; self->Publish("Busy"); return; }
            self->Prepare();
        });
    }
    void ItemUseService::Prepare()
    {
        const auto owner = town.lock(); if (!owner) return;
        const auto found = owner->players.find(playerId);
        if (found == owner->players.end() || found->second.session->GetSessionId() != sessionId)
        { Publish("Disconnected"); return; }
        auto& entry = found->second;
        if (entry.dungeonRoomId == 0) { Publish("NotInDungeon"); return; }
        const auto expected = ActionRPG::Items::DecimalId(command.at("revision"), true);
        if (entry.revision != expected) { Publish("RevisionConflict"); return; }
        const auto instanceId = command.at("instanceId").get<std::string>();
        const auto stack = std::find_if(entry.inventory.items.begin(), entry.inventory.items.end(),
            [&instanceId](const auto& row) { return row.instanceId == instanceId; });
        if (stack == entry.inventory.items.end() || stack->container != 2) { Publish("ItemNotFound"); return; }
        const auto& use = owner->itemsCatalog.definitions.at(stack->definitionId).use;
        if (use.type == ActionRPG::Items::Definition::Use::Type::None) { Publish("NotUsable"); return; }
        if (entry.progression.level < use.requiredLevel) { Publish("LevelRestricted"); return; }
        if ((use.type == ActionRPG::Items::Definition::Use::Type::Throw) != command.contains("facingLeft"))
        { Publish("InvalidRequest"); return; }
        identity.roomId = entry.dungeonRoomId; identity.revision = expected; identity.instanceId = instanceId;
        identity.cooldownMs = static_cast<std::uint32_t>(std::ceil(use.cooldownSeconds * 1000));
        auto original = command; original.erase("requestId"); original.erase("revision");
        identity.operationJson = original.dump(); definition = owner->itemsCatalog.presentations.at(stack->definitionId);
        Json prepare{{"kind", "Prepare"}, {"requestId", requestId}, {"characterId", std::to_string(identity.characterId)},
            {"ownerGeneration", std::to_string(identity.generation)}, {"operation", original}, {"definition", definition}};
        stagedPrepare = prepare; stagedRequestId = requestId;
        entry.itemUsePending = true; pending = true;
        const auto self = shared_from_this();
        Room(prepare, [self](Json reply)
        {
            if (reply.value("status", "Unknown") != "Prepared")
            {
                const auto status = reply.value("status", "Unknown");
                // A lost prepare response may have created a room receipt. Keep the character blocked.
                if (status != "Unknown") { self->pending = false; self->stagedRequestId.clear(); }
                self->Publish(status == "Unknown" ? "Pending" : status); return;
            }
            self->identity.roomIncarnation = reply.at("roomIncarnation");
            self->execution = {{"playerId", std::to_string(self->playerId)},
                {"processIncarnation", reply.at("processIncarnation")}, {"definition", self->definition}};
            self->identity.executionJson = self->execution.dump(); self->identity.requestId = self->requestId;
            auto request = self->identity; request.operation = UseOperation::Reserve;
            self->Call(request, [self](auto result)
            {
                if (!result) { self->Publish("Pending"); return; } // Query only; never replay an uncertain reserve.
                const auto code = result->states.front().result;
                if (!self->ApplySnapshot(std::move(*result))) { self->Publish("Pending"); return; }
                if (code == 0 && self->snapshot.use->state == 1) { self->Resolve(true); return; }
                if (self->snapshot.use->state != 0) { self->SettleTerminal(); return; }
                // A committed rejection proves this prepare was never reserved; cancel its room receipt.
                auto cancel = self->Control("Cancel"); cancel["executionHash"] = std::string(64, '0');
                self->Room(cancel, [self, code](Json reply)
                {
                    if (reply.value("status", "Unknown") != "Cancelled") { self->pending = true; self->Publish("Pending"); return; }
                    auto settle = self->Control("Settle"); settle["executionHash"] = std::string(64, '0'); settle["useState"] = "Cancelled";
                    self->Room(settle, [self, code](Json reply)
                    {
                        if (reply.value("status", "Unknown") == "Cancelled") self->stagedRequestId.clear();
                        else self->pending = true;
                        self->Publish(self->pending ? "Pending" : Persistence::ResultName(code));
                    });
                });
            });
        });
    }
    Json ItemUseService::Control(const char* inKind) const
    {
        const auto& row = snapshot.use;
        return {{"kind", inKind}, {"requestId", requestId}, {"characterId", std::to_string(identity.characterId)},
            {"ownerGeneration", std::to_string(row && row->state != 0 ? row->originGeneration : identity.generation)},
            {"roomIncarnation", identity.roomIncarnation}, {"processIncarnation", execution.value("processIncarnation", "")},
            {"executionHash", row && row->state != 0 ? row->executionHash : std::string(64, '0')}};
    }
    void ItemUseService::Resolve(bool inAllowExecute)
    {
        const auto& row = *snapshot.use;
        identity.roomId = row.roomId; identity.roomIncarnation = row.roomIncarnation;
        identity.executionHash = row.executionHash; identity.requestId = row.requestId;
        requestId = row.requestId;
        execution = Json::parse(row.executionJson, nullptr, false);
        if (!execution.is_object()) { Publish("Pending"); return; }
        try { playerId = ActionRPG::Items::DecimalId(execution.at("playerId")); }
        catch (...) { Publish("Pending"); return; }
        const auto self = shared_from_this();
        Room(Control(inAllowExecute ? "Execute" : "Query"), [self](Json reply)
        {
            const auto status = reply.value("status", "Unknown");
            if (status == "Prepared")
            {
                self->Room(self->Control("Cancel"), [self](Json reply)
                { self->Finalize(reply.value("status", "Unknown"), reply.value("reason", "CancelledBeforeEffect")); });
            }
            else self->Finalize(status, reply.value("reason", "RoomReceipt"));
        });
    }
    void ItemUseService::Finalize(const std::string& inRoomStatus, const std::string& inReason)
    {
        if (inRoomStatus != "Applied" && inRoomStatus != "Cancelled") { Publish("Pending"); return; }
        auto request = identity;
        request.operation = inRoomStatus == "Applied" ? UseOperation::Complete : UseOperation::Cancel;
        request.disposition = 0; request.reason = inReason.empty() ? "CancelledBeforeEffect" : inReason;
        const auto self = shared_from_this();
        Call(request, [self](auto result)
        {
            if (!result || !self->ApplySnapshot(std::move(*result))) { self->Publish("Pending"); return; }
            if (self->snapshot.use->state == 1) { self->Publish("Pending"); return; }
            self->SettleTerminal();
        });
    }
    void ItemUseService::SettleTerminal()
    {
        const auto& row = *snapshot.use;
        identity.roomId = row.roomId; identity.roomIncarnation = row.roomIncarnation; requestId = row.requestId;
        execution = Json::parse(row.executionJson, nullptr, false);
        try { playerId = ActionRPG::Items::DecimalId(execution.at("playerId")); }
        catch (...) { Publish("Pending"); return; }
        auto settle = Control("Settle");
        settle["useState"] = row.state == 2 ? "Applied" : row.state == 3 ? "Cancelled" : "ConsumedUnknown";
        const auto self = shared_from_this();
        Room(settle, [self](Json reply)
        {
            if (reply.value("status", "Unknown") != "Unknown") self->stagedRequestId.clear();
            self->Publish();
        });
    }
    void ItemUseService::CancelUnreserved(const std::string& inResult)
    {
        // A successful Get returned no ledger: a previously lost prepare/reserve can only be cancelled.
        const auto self = shared_from_this(); requestId = stagedRequestId;
        Room(stagedPrepare, [self, inResult](Json reply)
        {
            if (reply.value("status", "Unknown") != "Prepared" && reply.value("status", "Unknown") != "Cancelled")
            { self->pending = true; self->Publish("Pending"); return; }
            self->identity.roomIncarnation = reply.at("roomIncarnation");
            self->execution = {{"processIncarnation", reply.at("processIncarnation")}};
            self->Room(self->Control("Cancel"), [self, inResult](Json reply)
            {
                if (reply.value("status", "Unknown") != "Cancelled") { self->pending = true; self->Publish("Pending"); return; }
                auto settle = self->Control("Settle"); settle["useState"] = "Cancelled";
                self->Room(settle, [self, inResult](Json reply)
                {
                    if (reply.value("status", "Unknown") != "Cancelled") { self->pending = true; self->Publish("Pending"); return; }
                    self->stagedRequestId.clear(); self->pending = false; self->Publish(inResult);
                });
            });
        });
    }
    void ItemUseService::Query(std::string inRequestId, bool inInventoryState)
    {
        if (ownershipLost) return;
        if (inFlight) { if (!inInventoryState) SendBusy(inRequestId, "UseStatus"); return; }
        requestId = std::move(inRequestId); replyRequestId = requestId;
        action = "UseStatus"; stateQuery = inInventoryState; inFlight = true;
        auto request = identity; request.operation = UseOperation::Get;
        request.requestId = inInventoryState ? std::string{} : requestId;
        if (recoveringClaim && claimOrigin) request.requestId = claimOrigin->requestId;
        const auto self = shared_from_this();
        Call(request, [self](auto result)
        {
            if (!result || !self->ApplySnapshot(std::move(*result))) { self->Publish("DatabaseUnavailable"); return; }
            // Disposition 1 is a durable operator/server confirmation of fencing and irrecoverable origin loss.
            if (self->recoveringClaim && self->snapshot.use->state == 4) self->claimFenced = true;
            if (self->recoveringClaim && !self->claimFenced) { self->FenceClaim(); return; }
            if (self->snapshot.use->state == 1) { self->Resolve(false); return; }
            if (self->snapshot.use->state != 0) { self->SettleTerminal(); return; }
            if (!self->stagedRequestId.empty())
            { self->CancelUnreserved("None"); return; }
            self->Publish();
        });
    }
    void ItemUseService::RecoverClaim(std::string inSelectionId)
    {
        selectionId = std::move(inSelectionId); recoveringClaim = true; inFlight = true;
        if (!snapshot.use || snapshot.use->state != 1) { Publish(); return; }
        claimOrigin = *snapshot.use; FenceClaim();
    }
    void ItemUseService::FenceClaim()
    {
        if (!claimOrigin) { Publish("Pending"); return; }
        const auto& row = *claimOrigin;
        requestId = row.requestId; identity.roomId = row.roomId; identity.roomIncarnation = row.roomIncarnation;
        execution = Json::parse(row.executionJson, nullptr, false);
        try { playerId = ActionRPG::Items::DecimalId(execution.at("playerId")); }
        catch (...) { Publish("Pending"); return; }
        auto fence = Control("Fence"); fence["ownerGeneration"] = std::to_string(identity.generation);
        const auto self = shared_from_this();
        Room(fence, [self](Json reply)
        {
            if (reply.value("status", "Unknown") != "Fenced") { self->Publish("Pending"); return; }
            self->claimFenced = true;
            if (self->snapshot.use->state == 1) self->Resolve(false);
            else if (self->snapshot.use->state != 0) self->SettleTerminal();
            else self->Publish();
        });
    }
    void ItemUseService::ReconcileRelease()
    {
        if (inFlight) return;
        Query({}, true);
    }
    void ItemUseService::ClientDisconnected()
    {
        recoveringClaim = false;
        if (recoveryTimer) recoveryTimer->cancel();
        // Keep the service and its original operation for the admission release timer's reconciliation.
    }
    void ItemUseService::SendBusy(const std::string& inRequestId, const char* inAction)
    {
        const auto current = session.lock();
        if (!current || current->GetAccountId() != identity.accountId) return;
        Json reply{{"action", inAction}, {"requestId", inRequestId}, {"result", "Busy"}, {"useState", "Pending"},
            {"characterId", std::to_string(identity.characterId)},
            {"revision", std::to_string(snapshot.states.empty() ? 0 : snapshot.states.front().revision)},
            {"definitionId", ""}, {"cooldownsComplete", false}, {"cooldowns", nullptr}};
        current->Send(TownProtocol::Encode(TownProtocol::InventoryOperationResponse{reply.dump()}));
    }
    void ItemUseService::ScheduleClaimRecovery()
    {
        const auto owner = town.lock(); const auto current = session.lock();
        if (!owner || !owner->running || !current || current->GetAccountId() != identity.accountId || !recoveringClaim) return;
        if (!recoveryTimer) recoveryTimer = std::make_shared<asio::steady_timer>(owner->strand);
        recoveryTimer->expires_after(std::chrono::seconds(5));
        const auto weakSelf = weak_from_this();
        recoveryTimer->async_wait([weakSelf](const asio::error_code& error)
        {
            const auto self = weakSelf.lock();
            if (!error && self && self->recoveringClaim && !self->inFlight && !self->ownershipLost)
            {
                const auto town = self->town.lock(); const auto session = self->session.lock();
                if (town && town->running && session && session->GetAccountId() == self->identity.accountId)
                    self->Query({}, true);
            }
        });
    }
    void ItemUseService::Publish(const std::string& inResult)
    {
        const auto owner = town.lock(); if (!owner) return;
        inFlight = false;
        const auto currentSession = session.lock();
        const auto found = currentSession ? owner->players.find(currentSession->GetPlayerId()) : owner->players.end();
        if (found != owner->players.end() && found->second.session->GetSessionId() == sessionId)
        {
            // Receipt reconciliation targets its original room actor; the next command targets this connection.
            playerId = currentSession->GetPlayerId();
            found->second.itemUsePending = pending;
            if (inResult == "DatabaseUnavailable" || inResult == "Pending")
            { found->second.cooldownsComplete = false; found->second.itemCooldowns = nullptr; }
        }
        if (!currentSession || currentSession->GetAccountId() != identity.accountId) return;
        const auto state = snapshot.use ? snapshot.use->state : 0;
        const char* names[] = {"None", "Pending", "Applied", "Cancelled", "ConsumedUnknown"};
        std::string result = inResult.empty() ? (state == 2 ? "Succeeded" : names[state]) : inResult;
        if (recoveringClaim)
        {
            if (!pending && claimFenced && (result == "None" || result == "Succeeded" || result == "Cancelled" || result == "ConsumedUnknown")
                && !snapshot.states.empty() && currentSession->GetPlayerId() == 0)
            {
                auto character = snapshot.states.front(); character.result = 0;
                owner->EnterOnStrand(currentSession, character, identity.ownerToken, selectionId);
                recoveringClaim = false; playerId = currentSession->GetPlayerId();
                if (recoveryTimer) recoveryTimer->cancel();
                const auto entered = owner->players.find(playerId);
                if (entered != owner->players.end())
                { entered->second.cooldownsComplete = true; entered->second.itemCooldowns = Persistence::CooldownsJson(snapshot); }
            }
            else
            {
                ScheduleClaimRecovery();
                currentSession->Send(TownProtocol::Encode(TownProtocol::CharacterSelectResponse{
                Json{{"requestId", selectionId}, {"result", "Pending"},
                    {"pendingUseRequestId", claimOrigin ? claimOrigin->requestId : requestId},
                    {"characterId", std::to_string(identity.characterId)}, {"revision", std::to_string(snapshot.states.front().revision)}}.dump()}));
            }
            return;
        }
        if (stateQuery && found != owner->players.end())
        { owner->SendInventoryState(found->second, replyRequestId); return; }
        const bool complete = !snapshot.cooldowns.empty() && inResult != "DatabaseUnavailable" && inResult != "Pending";
        Json reply{{"action", action}, {"requestId", replyRequestId}, {"result", result}, {"useState", names[state]},
            {"characterId", std::to_string(identity.characterId)},
            {"revision", std::to_string(snapshot.states.empty() ? 0 : snapshot.states.front().revision)},
            {"definitionId", snapshot.use ? snapshot.use->definitionId : ""},
            {"cooldownsComplete", complete}, {"cooldowns", complete ? Persistence::CooldownsJson(snapshot) : Json(nullptr)}};
        if (!snapshot.states.empty()) reply["inventory"] = Json::parse(snapshot.states.front().inventory);
        if (pending && state == 0) { reply["useState"] = "Pending"; reply["result"] = "Pending"; }
        if (reply.at("result") == "Pending") reply["useState"] = "Pending";
        currentSession->Send(TownProtocol::Encode(TownProtocol::InventoryOperationResponse{reply.dump()}));
    }
}
