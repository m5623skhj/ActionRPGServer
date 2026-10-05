-- Tool infrastructure version 000000; only bootstrap an empty selected schema.
-- Kept across all Down operations. The runner records RUNNING after statement
-- one and SUCCEEDED only after the complete file; interrupted bootstrap stops.
-- Checksum format: sha256-utf8-lf-v1 (UTF-8, no BOM, CRLF/CR -> LF).

CREATE TABLE schema_migrations
(
    execution_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    version INT UNSIGNED NOT NULL,
    name VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    direction ENUM('UP', 'DOWN') CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    up_checksum CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    down_checksum CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    state ENUM('RUNNING', 'SUCCEEDED', 'FAILED') CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    started_at DATETIME(6) NOT NULL,
    finished_at DATETIME(6) NULL,
    CONSTRAINT pk_schema_migrations PRIMARY KEY (execution_id),
    CONSTRAINT ck_migrations_completion CHECK
        ((state = 'RUNNING' AND finished_at IS NULL)
        OR (state IN ('SUCCEEDED', 'FAILED') AND finished_at IS NOT NULL)),
    CONSTRAINT ck_migrations_bootstrap CHECK
        ((version = 0 AND direction = 'UP' AND down_checksum IS NULL)
        OR (version > 0 AND down_checksum IS NOT NULL))
) ENGINE = InnoDB;

DELIMITER $$

CREATE PROCEDURE get_schema_migration_history()
READS SQL DATA
SQL SECURITY DEFINER
BEGIN
    DECLARE lockName VARCHAR(64);
    DECLARE lockResult INT DEFAULT NULL;
    DECLARE ownsLock BOOLEAN DEFAULT FALSE;
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        IF ownsLock THEN
            DO RELEASE_LOCK(lockName);
        END IF;
        RESIGNAL;
    END;

    SET lockName = CONCAT('actionrpg:migrate:', LEFT(SHA2(DATABASE(), 256), 40));
    SELECT GET_LOCK(lockName, 0) INTO lockResult;
    IF lockResult IS NULL OR lockResult <> 1 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Migration inspection is unavailable.';
    END IF;
    SET ownsLock = TRUE;

    -- All result values are character columns (nullable where noted).
    -- Data result set 0: format, checksum format, schema, version, version comment.
    SELECT CAST('1' AS CHAR CHARACTER SET utf8mb4) AS history_format,
        CAST('sha256-utf8-lf-v1' AS CHAR CHARACTER SET utf8mb4) AS checksum_format,
        CAST(DATABASE() AS CHAR CHARACTER SET utf8mb4) AS database_name,
        CAST(@@version AS CHAR CHARACTER SET utf8mb4) AS engine_version,
        CAST(@@version_comment AS CHAR CHARACTER SET utf8mb4) AS engine_comment;

    -- Set 1: complete ordered audit trail, including unfinished/failed attempts.
    SELECT CAST(execution_id AS CHAR CHARACTER SET utf8mb4) AS execution_id,
        CAST(version AS CHAR CHARACTER SET utf8mb4) AS version,
        CAST(name AS CHAR CHARACTER SET utf8mb4) AS name,
        CAST(direction AS CHAR CHARACTER SET utf8mb4) AS direction,
        CAST(up_checksum AS CHAR CHARACTER SET utf8mb4) AS up_checksum,
        CAST(down_checksum AS CHAR CHARACTER SET utf8mb4) AS down_checksum,
        CAST(state AS CHAR CHARACTER SET utf8mb4) AS state,
        CAST(DATE_FORMAT(started_at, '%Y-%m-%dT%H:%i:%s.%fZ') AS CHAR CHARACTER SET utf8mb4) AS started_at,
        CAST(DATE_FORMAT(finished_at, '%Y-%m-%dT%H:%i:%s.%fZ') AS CHAR CHARACTER SET utf8mb4) AS finished_at
    FROM schema_migrations ORDER BY execution_id;

    -- Set 2: actual columns, not a success flag. Collation is nullable.
    SELECT CAST(c.TABLE_NAME AS CHAR CHARACTER SET utf8mb4) AS table_name,
        CAST(c.COLUMN_NAME AS CHAR CHARACTER SET utf8mb4) AS column_name,
        CAST(c.COLUMN_TYPE AS CHAR CHARACTER SET utf8mb4) AS column_type,
        CAST(c.IS_NULLABLE AS CHAR CHARACTER SET utf8mb4) AS is_nullable,
        CAST(c.COLLATION_NAME AS CHAR CHARACTER SET utf8mb4) AS collation_name,
        CAST(c.EXTRA AS CHAR CHARACTER SET utf8mb4) AS extra,
        CAST(t.ENGINE AS CHAR CHARACTER SET utf8mb4) AS engine,
        CAST(c.COLUMN_DEFAULT AS CHAR CHARACTER SET utf8mb4) AS column_default,
        CAST(c.CHARACTER_SET_NAME AS CHAR CHARACTER SET utf8mb4) AS character_set_name
    FROM information_schema.COLUMNS c
    JOIN information_schema.TABLES t ON t.TABLE_SCHEMA = c.TABLE_SCHEMA AND t.TABLE_NAME = c.TABLE_NAME
    WHERE c.TABLE_SCHEMA = DATABASE() AND c.TABLE_NAME IN ('schema_migrations', 'accounts', 'account_identities')
    ORDER BY c.TABLE_NAME, c.ORDINAL_POSITION;

    -- Set 3: constraints. Column/reference/check values may be NULL.
    SELECT CAST(t.TABLE_NAME AS CHAR CHARACTER SET utf8mb4) AS table_name,
        CAST(t.CONSTRAINT_NAME AS CHAR CHARACTER SET utf8mb4) AS constraint_name,
        CAST(t.CONSTRAINT_TYPE AS CHAR CHARACTER SET utf8mb4) AS constraint_type,
        CAST(k.COLUMN_NAME AS CHAR CHARACTER SET utf8mb4) AS column_name,
        CAST(k.REFERENCED_TABLE_NAME AS CHAR CHARACTER SET utf8mb4) AS referenced_table_name,
        CAST(k.REFERENCED_COLUMN_NAME AS CHAR CHARACTER SET utf8mb4) AS referenced_column_name,
        CAST(c.CHECK_CLAUSE AS CHAR CHARACTER SET utf8mb4) AS check_clause,
        CAST(CASE WHEN t.CONSTRAINT_TYPE = 'CHECK' THEN t.ENFORCED END AS CHAR CHARACTER SET utf8mb4) AS enforced,
        CAST(k.REFERENCED_TABLE_SCHEMA AS CHAR CHARACTER SET utf8mb4) AS referenced_table_schema,
        CAST(r.UPDATE_RULE AS CHAR CHARACTER SET utf8mb4) AS update_rule,
        CAST(r.DELETE_RULE AS CHAR CHARACTER SET utf8mb4) AS delete_rule
    FROM information_schema.TABLE_CONSTRAINTS t
    LEFT JOIN information_schema.KEY_COLUMN_USAGE k ON k.CONSTRAINT_SCHEMA = t.CONSTRAINT_SCHEMA
        AND k.TABLE_NAME = t.TABLE_NAME AND k.CONSTRAINT_NAME = t.CONSTRAINT_NAME
    LEFT JOIN information_schema.CHECK_CONSTRAINTS c ON c.CONSTRAINT_SCHEMA = t.CONSTRAINT_SCHEMA
        AND c.CONSTRAINT_NAME = t.CONSTRAINT_NAME
    LEFT JOIN information_schema.REFERENTIAL_CONSTRAINTS r ON r.CONSTRAINT_SCHEMA = t.CONSTRAINT_SCHEMA
        AND r.TABLE_NAME = t.TABLE_NAME AND r.CONSTRAINT_NAME = t.CONSTRAINT_NAME
    WHERE t.TABLE_SCHEMA = DATABASE() AND t.TABLE_NAME IN ('schema_migrations', 'accounts', 'account_identities')
    ORDER BY t.TABLE_NAME, t.CONSTRAINT_NAME, k.ORDINAL_POSITION;

    -- Set 4: actual index shape. Prefix length may be NULL.
    SELECT CAST(TABLE_NAME AS CHAR CHARACTER SET utf8mb4) AS table_name,
        CAST(INDEX_NAME AS CHAR CHARACTER SET utf8mb4) AS index_name,
        CAST(NON_UNIQUE AS CHAR CHARACTER SET utf8mb4) AS non_unique,
        CAST(SEQ_IN_INDEX AS CHAR CHARACTER SET utf8mb4) AS seq_in_index,
        CAST(COLUMN_NAME AS CHAR CHARACTER SET utf8mb4) AS column_name,
        CAST(SUB_PART AS CHAR CHARACTER SET utf8mb4) AS sub_part
    FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN ('schema_migrations', 'accounts', 'account_identities')
    ORDER BY TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX;

    -- Set 5: routine metadata. definition_utf8 removes charset introducers;
    -- use original SHOW CREATE definitions below for body comparisons.
    SELECT CAST(ROUTINE_NAME AS CHAR CHARACTER SET utf8mb4) AS routine_name,
        CAST(SECURITY_TYPE AS CHAR CHARACTER SET utf8mb4) AS security_type,
        CAST(SQL_DATA_ACCESS AS CHAR CHARACTER SET utf8mb4) AS sql_data_access,
        CAST(ROUTINE_DEFINITION AS CHAR CHARACTER SET utf8mb4) AS routine_definition
    FROM information_schema.ROUTINES
    WHERE ROUTINE_SCHEMA = DATABASE() AND ROUTINE_TYPE = 'PROCEDURE'
        AND ROUTINE_NAME IN ('get_schema_migration_history', 'login_google_account', 'get_auth_account_status')
    ORDER BY ROUTINE_NAME;

    -- Set 6: actual input signature; history inspection itself has no parameters.
    SELECT CAST(SPECIFIC_NAME AS CHAR CHARACTER SET utf8mb4) AS routine_name,
        CAST(ORDINAL_POSITION AS CHAR CHARACTER SET utf8mb4) AS ordinal_position,
        CAST(PARAMETER_MODE AS CHAR CHARACTER SET utf8mb4) AS parameter_mode,
        CAST(PARAMETER_NAME AS CHAR CHARACTER SET utf8mb4) AS parameter_name,
        CAST(DTD_IDENTIFIER AS CHAR CHARACTER SET utf8mb4) AS dtd_identifier,
        CAST(CHARACTER_SET_NAME AS CHAR CHARACTER SET utf8mb4) AS character_set_name,
        CAST(COLLATION_NAME AS CHAR CHARACTER SET utf8mb4) AS collation_name
    FROM information_schema.PARAMETERS
    WHERE SPECIFIC_SCHEMA = DATABASE()
        AND SPECIFIC_NAME IN ('get_schema_migration_history', 'login_google_account', 'get_auth_account_status')
    ORDER BY SPECIFIC_NAME, ORDINAL_POSITION;

    -- Sets 7/8/9: exact SHOW CREATE shape (six columns, one row each).
    -- SHOW uses original definition, retaining _binary string introducers.
    -- Nonexistent routines during Up/Down yield an all-NULL placeholder.
    SHOW CREATE PROCEDURE get_schema_migration_history;

    IF EXISTS (SELECT 1 FROM information_schema.ROUTINES
        WHERE ROUTINE_SCHEMA = DATABASE() AND ROUTINE_TYPE = 'PROCEDURE'
            AND ROUTINE_NAME = 'login_google_account') THEN
        SHOW CREATE PROCEDURE login_google_account;
    ELSE
        SELECT CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS `Procedure`,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS sql_mode,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS `Create Procedure`,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS character_set_client,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS collation_connection,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS `Database Collation`;
    END IF;

    IF EXISTS (SELECT 1 FROM information_schema.ROUTINES
        WHERE ROUTINE_SCHEMA = DATABASE() AND ROUTINE_TYPE = 'PROCEDURE'
            AND ROUTINE_NAME = 'get_auth_account_status') THEN
        SHOW CREATE PROCEDURE get_auth_account_status;
    ELSE
        SELECT CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS `Procedure`,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS sql_mode,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS `Create Procedure`,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS character_set_client,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS collation_connection,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS `Database Collation`;
    END IF;

    DO RELEASE_LOCK(lockName);
    SET ownsLock = FALSE;
END$$

DELIMITER ;
