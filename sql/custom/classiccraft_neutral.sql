-- classiccraft (fork only): the neutral Minecraft race (src/game/ClassicCraftNeutral.cpp, 2026-10-05).
-- Faction 990 (no reputation) and its template 990, every character's while ClassicCraft.NeutralRace
-- is on: a player (group 1, so monsters - hostile to the player group - attack it), friendly to
-- Alliance and Horde (groups 2 and 4), hostile to monsters (group 8). Alliance/Horde NPC templates
-- don't list the player group as an enemy, so their guards leave it alone; reputation factions
-- (capitals) go by the player's standing instead, which starts friendly. benilla knows this template
-- by id (the client's FactionTemplate.dbc doesn't have it).
DELETE FROM faction WHERE id = 990;
INSERT INTO faction (id, build, reputation_list_id, base_rep_race_mask1, base_rep_race_mask2,
    base_rep_race_mask3, base_rep_race_mask4, base_rep_class_mask1, base_rep_class_mask2,
    base_rep_class_mask3, base_rep_class_mask4, base_rep_value1, base_rep_value2, base_rep_value3,
    base_rep_value4, reputation_flags1, reputation_flags2, reputation_flags3, reputation_flags4, team,
    name, description)
VALUES (990, 4222, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'PLAYER, Minecraft', '');
DELETE FROM faction_template WHERE id = 990;
INSERT INTO faction_template (id, build, faction_id, faction_flags, our_mask, friendly_mask, hostile_mask,
    enemy_faction1, enemy_faction2, enemy_faction3, enemy_faction4, friend_faction1, friend_faction2,
    friend_faction3, friend_faction4)
VALUES (990, 4222, 990, 72, 1, 6, 8, 0, 0, 0, 0, 0, 0, 0, 0);
