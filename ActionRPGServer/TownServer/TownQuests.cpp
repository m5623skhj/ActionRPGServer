#include "TownInstance.h"
#include "PlayerSession.h"
#include <cmath>

namespace TownServer::Domain
{
    namespace Q = ActionRPG::Quests;
    using Q::Json;

    void TownInstance::SetQuestContextHandler(QuestContextHandler handler)
    { asio::dispatch(strand, [self = shared_from_this(), handler = std::move(handler)]() mutable { self->questContextHandler = std::move(handler); }); }
    Persistence::QuestRequest TownInstance::QuestIdentity(const QuestRuntime& runtime) const
    {
        Persistence::QuestRequest request;
        request.accountId = runtime.accountId; request.characterId = runtime.characterId;
        request.generation = runtime.generation; request.ownerToken = runtime.ownerToken; request.revision = runtime.revision;
        auto ids = Json::array(); for (const auto& [id, definition] : questCatalog.definitions) ids.push_back(id);
        request.questIdsJson = ids.dump(); return request;
    }
    std::uint64_t TownInstance::QuestNow(const QuestRuntime& runtime) const
    {
        return runtime.nowMs + static_cast<std::uint64_t>(std::max<std::int64_t>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - runtime.observed).count()));
    }
    bool TownInstance::QuestBusy(std::uint64_t sessionId) const
    {
        const auto found = questRuntimes.find(sessionId);
        return found != questRuntimes.end() && (found->second->busy || !found->second->queue.empty());
    }
    void TownInstance::ObserveQuestCharacter(std::uint64_t sessionId, const Persistence::CharacterState& state)
    {
        const auto found = questRuntimes.find(sessionId); if (found == questRuntimes.end()) return;
        auto& runtime = *found->second;
        if (state.characterId != runtime.characterId || state.generation != runtime.generation || state.revision < runtime.revision) return;
        try
        {
            auto growth = ActionRPG::PlayerSkills::CharacterProgression::Parse(Json::parse(state.progression));
            auto inventory = ActionRPG::Items::Inventory::Parse(Json::parse(state.inventory), itemsCatalog, runtime.definitionId, growth.level);
            runtime.revision = state.revision; runtime.progression = std::move(growth); runtime.inventory = std::move(inventory);
        }
        catch (...) { if (const auto session = runtime.session.lock()) session->Stop(); }
    }
    void TownInstance::ApplyQuestResponse(QuestRuntime& runtime, const Persistence::QuestResponse& response)
    {
        Q::Rules::Require(response.characterId == runtime.characterId && response.generation == runtime.generation
            && response.revision >= runtime.revision, "Quest owner/revision mismatch.");
        // A conflict contains no bag/growth snapshot. Never advance a stale bag's revision on that response.
        Q::Rules::Require(response.progression || response.revision == runtime.revision
            || (runtime.loaded && response.result == 0), "Quest conflict requires a fresh character admission.");
        auto growth = runtime.progression; auto inventory = runtime.inventory;
        if (response.progression)
        {
            growth = ActionRPG::PlayerSkills::CharacterProgression::Parse(Json::parse(*response.progression));
            Q::Rules::Require(progressionPolicy.Normalized(growth), "Invalid stored experience.");
            skillTrees.ValidateProgression(playerSkills, growth, runtime.definitionId);
        }
        if (response.inventory)
        {
            inventory = ActionRPG::Items::Inventory::Parse(Json::parse(*response.inventory), itemsCatalog, runtime.definitionId, growth.level);
        }
        runtime.progression = std::move(growth); runtime.inventory = std::move(inventory);
        runtime.state = response.quests; runtime.revision = response.revision;
        runtime.nowMs = response.serverNowMs; runtime.observed = std::chrono::steady_clock::now();
        const auto entry = players.find(runtime.playerId);
        if (entry != players.end() && entry->second.session->GetSessionId() == runtime.sessionId
            && entry->second.ownerGeneration == runtime.generation && entry->second.ownerToken == runtime.ownerToken)
        {
            const auto previous = entry->second.progression;
            entry->second.revision = runtime.revision; entry->second.progression = runtime.progression;
            entry->second.inventory = runtime.inventory;
            NotifyProgression(runtime.playerId, entry->second);
            if (response.inventory) SendInventoryState(entry->second, "");
            if (previous.experience != runtime.progression.experience || previous.level != runtime.progression.level)
                SendExperienceState(entry->second);
            if (previous.level != runtime.progression.level || previous.skillPoints != runtime.progression.skillPoints
                || previous.skillLevels != runtime.progression.skillLevels) SendSkillState(entry->second, "State");
        }
    }

    // The extra V7 read is part of admission work, not a background fetch after entering the game.
    void TownInstance::BeginQuestEntry(std::shared_ptr<Network::PlayerSession> session, Persistence::CharacterState state,
        std::string token, std::string requestId, std::function<void()> entered)
    {
        auto runtime = std::make_shared<QuestRuntime>(); runtime->sessionId = session->GetSessionId();
        runtime->accountId = session->GetAccountId(); runtime->characterId = state.characterId;
        runtime->generation = state.generation; runtime->ownerToken = std::move(token); runtime->session = session;
        runtime->definitionId = state.definitionId; runtime->refill = std::chrono::steady_clock::now();
        runtime->context = {{"mapId", defaultMapId}, {"roomId", "0"}, {"cursor", nullptr}};
        questRuntimes[runtime->sessionId] = runtime;
        ObserveQuestCharacter(runtime->sessionId, state);
        auto procedure = std::make_unique<Persistence::QuestStoreProcedure>(); procedure->req = QuestIdentity(*runtime);
        ++characterWork[runtime->sessionId];
        RunStoreProcedure(std::move(procedure), [runtime, state = std::move(state), requestId = std::move(requestId),
            entered = std::move(entered)](TownInstance& town, auto result) mutable
        {
            const auto session = runtime->session.lock(); bool success = false;
            try
            {
                if (!result.error && result.response && result.response->result == 0 && session
                    && session->GetAccountId() == runtime->accountId && session->GetPlayerId() == 0 && town.running)
                {
                    town.ApplyQuestResponse(*runtime, *result.response); runtime->loaded = true;
                    town.EnterOnStrand(session, state, runtime->ownerToken, requestId);
                    runtime->playerId = session->GetPlayerId(); success = runtime->playerId != 0;
                    if (success) { entered(); town.SendQuestState(runtime, requestId); }
                }
            }
            catch (...) {}
            if (!success)
            {
                town.characterReleases[runtime->sessionId] = {Persistence::Operation::Release, runtime->accountId,
                    runtime->characterId, runtime->generation, 0, 0, 0, 0, {}, runtime->ownerToken};
                if (session) session->Stop();
            }
            town.FinishCharacterWork(runtime->sessionId);
        });
    }

    void TownInstance::ReplyQuest(const std::shared_ptr<QuestRuntime>& runtime, const std::string& id, const std::string& result)
    {
        const auto session = runtime->session.lock();
        if (!session || session->GetAccountId() != runtime->accountId) return;
        session->Send(TownProtocol::Encode(TownProtocol::QuestActionResult{Json{{"requestId", id}, {"result", result},
            {"characterId", Q::Decimal(runtime->characterId)}, {"revision", Q::Decimal(runtime->revision)}}.dump()}));
    }

    void TownInstance::QueueQuest(std::shared_ptr<QuestRuntime> runtime, QuestWork work)
    {
        if (runtime->queue.size() >= Q::MAX_QUEUE) { if (work.done) work.done("Busy"); return; }
        if (!work.event.is_null() && work.event.contains("cursor"))
            for (const auto& pending : runtime->queue)
                if (!pending.event.is_null() && pending.event.value("cursor", Json{}) == work.event.at("cursor"))
                { if (work.done) work.done("Busy"); return; }
        runtime->queue.push_back(std::move(work)); PumpQuest(std::move(runtime));
    }

    void TownInstance::QuestRequest(std::shared_ptr<Network::PlayerSession> session, TownProtocol::PacketType type, std::string text)
    {
        const auto self = shared_from_this();
        asio::post(strand, [self, session = std::move(session), type, text = std::move(text)]()
        {
            const auto found = self->questRuntimes.find(session->GetSessionId());
            if (!self->running || found == self->questRuntimes.end() || !found->second->loaded
                || session->GetAccountId() != found->second->accountId || session->GetPlayerId() != found->second->playerId)
            { session->Stop(); return; }
            const auto runtime = found->second;
            try
            {
                Q::Rules::Require(text.size() <= 2048, "Quest request too large.");
                auto body = Json::parse(text); const auto id = body.at("requestId").get<std::string>();
                Q::Rules::Require(ActionRPG::Items::IsHexId(id, 64), "Invalid quest request ID.");
                const auto now = std::chrono::steady_clock::now();
                runtime->tokens = std::min(10.0, runtime->tokens + std::chrono::duration<double>(now - runtime->refill).count() * 5);
                runtime->refill = now;
                if (runtime->tokens < 1) { self->ReplyQuest(runtime, id, "RateLimited"); return; } runtime->tokens -= 1;
                if (type == TownProtocol::PacketType::QuestStateRequest)
                {
                    Q::Rules::Keys(body, {"requestId"});
                    if (now - runtime->lastQuery < std::chrono::seconds(1)) { self->ReplyQuest(runtime, id, "RateLimited"); return; }
                    runtime->lastQuery = now;
                    QuestWork work; work.internal = true; work.requestId = Persistence::RandomHex(32); work.command = {{"action", "Refresh"}};
                    work.done = [weak = self->weak_from_this(), runtime, id](auto)
                    { if (const auto owner = weak.lock()) owner->SendQuestState(runtime, id); };
                    self->QueueQuest(runtime, std::move(work)); return;
                }
                Q::Rules::Keys(body, {"requestId", "expectedRevision", "action"},
                    {"questId", "cycleKey", "choiceId", "npcId", "trackedQuestIds"});
                Q::Decimal(body.at("expectedRevision"));
                const auto action = body.at("action").get<std::string>();
                Q::Rules::Require(action == "Accept" || action == "Abandon" || action == "Report" || action == "Claim"
                    || action == "Track" || action == "Talk", "Invalid quest action.");
                if (action == "Talk")
                {
                    Q::Id(body.at("npcId"));
                    Q::Rules::Require(!body.contains("questId") && !body.contains("cycleKey") && !body.contains("choiceId")
                        && !body.contains("trackedQuestIds"), "Unexpected talk fields.");
                    if (now - runtime->lastNpc < std::chrono::milliseconds(500)) { self->ReplyQuest(runtime, id, "RateLimited"); return; }
                    runtime->lastNpc = now;
                }
                else if (action == "Track")
                {
                    Q::Rules::Require(body.at("trackedQuestIds").is_array() && body.at("trackedQuestIds").size() <= 3
                        && !body.contains("questId") && !body.contains("cycleKey") && !body.contains("npcId") && !body.contains("choiceId"), "Invalid track fields.");
                    std::set<std::string> unique;
                    for (const auto& quest : body.at("trackedQuestIds")) { Q::Id(quest); Q::Rules::Require(unique.insert(quest.get<std::string>()).second, "Duplicate tracking."); }
                }
                else
                {
                    Q::Id(body.at("questId")); Q::Rules::Require(body.at("cycleKey").is_string() && !body.contains("trackedQuestIds"), "Invalid quest identity.");
                    if (body.contains("npcId")) Q::Id(body.at("npcId"));
                    Q::Rules::Require(!body.contains("choiceId") || action == "Claim", "Unexpected choice.");
                }
                QuestWork work; work.requestId = id; work.command = std::move(body);
                work.done = [weak = self->weak_from_this(), runtime, id](std::string result)
                {
                    if (const auto owner = weak.lock()) { owner->ReplyQuest(runtime, id, result); owner->SendQuestState(runtime, id); }
                };
                self->QueueQuest(runtime, std::move(work));
            }
            catch (...) { session->Stop(); }
        });
    }

    /** Only one character mutation may own the shared revision. Busy gameplay services delay, never drop room facts. */
    void TownInstance::PumpQuest(std::shared_ptr<QuestRuntime> runtime)
    {
        if (!runtime->loaded || runtime->busy || runtime->queue.empty() || std::chrono::steady_clock::now() < runtime->nextRetry) return;
        const auto entry = players.find(runtime->playerId);
        const auto use = itemUseServices.find(runtime->sessionId);
        if ((entry != players.end() && (entry->second.saving || entry->second.itemUsePending))
            || (use != itemUseServices.end() && use->second->IsPending())) return;
        auto& work = runtime->queue.front(); const auto action = work.command.at("action").get<std::string>();
        const auto session = runtime->session.lock();
        const auto finish = [this, runtime](const std::string& result)
        {
            auto callback = std::move(runtime->queue.front().done); runtime->queue.pop_front();
            if (callback) callback(result);
        };
        if (!work.internal && (!session || session->GetAccountId() != runtime->accountId)) { finish("Disconnected"); return; }
        if (!work.contextReady && entry != players.end() && entry->second.dungeonRoomId != 0
            && (action == "Accept" || action == "LocationCheck" || action == "Refresh"))
        {
            if (!questContextHandler) { finish("DatabaseUnavailable"); return; }
            runtime->busy = true; const auto weak = weak_from_this();
            questContextHandler(entry->second.dungeonRoomId, runtime->playerId, [weak, runtime](std::string text)
            {
                if (const auto owner = weak.lock()) asio::post(owner->strand, [owner, runtime, text = std::move(text)]
                {
                    if (runtime->queue.empty()) return;
                    runtime->busy = false;
                    try { runtime->context = Json::parse(text); Q::Decimal(runtime->context.at("roomId"), false);
                        Q::Decimal(runtime->context.at("cursor").at("sequence")); runtime->queue.front().contextReady = true; }
                    catch (...) { auto done = std::move(runtime->queue.front().done); runtime->queue.pop_front(); if (done) done("DatabaseUnavailable"); return; }
                    owner->PumpQuest(runtime);
                });
            }); return;
        }
        auto proposed = runtime->state; auto inventory = runtime->inventory; auto growth = runtime->progression;
        auto request = QuestIdentity(*runtime); request.save = true; request.requestId = work.requestId;
        request.mode = action == "Claim" ? "Claim" : action == "Acquire" ? "Inventory" : "Progress";
        auto operation = work.command; operation.erase("requestId"); operation.erase("expectedRevision");
        const auto now = QuestNow(*runtime);
        const auto map = runtime->context.value("mapId", defaultMapId);
        const auto cursor = runtime->context.value("cursor", Json(nullptr));
        std::string status = "Succeeded";
        const auto atNpc = [this, &entry](const Json& id)
        {
            if (id.is_null()) return false;
            const auto npc = questCatalog.npcs.find(id.get<std::string>());
            if (npc == questCatalog.npcs.end() || entry == players.end() || entry->second.dungeonRoomId != 0) return false;
            const auto point = entry->second.player.GetPosition();
            return entry->second.mapId == npc->second.at("mapId").get<std::string>()
                && std::hypot(point.x - npc->second.at("x").get<float>(), point.y - npc->second.at("y").get<float>()) <= 96;
        };
        try
        {
            if (!work.retry)
            {
                if (work.command.contains("expectedRevision")) request.revision = Q::Decimal(work.command.at("expectedRevision"));
                if (request.revision == runtime->revision)
                {
                    proposed.Refresh(questCatalog, inventory, growth.level, map, now, cursor, action == "Refresh");
                    if (action == "Accept")
                    {
                        const auto id = work.command.at("questId").get<std::string>();
                        if (!questCatalog.definitions.contains(id)) status = "RequirementsNotMet";
                        else
                        {
                            const auto& definition = questCatalog.definitions.at(id);
                            if (work.command.at("cycleKey") != questCatalog.Cycle(definition, now)) status = "RequirementsNotMet";
                            else if (definition.at("accept").at("mode") == "Automatic") status = "RequirementsNotMet";
                            else if (definition.at("accept").at("mode") == "Npc" && !atNpc(definition.at("accept").at("npcId"))) status = "NotInRange";
                            else status = proposed.Accept(questCatalog, definition, growth.level, map, now, cursor);
                        }
                    }
                    else if (action == "Track")
                    {
                        std::vector<std::string> tracked;
                        for (const auto& id : work.command.at("trackedQuestIds"))
                        {
                            const bool live = std::any_of(proposed.instances.begin(), proposed.instances.end(), [&id](const auto& row)
                                { return row.second.at("questId") == id && Q::State::Live(row.second); });
                            if (!live) { status = "RequirementsNotMet"; break; } tracked.push_back(id.get<std::string>());
                        }
                        if (status == "Succeeded") proposed.tracked = std::move(tracked);
                    }
                    else if (action == "Talk")
                    {
                        if (!atNpc(work.command.at("npcId"))) status = "NotInRange";
                        else for (auto& [key, row] : proposed.instances)
                            proposed.Evaluate(questCatalog, row, inventory, now,
                                Json{{"kind", "TalkNpc"}, {"targetId", work.command.at("npcId")}});
                    }
                    else if (action == "Abandon" || action == "Report" || action == "Claim")
                    {
                        const auto key = Q::State::Key(work.command.at("questId"), work.command.at("cycleKey"));
                        if (!proposed.instances.contains(key)) status = "AlreadyCompleted";
                        else
                        {
                            auto& row = proposed.instances.at(key); const auto& definition = row.at("definition");
                            if (action == "Abandon")
                            {
                                if (!Q::State::Live(row)) status = "RequirementsNotMet";
                                else { row["state"] = "Abandoned"; row["stageIndex"] = 0;
                                    for (auto& step : row["counters"]) for (auto& count : step) count = 0; }
                            }
                            else if (!questCatalog.Within(definition, "reward", now)) status = "EventClosed";
                            else if (definition.at("complete").at("mode") == "NpcReport" && !atNpc(definition.at("complete").at("npcId"))) status = "NotInRange";
                            else if (action == "Report")
                            { if (row.at("state") != "ReadyToReport") status = "NotComplete"; else row["state"] = "RewardPending"; }
                            else if (row.at("state") != "RewardPending") status = "NotComplete";
                            else
                            {
                                auto rewards = definition.at("fixedRewards"); const auto choice = work.command.value("choiceId", std::string{});
                                if (!definition.at("choiceRewards").empty())
                                {
                                    const auto option = std::find_if(definition.at("choiceRewards").begin(), definition.at("choiceRewards").end(),
                                        [&choice](const auto& value) { return value.at("id") == choice; });
                                    if (option == definition.at("choiceRewards").end()) status = "InvalidChoice";
                                    else for (const auto& reward : option->at("rewards")) rewards.push_back(reward);
                                }
                                else if (!choice.empty()) status = "InvalidChoice";
                                if (status == "Succeeded") for (const auto& item : Q::State::TurnIn(definition))
                                { if (inventory.Consume(item.at("definitionId"), item.at("count")) != "Succeeded") { status = "RequirementsNotMet"; break; } }
                                std::map<std::string, std::uint32_t> acquired;
                                std::uint64_t experienceReward{};
                                if (status == "Succeeded") for (const auto& reward : rewards)
                                {
                                    if (reward.at("type") == "Item")
                                    {
                                        const auto id = reward.at("definitionId").get<std::string>(); const auto count = reward.at("count").get<std::uint32_t>();
                                        status = inventory.Acquire(itemsCatalog, id, count, [] { return Persistence::RandomHex(16); });
                                        if (status != "Succeeded") break; acquired[id] += count;
                                    }
                                    else if (reward.at("type") == "Experience")
                                    {
                                        const auto amount = ActionRPG::PlayerSkills::ExperienceValue(reward.at("amount"));
                                        if (amount > std::numeric_limits<std::uint64_t>::max() - experienceReward)
                                        { status = "RewardOverflow"; break; }
                                        experienceReward += amount;
                                    }
                                    else
                                    {
                                        const auto points = static_cast<std::uint64_t>(growth.skillPoints) + reward.at("amount").get<std::uint32_t>();
                                        if (points > std::numeric_limits<std::uint32_t>::max()) { status = "RewardOverflow"; break; }
                                        growth.skillPoints = static_cast<std::uint32_t>(points);
                                    }
                                }
                                if (status == "Succeeded" && !progressionPolicy.AddExperience(growth, experienceReward)) status = "RewardOverflow";
                                if (status == "Succeeded")
                                {
                                    row["state"] = "Completed"; request.mode = "Claim";
                                    for (const auto& [id, count] : acquired) for (auto& [other, target] : proposed.instances)
                                        proposed.Evaluate(questCatalog, target, inventory, now, Json{{"kind", "AcquireItem"}, {"targetId", id}, {"amount", count}});
                                }
                            }
                            if (status == "Succeeded") row["updatedAtMs"] = Q::Decimal(now);
                        }
                    }
                    else if (action == "Acquire")
                    {
                        const auto id = work.command.at("definitionId").get<std::string>(); const auto count = work.command.at("count").get<std::uint32_t>();
                        status = inventory.Acquire(itemsCatalog, id, count, [] { return Persistence::RandomHex(16); });
                        if (status == "Succeeded")
                        {
                            request.mode = "Inventory";
                            for (auto& [key, row] : proposed.instances) proposed.Evaluate(questCatalog, row, inventory, now,
                                Json{{"kind", "AcquireItem"}, {"targetId", id}, {"amount", count}});
                        }
                    }
                    else if (action == "RoomEvent" || action == "Location")
                    {
                        if (action != "RoomEvent" || std::find(work.event.at("creditedPlayerIds").begin(),
                            work.event.at("creditedPlayerIds").end(), Q::Decimal(runtime->playerId)) != work.event.at("creditedPlayerIds").end())
                            for (auto& [key, row] : proposed.instances) proposed.Evaluate(questCatalog, row, inventory, now, work.event);
                        if (action == "RoomEvent" && work.event.contains("experience")
                            && std::find(work.event.at("experiencePlayerIds").begin(), work.event.at("experiencePlayerIds").end(),
                                Q::Decimal(runtime->playerId)) != work.event.at("experiencePlayerIds").end())
                        {
                            request.mode = "Growth";
                            if (!progressionPolicy.AddExperience(growth, Q::Decimal(work.event.at("experience")))) status = "RewardOverflow";
                        }
                    }
                    else if (action == "LocationCheck")
                        for (auto& [key, row] : proposed.instances)
                        {
                            if (row.at("state") != "Active") continue;
                            const auto stage = row.at("stageIndex").get<std::size_t>();
                            std::size_t goalIndex{};
                            for (const auto& goal : row.at("definition").at("steps").at(stage).at("objectives"))
                            {
                                const auto current = row.at("counters").at(stage).at(goalIndex++).get<std::uint32_t>();
                                if (current >= goal.at("count").get<std::uint32_t>()) continue;
                                if (goal.at("type") != "Reach" || goal.at("target").at("mapId") != map) continue;
                                const auto& target = goal.at("target"); auto id = map; bool inside = true;
                                if (target.contains("region"))
                                {
                                    id = target.at("region").at("id").get<std::string>();
                                    const auto& region = target.at("region");
                                    inside = runtime->context.contains("x") && runtime->context.contains("y")
                                        && std::hypot(runtime->context.at("x").get<float>() - region.at("x").get<float>(),
                                            runtime->context.at("y").get<float>() - region.at("y").get<float>()) <= region.at("radius").get<float>();
                                }
                                if (inside)
                                {
                                    proposed.Evaluate(questCatalog, row, inventory, now,
                                        Json{{"kind", "RegionReached"}, {"mapId", map}, {"targetId", id}});
                                    if (row.at("stageIndex").get<std::size_t>() != stage && !cursor.is_null()) row["roomCursor"] = cursor;
                                    break;
                                }
                            }
                        }
                }
                if (status != "Succeeded") { finish(status); return; }
                proposed.Refresh(questCatalog, inventory, growth.level, map, now, cursor, false);
                const auto changes = proposed.Changes(runtime->state);
                if (changes.at("upserts").empty() && !changes.contains("trackedQuestIds") && request.mode == "Progress" && work.internal && action != "RoomEvent")
                { finish("Succeeded"); return; }
                request.operationJson = operation.dump(); request.changesJson = changes.dump();
                if (request.mode != "Progress") request.progression = growth.ToJson().dump();
                if (request.mode == "Claim" || request.mode == "Inventory") request.inventory = inventory.ToJson().dump();
                work.retry = request;
            }
            else request = *work.retry;
        }
        catch (...) { finish("InvalidRequest"); return; }
        runtime->busy = true;
        if (entry != players.end()) entry->second.saving = true;
        ++characterWork[runtime->sessionId];
        auto procedure = std::make_unique<Persistence::QuestStoreProcedure>(); procedure->req = std::move(request);
        RunStoreProcedure(std::move(procedure), [runtime](TownInstance& town, auto result)
        {
            runtime->busy = false;
            const auto found = town.players.find(runtime->playerId);
            if (found != town.players.end() && found->second.session->GetSessionId() == runtime->sessionId) found->second.saving = false;
            std::string status = "DatabaseUnavailable"; bool retry{};
            try
            {
                if (!result.error && result.response)
                {
                    const auto code = result.response->result;
                    status = code == 12 ? "QuestConflict" : code == 13 ? "AlreadyCompleted" : code == 14 ? "InvalidQuestState" : Persistence::ResultName(code);
                    if (code == 3 && result.response->revision > runtime->revision && !result.response->progression)
                    {
                        // Re-read the complete owned character through the existing status procedure.
                        // Its in-flight guard holds subsequent quest writes until the bag and growth agree.
                        const auto service = town.itemUseServices.find(runtime->sessionId);
                        if (service != town.itemUseServices.end()) service->second->Query({}, true);
                        else if (const auto session = runtime->session.lock()) session->Stop();
                    }
                    else if (code == 0 || code == 3 || code == 13) town.ApplyQuestResponse(*runtime, *result.response);
                    if (code == 5 || code == 1) if (const auto session = runtime->session.lock()) session->Stop();
                }
                retry = runtime->queue.front().command.at("action") == "RoomEvent" && (result.error || (result.response && result.response->result == 7));
                if (retry) runtime->nextRetry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                else if (result.error) if (const auto session = runtime->session.lock()) session->Stop();
            }
            catch (...) { if (const auto session = runtime->session.lock()) session->Stop(); }
            if (!retry)
            {
                auto done = std::move(runtime->queue.front().done); runtime->queue.pop_front();
                if (done) done(status);
                town.SendQuestState(runtime, "");
            }
            town.FinishCharacterWork(runtime->sessionId);
        });
    }

    void TownInstance::TickQuests()
    {
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [sessionId, runtime] : questRuntimes)
        {
            const auto entry = players.find(runtime->playerId);
            if (entry != players.end() && entry->second.session->GetSessionId() == sessionId)
            {
                if (entry->second.dungeonRoomId == 0)
                    runtime->context = {{"mapId", entry->second.mapId}, {"roomId", "0"}, {"cursor", nullptr}};
                if (runtime->loaded && runtime->queue.empty() && now >= runtime->nextRefresh && entry->second.session->GetAccountId() == runtime->accountId)
                {
                    runtime->nextRefresh = now + std::chrono::seconds(1);
                    QuestWork work; work.internal = true; work.requestId = Persistence::RandomHex(32); work.command = {{"action", "Refresh"}};
                    QueueQuest(runtime, std::move(work));
                    if (entry->second.dungeonRoomId != 0)
                    {
                        QuestWork location; location.internal = true; location.requestId = Persistence::RandomHex(32);
                        location.command = {{"action", "LocationCheck"}}; QueueQuest(runtime, std::move(location));
                    }
                }
                if (entry->second.dungeonRoomId == 0)
                {
                    const auto point = entry->second.player.GetPosition(); std::set<std::string> regions;
                    const auto check = [&](const Json& row)
                    {
                        if (row.at("state") != "Active") return;
                        const auto& quest = row.at("definition"); const auto stage = row.at("stageIndex").get<std::size_t>();
                        for (const auto& goal : quest.at("steps").at(stage).at("objectives"))
                        {
                            if (goal.at("type") != "Reach" || goal.at("target").at("mapId") != entry->second.mapId) continue;
                            const auto& target = goal.at("target"); std::string id = entry->second.mapId; bool inside = true;
                            if (target.contains("region"))
                            {
                                const auto& region = target.at("region"); id = region.at("id");
                                inside = std::hypot(point.x - region.at("x").get<float>(), point.y - region.at("y").get<float>()) <= region.at("radius").get<float>();
                            }
                            const auto key = Q::State::Key(row.at("questId"), row.at("cycleKey")) + ":"
                                + row.at("acceptGeneration").get<std::string>() + ":" + std::to_string(stage) + ":" + id;
                            if (!inside || !regions.insert(key).second || runtime->regions.contains(key)) continue;
                            if (runtime->queue.size() >= Q::MAX_QUEUE) { regions.erase(key); continue; }
                            QuestWork work; work.internal = true; work.requestId = Persistence::RandomHex(32); work.command = {{"action", "Location"}};
                            work.event = {{"kind", "RegionReached"}, {"mapId", entry->second.mapId}, {"targetId", id}}; QueueQuest(runtime, std::move(work));
                        }
                    };
                    for (const auto& [key, row] : runtime->state.instances) if (Q::State::Live(row)) check(row);
                    runtime->regions = std::move(regions);
                }
            }
            PumpQuest(runtime);
        }
    }

    void TownInstance::SendQuestState(const std::shared_ptr<QuestRuntime>& runtime, const std::string& requestId)
    {
        const auto session = runtime->session.lock(); if (!session || session->GetAccountId() != runtime->accountId || !runtime->loaded) return;
        const auto now = QuestNow(*runtime);
        auto rows = Json::array(); std::set<std::string> shown, items;
        const auto makeRow = [this, runtime, now, &items](const Json& definition, const Json* state)
        {
            const auto id = definition.at("id").get<std::string>(); const auto cycle = state ? state->at("cycleKey").get<std::string>() : questCatalog.Cycle(definition, now);
            const auto summary = runtime->state.summaries.find(id);
            const bool complete = (runtime->state.Completed(id) && definition.at("repeat") == "Once")
                || (summary != runtime->state.summaries.end() && summary->second.at("lastCompletedCycle") == cycle);
            const bool eligible = runtime->state.Eligible(questCatalog, definition, runtime->progression.level, runtime->context.value("mapId", defaultMapId), now);
            std::string phase = state ? state->at("state").get<std::string>() : complete ? "Completed" : eligible ? "Available" : "Unavailable";
            Json row{{"questId", id}, {"definitionVersion", definition.at("version")}, {"cycleKey", cycle},
                {"acceptGeneration", state ? state->at("acceptGeneration") : Json("0")}, {"state", phase},
                {"stageIndex", state ? state->at("stageIndex") : Json(complete ? definition.at("steps").size() : 0)}, {"objectives", Json::array()}, {"definition", definition},
                {"canAccept", !state && eligible && definition.at("accept").at("mode") != "Automatic"},
                {"canAbandon", state && Q::State::Live(*state)}, {"canReport", phase == "ReadyToReport" && questCatalog.Within(definition, "reward", now)},
                {"canClaim", phase == "RewardPending" && questCatalog.Within(definition, "reward", now)}, {"blockedReason", ""},
                {"turnInItems", Q::State::TurnIn(definition)}};
            const auto atNpc = [this, runtime](const Json& id)
            {
                if (id.is_null()) return false;
                const auto npc = questCatalog.npcs.find(id.get<std::string>()); const auto entry = players.find(runtime->playerId);
                if (npc == questCatalog.npcs.end() || entry == players.end() || entry->second.dungeonRoomId != 0
                    || entry->second.mapId != npc->second.at("mapId").get<std::string>()) return false;
                const auto point = entry->second.player.GetPosition();
                return std::hypot(point.x - npc->second.at("x").get<float>(), point.y - npc->second.at("y").get<float>()) <= 96;
            };
            if (definition.at("accept").at("mode") == "Npc" && row.at("canAccept") == true && !atNpc(definition.at("accept").at("npcId")))
            { row["canAccept"] = false; row["blockedReason"] = "NotInRange"; }
            if (definition.at("complete").at("mode") == "NpcReport" && !atNpc(definition.at("complete").at("npcId"))
                && (row.at("canReport") == true || row.at("canClaim") == true))
            { row["canReport"] = false; row["canClaim"] = false; row["blockedReason"] = "NotInRange"; }
            if (phase == "Unavailable") row["blockedReason"] = "RequirementsNotMet";
            const auto active = static_cast<std::size_t>(std::count_if(runtime->state.instances.begin(), runtime->state.instances.end(), [](const auto& row) { return Q::State::Live(row.second); }));
            if (row.at("canAccept") == true && active >= Q::MAX_ACTIVE) { row["canAccept"] = false; row["blockedReason"] = "QuestLimit"; }
            const auto stage = std::min<std::size_t>(row.at("stageIndex").get<std::size_t>(), definition.at("steps").size() - 1);
            const auto& goals = definition.at("steps").at(stage).at("objectives");
            for (std::size_t index = 0; index < goals.size(); ++index)
            {
                const auto count = state ? state->at("counters").at(stage).at(index).get<std::uint32_t>() : complete ? goals.at(index).at("count").get<std::uint32_t>() : 0;
                Json objective{{"id", goals.at(index).at("id")}, {"current", count}, {"required", goals.at(index).at("count")},
                    {"done", count >= goals.at(index).at("count").get<std::uint32_t>()}};
                if (goals.at(index).contains("description")) objective["description"] = goals.at(index).at("description");
                row["objectives"].push_back(std::move(objective));
            }
            for (const auto& item : row.at("turnInItems"))
            {
                const auto itemId = item.at("definitionId").get<std::string>(); items.insert(itemId);
                if (Q::State::Held(runtime->inventory, itemId) < item.at("count").get<std::uint32_t>())
                { row["canClaim"] = false; row["blockedReason"] = "RequirementsNotMet"; }
            }
            auto rewards = definition.at("fixedRewards"); for (const auto& choice : definition.at("choiceRewards")) for (const auto& reward : choice.at("rewards")) rewards.push_back(reward);
            for (const auto& reward : rewards) if (reward.at("type") == "Item") items.insert(reward.at("definitionId"));
            for (const auto& step : definition.at("steps")) for (const auto& goal : step.at("objectives"))
                if (goal.at("type") == "HoldItem" || goal.at("type") == "AcquireItem") items.insert(goal.at("target").at("itemDefinitionId"));
            return row;
        };
        for (const auto& [key, state] : runtime->state.instances)
        { if (Q::State::Live(state)) { rows.push_back(makeRow(state.at("definition"), &state)); shown.insert(key); } }
        for (const auto& [id, definition] : questCatalog.definitions)
            if (questCatalog.Within(definition, "visibility", now) && !shown.contains(Q::State::Key(id, questCatalog.Cycle(definition, now)))) rows.push_back(makeRow(definition, nullptr));
        auto npcs = Json::array();
        for (const auto& [id, definition] : questCatalog.npcs)
        {
            auto npc = definition; npc["canAccept"] = npc["canReport"] = npc["canClaim"] = false;
            for (const auto& row : rows)
            {
                if (row.at("definition").at("accept").at("npcId") == id && row.at("canAccept") == true) npc["canAccept"] = true;
                if (row.at("definition").at("complete").at("npcId") == id)
                { if (row.at("canReport") == true) npc["canReport"] = true; if (row.at("canClaim") == true) npc["canClaim"] = true; }
            }
            npcs.push_back(std::move(npc));
        }
        const auto snapshotId = Persistence::RandomHex(32);
        Json base{{"requestId", requestId}, {"snapshotId", snapshotId}, {"characterId", Q::Decimal(runtime->characterId)},
            {"revision", Q::Decimal(runtime->revision)}, {"serverNowMs", Q::Decimal(now)}, {"pageIndex", 0}, {"pageCount", 48},
            {"trackedQuestIds", runtime->state.tracked}, {"quests", Json::array()}, {"npcs", Json::array()}, {"itemDefinitions", Json::array()}};
        std::vector<Json> pages{base};
        const auto append = [&pages, &base](const char* key, const Json& value)
        {
            auto candidate = pages.back(); candidate[key].push_back(value);
            if (candidate.dump().size() > Q::MAX_PAGE_BYTES) { pages.push_back(base); pages.back()[key].push_back(value); }
            else pages.back() = std::move(candidate);
        };
        for (const auto& row : rows) append("quests", row);
        for (const auto& npc : npcs) append("npcs", npc);
        for (const auto& id : items) if (itemsCatalog.presentations.contains(id)) append("itemDefinitions", itemsCatalog.presentations.at(id));
        std::size_t bytes{};
        for (std::size_t index = 0; index < pages.size(); ++index)
        { pages[index]["pageIndex"] = index; pages[index]["pageCount"] = pages.size(); bytes += pages[index].dump().size(); }
        if (pages.size() > 48 || bytes > 512 * 1024) { session->Stop(); return; }
        for (const auto& page : pages)
        { const auto text = page.dump(); if (text.size() > Q::MAX_PAGE_BYTES) { session->Stop(); return; } session->Send(TownProtocol::Encode(TownProtocol::QuestStatePage{text})); }
    }

    void TownInstance::QuestEvents(std::uint64_t roomId, std::string text, std::function<void(bool)> done)
    {
        const auto self = shared_from_this();
        asio::post(strand, [self, roomId, text = std::move(text), done = std::move(done)]() mutable
        {
            try
            {
                const auto body = Json::parse(text); Q::Rules::Keys(body, {"events"});
                Q::Rules::Require(text.size() <= 16384 && body.at("events").is_array() && !body.at("events").empty() && body.at("events").size() <= 32, "Invalid quest event batch.");
                struct Completion { std::size_t remaining{}; bool succeeded{true}; std::function<void(bool)> done; };
                auto completion = std::make_shared<Completion>(); completion->done = done;
                std::vector<std::pair<std::shared_ptr<QuestRuntime>, QuestWork>> work;
                for (const auto& event : body.at("events"))
                {
                    Q::Rules::Keys(event, {"incarnation", "sequence", "dungeonId", "mapId", "kind", "targetId", "creditedPlayerIds"},
                        {"monsterInstanceId", "experience", "experiencePlayerIds", "rewardPolicyVersion"});
                    Q::Rules::Require(ActionRPG::Items::IsHexId(event.at("incarnation"), 32), "Invalid room incarnation.");
                    Q::Decimal(event.at("sequence"), false); Q::Id(event.at("mapId"));
                    Q::Rules::Integer(event.at("dungeonId"), 1, 1000000);
                    Q::Rules::Require(event.at("kind") == "MonsterKilled" || event.at("kind") == "RegionReached" || event.at("kind") == "DungeonCleared", "Invalid quest fact.");
                    Q::Rules::Require(event.at("creditedPlayerIds").is_array() && event.at("creditedPlayerIds").size() <= 4, "Invalid quest credit.");
                    std::set<PlayerId> unique;
                    for (const auto& player : event.at("creditedPlayerIds"))
                    {
                        const auto id = Q::Decimal(player, false); Q::Rules::Require(unique.insert(id).second, "Duplicate quest credit.");
                    }
                    const bool hasExperience = event.contains("experience");
                    Q::Rules::Require(hasExperience == event.contains("experiencePlayerIds")
                        && hasExperience == event.contains("rewardPolicyVersion"), "Incomplete experience fact.");
                    if (hasExperience)
                    {
                        Q::Rules::Require(event.at("kind") == "MonsterKilled" || event.at("kind") == "DungeonCleared", "Invalid experience fact kind.");
                        Q::Decimal(event.at("experience"));
                        Q::Rules::Integer(event.at("rewardPolicyVersion"), 1, 1000000);
                        Q::Rules::Require(event.at("experiencePlayerIds").is_array()
                            && event.at("experiencePlayerIds").size() <= 4, "Invalid experience recipients.");
                        std::set<PlayerId> experienceRecipients;
                        for (const auto& player : event.at("experiencePlayerIds"))
                        {
                            const auto id = Q::Decimal(player, false);
                            Q::Rules::Require(experienceRecipients.insert(id).second, "Duplicate experience credit.");
                            unique.insert(id);
                        }
                    }
                    Q::Rules::Require(unique.size() <= 4, "Too many Room fact recipients.");
                    for (const auto id : unique)
                    {
                        const auto runtime = std::find_if(self->questRuntimes.begin(), self->questRuntimes.end(),
                            [id](const auto& pair) { return pair.second->playerId == id; });
                        if (runtime == self->questRuntimes.end()) { completion->succeeded = false; continue; }
                        const auto entry = self->players.find(id);
                        if (entry == self->players.end() && runtime->second->detachedRoomId != roomId)
                        { completion->succeeded = false; continue; }
                        if (entry != self->players.end() && entry->second.dungeonRoomId != roomId && entry->second.reservedDungeonRoomId != roomId)
                        { completion->succeeded = false; continue; }
                        QuestWork task; task.internal = true; task.requestId = Persistence::RandomHex(32);
                        task.command = {{"action", "RoomEvent"}, {"originIncarnation", event.at("incarnation")},
                            {"roomId", Q::Decimal(roomId)}, {"eventSeq", event.at("sequence")}, {"event", event}};
                        task.event = event; task.event["cursor"] = {{"incarnation", event.at("incarnation")},
                            {"roomId", Q::Decimal(roomId)}, {"sequence", event.at("sequence")}};
                        task.done = [completion](std::string result)
                        { if (result != "Succeeded") completion->succeeded = false; if (--completion->remaining == 0) completion->done(completion->succeeded); };
                        work.emplace_back(runtime->second, std::move(task));
                    }
                }
                completion->remaining = work.size();
                if (work.empty()) { completion->done(completion->succeeded); return; }
                for (auto& [runtime, task] : work) self->QueueQuest(runtime, std::move(task));
            }
            catch (...) { if (done) done(false); }
        });
    }
}
