# Dot-sourced by Migrate.ps1. Definitions only; never connect or mutate on import.

function Get-MigrationDatabaseFailure([Exception]$Exception) {
    while ($null -ne $Exception) {
        if ($Exception -is [System.Data.Odbc.OdbcException] -and $Exception.Errors.Count -gt 0) {
            $record = $Exception.Errors[0]
            $codes = [System.Collections.Generic.List[string]]::new()
            for ($index = 0; $index -lt [Math]::Min(8, $Exception.Errors.Count); ++$index) {
                $errorRecord = $Exception.Errors[$index]
                $sqlState = [string]$errorRecord.SQLState
                if ($sqlState -cnotmatch '\A[A-Z0-9]{5}\z') { $sqlState = 'unknown' }
                $codes.Add("SQLSTATE=$sqlState; native_code=$([int]$errorRecord.NativeError)")
                if ($record.NativeError -eq 0 -and $errorRecord.NativeError -ne 0) { $record = $errorRecord }
            }
            $nativeCode = [int]$record.NativeError
            $hint = switch ($nativeCode) {
                1044 { 'Database access denied; check the migration account and Host.' }
                1045 { 'Login denied; check credentials locally.' }
                1049 { 'Database does not exist; use -CreateDatabase for initial setup.' }
                1050 { 'Table already exists; inspect partial application with -Status.' }
                1060 { 'Column already exists; inspect partial application with -Status.' }
                1064 { 'SQL syntax error; inspect the reported deployment version and statement.' }
                1142 { 'Required table privilege is missing; run -Plan to identify missing migration privileges.' }
                1146 { 'Required table is missing; inspect partial application with -Status.' }
                1227 { 'Required administrative privilege is missing; inspect the migration account grants.' }
                1305 { 'Required procedure is missing; -Status can inspect without calling it.' }
                1370 { 'Procedure EXECUTE/ALTER ROUTINE privilege is missing; inspect the migration account grants.' }
                default { 'Inspect the reported version/statement and local database diagnostics.' }
            }
            return (($codes -join ' | ') + '. ' + $hint)
        }
        $Exception = $Exception.InnerException
    }
    return 'No ODBC diagnostic is available; inspect local file/access prerequisites.'
}

function New-MigrationDatabaseFailure([Exception]$Exception) {
    # Never copy a driver message: it can contain SQL, values or connection secrets.
    $message = Get-MigrationDatabaseFailure $Exception
    $failure = [InvalidOperationException]::new($message)
    $failure.Data['SafeMigrationMessage'] = $message
    return $failure
}

function Get-MigrationStatementLabel([string]$Sql) {
    if ($Sql -match '(?is)^\s*(CREATE|DROP|ALTER)\s+(TABLE|PROCEDURE)\s+([A-Za-z_][A-Za-z_0-9]*)') {
        return ($Matches[1].ToUpperInvariant() + ' ' + $Matches[2].ToUpperInvariant() + ' ' + $Matches[3])
    }
    return 'schema statement'
}

# A local structural lint, not a substitute for the MySQL parser. In particular,
# catch an orphan END IF/BEGIN before any earlier DDL in the file can commit.
function Assert-MigrationRoutineBlocks([string]$Sql) {
    if ($Sql -notmatch '(?is)^CREATE\s+PROCEDURE\b') { return }
    $body = Get-RoutineBody $Sql
    $tokens = @([regex]::Matches($body, "'(?:''|\\.|[^'\\])*'|`"(?:`"`"|\\.|[^`"\\])*`"|``(?:````|[^``])*``|[A-Za-z_][A-Za-z_0-9]*|[^\s]") |
        ForEach-Object { $_.Value.ToUpperInvariant() })
    $stack = [System.Collections.Generic.List[string]]::new()
    for ($index = 0; $index -lt $tokens.Count; ++$index) {
        $word = $tokens[$index]
        if ($word -eq 'END') {
            Assert-Condition ($stack.Count -gt 0) 'Orphan END in deployment procedure; no DB connection was opened.'
            $expected = $stack[$stack.Count - 1]
            $closer = if ($index + 1 -lt $tokens.Count) { $tokens[$index + 1] } else { '' }
            if ($closer -in @('IF','WHILE','LOOP','REPEAT','CASE')) {
                Assert-Condition ($expected -ceq $closer) "Mismatched END $closer in deployment procedure; no DB connection was opened."
                ++$index
            } else {
                Assert-Condition ($expected -in @('BEGIN','CASE')) 'Missing block keyword after END in deployment procedure.'
            }
            $stack.RemoveAt($stack.Count - 1)
        } elseif ($word -in @('BEGIN','CASE','WHILE','LOOP','REPEAT')) {
            $stack.Add($word)
        } elseif ($word -eq 'IF') {
            $look = $index + 1
            if ($look -lt $tokens.Count -and $tokens[$look] -eq '(') {
                $depth = 0
                $functionCall = $false
                do {
                    if ($tokens[$look] -eq '(') { ++$depth }
                    elseif ($tokens[$look] -eq ')') { --$depth }
                    elseif ($tokens[$look] -eq ',' -and $depth -eq 1) { $functionCall = $true }
                    ++$look
                } while ($look -lt $tokens.Count -and $depth -gt 0)
                if ($functionCall) { continue } # IF(condition, true, false) expression
            }
            while ($look -lt $tokens.Count -and $tokens[$look] -notin @('THEN',';')) { ++$look }
            if ($look -ge $tokens.Count -or $tokens[$look] -ne 'THEN') { continue }
            $stack.Add('IF')
        }
    }
    Assert-Condition ($stack.Count -eq 0) 'Unclosed block in deployment procedure; no DB connection was opened.'
}

function Convert-MigrationSqlLiteral([string]$Value) {
    Assert-Condition ($Value.Length -le 255 -and -not $Value.Contains([string][char]0)) 'Unsupported role identifier.'
    return "'" + $Value.Replace('\','\\').Replace("'", "''") + "'"
}

function Read-MigrationGrants {
    $identity = Get-RuntimeDatabaseIdentity $script:connection $Database -AllowUnselected:$CreateDatabase
    $roles = Read-Sets 'SELECT ROLE_NAME, ROLE_HOST FROM information_schema.ENABLED_ROLES ORDER BY ROLE_NAME, ROLE_HOST'
    Assert-Condition ($roles.Sets.Count -eq 1 -and $roles.Sets[0].Columns -eq 2 -and $roles.Sets[0].Rows.Count -le 64) 'Invalid active role inventory.'
    $using = @($roles.Sets[0].Rows | ForEach-Object {
        (Convert-MigrationSqlLiteral $_[0]) + '@' + (Convert-MigrationSqlLiteral $_[1])
    })
    $query = 'SHOW GRANTS FOR CURRENT_USER'
    if ($using.Count) { $query += ' USING ' + ($using -join ', ') }
    $data = Read-Sets $query
    Assert-Condition ($data.Sets.Count -eq 1 -and $data.Sets[0].Columns -eq 1) 'Invalid privilege inspection result.'
    $grants = [System.Collections.Generic.List[object]]::new()
    foreach ($row in $data.Sets[0].Rows) {
        if ($row[0] -notmatch '^(GRANT|REVOKE)\s+(.+?)\s+ON\s+(.+?)\s+(?:TO|FROM)\s+') { continue }
        $grantAction = $Matches[1]; $privileges = @($Matches[2].Split(',') | ForEach-Object { $_.Trim().ToUpperInvariant() })
        $scope = $Matches[3]
        if ($scope -notmatch '^(?:(PROCEDURE|FUNCTION)\s+)?(\*|`(?:``|[^`])+`)\.(\*|`(?:``|[^`])+`)$') { continue }
        $kind = $Matches[1]; $schema = $Matches[2]; $target = $Matches[3]
        foreach ($privilege in $privileges) {
            $grants.Add([pscustomobject]@{ Allowed = ($grantAction -eq 'GRANT'); Privilege = $privilege;
                Kind = $kind; Schema = $schema.Trim('`').Replace('``','`'); Target = $target.Trim('`').Replace('``','`') })
        }
    }
    return [pscustomobject]@{ Identity = $identity; Grants = $grants; PartialRevokes = ((Read-Scalar 'SELECT @@partial_revokes') -ceq '1') }
}

function Test-MigrationGrant($Access, [string]$Privilege, [string]$Kind, [string]$Target) {
    $allowed = $false
    foreach ($grant in $Access.Grants) {
        if ($grant.Privilege -cne $Privilege -and $grant.Privilege -cne 'ALL PRIVILEGES') { continue }
        $schemaMatches = $grant.Schema -ceq '*' -or $grant.Schema -ceq $Database
        if (-not $schemaMatches -and -not $Access.PartialRevokes) {
            $pattern = [System.Text.StringBuilder]::new('^')
            for ($index = 0; $index -lt $grant.Schema.Length; ++$index) {
                $value = $grant.Schema[$index]
                if ($value -eq '\' -and $index + 1 -lt $grant.Schema.Length) { ++$index; [void]$pattern.Append([regex]::Escape([string]$grant.Schema[$index])) }
                elseif ($value -eq '%') { [void]$pattern.Append('.*') }
                elseif ($value -eq '_') { [void]$pattern.Append('.') }
                else { [void]$pattern.Append([regex]::Escape([string]$value)) }
            }
            [void]$pattern.Append('$'); $schemaMatches = $Database -cmatch $pattern.ToString()
        }
        if (-not $schemaMatches) { continue }
        if ($grant.Target -cne '*' -and ($grant.Target -cne $Target -or
            (($Kind -eq 'ROUTINE') -ne ($grant.Kind -eq 'PROCEDURE')))) { continue }
        if ($grant.Target -ceq '*' -and $grant.Kind) { continue }
        if (-not $grant.Allowed) { return $false } # Conservative for partial revokes.
        $allowed = $true
    }
    return $allowed
}

function Assert-EmptyMigrationDatabase {
    $objects = Read-Scalar 'SELECT (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()) + (SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE()) + (SELECT COUNT(*) FROM information_schema.EVENTS WHERE EVENT_SCHEMA = DATABASE()) + (SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA = DATABASE())'
    Assert-Condition ($objects -ceq '0') 'Refusing to baseline a nonempty/unmanaged schema.'
}

function Assert-MigrationPrivileges([int]$Head, [string]$Action, [switch]$Bootstrap, [switch]$Recovery) {
    $script:stage = 'migration privilege preflight (no audit or DDL writes)'
    $access = Read-MigrationGrants
    $requirements = [System.Collections.Generic.List[object]]::new()
    foreach ($name in @('SELECT','INSERT','UPDATE')) { $requirements.Add(@($name,'TABLE','schema_migrations')) }
    if ($Bootstrap) { $requirements.Add(@('CREATE','TABLE','schema_migrations')) }
    # Without these database-scoped privileges information_schema can hide
    # triggers/events. A zero count would not prove that retrying DDL is safe.
    if ($Recovery) {
        foreach ($privilege in @('TRIGGER','EVENT')) { $requirements.Add(@($privilege,'DATABASE','*')) }
    }
    if ($Action -eq 'DOWN' -and $Head -in @(1,4,5,6,7,8)) {
        $requirements.Add(@('LOCK TABLES','TABLE','schema_migrations'))
        $lockTables = switch ($Head) {
            1 { @('accounts','account_identities') }
            4 { @('characters','character_skills') }
            5 { @('character_operations','character_items','character_state') }
            6 { @('character_item_uses') }
            7 { @('character_quest_events','character_quest_tracking','character_quests') }
            8 { @('characters','character_quests') }
        }
        foreach ($table in $lockTables) {
            foreach ($privilege in @('SELECT','LOCK TABLES')) { $requirements.Add(@($privilege,'TABLE',$table)) }
        }
    }
    $versions = if ($Action -eq 'UP') { @(($Head + 1)..$script:catalog.Count | Where-Object { $_ -gt $Head -and $_ -le $script:catalog.Count }) }
        else { @($Head) }
    foreach ($version in $versions) {
        $file = if ($Action -eq 'UP') { $script:catalog[$version].Up } else { $script:catalog[$version].Down }
        foreach ($sql in $file.Statements) {
            if ($sql -match '(?is)^(CREATE|DROP|ALTER)\s+(TABLE|PROCEDURE)\s+([A-Za-z_][A-Za-z_0-9]*)') {
                $verb = $Matches[1].ToUpperInvariant(); $kind = $Matches[2].ToUpperInvariant(); $name = $Matches[3]
                if ($kind -eq 'PROCEDURE') {
                    if ($verb -eq 'CREATE') {
                        foreach ($privilege in @('CREATE ROUTINE','SELECT','INSERT','UPDATE','DELETE','EXECUTE')) { $requirements.Add(@($privilege,'DATABASE','*')) }
                    } else { $requirements.Add(@('ALTER ROUTINE','ROUTINE',$name)) }
                } elseif ($verb -eq 'ALTER') {
                    foreach ($privilege in @('ALTER','CREATE','INSERT')) { $requirements.Add(@($privilege,'TABLE',$name)) }
                } elseif ($verb -eq 'CREATE') { $requirements.Add(@('CREATE','TABLE',$name)) }
                else {
                    foreach ($table in ($sql -replace '(?is)^DROP\s+TABLE\s+', '').Split(',')) {
                        $table = $table.Trim()
                        Assert-Condition ($table -cmatch '^[A-Za-z_][A-Za-z_0-9]*$') 'Unsupported DROP TABLE preflight.'
                        foreach ($privilege in @('DROP','SELECT','LOCK TABLES')) { $requirements.Add(@($privilege,'TABLE',$table)) }
                    }
                }
            } else { Assert-Condition $false 'Unsupported top-level migration statement; extend privilege preflight before executing it.' }
            if ($sql -match '(?is)^CREATE\s+TABLE\b') {
                foreach ($reference in [regex]::Matches($sql, '(?i)\bREFERENCES\s+([A-Za-z_][A-Za-z_0-9]*)')) {
                    $requirements.Add(@('REFERENCES','TABLE',$reference.Groups[1].Value))
                }
            }
        }
    }
    $missing = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    foreach ($requirement in $requirements) {
        if (-not (Test-MigrationGrant $access $requirement[0] $requirement[1] $requirement[2])) {
            [void]$missing.Add($requirement[0])
            Write-Host ("Missing migration privilege: {0} on {1}.{2}; account={3}" -f $requirement[0],$Database,$requirement[2],$access.Identity.Principal)
        }
    }
    if ($missing.Count) {
        $grantDatabase = if ($access.PartialRevokes) { $Database } else { $Database.Replace('_','\_') }
        $grantSql = 'GRANT {0} ON `{1}`.* TO ''{2}''@''{3}'';' -f ([string]::Join(', ', $missing)),$grantDatabase,$access.Identity.User,$access.Identity.HostName
        Write-Host 'Ask the permission administrator to review this persistent migration/DEFINER-account grant (never a runtime account):'
        Write-Host $grantSql
        Assert-Condition $false 'Migration privilege preflight failed. No DDL/recovery audit was written. Fix this account/active roles, then rerun the same command.'
    }
    Write-Host "Migration privileges verified for $($access.Identity.Principal)."
}

function Read-MigrationAudit {
    $data = Read-Sets @'
SELECT CAST(m.execution_id AS CHAR), CAST(m.version AS CHAR), m.name, m.direction,
    m.up_checksum, m.down_checksum, m.state,
    DATE_FORMAT(m.started_at, '%Y-%m-%dT%H:%i:%s.%fZ'),
    DATE_FORMAT(m.finished_at, '%Y-%m-%dT%H:%i:%s.%fZ')
FROM schema_migrations m ORDER BY m.execution_id
'@
    Assert-Condition ($data.Sets.Count -eq 1 -and $data.Sets[0].Columns -eq 9) 'Invalid direct audit result.'
    return $data.Sets[0]
}

function Get-MigrationWorkflowState {
    $script:stage = 'read-only audit validation'
    $audit = Read-MigrationAudit
    $rows = $audit.Rows
    Assert-Condition ($rows.Count -gt 0) 'Missing bootstrap audit; inspect initial setup separately.'
    $last = $rows[$rows.Count - 1]
    if ($last[6] -ceq 'FAILED' -or $last[6] -ceq 'RUNNING') {
        $prefixRows = [System.Collections.Generic.List[object]]::new()
        for ($index = 0; $index -lt $rows.Count - 1; ++$index) { $prefixRows.Add($rows[$index]) }
        $prefix = [pscustomobject]@{ Sets = @($null, [pscustomobject]@{ Rows = $prefixRows }) }
        $head = Get-Head $prefix
        Assert-Condition ($last[0] -cmatch '^[1-9][0-9]*$' -and $last[1] -cmatch '^[1-9][0-9]*$' -and
            [uint64]$last[0] -gt [uint64]$prefixRows[$prefixRows.Count - 1][0]) 'Invalid pending audit identifier.'
        $version = [int]$last[1]
        Assert-Condition ($version -ge 2 -and $version -eq $head + 1 -and $script:catalog.ContainsKey($version)) 'Recovery requires the next Up version; use existing V0/V1 recovery or inspect manually.'
        $file = $script:catalog[$version]
        Assert-Condition ($last[2] -ceq $file.Name -and $last[3] -ceq 'UP' -and
            $last[4] -ceq $file.Up.Hash -and $last[5] -ceq $file.Down.Hash) 'Pending migration name/checksum mismatch; no recovery is allowed.'
        return [pscustomobject]@{ Head = $head; Pending = $last; Rows = $rows }
    }
    $snapshot = [pscustomobject]@{ Sets = @($null, $audit) }
    return [pscustomobject]@{ Head = (Get-Head $snapshot); Pending = $null; Rows = $rows }
}

function Show-MigrationStatus([bool]$HasHistory) {
    $script:stage = 'read-only status'
    Write-Host 'Status only: no DDL, audit update or GRANT will run.'
    if ($HasHistory) {
        $audit = Read-MigrationAudit
        foreach ($row in $audit.Rows) {
            Write-Host ("Audit execution={0}; version={1}; name={2}; direction={3}; state={4}; started={5}; finished={6}" -f
                $row[0],$row[1],$row[2],$row[3],$row[6],$row[7],$row[8])
        }
    } else { Write-Host 'No schema_migrations table; this database is not managed yet.' }
    $inventory = Read-Sets @'
SELECT 'TABLE', TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()
UNION ALL SELECT ROUTINE_TYPE, ROUTINE_NAME FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE()
UNION ALL SELECT 'COLUMN', CONCAT(TABLE_NAME, '.', COLUMN_NAME) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'experience'
ORDER BY 1, 2
'@
    foreach ($row in $inventory.Sets[0].Rows) { Write-Host ("Object: {0} {1}" -f $row[0],$row[1]) }
}

function Assert-UnchangedRecovery($Workflow) {
    $script:stage = 'verifying unchanged schema before recovery'
    $failed = $Workflow.Pending
    Assert-Condition ($null -ne $failed -and $failed[6] -ceq 'FAILED') 'Automatic recovery refuses RUNNING/unknown completion; inspect -Status without replaying DDL.'
    Assert-RecoveryAuditTime $failed
    $file = $script:catalog[[int]$failed[1]]
    # Only metadata definitions/additive columns in this catalog can be retried.
    # Top-level data changes and destructive table changes cannot be certified by schema comparison.
    foreach ($sql in $file.Up.Statements) {
        $definition = $sql -match '(?is)^CREATE\s+PROCEDURE\s+[A-Za-z_][A-Za-z_0-9]*\s*\('
        $table = $sql -match '(?is)^CREATE\s+TABLE\s+[A-Za-z_][A-Za-z_0-9]*\s*\(.*\)\s*ENGINE\s*=\s*InnoDB\s*$'
        $dropRoutine = $sql -match '(?is)^DROP\s+PROCEDURE\s+[A-Za-z_][A-Za-z_0-9]*\s*$'
        $addColumn = (Normalize-Sql $sql) -cmatch '^alter table [a-z_][a-z_0-9]* add column [a-z_][a-z_0-9]* bigint unsigned not null default 0$'
        Assert-Condition ($definition -or $table -or $dropRoutine -or $addColumn) 'Recovery cannot certify this statement class; no DDL or audit change was made.'
    }
    $snapshot = Get-Snapshot
    Assert-Condition ((ConvertTo-Json -InputObject $snapshot.Sets[1].Rows -Depth 4 -Compress) -ceq
        (ConvertTo-Json -InputObject $Workflow.Rows -Depth 4 -Compress)) 'Audit changed during recovery verification.'
    Assert-Structure $snapshot $Workflow.Head
    # Inspectors intentionally project managed objects. Reject extra objects too.
    $tables = @($snapshot.Sets[2].Rows | ForEach-Object { $_[0] } | Sort-Object -Unique)
    $counts = Read-Sets 'SELECT (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()), (SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE()), (SELECT COUNT(*) FROM information_schema.EVENTS WHERE EVENT_SCHEMA = DATABASE()), (SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA = DATABASE())'
    Assert-Condition ($counts.Sets.Count -eq 1 -and $counts.Sets[0].Columns -eq 4 -and $counts.Sets[0].Rows.Count -eq 1) 'Invalid recovery object inventory.'
    $count = $counts.Sets[0].Rows[0]
    Assert-Condition ($count[0] -ceq ([string]$tables.Count) -and $count[1] -ceq ([string]$snapshot.Sets[5].Rows.Count) -and
        $count[2] -ceq '0' -and $count[3] -ceq '0') 'Extra/partial schema objects exist; automatic recovery is blocked.'
    Write-Host ("Recovery verified: execution {0} FAILED; actual structure still matches V{1:D6}. Original audit will be preserved." -f $failed[0],$Workflow.Head)
}

function Confirm-UnchangedRecovery($Workflow) {
    Assert-Lock
    $script:stage = 'appending verified unchanged-schema confirmation (no DDL)'
    $failed = $Workflow.Pending
    $name = $failed[2] + '_unchanged_recovery'
    Assert-Condition ($name.Length -le 128) 'Recovery audit name exceeds its schema limit.'
    $insert = @'
INSERT INTO schema_migrations (version, name, direction, up_checksum, down_checksum, state, started_at, finished_at)
SELECT failed.version, ?, 'UP', failed.up_checksum, failed.down_checksum, 'SUCCEEDED', UTC_TIMESTAMP(6), UTC_TIMESTAMP(6)
FROM schema_migrations failed
LEFT JOIN schema_migrations later ON later.execution_id > failed.execution_id
WHERE later.execution_id IS NULL AND failed.execution_id = ? AND failed.version = ?
    AND CAST(failed.name AS BINARY) = CAST(? AS BINARY) AND failed.direction = 'UP'
    AND CAST(failed.up_checksum AS BINARY) = CAST(? AS BINARY)
    AND CAST(failed.down_checksum AS BINARY) = CAST(? AS BINARY)
    AND failed.state = 'FAILED' AND failed.finished_at IS NOT NULL AND UTC_TIMESTAMP(6) >= failed.finished_at
'@
    Assert-Condition ((Invoke-Write $insert @($name,$failed[0],$failed[1],$failed[2],$failed[4],$failed[5])) -eq 1) 'Cannot append unchanged-schema confirmation; inspect -Status before retrying.'
    $audit = Read-MigrationAudit
    Assert-Condition ($audit.Rows.Count -eq $Workflow.Rows.Count + 1) 'Unexpected recovery audit count.'
    for ($index = 0; $index -lt $Workflow.Rows.Count; ++$index) {
        Assert-Condition ((ConvertTo-Json -InputObject $audit.Rows[$index] -Compress) -ceq
            (ConvertTo-Json -InputObject $Workflow.Rows[$index] -Compress)) 'Recovery changed prior audit rows.'
    }
    Assert-Condition ((Get-Head ([pscustomobject]@{ Sets = @($null, $audit) })) -eq $Workflow.Head) 'Recovery must not advance the active schema version.'
    Write-Host 'Unchanged-schema confirmation appended. Retrying normal Up with the original deployment SQL.'
}
