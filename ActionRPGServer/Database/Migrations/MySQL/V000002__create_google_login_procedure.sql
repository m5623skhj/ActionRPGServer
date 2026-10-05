-- Target: MySQL 8.0.47. Logical version: 000002; predecessor: 000001.
-- DELIMITER is a client directive; the selected migration tool must support it
-- or submit the complete CREATE PROCEDURE statement without the directives.
-- The persistent, least-privileged migration principal becomes the definer.
-- No token verification occurs in SQL. Only the trusted server may call this
-- procedure after Google authentication. The caller owns the transaction.

DELIMITER $$

CREATE PROCEDURE login_google_account
(
    IN inSubject TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin
)
MODIFIES SQL DATA
SQL SECURITY DEFINER
BEGIN
    DECLARE accountId BIGINT UNSIGNED DEFAULT NULL;
    DECLARE accountStatus INT DEFAULT NULL;
    DECLARE wasCreated INT DEFAULT 0;
    DECLARE identityConflict BOOLEAN DEFAULT FALSE;

    -- Require the ODBC runner's explicit transaction, including savepoints.
    IF @@session.autocommit <> 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Explicit transaction required.';
    END IF;
    IF inSubject IS NULL OR OCTET_LENGTH(inSubject) NOT BETWEEN 1 AND 255
        OR OCTET_LENGTH(inSubject) <> CHAR_LENGTH(inSubject)
        OR LOCATE(0x00, CONVERT(inSubject USING binary)) > 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid Google subject.';
    END IF;

    -- A snapshot lookup avoids locking an absent identity's index gap.
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET accountId = NULL;
        SELECT account_id INTO accountId
        FROM account_identities
        WHERE provider = _binary'google' AND subject = CONVERT(inSubject USING binary);
    END;

    IF accountId IS NULL THEN
        SAVEPOINT google_login_create;
        INSERT INTO accounts (status, created_at, last_login_at)
        VALUES (0, UTC_TIMESTAMP(6), NULL);
        SET accountId = LAST_INSERT_ID();

        BEGIN
            -- Handle only the identity insert's unique-key race. Other SQL
            -- errors propagate; the ODBC runner rolls back the whole request.
            DECLARE CONTINUE HANDLER FOR 1062 SET identityConflict = TRUE;
            INSERT INTO account_identities (account_id, provider, subject, linked_at)
            VALUES (accountId, _binary'google', CONVERT(inSubject USING binary), UTC_TIMESTAMP(6));
        END;

        IF identityConflict THEN
            -- Remove the losing request's new account without ending the
            -- outer transaction. The winning identity is read below.
            ROLLBACK TO SAVEPOINT google_login_create;
        ELSE
            SET wasCreated = 1;
        END IF;
        RELEASE SAVEPOINT google_login_create;
    END IF;

    -- A locking read sees the committed winner even under REPEATABLE READ.
    -- Keep identity/account locks until the caller commits the result.
    SET accountId = NULL;
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET accountId = NULL;
        SELECT account_id INTO accountId
        FROM account_identities
        WHERE provider = _binary'google' AND subject = CONVERT(inSubject USING binary)
        FOR UPDATE;
    END;
    IF accountId IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Login identity is unavailable.';
    END IF;

    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET accountStatus = NULL;
        SELECT status INTO accountStatus
        FROM accounts WHERE account_id = accountId FOR UPDATE;
    END;
    IF accountStatus IS NULL OR accountStatus NOT IN (0, 1) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid login account state.';
    END IF;

    IF accountStatus = 0 THEN
        UPDATE accounts SET last_login_at = UTC_TIMESTAMP(6) WHERE account_id = accountId;
    END IF;

    -- Exactly one data result set and one row. All values are non-NULL.
    SELECT CAST(accountStatus AS SIGNED) AS result_code,
        CAST(accountId AS UNSIGNED) AS account_id,
        CAST(accountStatus AS SIGNED) AS account_status,
        CAST(wasCreated AS SIGNED) AS was_created;
END$$

DELIMITER ;
