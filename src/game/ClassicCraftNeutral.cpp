/*
 * classiccraft (fork only): the neutral Minecraft race (2026-10-05, user: "neutral forever").
 *
 * Every character is a Minecraft character (ClassicCraft.NeutralRace, default on): its stored race
 * only picks the model and Steve's dances. On top of it:
 *  - faction template NEUTRAL_FACTION_TEMPLATE (sql/custom/classiccraft_neutral.sql): a player
 *    (monsters attack it) friendly to Alliance and Horde, so neither side's guards or cities fight it;
 *  - one team (ALLIANCE) for everyone, so groups, guilds, chat and mail never meet a faction wall;
 *  - the race mask is every playable race: both factions' quests, items and gossip conditions;
 *  - base reputation: each faction's best race/class slot (NeutralRepIndex), so every capital starts
 *    friendly, never at war - benilla mirrors this rule for its own rank maths;
 *  - every player language (NPCs of either side are understood), no racial abilities;
 *  - no PvP-enforced Alliance/Horde territory (Player::UpdateZone).
 */

#include "ClassicCraft.h"
#include "Player.h"
#include "SpellMgr.h"
#include "World.h"
#include "Log.h"

namespace
{
    // SkillLine.dbc's racial lines: Human, Dwarf, Night Elf, Gnome, Orc, Undead, Tauren, Troll.
    constexpr uint16 RACIAL_SKILLS[] = { 754, 101, 126, 753, 125, 220, 124, 733 };

    // The player languages' spells: Common, Orcish, Dwarven, Darnassian, Taurahe, Gnomish, Troll,
    // Gutterspeak (each teaches its language skill).
    constexpr uint32 LANGUAGE_SPELLS[] = { 668, 669, 672, 671, 670, 7340, 7341, 17737 };

    bool IsRacialSkill(uint32 skill)
    {
        for (uint16 racial : RACIAL_SKILLS)
            if (racial == skill)
                return true;
        return false;
    }
}

namespace ClassicCraft
{
    bool NeutralRaceOn()
    {
        return sWorld.getConfig(CONFIG_BOOL_CLASSICCRAFT_NEUTRAL_RACE);
    }

    bool IsNeutral(Unit const* unit)
    {
        return unit && unit->IsPlayer() && NeutralRaceOn();
    }

    int NeutralRepIndex(FactionEntry const* faction, uint32 classMask)
    {
        int best = -1;
        for (int i = 0; i < 4; ++i)
        {
            uint32 const races = faction->BaseRepRaceMask[i];
            uint32 const classes = faction->BaseRepClassMask[i];
            // As the 1.12 client: a slot with neither mask is no slot.
            if (!races && !classes)
                continue;
            if (classes && !(classes & classMask))
                continue;
            if (best < 0 || faction->BaseRepValue[i] > faction->BaseRepValue[best])
                best = i;
        }
        return best;
    }

    bool IsRacialSpell(uint32 spellId)
    {
        SkillLineAbilityMapBounds bounds = sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (IsRacialSkill(itr->second->skillId))
                return true;
        return false;
    }

    void ApplyNeutralSpells(Player* player)
    {
        if (!IsNeutral(player))
            return;

        // Racial skills first: removing one wipes its spells (UpdateSkillTrainedSpells).
        uint32 removed = 0;
        for (uint16 skill : RACIAL_SKILLS)
        {
            if (player->HasSkill(skill))
            {
                player->SetSkill(skill, 0, 0);
                ++removed;
            }
        }
        // Then any racial spell left without its skill.
        std::vector<uint32> racial;
        for (auto const& itr : player->GetSpellMap())
            if (itr.second.state != PLAYERSPELL_REMOVED && IsRacialSpell(itr.first))
                racial.push_back(itr.first);
        for (uint32 spell : racial)
            player->RemoveSpell(spell, false, false);

        uint32 learned = 0;
        for (uint32 spell : LANGUAGE_SPELLS)
        {
            if (player->HasSpell(spell))
                continue;
            player->LearnSpell(spell, false); // silent while loading (not in world yet)
            ++learned;
        }

        if (removed || !racial.empty() || learned)
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[classiccraft] %s: neutral race (%u racial skills, %u racial spells removed, %u languages learned)",
                player->GetName(), removed, uint32(racial.size()), learned);
    }
}
