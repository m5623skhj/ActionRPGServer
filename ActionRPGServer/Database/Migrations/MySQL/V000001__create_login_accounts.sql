-- Target: MySQL 8.0.47 / InnoDB. Logical version: 000001; predecessor: none.
-- Requires an empty, explicitly selected game schema and strict SQL mode.
-- Two DDL statements commit independently. Stop and inspect on any failure;
-- do not replay a partially applied file or record success before both finish.

CREATE TABLE accounts
(
    account_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    status INT NOT NULL DEFAULT 0 COMMENT '0: active, 1: blocked',
    created_at DATETIME(6) NOT NULL COMMENT 'UTC',
    last_login_at DATETIME(6) NULL COMMENT 'UTC; last permitted DB login',
    CONSTRAINT pk_accounts PRIMARY KEY (account_id),
    CONSTRAINT ck_accounts_status CHECK (status IN (0, 1))
) ENGINE = InnoDB;

CREATE TABLE account_identities
(
    account_id BIGINT UNSIGNED NOT NULL,
    provider VARBINARY(32) NOT NULL,
    subject VARBINARY(255) NOT NULL,
    linked_at DATETIME(6) NOT NULL COMMENT 'UTC',
    CONSTRAINT pk_account_identities PRIMARY KEY (provider, subject),
    KEY ix_account_identities_account_id (account_id),
    CONSTRAINT fk_account_identities_account FOREIGN KEY (account_id)
        REFERENCES accounts (account_id) ON DELETE RESTRICT ON UPDATE RESTRICT,
    CONSTRAINT ck_account_identities_provider CHECK (provider = _binary'google'),
    CONSTRAINT ck_account_identities_subject CHECK (OCTET_LENGTH(subject) > 0)
) ENGINE = InnoDB;
