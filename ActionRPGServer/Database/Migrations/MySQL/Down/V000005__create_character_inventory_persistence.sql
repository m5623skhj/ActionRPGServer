-- V000005 rollback: only a never-used new schema can be removed.
-- The manual runner must verify all three tables empty while holding WRITE locks.
-- Preserve V0..V4, every existing character/skill, and all migration audit rows.
DROP TABLE character_operations, character_items, character_state;
DROP PROCEDURE emit_character_state;
DROP PROCEDURE list_characters;
DROP PROCEDURE create_character;
DROP PROCEDURE claim_character;
DROP PROCEDURE save_character_state;
DROP PROCEDURE release_character;
DROP PROCEDURE get_inventory_schema_migration_history;
