-- MySQL 8.0.46 / InnoDB. Preserve every V0..V7 deployment file.
-- Existing levels, spent/unspent SP and learned skills are never recalculated.
-- Room facts, quests, growth and receipts remain in one ODBC-owned transaction.
ALTER TABLE characters ADD COLUMN experience BIGINT UNSIGNED NOT NULL DEFAULT 0;

DELIMITER $$
DROP PROCEDURE apply_quest_inventory$$
CREATE PROCEDURE apply_quest_inventory
(
    IN inCharacterId BIGINT UNSIGNED,
    IN inProgressionJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
    IN inInventoryJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin
)
MODIFIES SQL DATA
SQL SECURITY DEFINER
apply_snapshot: BEGIN
    DECLARE nextLevel BIGINT UNSIGNED;
    DECLARE nextSp BIGINT UNSIGNED;
    DECLARE nextExperience BIGINT UNSIGNED;
    DECLARE storedSkills JSON;
    DECLARE skillKeys JSON;
    DECLARE skillsCount INT UNSIGNED DEFAULT 0;
    DECLARE itemsCount INT UNSIGNED DEFAULT 0;
    DECLARE itemIndex INT UNSIGNED DEFAULT 0;
    DECLARE skillId VARCHAR(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin;
    DECLARE skillRank BIGINT UNSIGNED;
    DECLARE instanceId VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin;
    DECLARE definitionId VARCHAR(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin;
    DECLARE itemCount BIGINT UNSIGNED;
    DECLARE itemContainer BIGINT UNSIGNED;
    DECLARE itemSlot BIGINT UNSIGNED;
    DECLARE itemValue JSON;
    -- Private: the public save caller owns the character locks, fencing and transaction.
    IF inProgressionJson IS NULL OR OCTET_LENGTH(inProgressionJson) > 32768 OR NOT JSON_VALID(inProgressionJson)
        OR (inInventoryJson IS NOT NULL AND (OCTET_LENGTH(inInventoryJson) > 32768 OR NOT JSON_VALID(inInventoryJson))) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid character snapshots.';
    END IF;
    IF COALESCE(JSON_TYPE(CAST(inProgressionJson AS JSON)), '') <> 'OBJECT'
        OR JSON_LENGTH(CAST(inProgressionJson AS JSON)) <> 4
        OR COALESCE(JSON_TYPE(JSON_EXTRACT(inProgressionJson, '$.level')), '') <> 'INTEGER'
        OR COALESCE(JSON_TYPE(JSON_EXTRACT(inProgressionJson, '$.skillPoints')), '') <> 'INTEGER'
        OR COALESCE(JSON_TYPE(JSON_EXTRACT(inProgressionJson, '$.skillLevels')), '') <> 'OBJECT'
        OR COALESCE(JSON_TYPE(JSON_EXTRACT(inProgressionJson, '$.experience')), '') <> 'STRING'
        OR (inInventoryJson IS NOT NULL AND (
            COALESCE(JSON_TYPE(CAST(inInventoryJson AS JSON)), '') <> 'OBJECT'
            OR JSON_LENGTH(CAST(inInventoryJson AS JSON)) <> 1
            OR COALESCE(JSON_TYPE(JSON_EXTRACT(inInventoryJson, '$.items')), '') <> 'ARRAY')) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid snapshot shape.';
    END IF;
    -- Validate as DECIMAL before unsigned conversion; negative/wrapped values must not pass.
    IF CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, '$.level')) AS DECIMAL(20,0)) NOT BETWEEN 1 AND 1000000
        OR CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, '$.skillPoints')) AS DECIMAL(20,0)) NOT BETWEEN 0 AND 4294967295 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid progression range.';
    END IF;
    IF JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, '$.experience')) NOT REGEXP '^(0|[1-9][0-9]{0,19})$'
        OR CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, '$.experience')) AS DECIMAL(20,0)) > 18446744073709551615 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid experience range.';
    END IF;
    SET nextExperience = CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, '$.experience')) AS UNSIGNED);
    SET nextLevel = CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, '$.level')) AS UNSIGNED);
    SET nextSp = CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, '$.skillPoints')) AS UNSIGNED);
    SET skillKeys = JSON_KEYS(JSON_EXTRACT(inProgressionJson, '$.skillLevels'));
    SET skillsCount = JSON_LENGTH(skillKeys);
    SET itemsCount = COALESCE(JSON_LENGTH(JSON_EXTRACT(inInventoryJson, '$.items')), 0);
    IF skillsCount > 256 OR itemsCount > 167 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Snapshot capacity exceeded.';
    END IF;
    -- Validate strings before assigning to bounded local variables (no silent truncation).
    SET itemIndex = 0;
    WHILE itemIndex < skillsCount DO
        IF OCTET_LENGTH(JSON_UNQUOTE(JSON_EXTRACT(skillKeys, CONCAT('$[', itemIndex, ']')))) NOT BETWEEN 1 AND 64
            OR JSON_UNQUOTE(JSON_EXTRACT(skillKeys, CONCAT('$[', itemIndex, ']'))) NOT REGEXP '^[A-Za-z][A-Za-z0-9_.-]{0,63}$' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid learned skill ID.';
        END IF;
        SET skillId = JSON_UNQUOTE(JSON_EXTRACT(skillKeys, CONCAT('$[', itemIndex, ']')));
        IF COALESCE(JSON_TYPE(JSON_EXTRACT(inProgressionJson, CONCAT('$.skillLevels.', JSON_QUOTE(skillId)))), '') <> 'INTEGER'
            OR CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, CONCAT('$.skillLevels.', JSON_QUOTE(skillId)))) AS DECIMAL(20,0))
                NOT BETWEEN 1 AND 1000000 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid learned skill level.';
        END IF;
        SET itemIndex = itemIndex + 1;
    END WHILE;
    IF inInventoryJson IS NULL THEN
        -- Growth cannot replace or erase learned skills, and never writes inventory.
        SELECT COALESCE(JSON_OBJECTAGG(CONVERT(skill_id USING utf8mb4), skill_level), JSON_OBJECT())
            INTO storedSkills FROM character_skills WHERE character_id = inCharacterId;
        IF SHA2(CAST(JSON_EXTRACT(inProgressionJson, '$.skillLevels') AS CHAR CHARACTER SET utf8mb4), 256)
            <> SHA2(CAST(storedSkills AS CHAR CHARACTER SET utf8mb4), 256) THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Growth cannot change learned skills.';
        END IF;
        UPDATE characters SET level = nextLevel, skill_points = nextSp, experience = nextExperience,
            updated_at = UTC_TIMESTAMP(6) WHERE character_id = inCharacterId;
        LEAVE apply_snapshot;
    END IF;
    SET itemIndex = 0;
    WHILE itemIndex < itemsCount DO
        SET itemValue = JSON_EXTRACT(inInventoryJson, CONCAT('$.items[', itemIndex, ']'));
        IF COALESCE(JSON_TYPE(itemValue), '') <> 'OBJECT' OR JSON_LENGTH(itemValue) <> 5
            OR COALESCE(JSON_TYPE(JSON_EXTRACT(itemValue, '$.instanceId')), '') <> 'STRING'
            OR COALESCE(JSON_TYPE(JSON_EXTRACT(itemValue, '$.definitionId')), '') <> 'STRING'
            OR COALESCE(JSON_TYPE(JSON_EXTRACT(itemValue, '$.count')), '') <> 'INTEGER'
            OR COALESCE(JSON_TYPE(JSON_EXTRACT(itemValue, '$.container')), '') <> 'INTEGER'
            OR COALESCE(JSON_TYPE(JSON_EXTRACT(itemValue, '$.slot')), '') <> 'INTEGER' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid item snapshot shape.';
        END IF;
        IF OCTET_LENGTH(JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.instanceId'))) <> 32
            OR JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.instanceId')) NOT REGEXP '^[0-9a-f]{32}$'
            OR OCTET_LENGTH(JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.definitionId'))) NOT BETWEEN 1 AND 64
            OR JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.definitionId')) NOT REGEXP '^[A-Za-z][A-Za-z0-9_.-]{0,63}$'
            OR CAST(JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.count')) AS DECIMAL(20,0)) NOT BETWEEN 1 AND 4294967295
            OR CAST(JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.container')) AS DECIMAL(20,0)) NOT BETWEEN 0 AND 4
            OR CAST(JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.slot')) AS DECIMAL(20,0)) NOT BETWEEN 0 AND 39 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid item ID, quantity or position.';
        END IF;
        SET itemContainer = CAST(JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.container')) AS UNSIGNED);
        SET itemSlot = CAST(JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.slot')) AS UNSIGNED);
        SET itemCount = CAST(JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.count')) AS UNSIGNED);
        IF (itemContainer IN (0, 4) AND itemCount <> 1) OR (itemContainer = 4 AND itemSlot > 6) THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid equipment position or quantity.';
        END IF;
        SET itemIndex = itemIndex + 1;
    END WHILE;
    UPDATE characters SET level = nextLevel, skill_points = nextSp, experience = nextExperience, updated_at = UTC_TIMESTAMP(6)
    WHERE character_id = inCharacterId;
    DELETE FROM character_skills WHERE character_id = inCharacterId;
    SET itemIndex = 0;
    WHILE itemIndex < skillsCount DO
        SET skillId = JSON_UNQUOTE(JSON_EXTRACT(skillKeys, CONCAT('$[', itemIndex, ']')));
        SET skillRank = CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson,
            CONCAT('$.skillLevels.', JSON_QUOTE(skillId)))) AS UNSIGNED);
        INSERT INTO character_skills (character_id, skill_id, skill_level)
        VALUES (inCharacterId, CONVERT(skillId USING binary), skillRank);
        SET itemIndex = itemIndex + 1;
    END WHILE;
    DELETE FROM character_items WHERE character_id = inCharacterId;
    SET itemIndex = 0;
    WHILE itemIndex < itemsCount DO
        SET itemValue = JSON_EXTRACT(inInventoryJson, CONCAT('$.items[', itemIndex, ']'));
        SET instanceId = JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.instanceId'));
        SET definitionId = JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.definitionId'));
        SET itemCount = CAST(JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.count')) AS UNSIGNED);
        SET itemContainer = CAST(JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.container')) AS UNSIGNED);
        SET itemSlot = CAST(JSON_UNQUOTE(JSON_EXTRACT(itemValue, '$.slot')) AS UNSIGNED);
        INSERT INTO character_items (instance_id, character_id, definition_id, quantity, container, slot)
        VALUES (UNHEX(instanceId), inCharacterId, CONVERT(definitionId USING binary), itemCount, itemContainer, itemSlot);
        SET itemIndex = itemIndex + 1;
    END WHILE;

END$$

DROP PROCEDURE emit_character_state$$
CREATE PROCEDURE emit_character_state
(
    IN inResultCode INT,
    IN inCharacterId BIGINT UNSIGNED,
    IN inIncludeInventory BOOLEAN
)
READS SQL DATA
SQL SECURITY DEFINER
BEGIN
    -- Internal only: no runtime EXECUTE grant. Public routines hold ownership locks.
    IF inCharacterId = 0 THEN
        SELECT CAST(inResultCode AS SIGNED) AS result_code,
            CAST(0 AS UNSIGNED) AS character_id, CAST(0 AS UNSIGNED) AS revision,
            CAST(0 AS UNSIGNED) AS owner_generation,
            CAST('' AS CHAR CHARACTER SET utf8mb4) AS name, CAST(0 AS UNSIGNED) AS definition_id,
            CAST('' AS CHAR CHARACTER SET utf8mb4) AS progression_json,
            CAST('' AS CHAR CHARACTER SET utf8mb4) AS inventory_json;
    ELSE
        SELECT CAST(inResultCode AS SIGNED) AS result_code, c.character_id,
            COALESCE(s.revision, CAST(0 AS UNSIGNED)) AS revision,
            COALESCE(s.owner_generation, CAST(0 AS UNSIGNED)) AS owner_generation,
            c.name, c.character_definition_id AS definition_id,
            CAST(JSON_OBJECT('level', c.level, 'skillPoints', c.skill_points, 'experience', CAST(c.experience AS CHAR), 'skillLevels',
            COALESCE((SELECT JSON_OBJECTAGG(CONVERT(skill_id USING utf8mb4), skill_level)
                FROM character_skills WHERE character_id = c.character_id), JSON_OBJECT())) AS CHAR CHARACTER SET utf8mb4) AS progression_json,
            CAST(IF(inIncludeInventory, JSON_OBJECT('items', COALESCE((SELECT JSON_ARRAYAGG(JSON_OBJECT(
            'instanceId', LOWER(HEX(instance_id)), 'definitionId', CONVERT(definition_id USING utf8mb4),
            'count', quantity, 'container', container, 'slot', slot))
            FROM character_items WHERE character_id = c.character_id), JSON_ARRAY())), JSON_OBJECT('items', JSON_ARRAY()))
                AS CHAR CHARACTER SET utf8mb4) AS inventory_json
        FROM characters c LEFT JOIN character_state s ON s.character_id = c.character_id
        WHERE c.character_id = inCharacterId;
    END IF;
END$$

DROP PROCEDURE list_characters$$
CREATE PROCEDURE list_characters
(
    IN inAccountId BIGINT UNSIGNED
)
MODIFIES SQL DATA
SQL SECURITY DEFINER
operation: BEGIN
    DECLARE accountStatus INT DEFAULT NULL;
    IF @@session.autocommit <> 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Explicit transaction required.';
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET accountStatus = NULL;
        SELECT status INTO accountStatus FROM accounts WHERE account_id = inAccountId FOR UPDATE;
    END;
    IF accountStatus IS NULL OR accountStatus <> 0 THEN
        CALL emit_character_state(1, 0, TRUE);
        LEAVE operation;
    END IF;
    IF NOT EXISTS (SELECT 1 FROM characters WHERE account_id = inAccountId) THEN
        CALL emit_character_state(0, 0, FALSE);
        LEAVE operation;
    END IF;
    -- A list is a summary. Only claim/save restore the actual inventory.
    SELECT CAST(0 AS SIGNED) AS result_code, c.character_id,
        COALESCE(s.revision, CAST(0 AS UNSIGNED)) AS revision,
        COALESCE(s.owner_generation, CAST(0 AS UNSIGNED)) AS owner_generation,
        c.name, c.character_definition_id AS definition_id,
        CAST(JSON_OBJECT('level', c.level, 'skillPoints', c.skill_points, 'experience', CAST(c.experience AS CHAR), 'skillLevels',
            COALESCE((SELECT JSON_OBJECTAGG(CONVERT(skill_id USING utf8mb4), skill_level)
                FROM character_skills WHERE character_id = c.character_id), JSON_OBJECT())) AS CHAR CHARACTER SET utf8mb4) AS progression_json,
        CAST(JSON_OBJECT('items', JSON_ARRAY()) AS CHAR CHARACTER SET utf8mb4) AS inventory_json
    FROM characters c LEFT JOIN character_state s ON s.character_id = c.character_id
    WHERE c.account_id = inAccountId ORDER BY c.character_id;
END$$

DROP PROCEDURE save_character_state$$
CREATE PROCEDURE save_character_state
(
    IN inAccountId BIGINT UNSIGNED,
    IN inCharacterId BIGINT UNSIGNED,
    IN inOwnerToken VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inOwnerGeneration BIGINT UNSIGNED,
    IN inRequestId VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inExpectedRevision BIGINT UNSIGNED,
    IN inOperationJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
    IN inProgressionJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
    IN inInventoryJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin
)
MODIFIES SQL DATA
SQL SECURITY DEFINER
operation: BEGIN
    DECLARE accountStatus INT DEFAULT NULL;
    DECLARE selectedCharacterId BIGINT UNSIGNED DEFAULT 0;
    DECLARE currentToken BINARY(32) DEFAULT NULL;
    DECLARE currentGeneration BIGINT UNSIGNED DEFAULT NULL;
    DECLARE currentRevision BIGINT UNSIGNED DEFAULT 0;
    DECLARE existingCharacterId BIGINT UNSIGNED DEFAULT 0;
    DECLARE existingKind VARBINARY(16) DEFAULT NULL;
    DECLARE existingHash BINARY(32) DEFAULT NULL;
    DECLARE payloadHash BINARY(32);
    IF inOwnerToken IS NULL OR OCTET_LENGTH(inOwnerToken) <> 64 OR inOwnerToken NOT REGEXP '^[0-9a-f]{64}$' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid owner token.';
    END IF;
    IF inRequestId IS NULL OR OCTET_LENGTH(inRequestId) <> 64 OR inRequestId NOT REGEXP '^[0-9a-f]{64}$' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid request ID.';
    END IF;
    IF @@session.autocommit <> 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Explicit transaction required.';
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET accountStatus = NULL;
        SELECT status INTO accountStatus FROM accounts WHERE account_id = inAccountId FOR UPDATE;
    END;
    IF accountStatus IS NULL OR accountStatus <> 0 THEN
        CALL emit_character_state(1, 0, TRUE);
        LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET selectedCharacterId = 0;
        SELECT character_id INTO selectedCharacterId FROM characters
        WHERE character_id = inCharacterId AND account_id = inAccountId FOR UPDATE;
    END;
    IF selectedCharacterId = 0 THEN
        CALL emit_character_state(2, 0, TRUE);
        LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET currentGeneration = NULL;
        SELECT owner_token, owner_generation, revision INTO currentToken, currentGeneration, currentRevision
        FROM character_state WHERE character_id = inCharacterId FOR UPDATE;
    END;
    IF currentGeneration IS NULL OR inOwnerGeneration IS NULL OR currentGeneration <> inOwnerGeneration
        OR currentToken IS NULL OR currentToken <> UNHEX(inOwnerToken) THEN
        CALL emit_character_state(5, 0, TRUE);
        LEAVE operation;
    END IF;
    IF inExpectedRevision IS NULL OR inOperationJson IS NULL OR OCTET_LENGTH(inOperationJson) > 2048
        OR NOT JSON_VALID(inOperationJson) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid original operation.';
    END IF;
    IF COALESCE(JSON_TYPE(CAST(inOperationJson AS JSON)), '') <> 'OBJECT' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid original operation object.';
    END IF;
    -- Identity describes the original command, not a newly calculated snapshot.
    SET payloadHash = UNHEX(SHA2(CONCAT('SAVE:', inAccountId, ':', inCharacterId, ':', inExpectedRevision, ':',
        CAST(CAST(inOperationJson AS JSON) AS CHAR CHARACTER SET utf8mb4)), 256));
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET existingCharacterId = 0;
        SELECT character_id, request_kind, payload_hash INTO existingCharacterId, existingKind, existingHash
        FROM character_operations WHERE account_id = inAccountId AND request_id = UNHEX(inRequestId) FOR UPDATE;
    END;
    IF existingCharacterId <> 0 THEN
        IF existingCharacterId = inCharacterId AND existingKind = _binary'SAVE' AND existingHash = payloadHash THEN
            CALL emit_character_state(0, inCharacterId, TRUE);
        ELSE
            CALL emit_character_state(6, inCharacterId, TRUE);
        END IF;
        LEAVE operation;
    END IF;
    -- Previously committed SAVE receipts remain readable; only new writes are fenced by Pending.
    IF EXISTS (SELECT 1 FROM character_item_uses WHERE active_character_id = inCharacterId) THEN
        CALL emit_character_state(7, inCharacterId, TRUE);
        LEAVE operation;
    END IF;
    IF currentRevision <> inExpectedRevision THEN
        CALL emit_character_state(3, inCharacterId, TRUE);
        LEAVE operation;
    END IF;
    -- Duplicate receipts are checked first: retransmission never re-applies its proposed state.
    IF inInventoryJson IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Legacy save requires an inventory snapshot.';
    END IF;
    CALL apply_quest_inventory(inCharacterId, inProgressionJson, inInventoryJson);
    UPDATE character_state SET revision = revision + 1 WHERE character_id = inCharacterId;
    INSERT INTO character_operations (account_id, request_id, character_id, request_kind, payload_hash, revision, created_at)
    VALUES (inAccountId, UNHEX(inRequestId), inCharacterId, _binary'SAVE', payloadHash, currentRevision + 1, UTC_TIMESTAMP(6));
    CALL emit_character_state(0, inCharacterId, TRUE);
END$$

DROP PROCEDURE emit_quest_state$$
CREATE PROCEDURE emit_quest_state
(
    IN inResultCode INT,
    IN inCharacterId BIGINT UNSIGNED,
    IN inSnapshotMode INT,
    IN inServerNowMs BIGINT UNSIGNED,
    IN inQuestIdsJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin
)
READS SQL DATA
SQL SECURITY DEFINER
BEGIN
    -- Private helper. The public caller already holds account/character/state locks.
    IF inCharacterId = 0 THEN
        SELECT CAST(inResultCode AS SIGNED) AS result_code,
            CAST(0 AS UNSIGNED) AS character_id, CAST(0 AS UNSIGNED) AS revision,
            CAST(0 AS UNSIGNED) AS owner_generation, inServerNowMs AS server_now_ms,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS progression_json,
            CAST(NULL AS CHAR CHARACTER SET utf8mb4) AS inventory_json,
            CAST(JSON_OBJECT('trackedQuestIds', JSON_ARRAY()) AS CHAR CHARACTER SET utf8mb4) AS tracking_json;
    ELSE
        SELECT CAST(inResultCode AS SIGNED) AS result_code, c.character_id,
            s.revision, s.owner_generation, inServerNowMs AS server_now_ms,
            CAST(IF(inSnapshotMode IN (1, 2), JSON_OBJECT('level', c.level, 'skillPoints', c.skill_points, 'experience', CAST(c.experience AS CHAR),
                'skillLevels', COALESCE((SELECT JSON_OBJECTAGG(CONVERT(skill_id USING utf8mb4), skill_level)
                FROM character_skills WHERE character_id = c.character_id), JSON_OBJECT())), NULL)
                AS CHAR CHARACTER SET utf8mb4) AS progression_json,
            CAST(IF(inSnapshotMode = 2, JSON_OBJECT('items', COALESCE((SELECT JSON_ARRAYAGG(JSON_OBJECT(
                'instanceId', LOWER(HEX(instance_id)), 'definitionId', CONVERT(definition_id USING utf8mb4),
                'count', quantity, 'container', container, 'slot', slot))
                FROM character_items WHERE character_id = c.character_id), JSON_ARRAY())), NULL)
                AS CHAR CHARACTER SET utf8mb4) AS inventory_json,
            CAST(COALESCE(t.tracking_json, JSON_OBJECT('trackedQuestIds', JSON_ARRAY()))
                AS CHAR CHARACTER SET utf8mb4) AS tracking_json
        FROM characters c JOIN character_state s ON s.character_id = c.character_id
        LEFT JOIN character_quest_tracking t ON t.character_id = c.character_id
        WHERE c.character_id = inCharacterId;
    END IF;
    -- At most 16 live instances plus 64 summaries; terminal rows remain in the DB.
    SELECT quest_id, cycle_key, state_json FROM
    (
        SELECT CONVERT(q.quest_id USING utf8mb4) AS quest_id,
            CONVERT(q.cycle_key USING utf8mb4) AS cycle_key,
            CAST(q.state_json AS CHAR CHARACTER SET utf8mb4) AS state_json
        FROM character_quests q WHERE q.character_id = inCharacterId
            AND q.status IN (_binary'Active', _binary'ReadyToReport', _binary'RewardPending')
        UNION ALL
        SELECT CONVERT(q.quest_id USING utf8mb4), CAST('' AS CHAR CHARACTER SET utf8mb4),
            CAST(JSON_OBJECT('kind', 'Summary',
                'completedEver', JSON_EXTRACT(IF(EXISTS (SELECT 1 FROM character_quests h
                    WHERE h.character_id = q.character_id AND h.quest_id = q.quest_id
                        AND h.status = _binary'Completed'), 'true', 'false'), '$'),
                'lastCompletedCycle', COALESCE((SELECT CONVERT(h.cycle_key USING utf8mb4)
                    FROM character_quests h WHERE h.character_id = q.character_id AND h.quest_id = q.quest_id
                        AND h.status = _binary'Completed' ORDER BY h.accept_generation DESC LIMIT 1), ''),
                'lastAcceptGeneration', CAST(q.accept_generation AS CHAR),
                'lastState', CONVERT(q.status USING utf8mb4),
                'lastCycleKey', CONVERT(q.cycle_key USING utf8mb4)) AS CHAR CHARACTER SET utf8mb4)
        FROM JSON_TABLE(inQuestIdsJson, '$[*]' COLUMNS (projected_id VARCHAR(64) PATH '$')) AS ids
        JOIN character_quests q ON q.character_id = inCharacterId
            AND q.quest_id = CONVERT(ids.projected_id USING binary)
            AND q.accept_generation = (SELECT MAX(h.accept_generation) FROM character_quests h
                WHERE h.character_id = q.character_id AND h.quest_id = q.quest_id)
    ) AS snapshot_rows ORDER BY quest_id, cycle_key;
END$$

DROP PROCEDURE save_character_quests$$
CREATE PROCEDURE save_character_quests
(
    IN inAccountId BIGINT UNSIGNED,
    IN inCharacterId BIGINT UNSIGNED,
    IN inOwnerToken VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inOwnerGeneration BIGINT UNSIGNED,
    IN inRequestId VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inExpectedRevision BIGINT UNSIGNED,
    IN inMode VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin,
    IN inOperationJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
    IN inChangesJson MEDIUMTEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
    IN inProgressionJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
    IN inInventoryJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
    IN inQuestIdsJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin
)
MODIFIES SQL DATA
SQL SECURITY DEFINER
operation: BEGIN
    DECLARE accountStatus INT DEFAULT NULL;
    DECLARE selectedCharacterId BIGINT UNSIGNED DEFAULT 0;
    DECLARE currentToken BINARY(32) DEFAULT NULL;
    DECLARE currentGeneration BIGINT UNSIGNED DEFAULT NULL;
    DECLARE currentRevision BIGINT UNSIGNED DEFAULT 0;
    DECLARE existingCharacterId BIGINT UNSIGNED DEFAULT 0;
    DECLARE existingKind VARBINARY(16) DEFAULT NULL;
    DECLARE existingHash BINARY(32) DEFAULT NULL;
    DECLARE payloadHash BINARY(32);
    DECLARE observedMs BIGINT UNSIGNED;
    DECLARE changeCount INT UNSIGNED;
    DECLARE claimTransitions INT UNSIGNED DEFAULT 0;
    DECLARE changeIndex INT UNSIGNED DEFAULT 0;
    DECLARE changeValue JSON;
    DECLARE stateValue JSON;
    DECLARE oldValue JSON;
    DECLARE questId VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin;
    DECLARE cycleKey VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin;
    DECLARE nextStatus VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin;
    DECLARE oldStatus VARBINARY(16) DEFAULT NULL;
    DECLARE nextGeneration BIGINT UNSIGNED;
    DECLARE oldGeneration BIGINT UNSIGNED;
    DECLARE maximumGeneration BIGINT UNSIGNED;
    DECLARE resultCode INT DEFAULT 0;
    DECLARE seenKeys JSON;
    DECLARE trackingValue JSON;
    DECLARE arrayIndex INT UNSIGNED;
    DECLARE counterIndex INT UNSIGNED;
    DECLARE stageCount INT UNSIGNED;
    DECLARE activeCount INT UNSIGNED;
    DECLARE resetHour INT UNSIGNED;
    DECLARE shiftedUtc DATETIME(6);
    DECLARE expectedCycle VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin;
    DECLARE repeatKind VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin;
    DECLARE intervalName VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin;
    DECLARE intervalStart DATETIME;
    DECLARE intervalEnd DATETIME;
    DECLARE utcValue VARCHAR(20) CHARACTER SET ascii COLLATE ascii_bin;
    DECLARE currentStage INT UNSIGNED;
    DECLARE eventRoom BIGINT UNSIGNED DEFAULT NULL;
    DECLARE eventSequence BIGINT UNSIGNED DEFAULT NULL;
    DECLARE eventIncarnation BINARY(16) DEFAULT NULL;
    DECLARE eventHash BINARY(32) DEFAULT NULL;
    DECLARE savedEventHash BINARY(32) DEFAULT NULL;
    DECLARE projectionIndex INT UNSIGNED DEFAULT 0;
    DECLARE projectionId VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin;
    DECLARE previousProjectionId VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin DEFAULT NULL;
    IF inQuestIdsJson IS NULL OR OCTET_LENGTH(inQuestIdsJson) > 8192 OR NOT JSON_VALID(inQuestIdsJson) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid quest projection.';
    END IF;
    IF JSON_TYPE(CAST(inQuestIdsJson AS JSON)) <> 'ARRAY' OR JSON_LENGTH(inQuestIdsJson) > 64 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid quest projection shape.';
    END IF;
    WHILE projectionIndex < JSON_LENGTH(inQuestIdsJson) DO
        IF COALESCE(JSON_TYPE(JSON_EXTRACT(inQuestIdsJson, CONCAT('$[', projectionIndex, ']'))), '') <> 'STRING'
            OR JSON_UNQUOTE(JSON_EXTRACT(inQuestIdsJson, CONCAT('$[', projectionIndex, ']'))) NOT REGEXP '^[A-Za-z][A-Za-z0-9_.-]{0,63}$' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid quest projection identifier.';
        END IF;
        SET projectionId = JSON_UNQUOTE(JSON_EXTRACT(inQuestIdsJson, CONCAT('$[', projectionIndex, ']')));
        IF previousProjectionId IS NOT NULL AND STRCMP(previousProjectionId, projectionId) >= 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Quest projection must be unique and sorted.';
        END IF;
        SET previousProjectionId = projectionId; SET projectionIndex = projectionIndex + 1;
    END WHILE;
    IF inOwnerToken IS NULL OR OCTET_LENGTH(inOwnerToken) <> 64 OR inOwnerToken NOT REGEXP '^[0-9a-f]{64}$'
        OR inRequestId IS NULL OR OCTET_LENGTH(inRequestId) <> 64 OR inRequestId NOT REGEXP '^[0-9a-f]{64}$'
        OR inExpectedRevision IS NULL OR inMode IS NULL OR inMode NOT IN ('Progress', 'Growth', 'Inventory', 'Claim')
        OR @@session.autocommit <> 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid quest command identity or transaction.';
    END IF;
    IF inOperationJson IS NULL OR OCTET_LENGTH(inOperationJson) > 2048 OR NOT JSON_VALID(inOperationJson)
        OR inChangesJson IS NULL OR OCTET_LENGTH(inChangesJson) > 262144 OR NOT JSON_VALID(inChangesJson) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid quest command JSON.';
    END IF;
    IF JSON_TYPE(CAST(inOperationJson AS JSON)) <> 'OBJECT'
        OR JSON_TYPE(CAST(inChangesJson AS JSON)) <> 'OBJECT'
        OR COALESCE(JSON_TYPE(JSON_EXTRACT(inChangesJson, '$.upserts')), '') <> 'ARRAY'
        OR JSON_LENGTH(JSON_EXTRACT(inChangesJson, '$.upserts')) > 32
        OR JSON_LENGTH(CAST(inChangesJson AS JSON)) NOT IN (1, 2)
        OR (JSON_LENGTH(CAST(inChangesJson AS JSON)) = 2
            AND NOT JSON_CONTAINS_PATH(inChangesJson, 'one', '$.trackedQuestIds'))
        OR (inMode = 'Progress' AND (inProgressionJson IS NOT NULL OR inInventoryJson IS NOT NULL))
        OR (inMode = 'Growth' AND (inProgressionJson IS NULL OR inInventoryJson IS NOT NULL))
        OR (inMode IN ('Inventory', 'Claim') AND (inProgressionJson IS NULL OR inInventoryJson IS NULL)) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid quest changes or snapshot mode.';
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET accountStatus = NULL;
        SELECT status INTO accountStatus FROM accounts WHERE account_id = inAccountId FOR UPDATE;
    END;
    SET observedMs = TIMESTAMPDIFF(MICROSECOND, CAST('1970-01-01 00:00:00' AS DATETIME), UTC_TIMESTAMP(3)) DIV 1000;
    IF accountStatus IS NULL OR accountStatus <> 0 THEN
        CALL emit_quest_state(1, 0, FALSE, observedMs, inQuestIdsJson); LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET selectedCharacterId = 0;
        SELECT character_id INTO selectedCharacterId FROM characters
        WHERE character_id = inCharacterId AND account_id = inAccountId FOR UPDATE;
    END;
    IF selectedCharacterId = 0 THEN
        CALL emit_quest_state(2, 0, FALSE, observedMs, inQuestIdsJson); LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET currentGeneration = NULL;
        SELECT owner_token, owner_generation, revision INTO currentToken, currentGeneration, currentRevision
        FROM character_state WHERE character_id = inCharacterId FOR UPDATE;
    END;
    SET observedMs = TIMESTAMPDIFF(MICROSECOND, CAST('1970-01-01 00:00:00' AS DATETIME), UTC_TIMESTAMP(3)) DIV 1000;
    IF currentGeneration IS NULL OR inOwnerGeneration IS NULL OR currentGeneration <> inOwnerGeneration
        OR currentToken IS NULL OR currentToken <> UNHEX(inOwnerToken) THEN
        CALL emit_quest_state(5, 0, FALSE, observedMs, inQuestIdsJson); LEAVE operation;
    END IF;
    SET payloadHash = UNHEX(SHA2(CONCAT('QUEST:', inMode, ':', inAccountId, ':', inCharacterId, ':',
        inExpectedRevision, ':', CAST(CAST(inOperationJson AS JSON) AS CHAR CHARACTER SET utf8mb4)), 256));
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET existingCharacterId = 0;
        SELECT character_id, request_kind, payload_hash INTO existingCharacterId, existingKind, existingHash
        FROM character_operations WHERE account_id = inAccountId AND request_id = UNHEX(inRequestId) FOR UPDATE;
    END;
    IF existingCharacterId <> 0 THEN
        IF existingCharacterId = inCharacterId AND existingKind = _binary'QUEST' AND existingHash = payloadHash THEN
            CALL emit_quest_state(0, inCharacterId, CASE WHEN inMode = 'Growth' THEN 1 WHEN inMode IN ('Inventory', 'Claim') THEN 2 ELSE 0 END, observedMs, inQuestIdsJson);
        ELSE
            CALL emit_quest_state(6, inCharacterId, FALSE, observedMs, inQuestIdsJson);
        END IF;
        LEAVE operation;
    END IF;
    -- A Room event has a durable identity independent of retry request IDs and revisions.
    IF JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.action')) = 'RoomEvent' THEN
        IF inMode NOT IN ('Progress', 'Growth')
            OR COALESCE(JSON_TYPE(JSON_EXTRACT(inOperationJson, '$.originIncarnation')), '') <> 'STRING'
            OR JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.originIncarnation')) NOT REGEXP '^[0-9a-f]{32}$'
            OR COALESCE(JSON_TYPE(JSON_EXTRACT(inOperationJson, '$.roomId')), '') <> 'STRING'
            OR COALESCE(JSON_TYPE(JSON_EXTRACT(inOperationJson, '$.eventSeq')), '') <> 'STRING'
            OR JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.roomId')) NOT REGEXP '^[1-9][0-9]{0,19}$'
            OR JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.eventSeq')) NOT REGEXP '^[1-9][0-9]{0,19}$'
            OR CAST(JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.roomId')) AS DECIMAL(20,0)) > 18446744073709551615
            OR CAST(JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.eventSeq')) AS DECIMAL(20,0)) > 18446744073709551615 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid trusted Room event identity.';
        END IF;
        SET eventRoom = CAST(JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.roomId')) AS UNSIGNED);
        SET eventSequence = CAST(JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.eventSeq')) AS UNSIGNED);
        SET eventIncarnation = UNHEX(JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.originIncarnation')));
        SET eventHash = UNHEX(SHA2(CAST(CAST(inOperationJson AS JSON) AS CHAR CHARACTER SET utf8mb4), 256));
        BEGIN
            DECLARE CONTINUE HANDLER FOR NOT FOUND SET savedEventHash = NULL;
            SELECT payload_hash INTO savedEventHash FROM character_quest_events
            WHERE character_id = inCharacterId AND room_id = eventRoom
                AND origin_incarnation = eventIncarnation AND event_sequence = eventSequence FOR UPDATE;
        END;
        IF savedEventHash IS NOT NULL THEN
            IF savedEventHash <> eventHash THEN
                CALL emit_quest_state(6, inCharacterId, FALSE, observedMs, inQuestIdsJson);
            ELSE
                INSERT INTO character_operations (account_id, request_id, character_id, request_kind, payload_hash, revision, created_at)
                VALUES (inAccountId, UNHEX(inRequestId), inCharacterId, _binary'QUEST', payloadHash, currentRevision, UTC_TIMESTAMP(6));
                CALL emit_quest_state(0, inCharacterId, CASE WHEN inMode = 'Growth' THEN 1 WHEN inMode IN ('Inventory', 'Claim') THEN 2 ELSE 0 END, observedMs, inQuestIdsJson);
            END IF;
            LEAVE operation;
        END IF;
    END IF;
    IF EXISTS (SELECT 1 FROM character_item_uses WHERE active_character_id = inCharacterId) THEN
        CALL emit_quest_state(7, inCharacterId, FALSE, observedMs, inQuestIdsJson); LEAVE operation;
    END IF;
    IF currentRevision <> inExpectedRevision THEN
        CALL emit_quest_state(3, inCharacterId, FALSE, observedMs, inQuestIdsJson); LEAVE operation;
    END IF;
    SET changeCount = JSON_LENGTH(JSON_EXTRACT(inChangesJson, '$.upserts'));
    SET seenKeys = JSON_ARRAY();
    -- Logical rejections roll back every row in the batch before returning their result code.
    SAVEPOINT quest_write;
    changes_block: BEGIN
        IF inMode = 'Claim' AND JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.action')) <> 'Claim' THEN
            SET resultCode = 14; LEAVE changes_block;
        END IF;
        WHILE changeIndex < changeCount DO
            SET changeValue = JSON_EXTRACT(inChangesJson, CONCAT('$.upserts[', changeIndex, ']'));
            SET stateValue = JSON_EXTRACT(changeValue, '$.state');
            IF COALESCE(JSON_TYPE(changeValue), '') <> 'OBJECT' OR JSON_LENGTH(changeValue) <> 3
                OR COALESCE(JSON_TYPE(stateValue), '') <> 'OBJECT' OR JSON_LENGTH(stateValue) <> 13
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(changeValue, '$.questId')), '') <> 'STRING'
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(changeValue, '$.cycleKey')), '') <> 'STRING'
                OR JSON_UNQUOTE(JSON_EXTRACT(changeValue, '$.questId')) NOT REGEXP '^[A-Za-z][A-Za-z0-9_.-]{0,63}$'
                OR JSON_UNQUOTE(JSON_EXTRACT(changeValue, '$.cycleKey')) NOT REGEXP '^(once|[0-9]{4}-[0-9]{2}-[0-9]{2})$' THEN
                SET resultCode = 14; LEAVE changes_block;
            END IF;
            SET questId = JSON_UNQUOTE(JSON_EXTRACT(changeValue, '$.questId'));
            SET cycleKey = JSON_UNQUOTE(JSON_EXTRACT(changeValue, '$.cycleKey'));
            IF JSON_CONTAINS(seenKeys, JSON_OBJECT('questId', questId, 'cycleKey', cycleKey)) THEN
                SET resultCode = 14; LEAVE changes_block;
            END IF;
            SET seenKeys = JSON_ARRAY_APPEND(seenKeys, '$', JSON_OBJECT('questId', questId, 'cycleKey', cycleKey));
            IF COALESCE(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.kind')), '') <> 'Instance'
                OR COALESCE(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.questId')), '') <> questId
                OR COALESCE(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.cycleKey')), '') <> cycleKey
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.definitionVersion')), '') <> 'INTEGER'
                OR CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.definitionVersion')) AS DECIMAL(20,0)) NOT BETWEEN 1 AND 4294967295
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.acceptGeneration')), '') <> 'STRING'
                OR JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.acceptGeneration')) NOT REGEXP '^[1-9][0-9]{0,19}$'
                OR CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.acceptGeneration')) AS DECIMAL(20,0)) > 18446744073709551615
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.stageIndex')), '') <> 'INTEGER'
                OR CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.stageIndex')) AS DECIMAL(20,0)) NOT BETWEEN 0 AND 8
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.counters')), '') <> 'ARRAY'
                OR JSON_LENGTH(JSON_EXTRACT(stateValue, '$.counters')) NOT BETWEEN 1 AND 8
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.definition')), '') <> 'OBJECT'
                OR OCTET_LENGTH(CAST(JSON_EXTRACT(stateValue, '$.definition') AS CHAR CHARACTER SET utf8mb4)) > 4096
                OR COALESCE(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.definition.id')), '') <> questId
                OR NOT (JSON_EXTRACT(stateValue, '$.definition.version') <=> JSON_EXTRACT(stateValue, '$.definitionVersion'))
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.acceptedAtMs')), '') <> 'STRING'
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.updatedAtMs')), '') <> 'STRING'
                OR JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.acceptedAtMs')) NOT REGEXP '^(0|[1-9][0-9]{0,19})$'
                OR JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.updatedAtMs')) NOT REGEXP '^(0|[1-9][0-9]{0,19})$'
                OR CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.acceptedAtMs')) AS DECIMAL(20,0)) > observedMs
                OR CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.updatedAtMs')) AS DECIMAL(20,0)) > observedMs
                OR NOT JSON_CONTAINS_PATH(stateValue, 'one', '$.roomCursor') THEN
                SET resultCode = 14; LEAVE changes_block;
            END IF;
            SET nextStatus = JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.state'));
            IF nextStatus IS NULL OR nextStatus NOT IN ('Active', 'ReadyToReport', 'RewardPending', 'Completed', 'Abandoned', 'Expired') THEN
                SET resultCode = 14; LEAVE changes_block;
            END IF;
            IF COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.resetPolicy')), '') <> 'OBJECT'
                OR JSON_LENGTH(JSON_EXTRACT(stateValue, '$.resetPolicy')) <> 4
                OR COALESCE(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.resetPolicy.timezone')), '') <> 'UTC'
                OR COALESCE(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.resetPolicy.weeklyDay')), '') <> 'Monday'
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.resetPolicy.dailyHour')), '') <> 'INTEGER'
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.resetPolicy.weeklyHour')), '') <> 'INTEGER'
                OR CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.resetPolicy.dailyHour')) AS DECIMAL(20,0)) NOT BETWEEN 0 AND 23
                OR CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.resetPolicy.weeklyHour')) AS DECIMAL(20,0)) NOT BETWEEN 0 AND 23 THEN
                SET resultCode = 14; LEAVE changes_block;
            END IF;
            SET repeatKind = JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.definition.repeat'));
            IF repeatKind IS NULL OR repeatKind NOT IN ('Once', 'Daily', 'Weekly') THEN
                SET resultCode = 14; LEAVE changes_block;
            END IF;
            SET resetHour = IF(repeatKind = 'Weekly',
                CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.resetPolicy.weeklyHour')) AS UNSIGNED),
                CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.resetPolicy.dailyHour')) AS UNSIGNED));
            SET shiftedUtc = DATE_SUB(UTC_TIMESTAMP(3), INTERVAL resetHour HOUR);
            SET expectedCycle = CASE WHEN repeatKind = 'Once' THEN 'once'
                WHEN repeatKind = 'Daily' THEN DATE_FORMAT(shiftedUtc, '%Y-%m-%d')
                ELSE DATE_FORMAT(DATE_SUB(shiftedUtc, INTERVAL WEEKDAY(shiftedUtc) DAY), '%Y-%m-%d') END;
            SET nextGeneration = CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.acceptGeneration')) AS UNSIGNED);
            SET currentStage = CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.stageIndex')) AS UNSIGNED);
            SET stageCount = JSON_LENGTH(JSON_EXTRACT(stateValue, '$.counters'));
            IF currentStage > stageCount OR (nextStatus = 'Active' AND currentStage = stageCount)
                OR (nextStatus IN ('ReadyToReport', 'RewardPending', 'Completed') AND currentStage <> stageCount)
                OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.definition.steps')), '') <> 'ARRAY'
                OR JSON_LENGTH(JSON_EXTRACT(stateValue, '$.definition.steps')) <> stageCount THEN
                SET resultCode = 14; LEAVE changes_block;
            END IF;
            SET arrayIndex = 0;
            WHILE arrayIndex < stageCount DO
                IF COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, CONCAT('$.counters[', arrayIndex, ']'))), '') <> 'ARRAY'
                    OR JSON_LENGTH(JSON_EXTRACT(stateValue, CONCAT('$.counters[', arrayIndex, ']'))) NOT BETWEEN 1 AND 8
                    OR JSON_LENGTH(JSON_EXTRACT(stateValue, CONCAT('$.counters[', arrayIndex, ']')))
                        <> JSON_LENGTH(JSON_EXTRACT(stateValue, CONCAT('$.definition.steps[', arrayIndex, '].objectives'))) THEN
                    SET resultCode = 14; LEAVE changes_block;
                END IF;
                SET counterIndex = 0;
                WHILE counterIndex < JSON_LENGTH(JSON_EXTRACT(stateValue, CONCAT('$.counters[', arrayIndex, ']'))) DO
                    IF COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, CONCAT('$.counters[', arrayIndex, '][', counterIndex, ']'))), '') <> 'INTEGER'
                        OR CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, CONCAT('$.counters[', arrayIndex, '][', counterIndex, ']'))) AS DECIMAL(20,0)) NOT BETWEEN 0 AND 4294967295
                        OR CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, CONCAT('$.counters[', arrayIndex, '][', counterIndex, ']'))) AS DECIMAL(20,0))
                            > CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, CONCAT('$.definition.steps[', arrayIndex, '].objectives[', counterIndex, '].count'))) AS DECIMAL(20,0))
                        OR (arrayIndex < currentStage AND NOT (JSON_EXTRACT(stateValue, CONCAT('$.counters[', arrayIndex, '][', counterIndex, ']'))
                            <=> JSON_EXTRACT(stateValue, CONCAT('$.definition.steps[', arrayIndex, '].objectives[', counterIndex, '].count'))))
                        OR (arrayIndex > currentStage AND JSON_UNQUOTE(JSON_EXTRACT(stateValue, CONCAT('$.counters[', arrayIndex, '][', counterIndex, ']'))) <> '0') THEN
                        SET resultCode = 14; LEAVE changes_block;
                    END IF;
                    SET counterIndex = counterIndex + 1;
                END WHILE;
                SET arrayIndex = arrayIndex + 1;
            END WHILE;
            IF JSON_TYPE(JSON_EXTRACT(stateValue, '$.roomCursor')) <> 'NULL' THEN
                IF COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.roomCursor')), '') <> 'OBJECT'
                    OR JSON_LENGTH(JSON_EXTRACT(stateValue, '$.roomCursor')) <> 3
                    OR COALESCE(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.roomCursor.incarnation')), '') NOT REGEXP '^[0-9a-f]{32}$'
                    OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.roomCursor.roomId')), '') <> 'STRING'
                    OR COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.roomCursor.sequence')), '') <> 'STRING'
                    OR JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.roomCursor.roomId')) NOT REGEXP '^[1-9][0-9]{0,19}$'
                    OR JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.roomCursor.sequence')) NOT REGEXP '^(0|[1-9][0-9]{0,19})$'
                    OR CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.roomCursor.roomId')) AS DECIMAL(20,0)) > 18446744073709551615
                    OR CAST(JSON_UNQUOTE(JSON_EXTRACT(stateValue, '$.roomCursor.sequence')) AS DECIMAL(20,0)) > 18446744073709551615 THEN
                    SET resultCode = 14; LEAVE changes_block;
                END IF;
            END IF;
            SET oldStatus = NULL; SET oldValue = NULL; SET oldGeneration = 0;
            BEGIN
                DECLARE CONTINUE HANDLER FOR NOT FOUND SET oldStatus = NULL;
                SELECT status, accept_generation, state_json INTO oldStatus, oldGeneration, oldValue
                FROM character_quests WHERE character_id = inCharacterId AND quest_id = CONVERT(questId USING binary)
                    AND cycle_key = CONVERT(cycleKey USING binary) ORDER BY accept_generation DESC LIMIT 1 FOR UPDATE;
            END;
            IF oldStatus = _binary'Completed' THEN SET resultCode = 13; LEAVE changes_block; END IF;
            IF oldStatus IS NULL OR oldStatus IN (_binary'Abandoned', _binary'Expired') THEN
                SELECT COALESCE(MAX(accept_generation), 0) INTO maximumGeneration FROM character_quests
                WHERE character_id = inCharacterId AND quest_id = CONVERT(questId USING binary);
                IF maximumGeneration = 18446744073709551615 OR nextGeneration <> maximumGeneration + 1
                    OR nextStatus NOT IN ('Active', 'ReadyToReport', 'RewardPending') THEN
                    SET resultCode = 12; LEAVE changes_block;
                END IF;
                IF cycleKey <> expectedCycle THEN SET resultCode = 14; LEAVE changes_block; END IF;
                SET intervalName = 'acceptance';
                SET stateValue = JSON_SET(stateValue, '$.acceptedAtMs', CAST(observedMs AS CHAR));
            ELSE
                IF nextGeneration <> oldGeneration
                    OR SHA2(CAST(JSON_EXTRACT(stateValue, '$.definition') AS CHAR CHARACTER SET utf8mb4), 256)
                        <> SHA2(CAST(JSON_EXTRACT(oldValue, '$.definition') AS CHAR CHARACTER SET utf8mb4), 256)
                    OR SHA2(CAST(JSON_EXTRACT(stateValue, '$.resetPolicy') AS CHAR CHARACTER SET utf8mb4), 256)
                        <> SHA2(CAST(JSON_EXTRACT(oldValue, '$.resetPolicy') AS CHAR CHARACTER SET utf8mb4), 256)
                    OR NOT (JSON_EXTRACT(stateValue, '$.definitionVersion') <=> JSON_EXTRACT(oldValue, '$.definitionVersion'))
                    OR NOT (JSON_EXTRACT(stateValue, '$.acceptedAtMs') <=> JSON_EXTRACT(oldValue, '$.acceptedAtMs')) THEN
                    SET resultCode = 12; LEAVE changes_block;
                END IF;
                SET intervalName = IF(oldStatus = _binary'Active', 'progress', 'reward');
                IF oldStatus = _binary'Active' AND nextStatus NOT IN ('Abandoned', 'Expired')
                    AND cycleKey <> expectedCycle THEN SET resultCode = 14; LEAVE changes_block; END IF;
                IF (oldStatus = _binary'RewardPending' AND nextStatus NOT IN ('RewardPending', 'Completed', 'Abandoned', 'Expired'))
                    OR (oldStatus = _binary'ReadyToReport' AND nextStatus NOT IN ('ReadyToReport', 'RewardPending', 'Abandoned', 'Expired'))
                    OR (nextStatus NOT IN ('Abandoned', 'Expired') AND currentStage < CAST(JSON_UNQUOTE(JSON_EXTRACT(oldValue, '$.stageIndex')) AS UNSIGNED)) THEN
                    SET resultCode = 14; LEAVE changes_block;
                END IF;
                IF nextStatus = 'Completed' AND (inMode <> 'Claim' OR oldStatus <> _binary'RewardPending'
                    OR COALESCE(JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.questId')), '') <> questId
                    OR COALESCE(JSON_UNQUOTE(JSON_EXTRACT(inOperationJson, '$.cycleKey')), '') <> cycleKey) THEN
                    SET resultCode = 14; LEAVE changes_block;
                END IF;
            END IF;
            IF nextStatus = 'Completed' THEN SET claimTransitions = claimTransitions + 1; END IF;
            -- Keep snapshot ISO UTC windows; state timestamps are decimal epoch milliseconds.
            IF NOT JSON_CONTAINS_PATH(stateValue, 'one', '$.definition.event') THEN
                SET resultCode = 14; LEAVE changes_block;
            END IF;
            IF JSON_TYPE(JSON_EXTRACT(stateValue, '$.definition.event')) <> 'NULL' THEN
                IF COALESCE(JSON_TYPE(JSON_EXTRACT(stateValue, '$.definition.event')), '') <> 'OBJECT' THEN
                    SET resultCode = 14; LEAVE changes_block;
                END IF;
                SET utcValue = JSON_UNQUOTE(JSON_EXTRACT(stateValue, CONCAT('$.definition.event.', intervalName, '.startUtc')));
                IF utcValue IS NULL OR utcValue NOT REGEXP '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$' THEN
                    SET resultCode = 14; LEAVE changes_block;
                END IF;
                SET intervalStart = STR_TO_DATE(utcValue, '%Y-%m-%dT%H:%i:%sZ');
                SET utcValue = JSON_UNQUOTE(JSON_EXTRACT(stateValue, CONCAT('$.definition.event.', intervalName, '.endUtc')));
                IF utcValue IS NULL OR utcValue NOT REGEXP '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$' THEN
                    SET resultCode = 14; LEAVE changes_block;
                END IF;
                SET intervalEnd = STR_TO_DATE(utcValue, '%Y-%m-%dT%H:%i:%sZ');
                IF intervalStart IS NULL OR intervalEnd IS NULL OR intervalStart >= intervalEnd
                    OR (nextStatus NOT IN ('Abandoned', 'Expired')
                        AND (UTC_TIMESTAMP(3) < intervalStart OR UTC_TIMESTAMP(3) >= intervalEnd)) THEN
                    SET resultCode = 14; LEAVE changes_block;
                END IF;
            END IF;
            SET stateValue = JSON_SET(stateValue, '$.updatedAtMs', CAST(observedMs AS CHAR));
            IF OCTET_LENGTH(CAST(stateValue AS CHAR CHARACTER SET utf8mb4)) > 8192 THEN
                SET resultCode = 14; LEAVE changes_block;
            END IF;
            INSERT INTO character_quests (character_id, quest_id, cycle_key, status, accept_generation, state_json, updated_at)
            VALUES (inCharacterId, CONVERT(questId USING binary), CONVERT(cycleKey USING binary), CONVERT(nextStatus USING binary),
                nextGeneration, stateValue, UTC_TIMESTAMP(6))
            ON DUPLICATE KEY UPDATE status = CONVERT(nextStatus USING binary), accept_generation = nextGeneration,
                state_json = stateValue, updated_at = UTC_TIMESTAMP(6);
            SET changeIndex = changeIndex + 1;
        END WHILE;
        IF inMode = 'Claim' AND claimTransitions <> 1 THEN SET resultCode = 14; LEAVE changes_block; END IF;
        SELECT COUNT(*) INTO activeCount FROM character_quests WHERE character_id = inCharacterId
            AND status IN (_binary'Active', _binary'ReadyToReport', _binary'RewardPending');
        IF activeCount > 16 THEN
            SET resultCode = 14; LEAVE changes_block;
        END IF;
        IF JSON_CONTAINS_PATH(inChangesJson, 'one', '$.trackedQuestIds') THEN
            SET trackingValue = JSON_EXTRACT(inChangesJson, '$.trackedQuestIds');
            IF JSON_TYPE(trackingValue) <> 'ARRAY' OR JSON_LENGTH(trackingValue) > 3 THEN
                SET resultCode = 14; LEAVE changes_block;
            END IF;
            SET arrayIndex = 0; SET seenKeys = JSON_ARRAY();
            WHILE arrayIndex < JSON_LENGTH(trackingValue) DO
                SET questId = JSON_UNQUOTE(JSON_EXTRACT(trackingValue, CONCAT('$[', arrayIndex, ']')));
                IF COALESCE(JSON_TYPE(JSON_EXTRACT(trackingValue, CONCAT('$[', arrayIndex, ']'))), '') <> 'STRING'
                    OR questId NOT REGEXP '^[A-Za-z][A-Za-z0-9_.-]{0,63}$'
                    OR JSON_CONTAINS(seenKeys, JSON_QUOTE(questId))
                    OR NOT EXISTS (SELECT 1 FROM character_quests WHERE character_id = inCharacterId
                        AND quest_id = CONVERT(questId USING binary) AND status IN (_binary'Active', _binary'ReadyToReport', _binary'RewardPending')) THEN
                    SET resultCode = 14; LEAVE changes_block;
                END IF;
                SET seenKeys = JSON_ARRAY_APPEND(seenKeys, '$', questId);
                SET arrayIndex = arrayIndex + 1;
            END WHILE;
            INSERT INTO character_quest_tracking (character_id, tracking_json)
            VALUES (inCharacterId, JSON_OBJECT('trackedQuestIds', trackingValue))
            ON DUPLICATE KEY UPDATE tracking_json = JSON_OBJECT('trackedQuestIds', trackingValue);
        END IF;
    END changes_block;
    IF resultCode <> 0 THEN
        ROLLBACK TO SAVEPOINT quest_write;
        RELEASE SAVEPOINT quest_write;
        CALL emit_quest_state(resultCode, inCharacterId, FALSE, observedMs, inQuestIdsJson); LEAVE operation;
    END IF;
    IF inMode <> 'Progress' THEN
        CALL apply_quest_inventory(inCharacterId, inProgressionJson, inInventoryJson);
    END IF;
    UPDATE character_state SET revision = revision + 1 WHERE character_id = inCharacterId;
    INSERT INTO character_operations (account_id, request_id, character_id, request_kind, payload_hash, revision, created_at)
    VALUES (inAccountId, UNHEX(inRequestId), inCharacterId, _binary'QUEST', payloadHash, currentRevision + 1, UTC_TIMESTAMP(6));
    IF eventRoom IS NOT NULL THEN
        INSERT INTO character_quest_events (character_id, room_id, origin_incarnation, event_sequence, payload_hash, created_at)
        VALUES (inCharacterId, eventRoom, eventIncarnation, eventSequence, eventHash, UTC_TIMESTAMP(6));
    END IF;
    RELEASE SAVEPOINT quest_write;
    CALL emit_quest_state(0, inCharacterId, CASE WHEN inMode = 'Growth' THEN 1 WHEN inMode IN ('Inventory', 'Claim') THEN 2 ELSE 0 END, observedMs, inQuestIdsJson);
END$$

CREATE PROCEDURE get_experience_schema_migration_history()
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
    SELECT CAST('6' AS CHAR CHARACTER SET utf8mb4) AS history_format,
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
    WHERE c.TABLE_SCHEMA = DATABASE() AND c.TABLE_NAME IN ('schema_migrations', 'accounts', 'account_identities', 'characters', 'character_skills', 'character_state', 'character_items', 'character_operations', 'character_item_uses', 'character_quest_events', 'character_quest_tracking', 'character_quests')
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
    WHERE t.TABLE_SCHEMA = DATABASE() AND t.TABLE_NAME IN ('schema_migrations', 'accounts', 'account_identities', 'characters', 'character_skills', 'character_state', 'character_items', 'character_operations', 'character_item_uses', 'character_quest_events', 'character_quest_tracking', 'character_quests')
    ORDER BY t.TABLE_NAME, t.CONSTRAINT_NAME, k.ORDINAL_POSITION;

    -- Set 4: actual index shape. Prefix length may be NULL.
    SELECT CAST(TABLE_NAME AS CHAR CHARACTER SET utf8mb4) AS table_name,
        CAST(INDEX_NAME AS CHAR CHARACTER SET utf8mb4) AS index_name,
        CAST(NON_UNIQUE AS CHAR CHARACTER SET utf8mb4) AS non_unique,
        CAST(SEQ_IN_INDEX AS CHAR CHARACTER SET utf8mb4) AS seq_in_index,
        CAST(COLUMN_NAME AS CHAR CHARACTER SET utf8mb4) AS column_name,
        CAST(SUB_PART AS CHAR CHARACTER SET utf8mb4) AS sub_part
    FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN ('schema_migrations', 'accounts', 'account_identities', 'characters', 'character_skills', 'character_state', 'character_items', 'character_operations', 'character_item_uses', 'character_quest_events', 'character_quest_tracking', 'character_quests')
    ORDER BY TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX;

    -- Set 5: routine metadata. definition_utf8 removes charset introducers;
    -- use original SHOW CREATE definitions below for body comparisons.
    SELECT CAST(ROUTINE_NAME AS CHAR CHARACTER SET utf8mb4) AS routine_name,
        CAST(SECURITY_TYPE AS CHAR CHARACTER SET utf8mb4) AS security_type,
        CAST(SQL_DATA_ACCESS AS CHAR CHARACTER SET utf8mb4) AS sql_data_access,
        CAST(ROUTINE_DEFINITION AS CHAR CHARACTER SET utf8mb4) AS routine_definition
    FROM information_schema.ROUTINES
    WHERE ROUTINE_SCHEMA = DATABASE() AND ROUTINE_TYPE = 'PROCEDURE'
        AND ROUTINE_NAME IN ('get_schema_migration_history', 'login_google_account', 'get_auth_account_status', 'get_character_schema_migration_history', 'emit_character_state', 'list_characters', 'create_character', 'claim_character', 'save_character_state', 'release_character', 'get_inventory_schema_migration_history', 'emit_item_use', 'reserve_item_use', 'get_item_use', 'complete_item_use', 'cancel_item_use', 'get_item_use_schema_migration_history', 'emit_quest_state', 'get_character_quests', 'save_character_quests', 'get_quest_schema_migration_history', 'apply_quest_inventory', 'get_experience_schema_migration_history')
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
        AND SPECIFIC_NAME IN ('get_schema_migration_history', 'login_google_account', 'get_auth_account_status', 'get_character_schema_migration_history', 'emit_character_state', 'list_characters', 'create_character', 'claim_character', 'save_character_state', 'release_character', 'get_inventory_schema_migration_history', 'emit_item_use', 'reserve_item_use', 'get_item_use', 'complete_item_use', 'cancel_item_use', 'get_item_use_schema_migration_history', 'emit_quest_state', 'get_character_quests', 'save_character_quests', 'get_quest_schema_migration_history', 'apply_quest_inventory', 'get_experience_schema_migration_history')
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

    -- Set 10: original extended inspector; legacy V0 inspector remains unchanged.
    SHOW CREATE PROCEDURE get_character_schema_migration_history;

    SHOW CREATE PROCEDURE emit_character_state;
    SHOW CREATE PROCEDURE list_characters;
    SHOW CREATE PROCEDURE create_character;
    SHOW CREATE PROCEDURE claim_character;
    SHOW CREATE PROCEDURE save_character_state;
    SHOW CREATE PROCEDURE release_character;
    SHOW CREATE PROCEDURE get_inventory_schema_migration_history;

    SHOW CREATE PROCEDURE emit_item_use;
    SHOW CREATE PROCEDURE reserve_item_use;
    SHOW CREATE PROCEDURE get_item_use;
    SHOW CREATE PROCEDURE complete_item_use;
    SHOW CREATE PROCEDURE cancel_item_use;
    SHOW CREATE PROCEDURE get_item_use_schema_migration_history;

    SHOW CREATE PROCEDURE emit_quest_state;
    SHOW CREATE PROCEDURE get_character_quests;
    SHOW CREATE PROCEDURE save_character_quests;
    SHOW CREATE PROCEDURE get_quest_schema_migration_history;
    SHOW CREATE PROCEDURE apply_quest_inventory;
    SHOW CREATE PROCEDURE get_experience_schema_migration_history;

    DO RELEASE_LOCK(lockName);
    SET ownsLock = FALSE;
END$$
DELIMITER ;
