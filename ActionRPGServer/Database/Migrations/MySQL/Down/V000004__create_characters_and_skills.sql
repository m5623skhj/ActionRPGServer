-- Logical version 000004 inverse. Run only through the stopped-service runner.
-- The runner locks both character tables and schema_migrations, verifies both
-- tables are empty, and drops the pair in one statement before releasing locks.
-- Existing accounts, V0 inspector, audit trail and login/status routines remain.
-- DROP PROCEDURE occurs after table locks are released; partial failure blocks.
DROP TABLE character_skills, characters;
DROP PROCEDURE get_character_schema_migration_history;
