-- Target: MySQL 8.0.47. Logical version: 000003; predecessor: 000002.
-- AuthServer rechecks its session's account before ticket issue/redemption.
-- This procedure does not create sessions/tickets, authenticate a caller,
-- update login timestamps, or replace migration history verification.
-- The caller owns the transaction; the persistent migration principal is
-- the definer. DELIMITER must be handled by the selected application tool.

DELIMITER $$

CREATE PROCEDURE get_auth_account_status
(
    IN inAccountId BIGINT UNSIGNED
)
READS SQL DATA
SQL SECURITY DEFINER
BEGIN
    DECLARE accountStatus INT DEFAULT NULL;

    IF @@session.autocommit <> 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Explicit transaction required.';
    END IF;
    IF inAccountId IS NULL OR inAccountId = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid account identifier.';
    END IF;

    -- A locking read observes the current committed state, rather than a
    -- prior REPEATABLE READ snapshot. Its shared lock ends at caller commit.
    BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET accountStatus = NULL;
        SELECT status INTO accountStatus
        FROM accounts WHERE account_id = inAccountId FOR SHARE;
    END;

    IF accountStatus IS NOT NULL AND accountStatus NOT IN (0, 1) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid account state.';
    END IF;

    -- Exactly one data result set and one row: 0 active, 1 blocked, 2 absent.
    -- Only account_status is NULL, and only for an absent account.
    SELECT CAST(COALESCE(accountStatus, 2) AS SIGNED) AS result_code,
        CAST(inAccountId AS UNSIGNED) AS account_id,
        CAST(accountStatus AS SIGNED) AS account_status;
END$$

DELIMITER ;
