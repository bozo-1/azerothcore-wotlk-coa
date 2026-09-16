/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "GossipDef.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "SpellScript.h"
#include <array>

namespace
{
enum TravelPermit : uint32
{
    ItemTravelPermit = 977028,
    SenderTravelPermit = 977028,
    MaxTravelLevel = 8
};

struct Destination
{
    char const* name;
    TeamId team;
    uint8 race;
};

constexpr std::array<Destination, 6> Destinations =
{{
    {"Elwynn Forest", TEAM_ALLIANCE, RACE_HUMAN},
    {"Dun Morogh", TEAM_ALLIANCE, RACE_DWARF},
    {"Teldrassil", TEAM_ALLIANCE, RACE_NIGHTELF},
    {"Tirisfal Glades", TEAM_HORDE, RACE_UNDEAD_PLAYER},
    {"Durotar", TEAM_HORDE, RACE_ORC},
    {"Mulgore", TEAM_HORDE, RACE_TAUREN}
}};

SpellCastResult CheckTravel(Player const* player)
{
    if (!player || !player->IsAlive())
        return SPELL_FAILED_CASTER_DEAD;
    if (player->GetLevel() > MaxTravelLevel)
        return SPELL_FAILED_HIGHLEVEL;
    if (player->IsInCombat())
        return SPELL_FAILED_AFFECTING_COMBAT;
    return SPELL_CAST_OK;
}

class spell_ascension_travel_permit : public SpellScript
{
    PrepareSpellScript(spell_ascension_travel_permit);

    bool Load() override
    {
        return GetCaster()->ToPlayer() && GetCastItem() && GetCastItem()->GetEntry() == ItemTravelPermit;
    }

    SpellCastResult CheckCast()
    {
        return CheckTravel(GetCaster()->ToPlayer());
    }

    void OpenMenu()
    {
        Player* player = GetCaster()->ToPlayer();
        Item* item = GetCastItem();
        if (!item || CheckTravel(player) != SPELL_CAST_OK)
            return;
        ClearGossipMenuFor(player);
        for (uint32 i = 0; i < Destinations.size(); ++i)
            if (Destinations[i].team == player->GetTeamId() &&
                sObjectMgr->GetPlayerInfo(Destinations[i].race, player->getClass()))
                AddGossipItemFor(player, GOSSIP_ICON_TAXI, Destinations[i].name, SenderTravelPermit, i);
        // The native item cast has already passed its cooldown checks and started the five-minute cooldown.
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, item->GetGUID());
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ascension_travel_permit::CheckCast);
        // AfterCast, not an effect handler. 1001088 is an Ascension client spell, so its recorded
        // effects and implicit targets are not ours to rely on: an effect handler is skipped whenever
        // the spell's own effect differs from the one it is bound to (the core only logs "did not match
        // dbc effect data") or the cast resolves no unit target, which leaves the permit starting its
        // cooldown and opening nothing. AfterCast runs once per cast that actually went through,
        // whatever the effects are, and only after the cooldown is committed -- a cast rejected by
        // CheckCast or by the item cooldown never reaches it.
        AfterCast += SpellCastFn(spell_ascension_travel_permit::OpenMenu);
    }
};

class item_ascension_travel_permit : public ItemScript
{
public:
    item_ascension_travel_permit() : ItemScript("item_ascension_travel_permit") { }

    void OnGossipSelect(Player* player, Item* item, uint32 sender, uint32 action) override
    {
        if (!player || !item || item->GetEntry() != ItemTravelPermit || sender != SenderTravelPermit)
            return;
        CloseGossipMenuFor(player);
        ClearGossipMenuFor(player);
        if (action >= Destinations.size() || CheckTravel(player) != SPELL_CAST_OK)
            return;
        Destination const& destination = Destinations[action];
        if (destination.team != player->GetTeamId())
            return;
        if (PlayerInfo const* start = sObjectMgr->GetPlayerInfo(destination.race, player->getClass()))
            player->TeleportTo(start->mapId, start->positionX, start->positionY, start->positionZ, start->orientation);
    }
};
}

void AddAscensionTravelPermitScripts()
{
    RegisterSpellScript(spell_ascension_travel_permit);
    new item_ascension_travel_permit();
}
