#include "GameRoom.h"
#include "ItemUseAction.h"
#include <cmath>

namespace GameRoomServer
{
    void GameRoom::CanRetire(std::function<void(bool)> inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, handler = std::move(inHandler)]()
        {
            handler(std::all_of(self->itemUses.begin(), self->itemUses.end(),
                [](const auto& row) { return row.second.settled; }));
        });
    }

    /** A living room keeps every receipt/tombstone. Only committed DB terminal confirmation settles a receipt. */
    void GameRoom::ItemUse(PlayerId inPlayerId, nlohmann::json inRequest,
        std::function<void(nlohmann::json)> inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, request = std::move(inRequest), handler = std::move(inHandler)]() mutable
        {
            using Json = nlohmann::json;
            using Rules = ActionRPG::PlayerSkills::Catalog;
            Json reply{{"status", "Unknown"}, {"roomIncarnation", self->itemUseIncarnation}};
            try
            {
                const auto kind = request.at("kind").get<std::string>();
                const auto id = request.at("requestId").get<std::string>();
                const auto characterId = ActionRPG::Items::DecimalId(request.at("characterId"));
                const auto generation = ActionRPG::Items::DecimalId(request.at("ownerGeneration"));
                Rules::Require(ActionRPG::Items::IsHexId(id, 64), "Invalid room use identity.");
                auto found = self->itemUses.find(id);
                if (kind != "Prepare" && request.at("roomIncarnation") != self->itemUseIncarnation)
                { handler(reply); return; }
                if (kind == "Fence")
                {
                    auto& fence = self->itemUseFences[characterId]; fence = std::max(fence, generation);
                    for (auto& [playerId, player] : self->players)
                        if (player.persistentCharacterId == characterId && player.ownerGeneration < generation)
                        { player.worldReady = false; self->enteredPlayers.erase(playerId); }
                    for (auto& [key, row] : self->itemUses)
                        if (row.characterId == characterId && row.generation < generation && row.state == "Prepared")
                        { row.state = "Cancelled"; row.reason = "OwnerFenced"; }
                    reply["status"] = "Fenced"; handler(reply); return;
                }
                if (found != self->itemUses.end() && (found->second.characterId != characterId
                    || found->second.generation != generation || found->second.playerId != inPlayerId))
                { reply["status"] = "RequestConflict"; handler(reply); return; }
                if (kind == "Prepare")
                {
                    const auto operation = request.at("operation").dump();
                    if (found != self->itemUses.end())
                    {
                        reply["status"] = found->second.operation == operation ? found->second.state : "RequestConflict";
                        handler(reply); return;
                    }
                    const auto player = self->players.find(inPlayerId);
                    if (player == self->players.end() || !self->enteredPlayers.contains(inPlayerId)
                        || !player->second.worldReady || player->second.persistentCharacterId != characterId
                        || player->second.ownerGeneration != generation || self->itemUseFences[characterId] > generation)
                    { reply["status"] = "StaleOwner"; handler(reply); return; }
                    if (self->itemUseCounts[characterId] >= 4096)
                    { reply["status"] = "UseLedgerFull"; handler(reply); return; }
                    const auto catalog = ActionRPG::Items::Catalog::Parse({{"format", "Items"}, {"schemaVersion", 1},
                        {"items", Json::array({request.at("definition")})}});
                    const auto& definition = catalog.definitions.begin()->second;
                    if (definition.use.type == ActionRPG::Items::Definition::Use::Type::None)
                    { reply["status"] = "NotUsable"; handler(reply); return; }
                    for (const auto& [key, row] : self->itemUses)
                        if (row.characterId == characterId && !row.settled && row.state != "Cancelled")
                        { reply["status"] = "Busy"; handler(reply); return; }
                    const auto deadline = player->second.itemCooldowns.find(definition.id);
                    if (deadline != player->second.itemCooldowns.end() && deadline->second > std::chrono::steady_clock::now())
                    { reply["status"] = "Cooldown"; handler(reply); return; }
                    auto& actor = player->second;
                    const bool throwReady = actor.actor.height == 0 && !actor.jumpPreparing
                        && actor.actor.reaction == Reaction::None && actor.actor.hitstopRemainingSeconds <= 0
                        && !actor.skill && !actor.slide.active && !IsShotFacingLocked(actor)
                        && actor.itemUseElapsedSeconds >= actor.itemUseDurationSeconds && self->projectiles.size() < 256;
                    ItemUseContext context{actor.actor.hp, self->combatDefinition->playerMaxHp, actor.progression.level,
                        self->state == State::Running && !self->clearRequested, throwReady, {}};
                    RecoveryItemAction recovery; ThrowItemAction throwing;
                    const ItemUseAction& action = definition.use.type == ActionRPG::Items::Definition::Use::Type::Recovery
                        ? static_cast<const ItemUseAction&>(recovery) : static_cast<const ItemUseAction&>(throwing);
                    const auto status = action.Validate(context, definition.use);
                    if (std::string_view(status) != "Succeeded")
                    { reply["status"] = status; handler(reply); return; }
                    ItemUseRecord row;
                    row.playerId = inPlayerId; row.characterId = characterId; row.generation = generation;
                    row.operation = operation; row.definitionId = definition.id; row.use = definition.use;
                    row.facingLeft = request.at("operation").value("facingLeft", false);
                    self->itemUses.emplace(id, std::move(row)); ++self->itemUseCounts[characterId];
                    reply["status"] = "Prepared"; handler(reply); return;
                }
                if (found == self->itemUses.end())
                {
                    // Absence alone is not a cancellation receipt: never authorize a DB refund here.
                    handler(reply); return;
                }
                auto& row = found->second;
                const auto hash = request.at("executionHash").get<std::string>();
                Rules::Require(ActionRPG::Items::IsHexId(hash, 64), "Invalid use execution hash.");
                if (!row.executionHash.empty() && row.executionHash != hash)
                { reply["status"] = "RequestConflict"; handler(reply); return; }
                row.executionHash = hash;
                if (kind == "Settle")
                {
                    const auto terminal = request.at("useState").get<std::string>();
                    if ((terminal == "Applied" && row.state == "Applied")
                        || (terminal == "Cancelled" && row.state == "Cancelled") || terminal == "ConsumedUnknown")
                        row.settled = true;
                }
                else if (kind == "Cancel" && row.state == "Prepared")
                { row.state = "Cancelled"; row.reason = "CancelledBeforeEffect"; }
                else if (kind == "Execute" && row.state == "Prepared")
                {
                    const auto player = self->players.find(inPlayerId);
                    if (player == self->players.end() || !self->enteredPlayers.contains(inPlayerId)
                        || !player->second.worldReady || player->second.ownerGeneration != generation
                        || self->itemUseFences[characterId] > generation)
                    { row.state = "Cancelled"; row.reason = "OwnerUnavailable"; }
                    else
                    {
                        auto& actor = player->second;
                        const bool isThrow = row.use.type == ActionRPG::Items::Definition::Use::Type::Throw;
                        const bool throwReady = actor.actor.height == 0 && !actor.jumpPreparing
                            && actor.actor.reaction == Reaction::None && actor.actor.hitstopRemainingSeconds <= 0
                            && !actor.skill && !actor.slide.active && !IsShotFacingLocked(actor)
                            && actor.itemUseElapsedSeconds >= actor.itemUseDurationSeconds && self->projectiles.size() < 256;
                        ItemUseContext context{actor.actor.hp, self->combatDefinition->playerMaxHp, actor.progression.level,
                            self->state == State::Running && !self->clearRequested, throwReady, {}};
                        RecoveryItemAction recovery; ThrowItemAction throwing;
                        const ItemUseAction& action = isThrow ? static_cast<const ItemUseAction&>(throwing)
                            : static_cast<const ItemUseAction&>(recovery);
                        const auto status = action.Validate(context, row.use);
                        if (std::string_view(status) != "Succeeded") { row.state = "Cancelled"; row.reason = status; }
                        else
                        {
                            // Prepare every allocation before changing HP or inserting the projectile.
                            ProjectileState projectile;
                            if (isThrow)
                            {
                                const float direction = row.facingLeft ? -1.0f : 1.0f;
                                projectile.id = self->nextProjectileId; projectile.ownerId = inPlayerId;
                                projectile.mapId = actor.mapId; projectile.position = actor.position;
                                projectile.position.x += direction * row.use.spawnForward;
                                projectile.height = row.use.spawnHeight; projectile.direction = direction;
                                projectile.remainingDistance = row.use.range; projectile.speed = row.use.speed;
                                projectile.radius = row.use.radius; projectile.damage = row.use.damage;
                                projectile.hitstopSeconds = row.use.hitstopSeconds;
                                projectile.itemDefinitionId = row.definitionId; projectile.visual = "Stone";
                                const auto& map = self->dungeonWorld.at("maps").at(actor.mapId);
                                bool spawnValid = true;
                                const auto steps = std::max(1, static_cast<int>(std::ceil(row.use.spawnForward / 4)));
                                for (int index = 0; index <= steps && spawnValid; ++index)
                                {
                                    const float fraction = static_cast<float>(index) / steps;
                                    spawnValid = DungeonDefinition::Movable(map, {actor.position.x
                                        + (projectile.position.x - actor.position.x) * fraction, actor.position.y});
                                }
                                if (!spawnValid)
                                { row.state = "Cancelled"; row.reason = "SpawnBlocked"; }
                                self->projectiles.reserve(self->projectiles.size() + 1);
                                context.spawnPreparedProjectile = [&]() { self->projectiles.push_back(std::move(projectile));
                                    ++self->nextProjectileId; };
                            }
                            if (row.state == "Prepared")
                            {
                                std::string definitionId = row.definitionId, motion = row.use.motionId;
                                auto& deadline = actor.itemCooldowns[row.definitionId];
                                // Mark conservatively before DoAction: a later exception never produces a refund receipt.
                                row.state = "Applied";
                                action.DoAction(context, row.use);
                                deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(
                                    static_cast<std::int64_t>(std::ceil(row.use.cooldownSeconds * 1000)));
                                actor.itemUseDefinitionId.swap(definitionId); actor.itemUseMotionId.swap(motion);
                                ++actor.itemUseSequence; actor.itemUseElapsedSeconds = 0;
                                actor.itemUseDurationSeconds = row.use.motionDurationSeconds;
                                if (isThrow) actor.facingLeft = row.facingLeft;
                                self->PublishRealtime();
                            }
                        }
                    }
                }
                reply["status"] = row.state; reply["reason"] = row.reason;
            }
            catch (const std::exception&) { /* Unknown is deliberately never a NotExecuted receipt. */ }
            handler(std::move(reply));
        });
    }
}
