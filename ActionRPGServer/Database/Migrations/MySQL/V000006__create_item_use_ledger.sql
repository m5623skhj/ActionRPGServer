-- V000006: durable item-use reservation and conservative cooldowns.
-- V0..V5 deployment files remain immutable. No Room network work runs in a DB transaction.
CREATE TABLE character_item_uses
(
    account_id BIGINT UNSIGNED NOT NULL,
    request_id BINARY(32) NOT NULL,
    character_id BIGINT UNSIGNED NOT NULL,
    origin_owner_generation BIGINT UNSIGNED NOT NULL,
    original_expected_revision BIGINT UNSIGNED NOT NULL,
    reserve_revision BIGINT UNSIGNED NOT NULL,
    instance_id BINARY(16) NOT NULL,
    definition_id VARBINARY(64) NOT NULL,
    container INT UNSIGNED NOT NULL,
    slot INT UNSIGNED NOT NULL,
    room_id BIGINT UNSIGNED NOT NULL,
    room_incarnation BINARY(16) NOT NULL,
    operation_json JSON NOT NULL,
    execution_json JSON NOT NULL,
    execution_hash BINARY(32) NOT NULL,
    cooldown_ms INT UNSIGNED NOT NULL,
    state INT UNSIGNED NOT NULL,
    active_character_id BIGINT UNSIGNED NULL,
    cooldown_until DATETIME(6) NULL,
    reason VARBINARY(64) NOT NULL,
    created_at DATETIME(6) NOT NULL,
    finalized_at DATETIME(6) NULL,
    PRIMARY KEY (account_id, request_id),
    UNIQUE KEY uk_character_item_uses_pending (active_character_id),
    KEY ix_character_item_uses_cooldown (character_id, definition_id, cooldown_until),
    CONSTRAINT fk_character_item_uses_operation FOREIGN KEY (account_id, request_id)
        REFERENCES character_operations (account_id, request_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    CONSTRAINT fk_character_item_uses_character FOREIGN KEY (character_id)
        REFERENCES characters (character_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    CONSTRAINT ck_character_item_uses_state CHECK (state IN (1, 2, 3, 4)),
    CONSTRAINT ck_character_item_uses_active CHECK
        ((state = 1 AND active_character_id IS NOT NULL AND active_character_id = character_id)
        OR (state IN (2, 3, 4) AND active_character_id IS NULL)),
    CONSTRAINT ck_character_item_uses_deadline CHECK
        ((state IN (1, 3) AND cooldown_until IS NULL)
        OR (state IN (2, 4) AND cooldown_until IS NOT NULL)),
    CONSTRAINT ck_character_item_uses_finalized CHECK
        ((state = 1 AND finalized_at IS NULL)
        OR (state IN (2, 3, 4) AND finalized_at IS NOT NULL)),
    CONSTRAINT ck_character_item_uses_cooldown CHECK (cooldown_ms > 0 AND cooldown_ms < 3600001),
    CONSTRAINT ck_character_item_uses_position CHECK (container = 2 AND slot < 40),
    CONSTRAINT ck_character_item_uses_definition CHECK (OCTET_LENGTH(definition_id) > 0),
    CONSTRAINT ck_character_item_uses_room CHECK (room_id > 0)
) ENGINE=InnoDB;

DELIMITER $$
CREATE PROCEDURE emit_item_use
(
    IN inResultCode INT,
    IN inCharacterId BIGINT UNSIGNED,
    IN inAccountId BIGINT UNSIGNED,
    IN inRequestId VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin
)
READS SQL DATA
SQL SECURITY DEFINER
BEGIN
    DECLARE observedAt DATETIME(6);
    DECLARE selectedRequest BINARY(32) DEFAULT NULL;
    DECLARE pendingRequest BINARY(32) DEFAULT NULL;
    DECLARE observedMs VARCHAR(20) CHARACTER SET ascii;
    -- One UTC sample per response; DATETIME arithmetic does not use session time_zone.
    SET observedAt = UTC_TIMESTAMP(3);
    SET observedMs = CAST(TIMESTAMPDIFF(MICROSECOND, CAST('1970-01-01 00:00:00' AS DATETIME), observedAt) DIV 1000 AS CHAR);
    IF inCharacterId <> 0 THEN
        SELECT MAX(request_id) INTO pendingRequest FROM character_item_uses
        WHERE account_id = inAccountId AND active_character_id = inCharacterId;
        IF inRequestId IS NULL THEN
            SET selectedRequest = pendingRequest;
        ELSE
            SET selectedRequest = UNHEX(inRequestId);
        END IF;
    END IF;
    -- Internal only. Callers have checked current owner and hold the character state lock.
    CALL emit_character_state(inResultCode, inCharacterId, TRUE);
    IF inCharacterId <> 0 AND EXISTS (SELECT 1 FROM character_item_uses
        WHERE account_id = inAccountId AND request_id = selectedRequest AND character_id = inCharacterId) THEN
        SELECT LOWER(HEX(request_id)) AS request_id, CAST(state AS SIGNED) AS use_state,
            origin_owner_generation, original_expected_revision, reserve_revision,
            LOWER(HEX(instance_id)) AS instance_id, CONVERT(definition_id USING utf8mb4) AS definition_id,
            container, slot, room_id, LOWER(HEX(room_incarnation)) AS room_incarnation,
            CAST(operation_json AS CHAR CHARACTER SET utf8mb4) AS operation_json,
            CAST(execution_json AS CHAR CHARACTER SET utf8mb4) AS execution_json,
            LOWER(HEX(execution_hash)) AS execution_hash, cooldown_ms,
            COALESCE(CAST(TIMESTAMPDIFF(MICROSECOND, CAST('1970-01-01 00:00:00' AS DATETIME), cooldown_until) DIV 1000 AS CHAR), '') AS cooldown_until_ms,
            CONVERT(reason USING utf8mb4) AS reason, observedMs AS server_now_ms,
            COALESCE(LOWER(HEX(pendingRequest)), '') AS pending_request_id
        FROM character_item_uses WHERE account_id = inAccountId AND request_id = selectedRequest
            AND character_id = inCharacterId;
    ELSE
        SELECT CAST('' AS CHAR CHARACTER SET utf8mb4) AS request_id, CAST(0 AS SIGNED) AS use_state,
            CAST(0 AS UNSIGNED) AS origin_owner_generation, CAST(0 AS UNSIGNED) AS original_expected_revision,
            CAST(0 AS UNSIGNED) AS reserve_revision, CAST('' AS CHAR CHARACTER SET utf8mb4) AS instance_id,
            CAST('' AS CHAR CHARACTER SET utf8mb4) AS definition_id, CAST(0 AS UNSIGNED) AS container,
            CAST(0 AS UNSIGNED) AS slot, CAST(0 AS UNSIGNED) AS room_id,
            CAST('' AS CHAR CHARACTER SET utf8mb4) AS room_incarnation,
            CAST('' AS CHAR CHARACTER SET utf8mb4) AS operation_json,
            CAST('' AS CHAR CHARACTER SET utf8mb4) AS execution_json,
            CAST('' AS CHAR CHARACTER SET utf8mb4) AS execution_hash, CAST(0 AS UNSIGNED) AS cooldown_ms,
            CAST('' AS CHAR CHARACTER SET utf8mb4) AS cooldown_until_ms,
            CAST('' AS CHAR CHARACTER SET utf8mb4) AS reason, observedMs AS server_now_ms,
            COALESCE(LOWER(HEX(pendingRequest)), '') AS pending_request_id;
    END IF;
    -- Full current Pending/Active snapshot, not merely the selected operation's historical deadline.
    IF inCharacterId <> 0 AND EXISTS (SELECT 1 FROM character_item_uses WHERE character_id = inCharacterId
        AND (state = 1 OR (state IN (2, 4) AND cooldown_until > observedAt))) THEN
        SELECT CONVERT(definition_id USING utf8mb4) AS definition_id,
            CASE WHEN MAX(state = 1) = 1 THEN 'Pending' ELSE 'Active' END AS cooldown_state,
            CASE WHEN MAX(state = 1) = 1 THEN ''
                ELSE CAST(TIMESTAMPDIFF(MICROSECOND, CAST('1970-01-01 00:00:00' AS DATETIME), MAX(cooldown_until)) DIV 1000 AS CHAR) END AS ready_utc_ms,
            CASE WHEN MAX(state = 1) = 1 THEN CAST(NULL AS UNSIGNED)
                ELSE CAST(TIMESTAMPDIFF(MICROSECOND, observedAt, MAX(cooldown_until)) DIV 1000 AS UNSIGNED) END AS remaining_ms,
            observedMs AS server_now_ms
        FROM character_item_uses WHERE character_id = inCharacterId
            AND (state = 1 OR (state IN (2, 4) AND cooldown_until > observedAt))
        GROUP BY definition_id ORDER BY definition_id;
    ELSE
        SELECT CAST('' AS CHAR CHARACTER SET utf8mb4) AS definition_id,
            CAST('None' AS CHAR CHARACTER SET utf8mb4) AS cooldown_state,
            CAST('' AS CHAR CHARACTER SET utf8mb4) AS ready_utc_ms,
            CAST(0 AS UNSIGNED) AS remaining_ms, observedMs AS server_now_ms;
    END IF;
END$$

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
        CALL emit_item_use(1, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET selectedCharacterId = 0;
        SELECT character_id INTO selectedCharacterId FROM characters
        WHERE character_id = inCharacterId AND account_id = inAccountId FOR UPDATE;
    END;
    IF selectedCharacterId = 0 THEN
        CALL emit_item_use(2, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    -- V4 characters may predate this auxiliary row. No user state is seeded.
    INSERT INTO character_state (character_id, revision, owner_generation, owner_token)
    VALUES (inCharacterId, 0, 0, NULL) ON DUPLICATE KEY UPDATE character_id = inCharacterId;
    SELECT owner_token, owner_generation INTO currentToken, currentGeneration
    FROM character_state WHERE character_id = inCharacterId FOR UPDATE;
    -- Retry the same claim without allocating another generation.
    IF currentToken = UNHEX(inOwnerToken) THEN
        CALL emit_item_use(0, inCharacterId, inAccountId, NULL);
        LEAVE operation;
    END IF;
    IF currentGeneration <> inExpectedOwnerGeneration THEN
        CALL emit_item_use(5, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    UPDATE character_state SET owner_generation = owner_generation + 1, owner_token = UNHEX(inOwnerToken)
    WHERE character_id = inCharacterId;
    CALL emit_item_use(0, inCharacterId, inAccountId, NULL);
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
    IF EXISTS (SELECT 1 FROM character_item_uses WHERE active_character_id = inCharacterId) THEN
        CALL emit_character_state(7, inCharacterId, TRUE);
        LEAVE operation;
    END IF;
    -- Idempotent only within this same generation. An old release cannot clear a newer claim.
    UPDATE character_state SET owner_token = NULL WHERE character_id = inCharacterId;
    CALL emit_character_state(0, inCharacterId, TRUE);
END$$

CREATE PROCEDURE reserve_item_use
(
    IN inAccountId BIGINT UNSIGNED,
    IN inCharacterId BIGINT UNSIGNED,
    IN inOwnerToken VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inOwnerGeneration BIGINT UNSIGNED,
    IN inRequestId VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inExpectedRevision BIGINT UNSIGNED,
    IN inInstanceId VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin,
    IN inRoomId BIGINT UNSIGNED,
    IN inRoomIncarnation VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin,
    IN inOperationJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
    IN inExecutionJson TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
    IN inCooldownMs INT UNSIGNED
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
    DECLARE itemQuantity INT UNSIGNED DEFAULT NULL;
    DECLARE itemDefinition VARBINARY(64);
    DECLARE itemSlot INT UNSIGNED;
    DECLARE observedAt DATETIME(6);
    IF inRequestId IS NULL OR OCTET_LENGTH(inRequestId) <> 64 OR inRequestId NOT REGEXP '^[0-9a-f]{64}$' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid use request ID.';
    END IF;
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
        CALL emit_item_use(1, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET selectedCharacterId = 0;
        SELECT character_id INTO selectedCharacterId FROM characters
        WHERE character_id = inCharacterId AND account_id = inAccountId FOR UPDATE;
    END;
    IF selectedCharacterId = 0 THEN
        CALL emit_item_use(2, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET currentGeneration = NULL;
        SELECT owner_token, owner_generation, revision INTO currentToken, currentGeneration, currentRevision
        FROM character_state WHERE character_id = inCharacterId FOR UPDATE;
    END;
    IF currentGeneration IS NULL OR inOwnerGeneration IS NULL OR currentGeneration <> inOwnerGeneration
        OR currentToken IS NULL OR currentToken <> UNHEX(inOwnerToken) THEN
        CALL emit_item_use(5, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    IF inExpectedRevision IS NULL OR inInstanceId IS NULL OR OCTET_LENGTH(inInstanceId) <> 32
        OR inInstanceId NOT REGEXP '^[0-9a-f]{32}$' OR inRoomId IS NULL OR inRoomId = 0
        OR inRoomIncarnation IS NULL OR OCTET_LENGTH(inRoomIncarnation) <> 32
        OR inRoomIncarnation NOT REGEXP '^[0-9a-f]{32}$' OR inOperationJson IS NULL
        OR OCTET_LENGTH(inOperationJson) > 2048 OR NOT JSON_VALID(inOperationJson) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid original use operation.';
    END IF;
    IF COALESCE(JSON_TYPE(CAST(inOperationJson AS JSON)), '') <> 'OBJECT' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid original use operation object.';
    END IF;
    IF OCTET_LENGTH(CAST(CAST(inOperationJson AS JSON) AS CHAR CHARACTER SET utf8mb4)) > 2048 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Canonical use operation exceeds the result contract.';
    END IF;
    -- The original command owns identity; newly calculated effects/cooldowns never replace a receipt.
    SET payloadHash = UNHEX(SHA2(CONCAT('ITEM_USE:', inAccountId, ':', inCharacterId, ':',
        inExpectedRevision, ':', inInstanceId, ':', inRoomId, ':', inRoomIncarnation, ':',
        CAST(CAST(inOperationJson AS JSON) AS CHAR CHARACTER SET utf8mb4)), 256));
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET existingCharacterId = 0;
        SELECT character_id, request_kind, payload_hash INTO existingCharacterId, existingKind, existingHash
        FROM character_operations WHERE account_id = inAccountId AND request_id = UNHEX(inRequestId) FOR UPDATE;
    END;
    IF existingCharacterId <> 0 THEN
        IF existingCharacterId = inCharacterId AND existingKind = _binary'ITEM_USE' AND existingHash = payloadHash
            AND EXISTS (SELECT 1 FROM character_item_uses WHERE account_id = inAccountId
                AND request_id = UNHEX(inRequestId) AND character_id = inCharacterId) THEN
            CALL emit_item_use(0, inCharacterId, inAccountId, inRequestId);
        ELSE
            CALL emit_item_use(6, inCharacterId, inAccountId, inRequestId);
        END IF;
        LEAVE operation;
    END IF;
    IF currentRevision <> inExpectedRevision THEN
        CALL emit_item_use(3, inCharacterId, inAccountId, NULL);
        LEAVE operation;
    END IF;
    IF EXISTS (SELECT 1 FROM character_item_uses WHERE active_character_id = inCharacterId) THEN
        CALL emit_item_use(7, inCharacterId, inAccountId, NULL);
        LEAVE operation;
    END IF;
    IF inExecutionJson IS NULL OR OCTET_LENGTH(inExecutionJson) > 2048 OR NOT JSON_VALID(inExecutionJson)
        OR inCooldownMs IS NULL OR inCooldownMs = 0 OR inCooldownMs > 3600000 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid server execution snapshot.';
    END IF;
    IF COALESCE(JSON_TYPE(CAST(inExecutionJson AS JSON)), '') <> 'OBJECT' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid server execution snapshot object.';
    END IF;
    IF OCTET_LENGTH(CAST(CAST(inExecutionJson AS JSON) AS CHAR CHARACTER SET utf8mb4)) > 2048 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Canonical execution snapshot exceeds the result contract.';
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET itemQuantity = NULL;
        SELECT quantity, definition_id, slot INTO itemQuantity, itemDefinition, itemSlot
        FROM character_items WHERE instance_id = UNHEX(inInstanceId) AND character_id = inCharacterId
            AND container = 2 FOR UPDATE;
    END;
    IF itemQuantity IS NULL THEN
        CALL emit_item_use(8, inCharacterId, inAccountId, NULL);
        LEAVE operation;
    END IF;
    SET observedAt = UTC_TIMESTAMP(3);
    IF EXISTS (SELECT 1 FROM character_item_uses WHERE character_id = inCharacterId
        AND definition_id = itemDefinition AND state IN (2, 4) AND cooldown_until > observedAt) THEN
        CALL emit_item_use(9, inCharacterId, inAccountId, NULL);
        LEAVE operation;
    END IF;
    IF itemQuantity = 1 THEN
        DELETE FROM character_items WHERE instance_id = UNHEX(inInstanceId);
    ELSE
        UPDATE character_items SET quantity = quantity - 1 WHERE instance_id = UNHEX(inInstanceId);
    END IF;
    UPDATE character_state SET revision = revision + 1 WHERE character_id = inCharacterId;
    INSERT INTO character_operations (account_id, request_id, character_id, request_kind, payload_hash, revision, created_at)
    VALUES (inAccountId, UNHEX(inRequestId), inCharacterId, _binary'ITEM_USE', payloadHash, currentRevision + 1, observedAt);
    INSERT INTO character_item_uses (account_id, request_id, character_id, origin_owner_generation,
        original_expected_revision, reserve_revision, instance_id, definition_id, container, slot,
        room_id, room_incarnation, operation_json, execution_json, execution_hash, cooldown_ms,
        state, active_character_id, cooldown_until, reason, created_at, finalized_at)
    VALUES (inAccountId, UNHEX(inRequestId), inCharacterId, inOwnerGeneration,
        inExpectedRevision, currentRevision + 1, UNHEX(inInstanceId), itemDefinition, 2, itemSlot,
        inRoomId, UNHEX(inRoomIncarnation), CAST(inOperationJson AS JSON), CAST(inExecutionJson AS JSON),
        UNHEX(SHA2(CAST(CAST(inExecutionJson AS JSON) AS CHAR CHARACTER SET utf8mb4), 256)), inCooldownMs,
        1, inCharacterId, NULL, _binary'', observedAt, NULL);
    CALL emit_item_use(0, inCharacterId, inAccountId, inRequestId);
END$$

CREATE PROCEDURE get_item_use
(
    IN inAccountId BIGINT UNSIGNED,
    IN inCharacterId BIGINT UNSIGNED,
    IN inOwnerToken VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inOwnerGeneration BIGINT UNSIGNED,
    IN inRequestId VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin
)
MODIFIES SQL DATA
SQL SECURITY DEFINER
operation: BEGIN
    DECLARE accountStatus INT DEFAULT NULL;
    DECLARE selectedCharacterId BIGINT UNSIGNED DEFAULT 0;
    DECLARE currentToken BINARY(32) DEFAULT NULL;
    DECLARE currentGeneration BIGINT UNSIGNED DEFAULT NULL;
    DECLARE currentRevision BIGINT UNSIGNED DEFAULT 0;
    IF inRequestId IS NOT NULL AND (OCTET_LENGTH(inRequestId) <> 64 OR inRequestId NOT REGEXP '^[0-9a-f]{64}$') THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid use request ID.';
    END IF;
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
        CALL emit_item_use(1, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET selectedCharacterId = 0;
        SELECT character_id INTO selectedCharacterId FROM characters
        WHERE character_id = inCharacterId AND account_id = inAccountId FOR UPDATE;
    END;
    IF selectedCharacterId = 0 THEN
        CALL emit_item_use(2, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET currentGeneration = NULL;
        SELECT owner_token, owner_generation, revision INTO currentToken, currentGeneration, currentRevision
        FROM character_state WHERE character_id = inCharacterId FOR UPDATE;
    END;
    IF currentGeneration IS NULL OR inOwnerGeneration IS NULL OR currentGeneration <> inOwnerGeneration
        OR currentToken IS NULL OR currentToken <> UNHEX(inOwnerToken) THEN
        CALL emit_item_use(5, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    IF inRequestId IS NOT NULL AND NOT EXISTS (SELECT 1 FROM character_item_uses
        WHERE account_id = inAccountId AND request_id = UNHEX(inRequestId) AND character_id = inCharacterId) THEN
        CALL emit_item_use(10, inCharacterId, inAccountId, inRequestId);
    ELSE
        CALL emit_item_use(0, inCharacterId, inAccountId, inRequestId);
    END IF;
END$$

CREATE PROCEDURE complete_item_use
(
    IN inAccountId BIGINT UNSIGNED,
    IN inCharacterId BIGINT UNSIGNED,
    IN inOwnerToken VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inOwnerGeneration BIGINT UNSIGNED,
    IN inRequestId VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inRoomId BIGINT UNSIGNED,
    IN inRoomIncarnation VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin,
    IN inExecutionHash VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin
)
MODIFIES SQL DATA
SQL SECURITY DEFINER
operation: BEGIN
    DECLARE accountStatus INT DEFAULT NULL;
    DECLARE selectedCharacterId BIGINT UNSIGNED DEFAULT 0;
    DECLARE currentToken BINARY(32) DEFAULT NULL;
    DECLARE currentGeneration BIGINT UNSIGNED DEFAULT NULL;
    DECLARE currentRevision BIGINT UNSIGNED DEFAULT 0;
    DECLARE useState INT UNSIGNED DEFAULT NULL;
    DECLARE useRoomId BIGINT UNSIGNED;
    DECLARE useIncarnation BINARY(16);
    DECLARE useHash BINARY(32);
    DECLARE useCooldown INT UNSIGNED;
    DECLARE finalizedAt DATETIME(6);
    IF inRequestId IS NULL OR OCTET_LENGTH(inRequestId) <> 64 OR inRequestId NOT REGEXP '^[0-9a-f]{64}$' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid use request ID.';
    END IF;
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
        CALL emit_item_use(1, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET selectedCharacterId = 0;
        SELECT character_id INTO selectedCharacterId FROM characters
        WHERE character_id = inCharacterId AND account_id = inAccountId FOR UPDATE;
    END;
    IF selectedCharacterId = 0 THEN
        CALL emit_item_use(2, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET currentGeneration = NULL;
        SELECT owner_token, owner_generation, revision INTO currentToken, currentGeneration, currentRevision
        FROM character_state WHERE character_id = inCharacterId FOR UPDATE;
    END;
    IF currentGeneration IS NULL OR inOwnerGeneration IS NULL OR currentGeneration <> inOwnerGeneration
        OR currentToken IS NULL OR currentToken <> UNHEX(inOwnerToken) THEN
        CALL emit_item_use(5, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    IF inRoomId IS NULL OR inRoomId = 0 OR inRoomIncarnation IS NULL
        OR OCTET_LENGTH(inRoomIncarnation) <> 32 OR inRoomIncarnation NOT REGEXP '^[0-9a-f]{32}$'
        OR inExecutionHash IS NULL OR OCTET_LENGTH(inExecutionHash) <> 64
        OR inExecutionHash NOT REGEXP '^[0-9a-f]{64}$' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid original Room execution identity.';
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET useState = NULL;
        SELECT state, room_id, room_incarnation, execution_hash, cooldown_ms
        INTO useState, useRoomId, useIncarnation, useHash, useCooldown
        FROM character_item_uses WHERE account_id = inAccountId AND request_id = UNHEX(inRequestId)
            AND character_id = inCharacterId FOR UPDATE;
    END;
    IF useState IS NULL THEN
        CALL emit_item_use(10, inCharacterId, inAccountId, inRequestId);
        LEAVE operation;
    END IF;
    IF useRoomId <> inRoomId OR useIncarnation <> UNHEX(inRoomIncarnation) OR useHash <> UNHEX(inExecutionHash) THEN
        CALL emit_item_use(11, inCharacterId, inAccountId, inRequestId);
        LEAVE operation;
    END IF;
    IF useState = 2 THEN
        CALL emit_item_use(0, inCharacterId, inAccountId, inRequestId);
        LEAVE operation;
    END IF;
    IF useState <> 1 THEN
        CALL emit_item_use(11, inCharacterId, inAccountId, inRequestId);
        LEAVE operation;
    END IF;
    -- Trusted Town calls this only after the pinned Room reports Applied.
    -- The SQL UTC sample is not a future physical COMMIT timestamp.
    SET finalizedAt = UTC_TIMESTAMP(3);
    UPDATE character_item_uses SET state = 2, active_character_id = NULL,
        cooldown_until = TIMESTAMPADD(MICROSECOND, useCooldown * 1000, finalizedAt),
        reason = _binary'Applied', finalized_at = finalizedAt
    WHERE account_id = inAccountId AND request_id = UNHEX(inRequestId);
    CALL emit_item_use(0, inCharacterId, inAccountId, inRequestId);
END$$

CREATE PROCEDURE cancel_item_use
(
    IN inAccountId BIGINT UNSIGNED,
    IN inCharacterId BIGINT UNSIGNED,
    IN inOwnerToken VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inOwnerGeneration BIGINT UNSIGNED,
    IN inRequestId VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inRoomId BIGINT UNSIGNED,
    IN inRoomIncarnation VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin,
    IN inExecutionHash VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin,
    IN inDisposition INT UNSIGNED,
    IN inReason VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin
)
MODIFIES SQL DATA
SQL SECURITY DEFINER
operation: BEGIN
    DECLARE accountStatus INT DEFAULT NULL;
    DECLARE selectedCharacterId BIGINT UNSIGNED DEFAULT 0;
    DECLARE currentToken BINARY(32) DEFAULT NULL;
    DECLARE currentGeneration BIGINT UNSIGNED DEFAULT NULL;
    DECLARE currentRevision BIGINT UNSIGNED DEFAULT 0;
    DECLARE useState INT UNSIGNED DEFAULT NULL;
    DECLARE useRoomId BIGINT UNSIGNED;
    DECLARE useIncarnation BINARY(16);
    DECLARE useHash BINARY(32);
    DECLARE useCooldown INT UNSIGNED;
    DECLARE finalizedAt DATETIME(6);
    DECLARE targetState INT UNSIGNED;
    DECLARE useInstance BINARY(16);
    DECLARE useDefinition VARBINARY(64);
    DECLARE useSlot INT UNSIGNED;
    DECLARE returnedQuantity INT UNSIGNED DEFAULT NULL;
    DECLARE returnedDefinition VARBINARY(64);
    DECLARE returnedContainer INT UNSIGNED;
    DECLARE returnedSlot INT UNSIGNED;
    IF inRequestId IS NULL OR OCTET_LENGTH(inRequestId) <> 64 OR inRequestId NOT REGEXP '^[0-9a-f]{64}$' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid use request ID.';
    END IF;
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
        CALL emit_item_use(1, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET selectedCharacterId = 0;
        SELECT character_id INTO selectedCharacterId FROM characters
        WHERE character_id = inCharacterId AND account_id = inAccountId FOR UPDATE;
    END;
    IF selectedCharacterId = 0 THEN
        CALL emit_item_use(2, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET currentGeneration = NULL;
        SELECT owner_token, owner_generation, revision INTO currentToken, currentGeneration, currentRevision
        FROM character_state WHERE character_id = inCharacterId FOR UPDATE;
    END;
    IF currentGeneration IS NULL OR inOwnerGeneration IS NULL OR currentGeneration <> inOwnerGeneration
        OR currentToken IS NULL OR currentToken <> UNHEX(inOwnerToken) THEN
        CALL emit_item_use(5, 0, inAccountId, NULL);
        LEAVE operation;
    END IF;
    IF inRoomId IS NULL OR inRoomId = 0 OR inRoomIncarnation IS NULL
        OR OCTET_LENGTH(inRoomIncarnation) <> 32 OR inRoomIncarnation NOT REGEXP '^[0-9a-f]{32}$'
        OR inExecutionHash IS NULL OR OCTET_LENGTH(inExecutionHash) <> 64
        OR inExecutionHash NOT REGEXP '^[0-9a-f]{64}$' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid original Room execution identity.';
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET useState = NULL;
        SELECT state, room_id, room_incarnation, execution_hash, cooldown_ms
        INTO useState, useRoomId, useIncarnation, useHash, useCooldown
        FROM character_item_uses WHERE account_id = inAccountId AND request_id = UNHEX(inRequestId)
            AND character_id = inCharacterId FOR UPDATE;
    END;
    IF useState IS NULL THEN
        CALL emit_item_use(10, inCharacterId, inAccountId, inRequestId);
        LEAVE operation;
    END IF;
    IF useRoomId <> inRoomId OR useIncarnation <> UNHEX(inRoomIncarnation) OR useHash <> UNHEX(inExecutionHash) THEN
        CALL emit_item_use(11, inCharacterId, inAccountId, inRequestId);
        LEAVE operation;
    END IF;
    IF inDisposition IS NULL OR inDisposition NOT IN (0, 1) OR inReason IS NULL
        OR OCTET_LENGTH(inReason) = 0 OR OCTET_LENGTH(inReason) > 64
        OR inReason NOT REGEXP '^[A-Za-z0-9_]+$' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid trusted use disposition.';
    END IF;
    SET targetState = IF(inDisposition = 0, 3, 4);
    IF useState = targetState THEN
        CALL emit_item_use(0, inCharacterId, inAccountId, inRequestId);
        LEAVE operation;
    END IF;
    IF useState <> 1 THEN
        CALL emit_item_use(11, inCharacterId, inAccountId, inRequestId);
        LEAVE operation;
    END IF;
    -- Disposition 0 requires the original live Room's cancel tombstone, not Query NotFound.
    -- Disposition 1 requires a confirmed fenced incarnation loss, not a network timeout.
    IF inDisposition = 0 THEN
        SELECT instance_id, definition_id, slot INTO useInstance, useDefinition, useSlot
        FROM character_item_uses WHERE account_id = inAccountId AND request_id = UNHEX(inRequestId);
        BEGIN
            DECLARE CONTINUE HANDLER FOR NOT FOUND SET returnedQuantity = NULL;
            SELECT quantity, definition_id, container, slot
            INTO returnedQuantity, returnedDefinition, returnedContainer, returnedSlot
            FROM character_items WHERE instance_id = useInstance AND character_id = inCharacterId FOR UPDATE;
        END;
        IF returnedQuantity IS NULL THEN
            INSERT INTO character_items (instance_id, character_id, definition_id, quantity, container, slot)
            VALUES (useInstance, inCharacterId, useDefinition, 1, 2, useSlot);
        ELSE
            IF returnedDefinition <> useDefinition OR returnedContainer <> 2 OR returnedSlot <> useSlot
                OR returnedQuantity = 4294967295 THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Reserved stack cannot be safely returned.';
            END IF;
            UPDATE character_items SET quantity = quantity + 1 WHERE instance_id = useInstance;
        END IF;
        UPDATE character_state SET revision = revision + 1 WHERE character_id = inCharacterId;
    END IF;
    SET finalizedAt = UTC_TIMESTAMP(3);
    UPDATE character_item_uses SET state = targetState, active_character_id = NULL,
        cooldown_until = IF(inDisposition = 0, NULL, TIMESTAMPADD(MICROSECOND, useCooldown * 1000, finalizedAt)),
        reason = CONVERT(inReason USING binary), finalized_at = finalizedAt
    WHERE account_id = inAccountId AND request_id = UNHEX(inRequestId);
    CALL emit_item_use(0, inCharacterId, inAccountId, inRequestId);
END$$

CREATE PROCEDURE get_item_use_schema_migration_history()
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
    SELECT CAST('4' AS CHAR CHARACTER SET utf8mb4) AS history_format,
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
    WHERE c.TABLE_SCHEMA = DATABASE() AND c.TABLE_NAME IN ('schema_migrations', 'accounts', 'account_identities', 'characters', 'character_skills', 'character_state', 'character_items', 'character_operations', 'character_item_uses')
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
    WHERE t.TABLE_SCHEMA = DATABASE() AND t.TABLE_NAME IN ('schema_migrations', 'accounts', 'account_identities', 'characters', 'character_skills', 'character_state', 'character_items', 'character_operations', 'character_item_uses')
    ORDER BY t.TABLE_NAME, t.CONSTRAINT_NAME, k.ORDINAL_POSITION;

    -- Set 4: actual index shape. Prefix length may be NULL.
    SELECT CAST(TABLE_NAME AS CHAR CHARACTER SET utf8mb4) AS table_name,
        CAST(INDEX_NAME AS CHAR CHARACTER SET utf8mb4) AS index_name,
        CAST(NON_UNIQUE AS CHAR CHARACTER SET utf8mb4) AS non_unique,
        CAST(SEQ_IN_INDEX AS CHAR CHARACTER SET utf8mb4) AS seq_in_index,
        CAST(COLUMN_NAME AS CHAR CHARACTER SET utf8mb4) AS column_name,
        CAST(SUB_PART AS CHAR CHARACTER SET utf8mb4) AS sub_part
    FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN ('schema_migrations', 'accounts', 'account_identities', 'characters', 'character_skills', 'character_state', 'character_items', 'character_operations', 'character_item_uses')
    ORDER BY TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX;

    -- Set 5: routine metadata. definition_utf8 removes charset introducers;
    -- use original SHOW CREATE definitions below for body comparisons.
    SELECT CAST(ROUTINE_NAME AS CHAR CHARACTER SET utf8mb4) AS routine_name,
        CAST(SECURITY_TYPE AS CHAR CHARACTER SET utf8mb4) AS security_type,
        CAST(SQL_DATA_ACCESS AS CHAR CHARACTER SET utf8mb4) AS sql_data_access,
        CAST(ROUTINE_DEFINITION AS CHAR CHARACTER SET utf8mb4) AS routine_definition
    FROM information_schema.ROUTINES
    WHERE ROUTINE_SCHEMA = DATABASE() AND ROUTINE_TYPE = 'PROCEDURE'
        AND ROUTINE_NAME IN ('get_schema_migration_history', 'login_google_account', 'get_auth_account_status', 'get_character_schema_migration_history', 'emit_character_state', 'list_characters', 'create_character', 'claim_character', 'save_character_state', 'release_character', 'get_inventory_schema_migration_history', 'emit_item_use', 'reserve_item_use', 'get_item_use', 'complete_item_use', 'cancel_item_use', 'get_item_use_schema_migration_history')
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
        AND SPECIFIC_NAME IN ('get_schema_migration_history', 'login_google_account', 'get_auth_account_status', 'get_character_schema_migration_history', 'emit_character_state', 'list_characters', 'create_character', 'claim_character', 'save_character_state', 'release_character', 'get_inventory_schema_migration_history', 'emit_item_use', 'reserve_item_use', 'get_item_use', 'complete_item_use', 'cancel_item_use', 'get_item_use_schema_migration_history')
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

    DO RELEASE_LOCK(lockName);
    SET ownsLock = FALSE;
END$$
DELIMITER ;

