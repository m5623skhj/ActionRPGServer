-- The runner refuses this Down unless all three quest tables are empty.
DROP TABLE character_quest_events, character_quest_tracking, character_quests;
DROP PROCEDURE get_quest_schema_migration_history;
DROP PROCEDURE save_character_quests;
DROP PROCEDURE get_character_quests;
DROP PROCEDURE emit_quest_state;
DROP PROCEDURE apply_quest_inventory;
