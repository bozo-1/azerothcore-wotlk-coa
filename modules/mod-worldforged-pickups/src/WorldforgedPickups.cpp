/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

/*
 * Worldforged pickups - the CoA world object that hands out a Worldforged base item,
 * which every character may loot once and then never again.
 *
 * The data (Database/Custom/worldforged-pickups.sql) restores the pickups themselves:
 * 1,555 objects - bags, buckets, bones, packets, caches - each one named after the base
 * item it holds, each holding exactly that item, spawned at the position the community
 * observed that object at. Two deliberate deviations from the captures support the rule:
 *
 *   * chest.consumable = 0, so a pickup stays spawned after it is emptied instead of
 *     despawning on a respawn timer (which would make it a realm-wide roll per respawn).
 *     The core already re-rolls a non-consumable chest's loot for the next opener:
 *     Player::SendLoot clears and fills while the object is GO_READY, and
 *     GameObject::Update returns it to GO_READY after a loot is released.
 *   * ScriptName 'worldforged_pickup', the marker this module keys on. The captures have
 *     an empty ScriptName.
 *
 * This module supplies what data cannot: the per-character, permanent memory.
 *
 *   * 'looted' is remembered per (character, spawn) in the characters database table
 *     character_worldforged_loot and in memory for the session.
 *   * The ledger is read in Player::LoadFromDB, not on login. This matters: the core
 *     sends a player the gameobjects around them before CharacterHandler calls
 *     OnPlayerLogin, so a ledger loaded that late arrives after the client has already
 *     been told the pickup is sparkling and lootable - and nothing corrects it, because
 *     the object's own state never changes. Loading during LoadFromDB is early enough
 *     that the first values update a player receives already carries the right flags.
 *   * A pickup the character already looted stays in the world - as it does on the realm -
 *     but is inert for that character alone: no sparkle, not selectable, not interactable
 *     (GameObjectAI::BuildClientFlags). Every other character still sees it sparkling.
 *   * A pickup the character may still loot sparkles
 *     (GO_DYNFLAG_LO_ACTIVATE | GO_DYNFLAG_LO_SPARKLE), which is how they are found.
 *   * Server-side the same rule is enforced twice, because a client can always ask:
 *     GameObjectAI::GossipHello refuses the use, and OnStateChanged opens nothing. The
 *     state hook also repairs a pickup left GO_ACTIVATED with spent loot (someone closed
 *     the loot window by logging out), which would otherwise show the next character an
 *     empty window instead of their own item.
 *
 * Identity: a pickup is recorded by its *spawn id* (the `gameobject`.`guid` row), never by
 * the runtime object GUID. This core hands out map-local generated GUIDs
 * (Map::GenerateLowGuid), so a runtime GUID is neither the database row nor stable across
 * grid reloads - keying on it would lose or invent history. The restoration writes its
 * spawns in a fixed guid block, so the ids are stable across re-imports too.
 */

#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "Item.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "World.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace
{
// ScriptName on gameobject_template and gameobject rows of every pickup.
constexpr char const* WorldforgedPickupScript = "worldforged_pickup";
constexpr char const* WorldforgedLootTable = "character_worldforged_loot";

[[nodiscard]] bool IsWorldforgedPickup(GameObject const* go)
{
    if (!go || !go->GetGOInfo() || !go->GetSpawnId())
        return false;

    uint32 const scriptId = go->GetScriptId();
    return scriptId != 0 && sObjectMgr->GetScriptName(scriptId) == WorldforgedPickupScript;
}

// Per-character record of the pickups already looted, backed by the characters database.
class WorldforgedLootStore
{
public:
    static WorldforgedLootStore& Instance()
    {
        static WorldforgedLootStore store;
        return store;
    }

    [[nodiscard]] bool HasLooted(uint32 characterGuid, uint32 spawnId) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto itr = _looted.find(characterGuid);
        return itr != _looted.end() && itr->second.count(spawnId) != 0;
    }

    // Called from Player::LoadFromDB, before the player can be sent a single gameobject.
    // The whole ledger for a character is small (one row per pickup looted so far) and is
    // read once, not per pickup.
    void Load(uint32 characterGuid)
    {
        std::unordered_set<uint32> looted;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT `spawn_id` FROM `{}` WHERE `guid` = {}", WorldforgedLootTable, characterGuid))
        {
            do
            {
                looted.insert((*result)[0].Get<uint32>());
            } while (result->NextRow());
        }

        std::lock_guard<std::mutex> lock(_mutex);
        _looted[characterGuid] = std::move(looted);
    }

    void Unload(uint32 characterGuid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _looted.erase(characterGuid);
    }

    void Record(uint32 characterGuid, uint32 spawnId, uint32 entry)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_looted[characterGuid].insert(spawnId).second)
                return;                                     // already known, nothing to write
        }

        CharacterDatabase.Execute(
            "INSERT IGNORE INTO `{}` (`guid`, `spawn_id`, `entry`) VALUES ({}, {}, {})",
            WorldforgedLootTable, characterGuid, spawnId, entry);
    }

private:
    WorldforgedLootStore() = default;

    mutable std::mutex _mutex;
    std::unordered_map<uint32, std::unordered_set<uint32>> _looted;
};

class WorldforgedPickupAI : public GameObjectAI
{
public:
    explicit WorldforgedPickupAI(GameObject* go) : GameObjectAI(go) { }

    // Sparkle marks a pickup this viewer may still loot; a viewer who already has must see
    // it as inert scenery - visible, like on the realm, but not openable.
    void BuildClientFlags(Player const* target, uint16& dynFlags, uint32& goFlags) override
    {
        if (!target)
            return;

        if (WorldforgedLootStore::Instance().HasLooted(target->GetGUID().GetCounter(), me->GetSpawnId()))
        {
            goFlags |= GO_FLAG_LOCKED | GO_FLAG_NOT_SELECTABLE;
            return;
        }

        if (sWorld->getBoolConfig(CONFIG_OBJECT_SPARKLES))
            dynFlags |= GO_DYNFLAG_LO_ACTIVATE | GO_DYNFLAG_LO_SPARKLE;
    }

    // The server-side refusal: a client that still has a spent pickup in memory (or a
    // macro) gets nothing to open.
    bool GossipHello(Player* player, bool /*reportUse*/) override
    {
        if (!player)
            return true;

        if (!WorldforgedLootStore::Instance().HasLooted(player->GetGUID().GetCounter(), me->GetSpawnId()))
            return false;

        if (me->loot.isLooted())
        {
            me->loot.clear();
            player->SendLootRelease(me->GetGUID());
        }

        return true;
    }

    void OnStateChanged(uint32 state, Unit* unit) override
    {
        if (state != GO_ACTIVATED || !unit)
            return;

        Player* player = unit->ToPlayer();
        if (!player)
            return;

        uint32 const characterGuid = player->GetGUID().GetCounter();
        uint32 const spawnId = me->GetSpawnId();

        // Spent for this character: hand out nothing, and do not leave an open loot.
        if (WorldforgedLootStore::Instance().HasLooted(characterGuid, spawnId))
        {
            if (me->loot.isLooted())
            {
                me->loot.clear();
                player->SendLootRelease(me->GetGUID());
            }
            return;
        }

        // Spent for someone else and left behind - a loot window closed by a logout never
        // released the object, so it is still GO_ACTIVATED with an empty loot. Re-arm it so
        // this character gets their own roll instead of an empty window. The flag guards the
        // nested state change this triggers: one re-arm per interaction, never a loop.
        if (!_rearming && me->loot.isLooted())
        {
            _rearming = true;
            me->loot.clear();
            me->SetLootState(GO_READY);
            player->SendLoot(me->GetGUID(), LOOT_CORPSE);
            _rearming = false;
        }
    }

private:
    bool _rearming = false;              // an empty loot table can never spin this
};

// The marker itself: the database says which gameobjects are pickups, this says what they do.
class worldforged_pickup_script : public GameObjectScript
{
public:
    worldforged_pickup_script() : GameObjectScript(WorldforgedPickupScript) { }

    GameObjectAI* GetAI(GameObject* go) const override
    {
        return new WorldforgedPickupAI(go);
    }
};

class worldforged_pickup_lifecycle : public PlayerScript
{
public:
    worldforged_pickup_lifecycle() : PlayerScript("worldforged_pickup_lifecycle",
        {PLAYERHOOK_ON_LOAD_FROM_DB, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_LOOT_ITEM}) { }

    // Early enough that no gameobject has been sent to this client yet.
    void OnPlayerLoadFromDB(Player* player) override
    {
        WorldforgedLootStore::Instance().Load(player->GetGUID().GetCounter());
    }

    void OnPlayerLogout(Player* player) override
    {
        WorldforgedLootStore::Instance().Unload(player->GetGUID().GetCounter());
    }

    // The moment the base item leaves a pickup, that pickup is spent for this character -
    // whether it was clicked, auto-stored or taken through the group window.
    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid lootguid) override
    {
        if (!player || !item || !lootguid.IsGameObject())
            return;

        GameObject* go = player->GetMap()->GetGameObject(lootguid);
        if (!IsWorldforgedPickup(go))
            return;

        WorldforgedLootStore::Instance().Record(player->GetGUID().GetCounter(), go->GetSpawnId(),
                                                go->GetEntry());

        // Make this client re-read the flags, so the pickup goes inert for this viewer now.
        go->ForceValuesUpdateAtIndex(GAMEOBJECT_FLAGS);
        go->ForceValuesUpdateAtIndex(GAMEOBJECT_DYNAMIC);
    }
};
}

void AddWorldforgedPickupsScripts()
{
    new worldforged_pickup_script();
    new worldforged_pickup_lifecycle();
}
