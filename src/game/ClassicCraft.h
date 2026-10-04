/*
 * classiccraft: Minecraft <-> WoW combat crossover (fork only).
 *
 * A bridged player's client (benilla + a real Minecraft client) owns the health of the player and of
 * the Minecraft mobs around them; the server owns WoW creatures. So:
 *  - Minecraft mobs exist here as invisible proxy creatures (CC_PROXY_*), moved by the client
 *    (CMSG_CC_ACTORS), so WoW AI can see, aggro on and fight them.
 *  - CMSG_CC_HIT: a Minecraft attacker (the player, or a proxy) hit a WoW creature; dealt here as real
 *    damage from that attacker (threat, combat, loot recipient, XP and kill credit as usual).
 *  - Damage dealt TO a bridged player or a proxy never touches health here: Unit::DealDamage hands it
 *    to ForwardDamage, which sends SMSG_CC_DAMAGE to the owning client for Minecraft to apply.
 *  - CMSG_CC_DIED: Minecraft killed the player; the WoW character dies too.
 *  - CMSG_CC_RESPAWN (2026-10-04): Minecraft's Respawn is the release: the character is resurrected
 *    at Steve's bed (a spot in the open world) or at its hearthstone location - no ghost run.
 *  - Kill XP (2026-10-02): a bridged player's kill XP is held (HoldKillXP, from Player::GiveXP) and
 *    sent as SMSG_CC_XP_DROP for Minecraft to drop as XP orbs at the corpse; picking them up claims
 *    it (CMSG_CC_XP_CLAIM), granted then through GiveXP as kill XP (rested bonus, the XP message).
 *    A ledger per player bounds the claims; unclaimed XP ends with the session.
 *  - Ore veins (2026-10-04): CMSG_CC_HARVEST / SMSG_CC_HARVEST - a vein mined with a Minecraft pickaxe
 *    (no Mining skill, no WoW loot), Minecraft drops the ore.
 *  - Kill loot (2026-10-03): SMSG_CC_KILL (OnKillLoot) - guid victim, u32 entry, f32 x, y, z, u32 level,
 *    rank, creature type, family, money (copper on the corpse), KillFlags, quest items put in the bags.
 */

#ifndef MANGOS_CLASSICCRAFT_H
#define MANGOS_CLASSICCRAFT_H

#include "Common.h"
#include "SharedDefines.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "Packet.h"

#include <vector>

class Unit;
class Player;
class Creature;
class WorldObject;
class WorldPacket;
struct CleanDamage;

namespace ClassicCraft
{
    // creature_template entries of the proxies (sql/custom/classiccraft_proxies.sql).
    constexpr uint32 PROXY_HOSTILE = 990001;   // zombies, skeletons...: enemies of every WoW unit
    constexpr uint32 PROXY_PASSIVE = 990002;   // cows, sheep...: ignored by WoW creatures
    constexpr uint32 PROXY_COMPANION = 990003; // the player's tamed wolves, golems: on the player's side

    // CMSG_CC_ACTORS kinds; 0 removes the actor.
    enum ActorKind : uint8
    {
        ACTOR_REMOVE = 0,
        ACTOR_HOSTILE = 1,
        ACTOR_PASSIVE = 2,
        ACTOR_COMPANION = 3,
    };

    // SMSG_CC_KILL flags.
    enum KillFlags : uint32
    {
        KILL_SKINNABLE = 0x1,
    };

    // CMSG_CC_HIT / SMSG_CC_DAMAGE flags.
    enum HitFlags : uint32
    {
        HIT_CRIT = 0x1,
        HIT_PROJECTILE = 0x2,
        // Spell schools on Minecraft hits (2026-10-04): wands, staves, elemental enchantments.
        HIT_PERIODIC = 0x4,   // a damage-over-time tick (logged as periodic)
        HIT_SLOW = 0x8,       // frost: the target is Chilled (HIT_SLOW_SPELL)
        HIT_SCHOOL_SHIFT = 8, // bits 8-10: SpellSchools (0 physical, 1 holy, 2 fire, 3 nature, 4 frost, 5 shadow, 6 arcane)
    };
    // Ice Armor's Chilled: -30% movement and slower attacks for 5 s, no damage.
    static constexpr uint32 HIT_SLOW_SPELL = 7321;
    // The combat log names a school hit by this spell ("Shoot", the wand's own).
    static constexpr uint32 HIT_SCHOOL_SPELL = 5019;

    // The proxy kind of a unit (0 if it is not a proxy).
    uint32 ProxyEntry(WorldObject const* obj);

    // Whether the player's client drives combat from Minecraft (CMSG_CC_HELLO).
    bool IsBridged(Unit const* unit);
    // Steve is down a Minecraft mine: hide/park the WoW character (on), or bring it back (off).
    void SetDownMine(Player* player, bool on);
    void SetBridged(Player* player, bool on);

    // Unit::DealDamage's hook: true when the victim's health belongs to a Minecraft client, in which
    // case the damage has been forwarded there and must not be applied here.
    bool ForwardDamage(Unit* attacker, Unit* victim, uint32 damage, CleanDamage const* cleanDamage, uint32 schoolMask);

    // WorldObject::GetReactionTo's hook: the proxies' reactions, or false to fall through.
    bool ReactionOverride(WorldObject const* self, WorldObject const* target, ReputationRank& out);

    // Every proxy of a player, removed (logout, map change, bridge off).
    void RemoveAllProxies(Player* player);

    // The proxies' AI ("ClassicCraftProxyAI"), registered from AIRegistry::Initialize.
    void RegisterProxyAI();

    // Player::GiveXP's hook: true when this kill XP is held for Minecraft's orbs (and sent as
    // SMSG_CC_XP_DROP), in which case GiveXP must not apply it now.
    bool HoldKillXP(Player* player, uint32 xp, Unit const* victim);

    // The victim of the kill XP being granted by a claim (empty otherwise), for GiveXP's rested
    // bonus and XP message, as the corpse may be gone.
    ObjectGuid ClaimedKill();

    // A player's held kill XP, forgotten (logout).
    void ClearXpDrops(Player* player);

    // Unit::Kill's hook, after the corpse's loot is made (2026-10-03): for a bridged looter, the
    // corpse's quest items go straight into the WoW bags (Steve can't open WoW's loot window), and
    // SMSG_CC_KILL tells Minecraft what died so it drops Minecraft loot there (the mod's rules: by
    // creature entry/type/family/level; money becomes emeralds).
    // Returns true when the corpse's loot was cleared (no WoW items or gold for a bridged looter,
    // 2026-10-03): Unit::Kill then leaves it unlootable (no sparkle).
    bool OnKillLoot(Player* looter, Creature* victim);

    // Phasing out the WoW inventory (user, 2026-10-03): when a character's bridge switches on,
    // everything it carries or wears that isn't a quest item goes, and empty bag slots get large
    // bags for the quest items. Bridged players also get no WoW quest reward items or gold (the
    // Minecraft side grants its own) and WoW armor doesn't reduce hits forwarded to Minecraft.
    void PrepareInventory(Player* player);
    // The item that fills empty bag slots: "20-slot Bag".
    constexpr uint32 QUEST_BAG = 1977;
}

// The client packets (all little-endian, fixed layouts).
namespace WorldPackets { namespace ClassicCraft
{
    // CMSG_CC_HELLO: u8 on.
    class Hello final : public ClientPacket
    {
    public:
        uint8 on = 0;
        Hello() : ClientPacket(CMSG_CC_HELLO) {}
        void ReadFromWorldPacket(WorldPacket& recv) override;
    };

    // CMSG_CC_HIT: u8 attacker kind (0 the player, 1 a proxy), u32 Minecraft id, guid target,
    // u32 WoW damage, u32 HitFlags.
    class Hit final : public ClientPacket
    {
    public:
        uint8 attackerKind = 0;
        uint32 mcId = 0;
        ObjectGuid target;
        uint32 damage = 0;
        uint32 flags = 0;
        Hit() : ClientPacket(CMSG_CC_HIT) {}
        void ReadFromWorldPacket(WorldPacket& recv) override;
    };

    // CMSG_CC_ACTORS: u16 count, then per actor u32 Minecraft id, u8 ActorKind, f32 x, y, z, o (WoW
    // yards), u8 health percent.
    struct ActorUpdate
    {
        uint32 mcId;
        uint8 kind;
        float x, y, z, o;
        uint8 hpPct;
    };
    class Actors final : public ClientPacket
    {
    public:
        std::vector<ActorUpdate> actors;
        Actors() : ClientPacket(CMSG_CC_ACTORS) {}
        void ReadFromWorldPacket(WorldPacket& recv) override;
    };

    // CMSG_CC_XP_CLAIM: u32 drop id (SMSG_CC_XP_DROP), u32 XP picked up.
    class XpClaim final : public ClientPacket
    {
    public:
        uint32 dropId = 0;
        uint32 xp = 0;
        XpClaim() : ClientPacket(CMSG_CC_XP_CLAIM) {}
        void ReadFromWorldPacket(WorldPacket& recv) override;
    };

    // CMSG_CC_WAYGATE: travel through a Minecraft waygate (2026-10-03): u32 map, f32 x, y, z, o (the
    // destination waygate's spot), u64 its owner. Open world at both ends; another player's waygate
    // only when its owner has us as a WoW friend.
    class Waygate final : public ClientPacket
    {
    public:
        uint32 map = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
        uint64 owner = 0;
        Waygate() : ClientPacket(CMSG_CC_WAYGATE) {}
        void ReadFromWorldPacket(WorldPacket& recv) override;
    };

    // CMSG_CC_MINE: u8 1 = Steve went down a Minecraft mine (a separate Minecraft dimension) and the
    // WoW character waits parked at the entrance: hidden, out of combat, not attackable; 0 = back.
    class Mine final : public ClientPacket
    {
    public:
        uint8 down = 0;
        Mine() : ClientPacket(CMSG_CC_MINE) {}
        void ReadFromWorldPacket(WorldPacket& recv) override;
    };

    // CMSG_CC_RESPAWN: Steve respawned after a death (2026-10-04): u32 kind (RESPAWN_HOME = the
    // hearthstone location, RESPAWN_AT = the spot given: his bed), u32 map, f32 x, y, z, o.
    class Respawn final : public ClientPacket
    {
    public:
        enum Kind : uint32 { RESPAWN_HOME = 0, RESPAWN_AT = 1 };
        uint32 kind = RESPAWN_HOME;
        uint32 map = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
        Respawn() : ClientPacket(CMSG_CC_RESPAWN) {}
        void ReadFromWorldPacket(WorldPacket& recv) override;
    };

    // CMSG_CC_HARVEST: Steve mined a WoW ore vein with a Minecraft pickaxe or gathered a herb (2026-10-04): u64 the node.
    // Counts as one of the vein's uses (WoW's own min/max opens) and gives no WoW loot; the reply
    // SMSG_CC_HARVEST (u64 vein, u32 entry, f32 x, y, z, u8 ok, u8 depleted) lets Minecraft drop ore.
    // Treasure chests too (2026-10-04): opened once, consumed (depleted), Minecraft fills a chest screen.
    class Harvest final : public ClientPacket
    {
    public:
        uint64 guid = 0;
        Harvest() : ClientPacket(CMSG_CC_HARVEST) {}
        void ReadFromWorldPacket(WorldPacket& recv) override;
    };
}}

#endif
