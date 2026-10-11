-- V000006 Down is refused by the runner if ANY ledger row exists.
-- Restore V5 routines exactly; never erase terminal use history or receipts.
DROP TABLE character_item_uses;
DELIMITER $$
DROP PROCEDURE emit_item_use$$
DROP PROCEDURE reserve_item_use$$
DROP PROCEDURE get_item_use$$
DROP PROCEDURE complete_item_use$$
DROP PROCEDURE cancel_item_use$$
DROP PROCEDURE get_item_use_schema_migration_history$$
DROP PROCEDURE claim_character$$
CREATE PROCEDURE claim_character
(
    IN inAccountId BIGINT UNSIGNED,
    IN inCharacterId BIGINT UNSIGNED,
    IN inOwnerToken VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inExpectedOwnerGeneration BIGINT UNSIGNED
)
MODIFIES SQL DATA
SQL SECURITY DEFINER
operation: BEGIN
    DECLARE accountStatus INT DEFAULT NULL;
    DECLARE selectedCharacterId BIGINT UNSIGNED DEFAULT 0;
    DECLARE currentToken BINARY(32) DEFAULT NULL;
    DECLARE currentGeneration BIGINT UNSIGNED DEFAULT 0;
    IF inOwnerToken IS NULL OR OCTET_LENGTH(inOwnerToken) <> 64 OR inOwnerToken NOT REGEXP '^[0-9a-f]{64}$' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid owner token.';
    END IF;
    IF inExpectedOwnerGeneration IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid owner generation.';
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
    -- V4 characters may predate this auxiliary row. No user state is seeded.
    INSERT INTO character_state (character_id, revision, owner_generation, owner_token)
    VALUES (inCharacterId, 0, 0, NULL) ON DUPLICATE KEY UPDATE character_id = inCharacterId;
    SELECT owner_token, owner_generation INTO currentToken, currentGeneration
    FROM character_state WHERE character_id = inCharacterId FOR UPDATE;
    -- Retry the same claim without allocating another generation.
    IF currentToken = UNHEX(inOwnerToken) THEN
        CALL emit_character_state(0, inCharacterId, TRUE);
        LEAVE operation;
    END IF;
    IF currentGeneration <> inExpectedOwnerGeneration THEN
        CALL emit_character_state(5, 0, TRUE);
        LEAVE operation;
    END IF;
    UPDATE character_state SET owner_generation = owner_generation + 1, owner_token = UNHEX(inOwnerToken)
    WHERE character_id = inCharacterId;
    CALL emit_character_state(0, inCharacterId, TRUE);
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
    DECLARE nextLevel BIGINT UNSIGNED;
    DECLARE nextSp BIGINT UNSIGNED;
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
    IF currentRevision <> inExpectedRevision THEN
        CALL emit_character_state(3, inCharacterId, TRUE);
        LEAVE operation;
    END IF;
    -- Duplicate receipts are checked first: retransmission never re-applies its proposed state.
    IF inProgressionJson IS NULL OR inInventoryJson IS NULL
        OR OCTET_LENGTH(inProgressionJson) > 32768 OR OCTET_LENGTH(inInventoryJson) > 32768
        OR NOT JSON_VALID(inProgressionJson) OR NOT JSON_VALID(inInventoryJson) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid character snapshots.';
    END IF;
    IF COALESCE(JSON_TYPE(CAST(inProgressionJson AS JSON)), '') <> 'OBJECT'
        OR JSON_LENGTH(CAST(inProgressionJson AS JSON)) <> 3
        OR COALESCE(JSON_TYPE(JSON_EXTRACT(inProgressionJson, '$.level')), '') <> 'INTEGER'
        OR COALESCE(JSON_TYPE(JSON_EXTRACT(inProgressionJson, '$.skillPoints')), '') <> 'INTEGER'
        OR COALESCE(JSON_TYPE(JSON_EXTRACT(inProgressionJson, '$.skillLevels')), '') <> 'OBJECT'
        OR COALESCE(JSON_TYPE(CAST(inInventoryJson AS JSON)), '') <> 'OBJECT'
        OR JSON_LENGTH(CAST(inInventoryJson AS JSON)) <> 1
        OR COALESCE(JSON_TYPE(JSON_EXTRACT(inInventoryJson, '$.items')), '') <> 'ARRAY' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid snapshot shape.';
    END IF;
    -- Validate as DECIMAL before unsigned conversion; negative/wrapped values must not pass.
    IF CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, '$.level')) AS DECIMAL(20,0)) NOT BETWEEN 1 AND 1000000
        OR CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, '$.skillPoints')) AS DECIMAL(20,0)) NOT BETWEEN 0 AND 4294967295 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid progression range.';
    END IF;
    SET nextLevel = CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, '$.level')) AS UNSIGNED);
    SET nextSp = CAST(JSON_UNQUOTE(JSON_EXTRACT(inProgressionJson, '$.skillPoints')) AS UNSIGNED);
    SET skillKeys = JSON_KEYS(JSON_EXTRACT(inProgressionJson, '$.skillLevels'));
    SET skillsCount = JSON_LENGTH(skillKeys);
    SET itemsCount = JSON_LENGTH(JSON_EXTRACT(inInventoryJson, '$.items'));
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
    -- Replace only this locked character's state. The caller rolls back every change on any SQL/mapping failure.
    -- No transient UNIQUE-position collision occurs during a full-bag equipment swap.
    UPDATE characters SET level = nextLevel, skill_points = nextSp, updated_at = UTC_TIMESTAMP(6)
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
    UPDATE character_state SET revision = revision + 1 WHERE character_id = inCharacterId;
    INSERT INTO character_operations (account_id, request_id, character_id, request_kind, payload_hash, revision, created_at)
    VALUES (inAccountId, UNHEX(inRequestId), inCharacterId, _binary'SAVE', payloadHash, currentRevision + 1, UTC_TIMESTAMP(6));
    CALL emit_character_state(0, inCharacterId, TRUE);
END$$
DROP PROCEDURE release_character$$
CREATE PROCEDURE release_character
(
    IN inAccountId BIGINT UNSIGNED,
    IN inCharacterId BIGINT UNSIGNED,
    IN inOwnerToken VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inOwnerGeneration BIGINT UNSIGNED
)
MODIFIES SQL DATA
SQL SECURITY DEFINER
operation: BEGIN
    DECLARE accountStatus INT DEFAULT NULL;
    DECLARE selectedCharacterId BIGINT UNSIGNED DEFAULT 0;
    DECLARE currentToken BINARY(32) DEFAULT NULL;
    DECLARE currentGeneration BIGINT UNSIGNED DEFAULT NULL;
    IF inOwnerToken IS NULL OR OCTET_LENGTH(inOwnerToken) <> 64 OR inOwnerToken NOT REGEXP '^[0-9a-f]{64}$' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid owner token.';
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
        SELECT owner_token, owner_generation INTO currentToken, currentGeneration
        FROM character_state WHERE character_id = inCharacterId FOR UPDATE;
    END;
    IF currentGeneration IS NULL OR inOwnerGeneration IS NULL OR currentGeneration <> inOwnerGeneration
        OR (currentToken IS NOT NULL AND currentToken <> UNHEX(inOwnerToken)) THEN
        CALL emit_character_state(5, 0, TRUE);
        LEAVE operation;
    END IF;
    -- Idempotent only within this same generation. An old release cannot clear a newer claim.
    UPDATE character_state SET owner_token = NULL WHERE character_id = inCharacterId;
    CALL emit_character_state(0, inCharacterId, TRUE);
END$$
DELIMITER ;

