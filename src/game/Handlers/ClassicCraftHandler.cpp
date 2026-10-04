/*
 * classiccraft: Minecraft <-> WoW combat crossover (fork only). See ClassicCraft.h.
 */

#include "ClassicCraft.h"
#include "Common.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Creature.h"
#include "Map.h"
#include "CreatureAI.h"
#include "CreatureAIImpl.h"
#include "MotionMaster.h"
#include "TemporarySummon.h"
#include "SocialMgr.h"
#include "Database/DatabaseEnv.h"
#include "LootMgr.h"
#include "Bag.h"
#include "ObjectMgr.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // Proxies are never wounded here (their health is Minecraft's); this is only what WoW sees.
    constexpr uint32 PROXY_MAX_HEALTH = 1000;
    // A hit or an actor farther than this from the player is ignored (yards).
    constexpr float MAX_REACH = 100.0f;

    struct State
    {
        std::mutex lock;
        std::unordered_set<uint64> bridged;                                  // player guids
        std::unordered_map<uint64, std::unordered_map<uint32, uint64>> proxies; // player -> mcId -> proxy
        std::unordered_map<uint64, std::pair<uint64, uint32>> owners;        // proxy -> (player, mcId)
    };

    State& state()
    {
        static State s;
        return s;
    }

    // Set while the server itself must hurt a bridged unit (Minecraft killed the player).
    thread_local bool t_bypass = false;

    // Held kill XP (HoldKillXP): per player, per drop id.
    struct XpDrop
    {
        uint32 remaining;
        uint64 victim;
        uint32 createdMs;
    };
    // Past this a drop is forgotten: Minecraft despawns an orb after 5 minutes.
    constexpr uint32 XP_DROP_LIFE_MS = 10 * MINUTE * IN_MILLISECONDS;
    struct XpLedger
    {
        std::mutex lock;
        uint32 nextId = 1;
        std::unordered_map<uint64, std::unordered_map<uint32, XpDrop>> drops; // player -> id -> drop
    };
    XpLedger& ledger()
    {
        static XpLedger l;
        return l;
    }
    // The victim of the claim being granted (ClaimedKill).
    thread_local uint64 t_claimVictim = 0;

    uint32 EntryFor(uint8 kind)
    {
        switch (kind)
        {
            case ClassicCraft::ACTOR_HOSTILE: return ClassicCraft::PROXY_HOSTILE;
            case ClassicCraft::ACTOR_PASSIVE: return ClassicCraft::PROXY_PASSIVE;
            case ClassicCraft::ACTOR_COMPANION: return ClassicCraft::PROXY_COMPANION;
            default: return 0;
        }
    }

    void Despawn(Map* map, uint64 proxyGuid)
    {
        if (!map)
            return;
        Creature* c = map->GetCreature(ObjectGuid(proxyGuid));
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[classiccraft] proxy %s despawn%s", ObjectGuid(proxyGuid).GetString().c_str(),
            c ? "" : ": NOT FOUND");
        if (!c)
            return;
        // UnSummon removes it; ForcedDespawn would only kill it, and a dead summon lingers (and
        // respawns where it stood).
        if (c->IsTemporarySummon())
            static_cast<TemporarySummon*>(c)->UnSummon();
        else
            c->AddObjectToRemoveList();
    }
}

uint32 ClassicCraft::ProxyEntry(WorldObject const* obj)
{
    if (!obj || !obj->IsCreature())
        return 0;
    uint32 entry = obj->GetEntry();
    return (entry == PROXY_HOSTILE || entry == PROXY_PASSIVE || entry == PROXY_COMPANION) ? entry : 0;
}

bool ClassicCraft::IsBridged(Unit const* unit)
{
    if (!unit || !unit->IsPlayer())
        return false;
    State& s = state();
    std::lock_guard<std::mutex> g(s.lock);
    return s.bridged.count(unit->GetObjectGuid().GetRawValue()) != 0;
}

void ClassicCraft::SetBridged(Player* player, bool on)
{
    bool turnedOn = false;
    {
        State& s = state();
        std::lock_guard<std::mutex> g(s.lock);
        if (on)
            turnedOn = s.bridged.insert(player->GetObjectGuid().GetRawValue()).second;
        else
            s.bridged.erase(player->GetObjectGuid().GetRawValue());
    }
    if (turnedOn)
        PrepareInventory(player); // HELLO repeats every 10 s: only when the bridge switches on
    if (!on)
        RemoveAllProxies(player);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[classiccraft] %s: Minecraft combat %s", player->GetName(), on ? "on" : "off");
}

void ClassicCraft::RemoveAllProxies(Player* player)
{
    std::unordered_map<uint32, uint64> mine;
    {
        State& s = state();
        std::lock_guard<std::mutex> g(s.lock);
        auto it = s.proxies.find(player->GetObjectGuid().GetRawValue());
        if (it == s.proxies.end())
            return;
        mine.swap(it->second);
        s.proxies.erase(it);
        for (auto const& p : mine)
            s.owners.erase(p.second);
    }
    for (auto const& p : mine)
        Despawn(player->GetMap(), p.second);
}

bool ClassicCraft::ForwardDamage(Unit* attacker, Unit* victim, uint32 damage, CleanDamage const* cleanDamage, uint32 school)
{
    if (t_bypass || !victim)
        return false;

    Player* owner = nullptr;
    uint8 victimKind = 0;
    uint32 mcId = 0;
    if (victim->IsPlayer())
    {
        if (!IsBridged(victim))
            return false;
        owner = victim->ToPlayer();
    }
    else if (ProxyEntry(victim))
    {
        uint64 ownerGuid = 0;
        {
            State& s = state();
            std::lock_guard<std::mutex> g(s.lock);
            auto it = s.owners.find(victim->GetObjectGuid().GetRawValue());
            if (it != s.owners.end())
            {
                ownerGuid = it->second.first;
                mcId = it->second.second;
            }
        }
        victimKind = 1;
        // A proxy's health is Minecraft's even when its owner is gone: never wound it here.
        if (!ownerGuid)
            return true;
        owner = ObjectAccessor::FindPlayer(ObjectGuid(ownerGuid));
        if (!owner)
            return true;
    }
    else
        return false;

    if (!damage)
        return true;

    uint32 flags = 0;
    if (cleanDamage && cleanDamage->hitOutCome == MELEE_HIT_CRIT)
        flags |= HIT_CRIT;

    WorldPacket data(SMSG_CC_DAMAGE, 1 + 4 + 8 + 4 + 4 + 4 + 4);
    data << uint8(victimKind);
    data << uint32(mcId);
    data << (attacker ? attacker->GetObjectGuid() : ObjectGuid());
    data << uint32(damage);
    data << uint32(attacker ? attacker->GetLevel() : victim->GetLevel());
    data << uint32(flags);
    data << uint32(school);
    owner->GetSession()->SendPacket(&data);
    return true;
}

bool ClassicCraft::ReactionOverride(WorldObject const* self, WorldObject const* target, ReputationRank& out)
{
    uint32 a = ProxyEntry(self);
    uint32 b = ProxyEntry(target);
    if (!a && !b)
        return false;
    // Companions are on their owner's side, as a hunter's pet is: everything reacts to them as to
    // their owner, and they to everything as their owner does (the creature-vs-creature rules would
    // otherwise compare factions, and a raider hostile to the player could not attack its wolf).
    if (a == PROXY_COMPANION || b == PROXY_COMPANION)
    {
        if ((a == PROXY_COMPANION && b == PROXY_HOSTILE) || (b == PROXY_COMPANION && a == PROXY_HOSTILE))
        {
            out = REP_HOSTILE;
            return true;
        }
        if (a == PROXY_COMPANION && b == 0)
        {
            Unit* owner = static_cast<Unit const*>(self)->GetOwner();
            if (!owner || owner == target)
                return false;
            out = owner->GetReactionTo(target);
            return true;
        }
        if (b == PROXY_COMPANION && a == 0)
        {
            Unit* owner = static_cast<Unit const*>(target)->GetOwner();
            if (!owner || owner == self)
                return false;
            out = self->GetReactionTo(owner);
            return true;
        }
        return false;
    }
    if (a == PROXY_PASSIVE || b == PROXY_PASSIVE)
    {
        out = REP_NEUTRAL;
        return true;
    }
    // Hostile Minecraft mobs: friends with each other, enemies of every WoW unit but critters.
    if (a == PROXY_HOSTILE && b == PROXY_HOSTILE)
    {
        out = REP_FRIENDLY;
        return true;
    }
    WorldObject const* other = a ? target : self;
    if (other->IsUnit() && static_cast<Unit const*>(other)->GetCreatureType() == CREATURE_TYPE_CRITTER)
    {
        out = REP_NEUTRAL;
        return true;
    }
    out = REP_HOSTILE;
    return true;
}

// ---- proxy AI ------------------------------------------------------------------------------------

namespace
{
    // A proxy is moved only by its Minecraft mob (CMSG_CC_ACTORS) and never fights on its own. Stock
    // NullAI still evades when left in combat with no attacker, and evading walks a creature home (or,
    // with an owner, after its owner) - the proxy then ran off and dragged its attacker along.
    class ProxyAI final : public CreatureAI
    {
    public:
        explicit ProxyAI(Creature* c) : CreatureAI(c)
        {
            c->AddUnitState(UNIT_STATE_NO_SEARCH_FOR_OTHERS);
            m_bMeleeAttack = false;
            m_bCombatMovement = false;
        }

        void MoveInLineOfSight(Unit*) override {}
        void AttackStart(Unit*) override {}
        void AttackedBy(Unit*) override {}
        void EnterEvadeMode() override { LeaveCombat(); }

        void UpdateAI(uint32 const) override
        {
            if (m_creature->IsInCombat() && m_creature->GetAttackers().empty())
                LeaveCombat();
        }

        static int Permissible(Creature const*) { return PERMIT_BASE_NO; }

    private:
        void LeaveCombat()
        {
            m_creature->DeleteThreatList();
            m_creature->CombatStop(true);
        }
    };
}

void ClassicCraft::RegisterProxyAI()
{
    (new CreatureAIFactory<ProxyAI>("ClassicCraftProxyAI"))->RegisterSelf();
}

// ---- packets -------------------------------------------------------------------------------------

void WorldPackets::ClassicCraft::Hello::ReadFromWorldPacket(WorldPacket& recv)
{
    recv >> on;
}

bool ClassicCraft::HoldKillXP(Player* player, uint32 xp, Unit const* victim)
{
    if (t_claimVictim || !victim || !victim->IsCreature() || !player->IsAlive() || !IsBridged(player))
        return false;
    uint32 id;
    {
        XpLedger& l = ledger();
        std::lock_guard<std::mutex> g(l.lock);
        uint32 now = WorldTimer::getMSTime();
        auto& mine = l.drops[player->GetObjectGuid().GetRawValue()];
        for (auto it = mine.begin(); it != mine.end();)
            it = WorldTimer::getMSTimeDiff(it->second.createdMs, now) < XP_DROP_LIFE_MS ? std::next(it) : mine.erase(it);
        id = l.nextId++;
        if (xp)
            mine[id] = XpDrop{ xp, victim->GetObjectGuid().GetRawValue(), now };
    }
    // A grey kill (0 XP) drops no orbs (user, 2026-10-02): nothing to send.
    if (!xp)
        return true;
    Creature const* creature = static_cast<Creature const*>(victim);
    WorldPacket data(SMSG_CC_XP_DROP, 4 + 8 + 4 * 3 + 4 + 4 + 4);
    data << uint32(id);
    data << victim->GetObjectGuid();
    data << float(victim->GetPositionX()) << float(victim->GetPositionY()) << float(victim->GetPositionZ());
    data << uint32(xp);
    data << uint32(victim->GetLevel());
    data << uint32(creature->GetCreatureInfo()->rank);
    player->GetSession()->SendPacket(&data);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[classiccraft] %s: kill XP %u held as drop %u (%s)", player->GetName(), xp, id,
        victim->GetObjectGuid().GetString().c_str());
    return true;
}

ObjectGuid ClassicCraft::ClaimedKill()
{
    return ObjectGuid(t_claimVictim);
}

void ClassicCraft::ClearXpDrops(Player* player)
{
    XpLedger& l = ledger();
    std::lock_guard<std::mutex> g(l.lock);
    l.drops.erase(player->GetObjectGuid().GetRawValue());
}

bool ClassicCraft::OnKillLoot(Player* looter, Creature* victim)
{
    if (!looter || !victim || !IsBridged(looter))
        return false;
    Loot& loot = victim->loot;
    // The corpse's quest items (this player's quest drops, and quest starters), into the WoW bags.
    uint32 questLooted = 0;
    auto const& quest = loot.GetPlayerQuestItems();
    auto mine = quest.find(looter->GetGUIDLow());
    uint32 slots = uint32(loot.items.size()) + (mine != quest.end() ? uint32(mine->second->size()) : 0);
    for (uint32 slot = 0; slot < slots; ++slot)
    {
        QuestItem* qitem = nullptr;
        QuestItem* ffaitem = nullptr;
        QuestItem* conditem = nullptr;
        LootItem* item = loot.LootItemInSlot(slot, looter->GetGUIDLow(), &qitem, &ffaitem, &conditem);
        if (!item || !item->AllowedForPlayer(looter, victim))
            continue;
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(item->itemid);
        if (!qitem && !(proto && proto->StartQuest))
            continue;
        ItemPosCountVec dest;
        if (looter->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item->itemid, item->count) != EQUIP_ERR_OK)
            continue; // bags full: it stays on the corpse (WoW UI mode can still loot it)
        Item* stored = looter->StoreNewItem(dest, item->itemid, true, item->randomPropertyId);
        if (!stored)
            continue;
        if (qitem)
            qitem->is_looted = true;
        else if (ffaitem)
            ffaitem->is_looted = true;
        else if (conditem)
            conditem->is_looted = true;
        if (!item->freeforall)
            item->is_looted = true;
        if (loot.unlootedCount)
            --loot.unlootedCount;
        looter->SendNewItem(stored, item->count, false, false, true);
        looter->OnReceivedItem(stored);
        ++questLooted;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[classiccraft] %s: quest item %u x%u from %s into the bags", looter->GetName(),
            item->itemid, item->count, victim->GetName());
    }

    CreatureInfo const* info = victim->GetCreatureInfo();
    uint32 flags = 0;
    if (info->skinning_loot_id)
        flags |= KILL_SKINNABLE;
    WorldPacket data(SMSG_CC_KILL, 8 + 4 + 4 * 3 + 4 * 7);
    data << victim->GetObjectGuid();
    data << uint32(victim->GetEntry());
    data << float(victim->GetPositionX()) << float(victim->GetPositionY()) << float(victim->GetPositionZ());
    data << uint32(victim->GetLevel());
    data << uint32(info->rank);
    data << uint32(info->type);
    data << uint32(info->pet_family);
    data << uint32(loot.gold);
    data << uint32(flags);
    data << uint32(questLooted);
    looter->GetSession()->SendPacket(&data);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[classiccraft] %s: killed %s (entry %u, level %u, %u copper) - Minecraft loot",
        looter->GetName(), victim->GetName(), victim->GetEntry(), victim->GetLevel(), loot.gold);
    // No WoW loot left for a Minecraft player: its quest items are in the bags, its money became
    // emeralds (the gold above), the rest is gone, so the corpse doesn't sparkle.
    loot.clear();
    return true;
}

void ClassicCraft::PrepareInventory(Player* player)
{
    // What stays: quest items, quest starters, what an active quest asks for or handed out, our bags.
    std::unordered_set<uint32> keep{ QUEST_BAG };
    for (uint32 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        if (Quest const* q = sObjectMgr.GetQuestTemplate(
                player->GetUInt32Value(PLAYER_QUEST_LOG_1_1 + slot * MAX_QUEST_OFFSET + QUEST_ID_OFFSET)))
        {
            for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                keep.insert(q->ReqItemId[i]);
            for (uint32 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
                keep.insert(q->ReqSourceId[i]);
            keep.insert(q->GetSrcItemId());
        }
    }
    auto kept = [&](Item const* item)
    {
        ItemPrototype const* proto = item->GetProto();
        return keep.count(item->GetEntry()) || (proto && (proto->Class == ITEM_CLASS_QUEST || proto->StartQuest));
    };
    uint32 removed = 0;
    auto sweep = [&](uint8 bag, uint8 slot)
    {
        if (Item* item = player->GetItemByPos(bag, slot))
        {
            if (item->IsBag() || kept(item))
                return;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[classiccraft] %s: WoW item %s removed", player->GetName(),
                item->GetProto() ? item->GetProto()->Name1 : "?");
            player->DestroyItem(bag, slot, true);
            ++removed;
        }
    };
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        sweep(INVENTORY_SLOT_BAG_0, slot);
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        sweep(INVENTORY_SLOT_BAG_0, slot);
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Item* bagItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
        if (!bagItem || !bagItem->IsBag())
            continue;
        Bag* bag = static_cast<Bag*>(bagItem);
        for (uint8 j = 0; j < bag->GetBagSize(); ++j)
            sweep(bagSlot, j);
    }
    // Large bags in the empty bag slots, for quest items.
    uint32 bags = 0;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            continue;
        uint16 dest;
        if (player->CanEquipNewItem(bagSlot, dest, QUEST_BAG, false) == EQUIP_ERR_OK && player->EquipNewItem(dest, QUEST_BAG, true))
            ++bags;
    }
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[classiccraft] %s: WoW inventory prepared - %u items removed, %u quest bags added",
        player->GetName(), removed, bags);
}

void WorldPackets::ClassicCraft::XpClaim::ReadFromWorldPacket(WorldPacket& recv)
{
    recv >> dropId >> xp;
}

void WorldPackets::ClassicCraft::Hit::ReadFromWorldPacket(WorldPacket& recv)
{
    recv >> attackerKind >> mcId >> target >> damage >> flags;
}

void WorldPackets::ClassicCraft::Actors::ReadFromWorldPacket(WorldPacket& recv)
{
    uint16 count;
    recv >> count;
    actors.resize(count);
    for (auto& a : actors)
        recv >> a.mcId >> a.kind >> a.x >> a.y >> a.z >> a.o >> a.hpPct;
}

void WorldSession::HandleCCHelloOpcode(WorldPackets::ClassicCraft::Hello const& packet)
{
    if (Player* player = GetPlayer())
        ClassicCraft::SetBridged(player, packet.on != 0);
}

void WorldSession::HandleCCHitOpcode(WorldPackets::ClassicCraft::Hit const& packet)
{
    uint8 attackerKind = packet.attackerKind;
    uint32 mcId = packet.mcId, damage = packet.damage, flags = packet.flags;
    ObjectGuid targetGuid = packet.target;

    Player* player = GetPlayer();
    if (!player || !player->IsInWorld() || !ClassicCraft::IsBridged(player))
        return;
    Map* map = player->GetMap();
    Creature* target = map->GetCreature(targetGuid);
    if (!target || !target->IsAlive() || ClassicCraft::ProxyEntry(target))
        return;
    if (!player->IsWithinDistInMap(target, MAX_REACH))
        return;

    Unit* attacker = nullptr;
    if (attackerKind == 0)
    {
        if (!player->IsAlive())
            return;
        attacker = player;
    }
    else
    {
        uint64 proxyGuid = 0;
        {
            State& s = state();
            std::lock_guard<std::mutex> g(s.lock);
            auto it = s.proxies.find(player->GetObjectGuid().GetRawValue());
            if (it != s.proxies.end())
            {
                auto p = it->second.find(mcId);
                if (p != it->second.end())
                    proxyGuid = p->second;
            }
        }
        attacker = proxyGuid ? map->GetCreature(ObjectGuid(proxyGuid)) : nullptr;
        if (!attacker)
            return;
    }

    damage = std::min(damage, target->GetMaxHealth());
    bool crit = (flags & ClassicCraft::HIT_CRIT) != 0;
    CleanDamage clean(damage, BASE_ATTACK, crit ? MELEE_HIT_CRIT : MELEE_HIT_NORMAL, 0, 0);
    uint32 hitInfo = HITINFO_AFFECTS_VICTIM | (crit ? HITINFO_CRITICALHIT : 0);
    attacker->SendAttackStateUpdate(hitInfo, target, SPELL_SCHOOL_MASK_NORMAL, damage, 0, 0, VICTIMSTATE_NORMAL, 0);
    attacker->DealDamage(target, damage, &clean, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
}

void WorldSession::HandleCCActorsOpcode(WorldPackets::ClassicCraft::Actors const& packet)
{
    Player* player = GetPlayer();
    if (!player || !player->IsInWorld() || !ClassicCraft::IsBridged(player))
        return;
    Map* map = player->GetMap();
    uint64 me = player->GetObjectGuid().GetRawValue();
    State& s = state();

    for (auto const& a : packet.actors)
    {
        uint32 mcId = a.mcId;
        uint8 kind = a.kind, hpPct = a.hpPct;
        float x = a.x, y = a.y, z = a.z, o = a.o;

        uint64 proxyGuid = 0;
        {
            std::lock_guard<std::mutex> g(s.lock);
            auto& mine = s.proxies[me];
            auto it = mine.find(mcId);
            if (it != mine.end())
                proxyGuid = it->second;
        }
        Creature* proxy = proxyGuid ? map->GetCreature(ObjectGuid(proxyGuid)) : nullptr;
        uint32 entry = EntryFor(kind);

        // Removed, changed kind, or left behind on another map: drop the old proxy.
        if (proxyGuid && (!proxy || entry == 0 || proxy->GetEntry() != entry))
        {
            Despawn(map, proxyGuid);
            std::lock_guard<std::mutex> g(s.lock);
            s.proxies[me].erase(mcId);
            s.owners.erase(proxyGuid);
            proxy = nullptr;
        }
        if (!entry)
            continue;
        if (player->GetDistance(x, y, z) > MAX_REACH)
            continue;

        if (!proxy)
        {
            proxy = map->SummonCreature(entry, x, y, z, o, TEMPSUMMON_MANUAL_DESPAWN, 0, false);
            if (!proxy)
                continue;
            proxy->SetLevel(player->GetLevel());
            proxy->SetMaxHealth(PROXY_MAX_HEALTH);
            if (kind == ClassicCraft::ACTOR_COMPANION)
            {
                proxy->SetOwnerGuid(player->GetObjectGuid());
                proxy->SetFactionTemplateId(player->GetFactionTemplateId());
                // As a hunter's pet: player-vs-creature attack rules, so neutral creatures the player
                // pulled can turn on the wolf too (creature-vs-creature needs one side hostile).
                proxy->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
            }
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[classiccraft] proxy %s spawned for Minecraft mob %u kind %u",
                proxy->GetObjectGuid().GetString().c_str(), mcId, kind);
            std::lock_guard<std::mutex> g(s.lock);
            s.proxies[me][mcId] = proxy->GetObjectGuid().GetRawValue();
            s.owners[proxy->GetObjectGuid().GetRawValue()] = { me, mcId };
        }
        else
        {
            // Whatever else tried to move it (knockback, fear...), the Minecraft mob decides.
            if (proxy->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
            {
                proxy->GetMotionMaster()->Clear(false, true);
                proxy->GetMotionMaster()->MoveIdle();
            }
            map->CreatureRelocation(proxy, x, y, z, o);
        }
        proxy->SetHealth(std::max<uint32>(1, PROXY_MAX_HEALTH * std::min<uint32>(hpPct, 100) / 100));
    }
}

void WorldSession::HandleCCDiedOpcode(NullClientPacket const& /*packet*/)
{
    Player* player = GetPlayer();
    if (!player || !player->IsInWorld() || !player->IsAlive() || !ClassicCraft::IsBridged(player))
        return;
    // Dying in a mine: the character comes back into view for its ghost run.
    ClassicCraft::SetDownMine(player, false);
    // Minecraft killed the player: the WoW character dies too, god mode or not.
    uint32 threshold = player->GetInvincibilityHpThreshold();
    player->SetInvincibilityHpThreshold(0);
    t_bypass = true;
    player->DealDamage(player, player->GetHealth(), nullptr, SELF_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    t_bypass = false;
    player->SetInvincibilityHpThreshold(threshold);
}

void WorldSession::HandleCCXpClaimOpcode(WorldPackets::ClassicCraft::XpClaim const& packet)
{
    Player* player = GetPlayer();
    // Dead (a ghost can't pick orbs up anyway), GiveXP would drop it: keep it for later.
    if (!player || !player->IsInWorld() || !player->IsAlive() || !ClassicCraft::IsBridged(player) || !packet.xp)
        return;
    uint32 xp;
    uint64 victim;
    {
        XpLedger& l = ledger();
        std::lock_guard<std::mutex> g(l.lock);
        auto mine = l.drops.find(player->GetObjectGuid().GetRawValue());
        if (mine == l.drops.end())
            return;
        auto drop = mine->second.find(packet.dropId);
        if (drop == mine->second.end())
            return; // unknown, used up or expired: an orb from an earlier session
        xp = std::min(packet.xp, drop->second.remaining);
        victim = drop->second.victim;
        drop->second.remaining -= xp;
        if (!drop->second.remaining)
            mine->second.erase(drop);
    }
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[classiccraft] %s: claimed %u XP from drop %u", player->GetName(), xp, packet.dropId);
    t_claimVictim = victim;
    player->GiveXP(xp, nullptr);
    t_claimVictim = 0;
}

void WorldPackets::ClassicCraft::Waygate::ReadFromWorldPacket(WorldPacket& recv)
{
    recv >> map >> x >> y >> z >> o >> owner;
}

// A Minecraft waygate (the Fabric mod's arcane portal network): teleports the player to another
// waygate. Open world only at both ends (user, 2026-10-03); another player's waygate is shared
// through WoW's friend list, read from the database since its owner may be offline.
void WorldSession::HandleCCWaygateOpcode(WorldPackets::ClassicCraft::Waygate const& packet)
{
    Player* player = GetPlayer();
    if (!player || !player->IsInWorld() || !ClassicCraft::IsBridged(player))
        return;
    MapEntry const* dest = sMapStorage.LookupEntry<MapEntry>(packet.map);
    if (!dest || !dest->IsContinent() || !player->GetMap()->IsContinent())
    {
        SendNotification("Waygates work only in the open world.");
        return;
    }
    if (!MaNGOS::IsValidMapCoord(packet.x, packet.y, packet.z, packet.o))
        return;
    uint64 const me = player->GetObjectGuid().GetRawValue();
    if (packet.owner && packet.owner != me)
    {
        ObjectGuid owner(packet.owner);
        std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery(
            "SELECT 1 FROM character_social WHERE guid = %u AND friend = %u AND (flags & %u) != 0",
            owner.GetCounter(), player->GetGUIDLow(), uint32(SOCIAL_FLAG_FRIEND));
        if (!result)
        {
            SendNotification("That waygate's owner has not shared it with you.");
            return;
        }
    }
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[classiccraft] %s: waygate to map %u (%.1f, %.1f, %.1f)",
        player->GetName(), packet.map, packet.x, packet.y, packet.z);
    player->TeleportTo(packet.map, packet.x, packet.y, packet.z, packet.o);
}

void WorldPackets::ClassicCraft::Mine::ReadFromWorldPacket(WorldPacket& recv)
{
    recv >> down;
}

// While Steve mines in a Minecraft mine dimension (2026-10-03) the WoW character waits at the mine's
// entrance: nobody sees it, creatures forget it and can't attack it. Idempotent both ways.
void ClassicCraft::SetDownMine(Player* player, bool on)
{
    uint32 const flags = UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC;
    if (on)
    {
        player->CombatStop(true);
        player->GetHostileRefManager().deleteReferences();
        player->SetFlag(UNIT_FIELD_FLAGS, flags);
        player->SetVisibility(VISIBILITY_OFF);
    }
    else if (player->HasFlag(UNIT_FIELD_FLAGS, flags) || player->GetVisibility() == VISIBILITY_OFF)
    {
        player->RemoveFlag(UNIT_FIELD_FLAGS, flags);
        if (player->IsGMVisible())
            player->SetVisibility(VISIBILITY_ON);
    }
}

void WorldSession::HandleCCMineOpcode(WorldPackets::ClassicCraft::Mine const& packet)
{
    Player* player = GetPlayer();
    if (!player || !player->IsInWorld() || !ClassicCraft::IsBridged(player))
        return;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[classiccraft] %s: %s a mine", player->GetName(), packet.down ? "down" : "up from");
    ClassicCraft::SetDownMine(player, packet.down != 0);
}
