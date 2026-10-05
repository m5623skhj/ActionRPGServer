# Windows PowerShell 5.1; one ODBC connection owns the DB lock and all execution.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Up', 'Down')][string]$Direction,
    [Parameter(Mandatory = $true)][ValidateLength(1, 64)][string]$Database,
    [switch]$ServicesStopped
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Data
$script:connection = $null
$script:lockName = $null
$script:ownsLock = $false
$script:warningSeen = $false
$script:stage = 'local file validation'

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        $failure = [System.InvalidOperationException]::new($Message)
        $failure.Data['SafeMigrationMessage'] = $Message
        throw $failure
    }
}

# DELIMITER is a client directive, not SQL. Comments are removed only outside
# quoted tokens; semicolons in stored procedure bodies keep their meaning.
function Split-Sql([string]$Text) {
    $statements = [System.Collections.Generic.List[string]]::new()
    $buffer = [System.Text.StringBuilder]::new()
    $delimiter = ';'
    $quote = [char]0
    $blockComment = $false
    $lineComment = $false
    $lineStart = $true
    for ($i = 0; $i -lt $Text.Length; ++$i) {
        $c = $Text[$i]
        $next = if ($i + 1 -lt $Text.Length) { $Text[$i + 1] } else { [char]0 }
        if ($lineComment) {
            if ($c -eq "`n") { $lineComment = $false; $lineStart = $true; [void]$buffer.Append("`n") }
            continue
        }
        if ($blockComment) {
            if ($c -eq '*' -and $next -eq '/') { $blockComment = $false; ++$i; [void]$buffer.Append(' ') }
            continue
        }
        if ($quote -ne [char]0) {
            [void]$buffer.Append($c)
            if ($c -eq '\' -and $quote -ne '`' -and $next -ne [char]0) { [void]$buffer.Append($next); ++$i }
            elseif ($c -eq $quote) {
                if ($next -eq $quote) { [void]$buffer.Append($next); ++$i } else { $quote = [char]0 }
            }
            continue
        }
        if ($lineStart) {
            $end = $Text.IndexOf("`n", $i)
            if ($end -lt 0) { $end = $Text.Length }
            $line = $Text.Substring($i, $end - $i)
            if ($line -match '^\s*DELIMITER\s+(\S+)\s*$') {
                Assert-Condition ($buffer.ToString().Trim().Length -eq 0) 'DELIMITER inside an unfinished statement.'
                $delimiter = $Matches[1]
                Assert-Condition ($delimiter -eq ';' -or $delimiter -eq '$$') 'Unsupported SQL delimiter.'
                $i = $end
                continue
            }
            $lineStart = $false
        }
        if ($c -eq '/' -and $next -eq '*') {
            Assert-Condition ($i + 2 -ge $Text.Length -or $Text[$i + 2] -ne '!') 'Executable SQL comments are unsupported.'
            $blockComment = $true; ++$i; continue
        }
        if ($c -eq '#' -or ($c -eq '-' -and $next -eq '-' -and
            ($i + 2 -ge $Text.Length -or [char]::IsWhiteSpace($Text[$i + 2])))) {
            $lineComment = $true; continue
        }
        if ($c -eq "'" -or $c -eq '"' -or $c -eq '`') { $quote = $c; [void]$buffer.Append($c); continue }
        if ($i + $delimiter.Length -le $Text.Length -and $Text.Substring($i, $delimiter.Length) -ceq $delimiter) {
            $statement = $buffer.ToString().Trim()
            Assert-Condition ($statement.Length -gt 0) 'Empty SQL statement.'
            $statements.Add($statement); [void]$buffer.Clear(); $i += $delimiter.Length - 1
            continue
        }
        [void]$buffer.Append($c)
        if ($c -eq "`n") { $lineStart = $true }
    }
    Assert-Condition ($quote -eq [char]0 -and -not $blockComment -and $buffer.ToString().Trim().Length -eq 0) 'Unterminated SQL statement/comment/string.'
    Assert-Condition ($statements.Count -gt 0) 'SQL file has no statements.'
    return [pscustomobject]@{ Statements = $statements }
}

function Read-SqlFile([string]$Path) {
    $utf8 = [System.Text.UTF8Encoding]::new($false, $true)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    Assert-Condition ($bytes.Length -gt 0 -and $bytes.Length -le 1048576) 'SQL file is empty/too large.'
    $text = $utf8.GetString($bytes)
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    $text = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    Assert-Condition (-not $text.Contains([string][char]0)) 'NUL in SQL file.'
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $hash = ([BitConverter]::ToString($sha.ComputeHash($utf8.GetBytes($text)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    return [pscustomobject]@{ Path = $Path; Hash = $hash; Statements = (Split-Sql $text).Statements }
}

function New-Command([string]$Sql, [object[]]$Values = @()) {
    $command = $script:connection.CreateCommand()
    $command.CommandText = $Sql
    $command.CommandTimeout = 30
    foreach ($value in $Values) {
        $parameter = $command.Parameters.Add('?', [System.Data.Odbc.OdbcType]::NVarChar, 4096)
        $parameter.Value = if ($null -eq $value) { [DBNull]::Value } else { [string]$value }
    }
    $script:warningSeen = $false
    return $command
}

function Invoke-Write([string]$Sql, [object[]]$Values = @()) {
    $command = New-Command $Sql $Values
    try {
        $count = $command.ExecuteNonQuery()
        Assert-Condition (-not $script:warningSeen) 'ODBC warning; execution requires inspection.'
        return $count
    } finally { $command.Dispose() }
}

function Read-Sets([string]$Sql, [object[]]$Values = @()) {
    $command = New-Command $Sql $Values
    $reader = $null
    $sets = [System.Collections.Generic.List[object]]::new()
    $rowCount = 0
    try {
        $reader = $command.ExecuteReader()
        do {
            if ($reader.FieldCount -gt 0) {
                $rows = [System.Collections.Generic.List[object]]::new()
                while ($reader.Read()) {
                    Assert-Condition (++$rowCount -le 4096) 'Inspection row limit exceeded; coordinate audit retention without deleting history.'
                    $cells = [object[]]::new($reader.FieldCount)
                    for ($i = 0; $i -lt $cells.Length; ++$i) {
                        $cells[$i] = if ($reader.IsDBNull($i)) { $null } else { [string]$reader.GetValue($i) }
                        Assert-Condition ($null -eq $cells[$i] -or $cells[$i].Length -le 32768) 'Inspection value limit exceeded.'
                    }
                    $rows.Add($cells)
                }
                $sets.Add([pscustomobject]@{ Columns = $reader.FieldCount; Rows = $rows })
            }
        } while ($reader.NextResult())
        Assert-Condition (-not $script:warningSeen) 'ODBC warning; inspection requires review.'
        return [pscustomobject]@{ Sets = $sets }
    } finally {
        if ($null -ne $reader) { $reader.Dispose() }
        $command.Dispose()
    }
}

function Read-Scalar([string]$Sql, [object[]]$Values = @()) {
    $data = Read-Sets $Sql $Values
    Assert-Condition ($data.Sets.Count -eq 1 -and $data.Sets[0].Columns -eq 1 -and $data.Sets[0].Rows.Count -eq 1) 'Invalid scalar inspection result.'
    return $data.Sets[0].Rows[0][0]
}

function Assert-Lock {
    Assert-Condition ((Read-Scalar 'SELECT IS_USED_LOCK(?) = CONNECTION_ID()' @($script:lockName)) -ceq '1') 'Migration lock was lost.'
}

# Normalize insignificant SQL tokens for metadata comparison. Quoted strings
# retain case/whitespace; character-set introducers other than _binary do not
# change these ASCII CHECK literals. No SQL body is executed by this function.
function Normalize-Sql([string]$Sql) {
    $tokens = [regex]::Matches($Sql, "'(?:''|\\.|[^'\\])*'|`"(?:`"`"|\\.|[^`"\\])*`"|``(?:````|[^``])*``|[A-Za-z_][A-Za-z_0-9]*|[^\s]")
    $result = [System.Text.StringBuilder]::new()
    foreach ($token in $tokens) {
        $value = $token.Value
        if ($value.StartsWith('`')) { $value = $value.Substring(1, $value.Length - 2).Replace('``', '`') }
        if ($value -match '^_(ascii|utf8mb4|utf8mb3|utf8|latin1)$') { continue }
        if (-not $value.StartsWith("'") -and -not $value.StartsWith('"')) { $value = $value.ToLowerInvariant() }
        if ($result.Length -gt 0) { [void]$result.Append(' ') }
        [void]$result.Append($value)
    }
    return $result.ToString()
}

function Get-RoutineBody([string]$Statement) {
    # SHOW CREATE prefixes a quoted DEFINER. A user/host named 'begin' must
    # never be mistaken for the body opener; quoted tokens are skipped whole.
    $pattern = "'(?:''|\\.|[^'\\])*'|`"(?:`"`"|\\.|[^`"\\])*`"|``(?:````|[^``])*``|\bBEGIN\b"
    foreach ($token in [regex]::Matches($Statement, $pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
        if ($token.Value -ieq 'BEGIN') { return $Statement.Substring($token.Index) }
    }
    Assert-Condition $false 'Original routine body missing.'
}

# INFORMATION_SCHEMA adds parentheses around predicates. Parse only the
# AND/OR grouping of these known CHECKs, retaining every atomic SQL token.
function Normalize-Check([string]$Sql) {
    function Convert-Boolean([string]$Expression) {
        $expression = $Expression.Trim()
        while ($expression.StartsWith('(') -and $expression.EndsWith(')')) {
            $depth = 0; $wrapped = $true
            $tokens = [regex]::Matches($expression, "'(?:''|\\.|[^'\\])*'|\(|\)|[^\s]+")
            foreach ($token in $tokens) {
                if ($token.Value -eq '(') { ++$depth }
                elseif ($token.Value -eq ')') { --$depth }
                if ($depth -eq 0 -and $token.Index + $token.Length -lt $expression.Length) { $wrapped = $false; break }
            }
            if (-not $wrapped) { break }
            $expression = $expression.Substring(1, $expression.Length - 2).Trim()
        }
        foreach ($operator in @('or', 'and')) {
            $depth = 0; $start = 0
            $parts = [System.Collections.Generic.List[string]]::new()
            foreach ($token in [regex]::Matches($expression, "'(?:''|\\.|[^'\\])*'|\(|\)|[^\s]+")) {
                if ($token.Value -eq '(') { ++$depth }
                elseif ($token.Value -eq ')') { --$depth }
                elseif ($depth -eq 0 -and $token.Value -ceq $operator) {
                    $parts.Add((Convert-Boolean $expression.Substring($start, $token.Index - $start)))
                    $start = $token.Index + $token.Length
                }
            }
            if ($parts.Count -gt 0) {
                $parts.Add((Convert-Boolean $expression.Substring($start)))
                return $operator + '[' + [string]::Join('|', $parts) + ']'
            }
        }
        return $expression
    }
    return Convert-Boolean (Normalize-Sql $Sql)
}

function Get-Snapshot {
    Assert-Lock
    $snapshot = Read-Sets 'CALL get_schema_migration_history()'
    $shapes = @(5, 9, 9, 11, 6, 4, 7, 6, 6, 6)
    Assert-Condition ($snapshot.Sets.Count -eq $shapes.Count) 'Invalid history result set count.'
    for ($i = 0; $i -lt $shapes.Count; ++$i) {
        Assert-Condition ($snapshot.Sets[$i].Columns -eq $shapes[$i]) 'Invalid history result shape.'
    }
    Assert-Condition ($snapshot.Sets[0].Rows.Count -eq 1) 'Invalid history header.'
    $header = $snapshot.Sets[0].Rows[0]
    Assert-Condition ($header[0] -ceq '1' -and $header[1] -ceq 'sha256-utf8-lf-v1' -and
        $header[2] -ceq $Database -and $header[3] -match '^8\.0\.47(?:$|[-+])' -and $header[4] -notmatch 'MariaDB') 'Unsupported target/inspection format.'
    return $snapshot
}

function Get-Head($Snapshot) {
    $rows = $Snapshot.Sets[1].Rows
    Assert-Condition ($rows.Count -gt 0) 'Missing bootstrap audit.'
    [uint64]$previousId = 0
    $head = 0
    for ($i = 0; $i -lt $rows.Count; ++$i) {
        $row = $rows[$i]
        Assert-Condition ($row[0] -cmatch '^[1-9][0-9]*$' -and $row[1] -cmatch '^(0|[1-9][0-9]*)$') 'Invalid audit identifier/version.'
        [uint64]$id = $row[0]
        [int]$version = $row[1]
        Assert-Condition ($id -gt $previousId -and $row[6] -ceq 'SUCCEEDED' -and
            $row[7] -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z$' -and
            $row[8] -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z$' -and
            [string]::CompareOrdinal($row[8], $row[7]) -ge 0) 'Failed, unfinished, or invalid audit; inspect before repair.'
        $previousId = $id
        if ($i -eq 0) {
            Assert-Condition ($version -eq 0 -and $row[2] -ceq 'migration_history' -and $row[3] -ceq 'UP' -and
                $row[4] -ceq $script:bootstrap.Hash -and $null -eq $row[5]) 'Bootstrap checksum/history mismatch.'
            continue
        }
        Assert-Condition ($version -gt 0 -and $script:catalog.ContainsKey($version)) 'Unknown/missing migration version.'
        $file = $script:catalog[$version]
        Assert-Condition ($row[2] -ceq $file.Name -and $row[4] -ceq $file.Up.Hash -and $row[5] -ceq $file.Down.Hash) 'Migration name/checksum mismatch.'
        if ($row[3] -ceq 'UP') {
            Assert-Condition ($version -eq $head + 1) 'Non-contiguous Up history.'
            $head = $version
        } elseif ($row[3] -ceq 'DOWN') {
            Assert-Condition ($head -gt 0 -and $version -eq $head) 'Invalid Down history.'
            --$head
        } else { Assert-Condition $false 'Unknown audit direction.' }
    }
    return $head
}

function Assert-Rows($Actual, $Expected, [string]$Label) {
    $actualKeys = @($Actual | ForEach-Object { ConvertTo-Json -InputObject $_ -Compress } | Sort-Object)
    $expectedKeys = @($Expected | ForEach-Object { ConvertTo-Json -InputObject $_ -Compress } | Sort-Object)
    Assert-Condition ($actualKeys.Count -eq $expectedKeys.Count) "$Label count mismatch."
    for ($i = 0; $i -lt $actualKeys.Count; ++$i) {
        Assert-Condition ($actualKeys[$i] -ceq $expectedKeys[$i]) "$Label mismatch."
    }
}

function Assert-Structure($Snapshot, [int]$Head) {
    Assert-Condition ($Head -ge 0 -and $Head -le 3) 'Update the schema inspection contract before adding schema versions.'
    $columns = [System.Collections.Generic.List[object]]::new()
    $constraints = [System.Collections.Generic.List[object]]::new()
    $indexes = [System.Collections.Generic.List[object]]::new()
    # Field order matches the inspection SQL. NULL and empty string differ.
    $definitions = @(
        @('execution_id','bigint unsigned','NO',$null,'auto_increment',$null,$null),
        @('version','int unsigned','NO',$null,'',$null,$null),
        @('name','varchar(128)','NO','ascii_bin','',$null,'ascii'),
        @('direction',"enum('UP','DOWN')",'NO','ascii_bin','',$null,'ascii'),
        @('up_checksum','char(64)','NO','ascii_bin','',$null,'ascii'),
        @('down_checksum','char(64)','YES','ascii_bin','',$null,'ascii'),
        @('state',"enum('RUNNING','SUCCEEDED','FAILED')",'NO','ascii_bin','',$null,'ascii'),
        @('started_at','datetime(6)','NO',$null,'',$null,$null),
        @('finished_at','datetime(6)','YES',$null,'',$null,$null)
    )
    foreach ($d in $definitions) { $columns.Add(@('schema_migrations',$d[0],$d[1],$d[2],$d[3],$d[4],'InnoDB',$d[5],$d[6])) }
    $constraints.Add(@('schema_migrations','PRIMARY','PRIMARY KEY','execution_id',$null,$null,$null,$null,$null,$null,$null))
    $constraints.Add(@('schema_migrations','ck_migrations_completion','CHECK',$null,$null,$null,
        (Normalize-Check "((state = 'RUNNING' AND finished_at IS NULL) OR (state IN ('SUCCEEDED', 'FAILED') AND finished_at IS NOT NULL))"),'YES',$null,$null,$null))
    $constraints.Add(@('schema_migrations','ck_migrations_bootstrap','CHECK',$null,$null,$null,
        (Normalize-Check "((version = 0 AND direction = 'UP' AND down_checksum IS NULL) OR (version > 0 AND down_checksum IS NOT NULL))"),'YES',$null,$null,$null))
    $indexes.Add(@('schema_migrations','PRIMARY','0','1','execution_id',$null))
    if ($Head -ge 1) {
        foreach ($d in @(@('account_id','bigint unsigned','NO','auto_increment',$null),@('status','int','NO','','0'),
            @('created_at','datetime(6)','NO','',$null),@('last_login_at','datetime(6)','YES','',$null))) {
            $columns.Add(@('accounts',$d[0],$d[1],$d[2],$null,$d[3],'InnoDB',$d[4],$null))
        }
        foreach ($d in @(@('account_id','bigint unsigned'),@('provider','varbinary(32)'),@('subject','varbinary(255)'),@('linked_at','datetime(6)'))) {
            $columns.Add(@('account_identities',$d[0],$d[1],'NO',$null,'','InnoDB',$null,$null))
        }
        $constraints.Add(@('accounts','PRIMARY','PRIMARY KEY','account_id',$null,$null,$null,$null,$null,$null,$null))
        $constraints.Add(@('accounts','ck_accounts_status','CHECK',$null,$null,$null,(Normalize-Check '(status IN (0, 1))'),'YES',$null,$null,$null))
        foreach ($column in @('provider','subject')) { $constraints.Add(@('account_identities','PRIMARY','PRIMARY KEY',$column,$null,$null,$null,$null,$null,$null,$null)) }
        $constraints.Add(@('account_identities','fk_account_identities_account','FOREIGN KEY','account_id','accounts','account_id',$null,$null,$Database,'RESTRICT','RESTRICT'))
        $constraints.Add(@('account_identities','ck_account_identities_provider','CHECK',$null,$null,$null,(Normalize-Check "(provider = _binary'google')"),'YES',$null,$null,$null))
        $constraints.Add(@('account_identities','ck_account_identities_subject','CHECK',$null,$null,$null,(Normalize-Check '(OCTET_LENGTH(subject) > 0)'),'YES',$null,$null,$null))
        $indexes.Add(@('accounts','PRIMARY','0','1','account_id',$null))
        $indexes.Add(@('account_identities','PRIMARY','0','1','provider',$null))
        $indexes.Add(@('account_identities','PRIMARY','0','2','subject',$null))
        $indexes.Add(@('account_identities','ix_account_identities_account_id','1','1','account_id',$null))
    }
    $actualConstraints = [System.Collections.Generic.List[object]]::new()
    foreach ($row in $Snapshot.Sets[3].Rows) {
        $copy = $row.Clone()
        if ($null -ne $copy[6]) { $copy[6] = Normalize-Check $copy[6] }
        $actualConstraints.Add($copy)
    }
    $orderedColumns = [System.Collections.Generic.List[object]]::new()
    foreach ($table in @('account_identities','accounts','schema_migrations')) {
        foreach ($column in $columns) { if ($column[0] -ceq $table) { $orderedColumns.Add($column) } }
    }
    Assert-Condition ((ConvertTo-Json -InputObject $Snapshot.Sets[2].Rows -Compress -Depth 4) -ceq
        (ConvertTo-Json -InputObject $orderedColumns -Compress -Depth 4)) 'Columns/order/defaults/engine mismatch.'
    Assert-Rows $actualConstraints $constraints 'Constraints'
    Assert-Rows $Snapshot.Sets[4].Rows $indexes 'Indexes'
    $routines = [System.Collections.Generic.List[object]]::new()
    $parameters = [System.Collections.Generic.List[object]]::new()
    $files = @($script:bootstrap)
    if ($Head -ge 2) { $files += $script:catalog[2].Up; $parameters.Add(@('login_google_account','1','IN','inSubject','text','utf8mb4','utf8mb4_bin')) }
    if ($Head -ge 3) { $files += $script:catalog[3].Up; $parameters.Add(@('get_auth_account_status','1','IN','inAccountId','bigint unsigned',$null,$null)) }
    foreach ($file in $files) {
        foreach ($statement in $file.Statements) {
            if ($statement -match '(?is)^CREATE\s+PROCEDURE\s+(\w+).*?\bBEGIN\b') {
                $name = $Matches[1]
                $access = if ($statement -match '(?i)MODIFIES\s+SQL\s+DATA') { 'MODIFIES SQL DATA' } else { 'READS SQL DATA' }
                $body = Get-RoutineBody $statement
                $routines.Add(@($name,'DEFINER',$access,(Normalize-Sql $body)))
            }
        }
    }
    $originalBodies = @{}
    $routineNames = @('get_schema_migration_history','login_google_account','get_auth_account_status')
    for ($i = 0; $i -lt $routineNames.Count; ++$i) {
        $set = $Snapshot.Sets[7 + $i]
        Assert-Condition ($set.Rows.Count -eq 1) 'Invalid SHOW CREATE row count.'
        $row = $set.Rows[0]
        $present = $i -eq 0 -or ($i -eq 1 -and $Head -ge 2) -or ($i -eq 2 -and $Head -ge 3)
        if (-not $present) {
            foreach ($cell in $row) { Assert-Condition ($null -eq $cell) 'Unexpected original routine definition.' }
            continue
        }
        foreach ($cell in $row) { Assert-Condition (-not [string]::IsNullOrEmpty($cell)) 'SHOW CREATE definition/metadata unavailable.' }
        Assert-Condition ($row[0] -ceq $routineNames[$i] -and
            $row[1] -match '(^|,)(STRICT_TRANS_TABLES|STRICT_ALL_TABLES)(,|$)' -and
            $row[1] -notmatch '(^|,)(NO_BACKSLASH_ESCAPES|ANSI_QUOTES|PIPES_AS_CONCAT)(,|$)') 'Unexpected routine name/creation SQL mode.'
        $statements = (Split-Sql ('DELIMITER $$' + "`n" + $row[2] + '$$' + "`n")).Statements
        Assert-Condition ($statements.Count -eq 1) 'Invalid original routine definition.'
        $body = Get-RoutineBody $statements[0]
        $originalBodies.Add($row[0], (Normalize-Sql $body))
    }
    $actualRoutines = [System.Collections.Generic.List[object]]::new()
    foreach ($row in $Snapshot.Sets[5].Rows) {
        Assert-Condition ($null -ne $row[3]) 'Routine definition is unavailable.'
        Assert-Condition ($originalBodies.ContainsKey($row[0])) 'Original routine definition is unavailable.'
        $actualRoutines.Add(@($row[0],$row[1],$row[2],$originalBodies[$row[0]]))
    }
    Assert-Rows $actualRoutines $routines 'Routine definitions'
    Assert-Rows $Snapshot.Sets[6].Rows $parameters 'Routine parameters'
}

function Start-Audit([int]$Version, [string]$Name, [string]$Action, [string]$UpHash, $DownHash) {
    Assert-Lock
    $insertSql = 'INSERT INTO schema_migrations (version, name, direction, up_checksum, down_checksum, state, started_at) VALUES (?, ?, ?, ?, ?, ?, UTC_TIMESTAMP(6))'
    Assert-Condition ((Invoke-Write $insertSql @($Version,$Name,$Action,$UpHash,$DownHash,'RUNNING')) -eq 1) 'Cannot start audit.'
    return [uint64](Read-Scalar 'SELECT LAST_INSERT_ID()')
}

function Complete-Audit([uint64]$Id, [string]$State) {
    Assert-Lock
    Assert-Condition ((Invoke-Write "UPDATE schema_migrations SET state = ?, finished_at = UTC_TIMESTAMP(6) WHERE execution_id = ? AND state = 'RUNNING'" @($State,$Id)) -eq 1) 'Cannot complete audit.'
}

# RUNNING is committed before the first DDL. A crash/unknown completion remains
# visible; an inverse migration is never used to guess partial-failure recovery.
function Invoke-Migration([int]$Version, [string]$Name, [string]$Action, $Up, $Down, [int]$TargetHead, [switch]$Bootstrap) {
    $id = [uint64]0
    $tableLocks = $false
    try {
        if ($Action -eq 'DOWN' -and $Version -eq 1) {
            [void](Invoke-Write 'LOCK TABLES accounts WRITE, account_identities WRITE, schema_migrations WRITE')
            $tableLocks = $true
            Assert-Condition ((Read-Scalar 'SELECT COUNT(*) FROM accounts') -ceq '0' -and
                (Read-Scalar 'SELECT COUNT(*) FROM account_identities') -ceq '0') 'Down 000001 refused: account data exists.'
        }
        $script:stage = "$Action version $Version"
        $file = if ($Action -eq 'UP') { $Up } else { $Down }
        $start = 0
        if ($Bootstrap) {
            # No guessed baseline: only an empty schema can create this table.
            [void](Invoke-Write $file.Statements[0]); $start = 1
        }
        $downHash = if ($null -eq $Down) { $null } else { $Down.Hash }
        $id = Start-Audit $Version $Name $Action $Up.Hash $downHash
        for ($i = $start; $i -lt $file.Statements.Count; ++$i) {
            $script:stage = "$Action version $Version statement $($i + 1)"
            Assert-Lock
            [void](Invoke-Write $file.Statements[$i])
        }
        if ($tableLocks) { [void](Invoke-Write 'UNLOCK TABLES'); $tableLocks = $false }
        Assert-Structure (Get-Snapshot) $TargetHead
        Complete-Audit $id 'SUCCEEDED'
        Write-Host "$Action V$('{0:D6}' -f $Version) succeeded."
    } catch {
        if ($tableLocks) { try { [void](Invoke-Write 'UNLOCK TABLES') } catch {}; $tableLocks = $false }
        if ($id -gt 0) { try { Complete-Audit $id 'FAILED' } catch {} }
        throw
    }
}

try {
    Assert-Condition $ServicesStopped.IsPresent 'Stop all DB-using services first, then supply -ServicesStopped.'
    Assert-Condition ([Environment]::Is64BitProcess) 'Use 64-bit Windows PowerShell and a matching 64-bit MySQL ODBC driver.'
    $root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../ActionRPGServer/Database/Migrations/MySQL'))
    $script:bootstrap = Read-SqlFile (Join-Path $root 'Infrastructure/V000000__migration_history.sql')
    Assert-Condition ($script:bootstrap.Statements.Count -eq 2) 'Invalid bootstrap file.'
    $script:catalog = @{}
    foreach ($item in Get-ChildItem -LiteralPath $root -Filter '*.sql' -File) {
        Assert-Condition ($item.Name -cmatch '^V([0-9]{6})__([a-z][a-z0-9_]*)\.sql$') 'Invalid migration filename.'
        $version = [int]$Matches[1]; $name = $Matches[2]
        Assert-Condition ($version -gt 0 -and -not $script:catalog.ContainsKey($version)) 'Duplicate/reserved migration version.'
        $script:catalog.Add($version, [pscustomobject]@{ Name = $name; Up = (Read-SqlFile $item.FullName); Down = (Read-SqlFile (Join-Path (Join-Path $root 'Down') $item.Name)) })
    }
    Assert-Condition ($script:catalog.Count -eq 3) 'Current schema inspection supports versions 000001..000003; extend the contract with new migrations.'
    for ($i = 1; $i -le $script:catalog.Count; ++$i) { Assert-Condition ($script:catalog.ContainsKey($i)) 'Migration version gap.' }
    Assert-Condition ((@(Get-ChildItem -LiteralPath (Join-Path $root 'Down') -Filter '*.sql' -File)).Count -eq $script:catalog.Count) 'Unexpected Down files.'
    Assert-Condition ((Normalize-Sql $script:catalog[1].Down.Statements[0]) -ceq 'drop table account_identities , accounts' -and
        $script:catalog[1].Down.Statements.Count -eq 1) 'Unsafe Down 000001 contract.'
    $secret = [Environment]::GetEnvironmentVariable('ACTIONRPG_MIGRATION_CONNECTION_STRING')
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($secret)) 'Set ACTIONRPG_MIGRATION_CONNECTION_STRING using your protected local environment.'
    $script:stage = 'ODBC connection/target verification'
    $script:connection = [System.Data.Odbc.OdbcConnection]::new($secret)
    $secret = $null
    $handler = [System.Data.Odbc.OdbcInfoMessageEventHandler]{ param($sender, $eventArgs) $script:warningSeen = $true }
    $script:connection.add_InfoMessage($handler)
    $script:connection.Open()
    Assert-Condition (-not $script:warningSeen) 'ODBC connection warning.'
    $target = (Read-Sets 'SELECT DATABASE(), @@version, @@version_comment, @@hostname, @@port, @@session.sql_mode, @@session.autocommit').Sets[0].Rows[0]
    Assert-Condition ($target[0] -ceq $Database -and $target[1] -match '^8\.0\.47(?:$|[-+])' -and
        $target[2] -notmatch 'MariaDB' -and $target[5] -match '(^|,)(STRICT_TRANS_TABLES|STRICT_ALL_TABLES)(,|$)' -and
        $target[5] -notmatch '(^|,)(NO_BACKSLASH_ESCAPES|ANSI_QUOTES|PIPES_AS_CONCAT)(,|$)' -and $target[6] -ceq '1') 'Wrong schema/MySQL version/SQL mode/autocommit.'
    Write-Host "Target: $($target[3]):$($target[4]) / $Database / MySQL $($target[1])"
    $script:lockName = Read-Scalar "SELECT CONCAT('actionrpg:migrate:', LEFT(SHA2(DATABASE(), 256), 40))"
    Assert-Condition ((Read-Scalar 'SELECT GET_LOCK(?, 0)' @($script:lockName)) -ceq '1') 'Another migrator/schema inspector owns this DB lock.'
    $script:ownsLock = $true
    $script:stage = 'history/actual schema verification'
    $hasHistory = Read-Scalar "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'schema_migrations'"
    if ($hasHistory -ceq '0') {
        Assert-Condition ($Direction -eq 'Up') 'Unmanaged schema: Down requires a verified migration history.'
        $objects = Read-Scalar 'SELECT (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()) + (SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE()) + (SELECT COUNT(*) FROM information_schema.EVENTS WHERE EVENT_SCHEMA = DATABASE()) + (SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA = DATABASE())'
        Assert-Condition ($objects -ceq '0') 'Refusing to baseline a nonempty/unmanaged schema.'
        Write-Host 'Plan: bootstrap V000000, then Up V000001 -> V000003.'
        Invoke-Migration 0 'migration_history' 'UP' $script:bootstrap $null 0 -Bootstrap
    }
    $snapshot = Get-Snapshot
    $head = Get-Head $snapshot
    Assert-Structure $snapshot $head
    if ($Direction -eq 'Up') {
        Write-Host "Plan: active V$('{0:D6}' -f $head) -> V000003; Up each pending version in order."
        for ($version = $head + 1; $version -le $script:catalog.Count; ++$version) {
            $file = $script:catalog[$version]
            Invoke-Migration $version $file.Name 'UP' $file.Up $file.Down $version
        }
    } elseif ($head -gt 0) {
        Write-Host "Plan: Down V$('{0:D6}' -f $head) only -> V$('{0:D6}' -f ($head - 1))."
        $file = $script:catalog[$head]
        Invoke-Migration $head $file.Name 'DOWN' $file.Up $file.Down ($head - 1)
    } else { Write-Host 'No active application migration to roll back; V000000 audit infrastructure remains.' }
    $script:stage = 'final verification'
    $final = Get-Snapshot
    $finalHead = Get-Head $final
    Assert-Structure $final $finalHead
    Write-Host "Complete. Active version: V$('{0:D6}' -f $finalHead)."
} catch {
    # Any driver/framework exception may include connection details or SQL.
    # Only explicitly tagged, credential-free precondition messages are shown.
    $exception = $_.Exception
    $detail = 'File/ODBC/inspection failed; inspect the target using protected diagnostics.'
    while ($null -ne $exception) {
        if ($exception.Data.Contains('SafeMigrationMessage')) { $detail = [string]$exception.Data['SafeMigrationMessage']; break }
        $exception = $exception.InnerException
    }
    [Console]::Error.WriteLine("Migration stopped at $script:stage. $detail")
    [Console]::Error.WriteLine('Do not replay partial DDL or edit audit rows. Inspect actual schema and RUNNING/FAILED audit before an approved recovery.')
    exit 1
} finally {
    if ($null -ne $script:connection) {
        if ($script:ownsLock -and $script:connection.State -eq [System.Data.ConnectionState]::Open) {
            try { [void](Read-Scalar 'SELECT RELEASE_LOCK(?)' @($script:lockName)) } catch {}
        }
        $script:connection.Dispose()
    }
}
