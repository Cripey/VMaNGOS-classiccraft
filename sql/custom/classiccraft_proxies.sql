-- classiccraft (fork only): Minecraft mob proxies (src/game/ClassicCraft.h). Copies of the invisible
-- "Alzinn Trigger" (11494, display 11686) with ClassicCraftProxyAI (never moves or fights by itself);
-- ClassicCraft::ReactionOverride decides who they fight. Faction 14 (monster) makes hostile proxies
-- hostile to players, which GuardAI keys on; companions take their owner's faction at summon.
DELETE FROM creature_template WHERE entry IN (990001, 990002, 990003);
DROP TEMPORARY TABLE IF EXISTS cc_proxy;
CREATE TEMPORARY TABLE cc_proxy AS SELECT * FROM creature_template WHERE entry = 11494 LIMIT 1;
UPDATE cc_proxy SET entry = 990001, name = 'Minecraft Mob', subname = 'classiccraft', faction = 14,
    ai_name = 'ClassicCraftProxyAI', script_name = '', level_min = 1, level_max = 1;
INSERT INTO creature_template SELECT * FROM cc_proxy;
UPDATE cc_proxy SET entry = 990002, name = 'Minecraft Animal', faction = 35;
INSERT INTO creature_template SELECT * FROM cc_proxy;
UPDATE cc_proxy SET entry = 990003, name = 'Minecraft Companion', faction = 35;
INSERT INTO creature_template SELECT * FROM cc_proxy;
DROP TEMPORARY TABLE cc_proxy;
