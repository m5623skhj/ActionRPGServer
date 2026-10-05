-- Reverse logical version 000001. Use DownMigration.bat with services stopped.
-- The runner holds write locks and rejects any account or identity data.
-- History infrastructure is retained. This does not recover deleted data.
DROP TABLE account_identities, accounts;
