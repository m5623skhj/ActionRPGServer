# Windows PowerShell 5.1; one ODBC connection owns the DB lock, creation and migrations.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Up', 'Down')][string]$Direction,
    [ValidateLength(1, 64)][string]$Database,
    [switch]$ServicesStopped,
    [switch]$CreateDatabase,
    [switch]$InspectOnly,
    [ValidateRange(0, 5)][int]$InspectVersion,
    [switch]$RecoverBootstrap,
    [switch]$RecoverAccounts
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

function Read-MigrationValue([string]$Label, [string]$Default) {
    $value = ([string](Read-Host "$Label [$Default]")).Trim()
    if ($value.Length -eq 0) { return $Default }
    return $value
}

function Get-MigrationConnectionString {
    $configured = [Environment]::GetEnvironmentVariable('ACTIONRPG_MIGRATION_CONNECTION_STRING')
    if (-not [string]::IsNullOrWhiteSpace($configured)) { return $configured }

    $drivers = @(Get-OdbcDriver -Platform '64-bit' | Where-Object { $_.Name -match 'MySQL.*Unicode' })
    Assert-Condition ($drivers.Count -gt 0) 'Install a 64-bit MySQL Unicode ODBC driver.'
    $driverNumber = 1
    if ($drivers.Count -gt 1) {
        for ($index = 0; $index -lt $drivers.Count; ++$index) {
            Write-Host ('[{0}] {1}' -f ($index + 1), $drivers[$index].Name)
        }
        $choice = Read-MigrationValue 'ODBC driver number' '1'
        Assert-Condition ([int]::TryParse($choice, [ref]$driverNumber) -and
            $driverNumber -ge 1 -and $driverNumber -le $drivers.Count) 'Choose a listed ODBC driver number.'
    }

    $server = Read-MigrationValue 'MySQL host' '127.0.0.1'
    $portText = Read-MigrationValue 'MySQL port' '3306'
    $port = 0
    Assert-Condition ([int]::TryParse($portText, [ref]$port) -and $port -ge 1 -and $port -le 65535) 'Invalid MySQL port.'
    $user = Read-MigrationValue 'Migration account' 'actionrpg_migrator'
    Write-Host "Connection: $server`:$port / $Database / $user (TLS required)"

    $builder = [System.Data.Odbc.OdbcConnectionStringBuilder]::new()
    $password = $null
    $passwordPointer = [IntPtr]::Zero
    try {
        $builder.set_Driver([string]$drivers[$driverNumber - 1].Name)
        $builder['SERVER'] = $server
        $builder['PORT'] = [string]$port
        $builder['DATABASE'] = $Database
        $builder['UID'] = $user
        $builder['SSLMODE'] = 'REQUIRED'
        $password = Read-Host 'Migration account password' -AsSecureString
        Assert-Condition ($null -ne $password -and $password.Length -gt 0) 'A migration account password is required.'
        $passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($password)
        $builder['PWD'] = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordPointer)
        return $builder.ConnectionString
    } finally {
        if ($passwordPointer -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordPointer) }
        if ($null -ne $password) { $password.Dispose() }
        $builder.Clear()
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

# Validate the server/session before optional DDL and again after selecting the DB.
function Get-Target {
    $data = Read-Sets 'SELECT DATABASE(), @@version, @@version_comment, @@hostname, @@port, @@session.sql_mode, @@session.autocommit'
    Assert-Condition ($data.Sets.Count -eq 1 -and $data.Sets[0].Columns -eq 7 -and
        $data.Sets[0].Rows.Count -eq 1) 'Invalid target inspection result.'
    $target = $data.Sets[0].Rows[0]
    Assert-Condition ($target[1] -match '^8\.0\.46(?:$|[-+])' -and $target[2] -notmatch 'MariaDB' -and
        $target[5] -match '(^|,)(STRICT_TRANS_TABLES|STRICT_ALL_TABLES)(,|$)' -and
        $target[5] -notmatch '(^|,)(NO_BACKSLASH_ESCAPES|ANSI_QUOTES|PIPES_AS_CONCAT)(,|$)' -and
        $target[6] -ceq '1') 'Wrong MySQL version/SQL mode/autocommit.'
    return [pscustomobject]@{ Database = $target[0]; Version = $target[1]; Host = $target[3]; Port = $target[4] }
}

# The caller holds the normal schema lock before checking/creating the DB. USE
# stays on that connection so CREATE DATABASE's implicit commit cannot drop it.
function Initialize-Database {
    Assert-Lock
    $exists = Read-Scalar 'SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE CAST(SCHEMA_NAME AS BINARY) = CAST(? AS BINARY)' @($Database)
    Assert-Condition ($exists -ceq '0' -or $exists -ceq '1') 'Invalid database existence result.'
    # -CreateDatabase only accepts lowercase ASCII identifiers, checked before Open.
    $identifier = [string][char]96 + $Database + [string][char]96
    if ($exists -ceq '0') {
        $script:stage = 'database creation'
        Write-Host "Plan: create database $Database (utf8mb4 / utf8mb4_0900_ai_ci), then apply migrations."
        Assert-Lock
        [void](Invoke-Write ('CREATE DATABASE ' + $identifier + ' CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci'))
        Write-Host "Database $Database created; later failures do not remove it."
    } else { Write-Host "Database $Database exists; inspecting its migration history and structure next." }
    $script:stage = 'database selection/target verification'
    Assert-Lock
    [void](Invoke-Write ('USE ' + $identifier))
    Assert-Lock
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

# Decode only the observed CHECK metadata form: a charset followed by an ASCII
# literal with escaped delimiter quotes. Preserve quoted contents and reject
# other unquoted backslashes; never apply this to deployment SQL/routine bodies.
function Convert-CheckMetadata([string]$Sql) {
    $pattern = @'
'(?:''|\\.|[^'\\])*'|`(?:``|[^`])*`|(?<charset>\b_(?:ascii|utf8mb4|utf8mb3|utf8|latin1|binary))\s*\\'(?<literal>[A-Za-z0-9_]+)\\'|(?<invalid>\\)|[\s\S]
'@
    return [regex]::Replace($Sql, $pattern, [System.Text.RegularExpressions.MatchEvaluator]{
        param($match)
        Assert-Condition (-not $match.Groups['invalid'].Success) 'Unsupported CHECK metadata escape.'
        if ($match.Groups['charset'].Success) {
            return $match.Groups['charset'].Value + "'" + $match.Groups['literal'].Value + "'"
        }
        return $match.Value
    }, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
}

# INFORMATION_SCHEMA adds parentheses around predicates. Parse only the
# AND/OR grouping of these known CHECKs, retaining every atomic SQL token.
function Normalize-Check([string]$Sql, [switch]$Metadata) {
    if ($Metadata) { $Sql = Convert-CheckMetadata $Sql }
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
    # MySQL renders OCTET_LENGTH as its byte-length synonym LENGTH in CHECKs.
    # Skip string literals and normalize only function-call tokens in this comparison.
    $normalized = Normalize-Sql $Sql
    $normalized = [regex]::Replace($normalized, "'(?:''|\\.|[^'\\])*'|(?<function>\boctet_length)(?=\s*\()", [System.Text.RegularExpressions.MatchEvaluator]{
        param($match)
        if ($match.Groups['function'].Success) { return 'length' }
        return $match.Value
    })
    return Convert-Boolean $normalized
}

function Get-Snapshot {
    Assert-Lock
    $inventory = (Read-Scalar "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE() AND ROUTINE_TYPE = 'PROCEDURE' AND ROUTINE_NAME = 'get_inventory_schema_migration_history'") -ceq '1'
    $expanded = $inventory -or (Read-Scalar "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE() AND ROUTINE_TYPE = 'PROCEDURE' AND ROUTINE_NAME = 'get_character_schema_migration_history'") -ceq '1'
    $snapshot = if ($inventory) { Read-Sets 'CALL get_inventory_schema_migration_history()' }
        elseif ($expanded) { Read-Sets 'CALL get_character_schema_migration_history()' }
        else { Read-Sets 'CALL get_schema_migration_history()' }
    $shapes = @(5, 9, 9, 11, 6, 4, 7, 6, 6, 6)
    if ($expanded) { $shapes += 6 }
    if ($inventory) { $shapes += @(6, 6, 6, 6, 6, 6, 6) }
    Assert-Condition ($snapshot.Sets.Count -eq $shapes.Count) 'Invalid history result set count.'
    for ($i = 0; $i -lt $shapes.Count; ++$i) {
        Assert-Condition ($snapshot.Sets[$i].Columns -eq $shapes[$i]) 'Invalid history result shape.'
    }
    Assert-Condition ($snapshot.Sets[0].Rows.Count -eq 1) 'Invalid history header.'
    $header = $snapshot.Sets[0].Rows[0]
    $format = if ($inventory) { '3' } elseif ($expanded) { '2' } else { '1' }
    Assert-Condition ($header[0] -ceq $format -and $header[1] -ceq 'sha256-utf8-lf-v1' -and
        $header[2] -ceq $Database -and $header[3] -match '^8\.0\.46(?:$|[-+])' -and $header[4] -notmatch 'MariaDB') 'Unsupported target/inspection format.'
    return $snapshot
}

function Get-Head($Snapshot) {
    $rows = $Snapshot.Sets[1].Rows
    Assert-Condition ($rows.Count -gt 0) 'Missing bootstrap audit.'
    [uint64]$previousId = 0
    $head = 0
    $bootstrapFailureFinished = $null
    $accountsFailureFinished = $null
    $hasApplicationHistory = $false
    for ($i = 0; $i -lt $rows.Count; ++$i) {
        $row = $rows[$i]
        Assert-Condition ($row[0] -cmatch '^[1-9][0-9]*$' -and $row[1] -cmatch '^(0|[1-9][0-9]*)$') 'Invalid audit identifier/version.'
        [uint64]$id = $row[0]
        [int]$version = $row[1]
        Assert-Condition ($id -gt $previousId -and
            $row[7] -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z$' -and
            $row[8] -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z$' -and
            [string]::CompareOrdinal($row[8], $row[7]) -ge 0) 'Failed, unfinished, or invalid audit; inspect before repair.'
        $previousId = $id
        if ($null -ne $bootstrapFailureFinished) {
            Assert-Condition ($i -eq 1 -and $version -eq 0 -and $row[2] -ceq 'migration_history_recovery' -and
                $row[3] -ceq 'UP' -and $row[4] -ceq $script:bootstrap.Hash -and $null -eq $row[5] -and
                $row[6] -ceq 'SUCCEEDED' -and [string]::CompareOrdinal($row[7], $bootstrapFailureFinished) -ge 0) 'Invalid/missing bootstrap recovery confirmation.'
            $bootstrapFailureFinished = $null
            continue
        }
        if ($null -ne $accountsFailureFinished) {
            $file = $script:catalog[1]
            Assert-Condition ($version -eq 1 -and $row[2] -ceq 'create_login_accounts_recovery' -and
                $row[3] -ceq 'UP' -and $row[4] -ceq $file.Up.Hash -and $row[5] -ceq $file.Down.Hash -and
                $row[6] -ceq 'SUCCEEDED' -and [string]::CompareOrdinal($row[7], $accountsFailureFinished) -ge 0) 'Invalid/missing account-table recovery confirmation.'
            $accountsFailureFinished = $null
            $hasApplicationHistory = $true
            $head = 1
            continue
        }
        if ($i -eq 0) {
            Assert-Condition ($version -eq 0 -and $row[2] -ceq 'migration_history' -and $row[3] -ceq 'UP' -and
                $row[4] -ceq $script:bootstrap.Hash -and $null -eq $row[5]) 'Bootstrap checksum/history mismatch.'
            if ($row[6] -ceq 'FAILED') {
                Assert-Condition ($rows.Count -ge 2) 'Failed bootstrap requires explicit recovery; no confirmation exists.'
                $bootstrapFailureFinished = $row[8]
            } else { Assert-Condition ($row[6] -ceq 'SUCCEEDED') 'Unfinished/invalid bootstrap audit.' }
            continue
        }
        Assert-Condition ($version -gt 0 -and $script:catalog.ContainsKey($version)) 'Unknown/missing migration version.'
        $file = $script:catalog[$version]
        Assert-Condition ($row[2] -ceq $file.Name -and $row[4] -ceq $file.Up.Hash -and $row[5] -ceq $file.Down.Hash) 'Migration name/checksum mismatch.'
        if (-not $hasApplicationHistory -and $head -eq 0 -and $version -eq 1 -and
            $row[3] -ceq 'UP' -and $row[6] -ceq 'FAILED') {
            Assert-Condition ($i + 1 -lt $rows.Count) 'Failed account-table migration requires explicit recovery; no confirmation exists.'
            $accountsFailureFinished = $row[8]
            continue
        }
        Assert-Condition ($row[6] -ceq 'SUCCEEDED') 'Failed/unfinished migration; inspect before repair.'
        $hasApplicationHistory = $true
        if ($row[3] -ceq 'UP') {
            Assert-Condition ($version -eq $head + 1) 'Non-contiguous Up history.'
            $head = $version
        } elseif ($row[3] -ceq 'DOWN') {
            Assert-Condition ($head -gt 0 -and $version -eq $head) 'Invalid Down history.'
            --$head
        } else { Assert-Condition $false 'Unknown audit direction.' }
    }
    Assert-Condition ($null -eq $bootstrapFailureFinished) 'Missing bootstrap recovery confirmation.'
    Assert-Condition ($null -eq $accountsFailureFinished) 'Missing account-table recovery confirmation.'
    return $head
}

function Assert-Rows($Actual, $Expected, [string]$Label) {
    $actualKeys = @($Actual | ForEach-Object { ConvertTo-Json -InputObject $_ -Compress } | Sort-Object)
    $expectedKeys = @($Expected | ForEach-Object { ConvertTo-Json -InputObject $_ -Compress } | Sort-Object)
    if ($InspectOnly -and $Label -ceq 'Constraints') {
        # Only schema metadata is printed; never emit driver exceptions or connection values.
        for ($i = 0; $i -lt $actualKeys.Count; ++$i) { Write-Host "Constraints actual[$i]: $($actualKeys[$i])" }
        for ($i = 0; $i -lt $expectedKeys.Count; ++$i) { Write-Host "Constraints expected[$i]: $($expectedKeys[$i])" }
    }
    Assert-Condition ($actualKeys.Count -eq $expectedKeys.Count) "$Label count mismatch."
    for ($i = 0; $i -lt $actualKeys.Count; ++$i) {
        Assert-Condition ($actualKeys[$i] -ceq $expectedKeys[$i]) "$Label mismatch at sorted row $i."
    }
}

function Assert-Structure($Snapshot, [int]$Head) {
    Assert-Condition ($Head -ge 0 -and $Head -le 5) 'Update the schema inspection contract before adding schema versions.'
    $resultCount = if ($Head -ge 5) { 18 } elseif ($Head -ge 4) { 11 } else { 10 }
    $format = if ($Head -ge 5) { '3' } elseif ($Head -ge 4) { '2' } else { '1' }
    Assert-Condition ($Snapshot.Sets.Count -eq $resultCount -and $Snapshot.Sets[0].Rows[0][0] -ceq $format) 'Inspector/version mismatch.'
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
    if ($Head -ge 4) {
        foreach ($d in @(
            @('character_id','bigint unsigned',$null,'auto_increment',$null),
            @('account_id','bigint unsigned',$null,'',$null),
            @('character_definition_id','int unsigned',$null,'',$null),
            @('level','int unsigned',$null,'',$null),
            @('name','varchar(32)','utf8mb4_0900_bin','','utf8mb4'),
            @('skill_points','int unsigned',$null,'',$null),
            @('created_at','datetime(6)',$null,'',$null),
            @('updated_at','datetime(6)',$null,'',$null))) {
            $columns.Add(@('characters',$d[0],$d[1],'NO',$d[2],$d[3],'InnoDB',$null,$d[4]))
        }
        foreach ($d in @(@('character_id','bigint unsigned'),@('skill_id','varbinary(64)'),@('skill_level','int unsigned'))) {
            $columns.Add(@('character_skills',$d[0],$d[1],'NO',$null,'','InnoDB',$null,$null))
        }
        $constraints.Add(@('characters','PRIMARY','PRIMARY KEY','character_id',$null,$null,$null,$null,$null,$null,$null))
        $constraints.Add(@('characters','uk_characters_name','UNIQUE','name',$null,$null,$null,$null,$null,$null,$null))
        $constraints.Add(@('characters','fk_characters_account','FOREIGN KEY','account_id','accounts','account_id',$null,$null,$Database,'RESTRICT','RESTRICT'))
        foreach ($d in @(@('ck_characters_definition','character_definition_id > 0'),
            @('ck_characters_level','level > 0 AND level < 1000001'),
            @('ck_characters_name','OCTET_LENGTH(name) > 0 AND OCTET_LENGTH(name) < 33'))) {
            $constraints.Add(@('characters',$d[0],'CHECK',$null,$null,$null,(Normalize-Check $d[1]),'YES',$null,$null,$null))
        }
        foreach ($column in @('character_id','skill_id')) { $constraints.Add(@('character_skills','PRIMARY','PRIMARY KEY',$column,$null,$null,$null,$null,$null,$null,$null)) }
        $constraints.Add(@('character_skills','fk_character_skills_character','FOREIGN KEY','character_id','characters','character_id',$null,$null,$Database,'RESTRICT','RESTRICT'))
        foreach ($d in @(@('ck_character_skills_id','OCTET_LENGTH(skill_id) > 0'),
            @('ck_character_skills_level','skill_level > 0 AND skill_level < 1000001'))) {
            $constraints.Add(@('character_skills',$d[0],'CHECK',$null,$null,$null,(Normalize-Check $d[1]),'YES',$null,$null,$null))
        }
        $indexes.Add(@('characters','PRIMARY','0','1','character_id',$null))
        $indexes.Add(@('characters','uk_characters_name','0','1','name',$null))
        $indexes.Add(@('characters','ix_characters_account_id','1','1','account_id',$null))
        $indexes.Add(@('character_skills','PRIMARY','0','1','character_id',$null))
        $indexes.Add(@('character_skills','PRIMARY','0','2','skill_id',$null))
    }
    if ($Head -ge 5) {
        $columns.Add(@('character_state','character_id','bigint unsigned','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_state','revision','bigint unsigned','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_state','owner_generation','bigint unsigned','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_state','owner_token','binary(32)','YES',$null,'','InnoDB',$null,$null))
        $constraints.Add(@('character_state','PRIMARY','PRIMARY KEY','character_id',$null,$null,$null,$null,$null,$null,$null))
        $constraints.Add(@('character_state','fk_character_state_character','FOREIGN KEY','character_id','characters','character_id',$null,$null,$Database,'RESTRICT','RESTRICT'))
        $indexes.Add(@('character_state','PRIMARY','0','1','character_id',$null))
        $columns.Add(@('character_items','instance_id','binary(16)','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_items','character_id','bigint unsigned','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_items','definition_id','varbinary(64)','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_items','quantity','int unsigned','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_items','container','int unsigned','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_items','slot','int unsigned','NO',$null,'','InnoDB',$null,$null))
        $constraints.Add(@('character_items','PRIMARY','PRIMARY KEY','instance_id',$null,$null,$null,$null,$null,$null,$null))
        $constraints.Add(@('character_items','uk_character_items_position','UNIQUE','character_id',$null,$null,$null,$null,$null,$null,$null))
        $constraints.Add(@('character_items','uk_character_items_position','UNIQUE','container',$null,$null,$null,$null,$null,$null,$null))
        $constraints.Add(@('character_items','uk_character_items_position','UNIQUE','slot',$null,$null,$null,$null,$null,$null,$null))
        $constraints.Add(@('character_items','fk_character_items_character','FOREIGN KEY','character_id','characters','character_id',$null,$null,$Database,'RESTRICT','RESTRICT'))
        $constraints.Add(@('character_items','ck_character_items_definition','CHECK',$null,$null,$null,(Normalize-Check 'OCTET_LENGTH(definition_id) > 0'),'YES',$null,$null,$null))
        $constraints.Add(@('character_items','ck_character_items_quantity','CHECK',$null,$null,$null,(Normalize-Check 'quantity > 0'),'YES',$null,$null,$null))
        $constraints.Add(@('character_items','ck_character_items_container','CHECK',$null,$null,$null,(Normalize-Check 'container < 5'),'YES',$null,$null,$null))
        $constraints.Add(@('character_items','ck_character_items_equipment','CHECK',$null,$null,$null,(Normalize-Check 'container = 1 OR container = 2 OR container = 3 OR quantity = 1'),'YES',$null,$null,$null))
        $constraints.Add(@('character_items','ck_character_items_slot','CHECK',$null,$null,$null,(Normalize-Check '(container < 4 AND slot < 40) OR (container = 4 AND slot < 7)'),'YES',$null,$null,$null))
        $indexes.Add(@('character_items','PRIMARY','0','1','instance_id',$null))
        $indexes.Add(@('character_items','uk_character_items_position','0','1','character_id',$null))
        $indexes.Add(@('character_items','uk_character_items_position','0','2','container',$null))
        $indexes.Add(@('character_items','uk_character_items_position','0','3','slot',$null))
        $columns.Add(@('character_operations','account_id','bigint unsigned','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_operations','request_id','binary(32)','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_operations','character_id','bigint unsigned','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_operations','request_kind','varbinary(16)','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_operations','payload_hash','binary(32)','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_operations','revision','bigint unsigned','NO',$null,'','InnoDB',$null,$null))
        $columns.Add(@('character_operations','created_at','datetime(6)','NO',$null,'','InnoDB',$null,$null))
        $constraints.Add(@('character_operations','PRIMARY','PRIMARY KEY','account_id',$null,$null,$null,$null,$null,$null,$null))
        $constraints.Add(@('character_operations','PRIMARY','PRIMARY KEY','request_id',$null,$null,$null,$null,$null,$null,$null))
        $constraints.Add(@('character_operations','fk_character_operations_account','FOREIGN KEY','account_id','accounts','account_id',$null,$null,$Database,'RESTRICT','RESTRICT'))
        $constraints.Add(@('character_operations','fk_character_operations_character','FOREIGN KEY','character_id','characters','character_id',$null,$null,$Database,'RESTRICT','RESTRICT'))
        $indexes.Add(@('character_operations','PRIMARY','0','1','account_id',$null))
        $indexes.Add(@('character_operations','PRIMARY','0','2','request_id',$null))
        $indexes.Add(@('character_operations','ix_character_operations_character','1','1','character_id',$null))
    }
    $actualConstraints = [System.Collections.Generic.List[object]]::new()
    foreach ($row in $Snapshot.Sets[3].Rows) {
        if ($InspectOnly) { Write-Host ('Constraints raw: ' + (ConvertTo-Json -InputObject $row -Compress)) }
        $copy = $row.Clone()
        if ($null -ne $copy[6]) { $copy[6] = Normalize-Check $copy[6] -Metadata }
        $actualConstraints.Add($copy)
    }
    $orderedColumns = [System.Collections.Generic.List[object]]::new()
    foreach ($table in @('account_identities','accounts','character_items','character_operations','character_skills','character_state','characters','schema_migrations')) {
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
    if ($Head -ge 4) { $files += $script:catalog[4].Up }
    if ($Head -ge 5) {
        $files += $script:catalog[5].Up
        $parameters.Add(@('emit_character_state','1','IN','inResultCode','int',$null,$null))
        $parameters.Add(@('emit_character_state','2','IN','inCharacterId','bigint unsigned',$null,$null))
        $parameters.Add(@('emit_character_state','3','IN','inIncludeInventory','tinyint(1)',$null,$null))
        $parameters.Add(@('list_characters','1','IN','inAccountId','bigint unsigned',$null,$null))
        $parameters.Add(@('create_character','1','IN','inAccountId','bigint unsigned',$null,$null))
        $parameters.Add(@('create_character','2','IN','inRequestId','varchar(64)','ascii','ascii_bin'))
        $parameters.Add(@('create_character','3','IN','inName','text','utf8mb4','utf8mb4_0900_bin'))
        $parameters.Add(@('create_character','4','IN','inDefinitionId','int unsigned',$null,$null))
        $parameters.Add(@('create_character','5','IN','inInitialLevel','int unsigned',$null,$null))
        $parameters.Add(@('create_character','6','IN','inInitialSp','int unsigned',$null,$null))
        $parameters.Add(@('claim_character','1','IN','inAccountId','bigint unsigned',$null,$null))
        $parameters.Add(@('claim_character','2','IN','inCharacterId','bigint unsigned',$null,$null))
        $parameters.Add(@('claim_character','3','IN','inOwnerToken','varchar(64)','ascii','ascii_bin'))
        $parameters.Add(@('claim_character','4','IN','inExpectedOwnerGeneration','bigint unsigned',$null,$null))
        $parameters.Add(@('save_character_state','1','IN','inAccountId','bigint unsigned',$null,$null))
        $parameters.Add(@('save_character_state','2','IN','inCharacterId','bigint unsigned',$null,$null))
        $parameters.Add(@('save_character_state','3','IN','inOwnerToken','varchar(64)','ascii','ascii_bin'))
        $parameters.Add(@('save_character_state','4','IN','inOwnerGeneration','bigint unsigned',$null,$null))
        $parameters.Add(@('save_character_state','5','IN','inRequestId','varchar(64)','ascii','ascii_bin'))
        $parameters.Add(@('save_character_state','6','IN','inExpectedRevision','bigint unsigned',$null,$null))
        $parameters.Add(@('save_character_state','7','IN','inOperationJson','text','utf8mb4','utf8mb4_bin'))
        $parameters.Add(@('save_character_state','8','IN','inProgressionJson','text','utf8mb4','utf8mb4_bin'))
        $parameters.Add(@('save_character_state','9','IN','inInventoryJson','text','utf8mb4','utf8mb4_bin'))
        $parameters.Add(@('release_character','1','IN','inAccountId','bigint unsigned',$null,$null))
        $parameters.Add(@('release_character','2','IN','inCharacterId','bigint unsigned',$null,$null))
        $parameters.Add(@('release_character','3','IN','inOwnerToken','varchar(64)','ascii','ascii_bin'))
        $parameters.Add(@('release_character','4','IN','inOwnerGeneration','bigint unsigned',$null,$null))
    }
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
    if ($Head -ge 4) { $routineNames += 'get_character_schema_migration_history' }
    if ($Head -ge 5) { $routineNames += @('emit_character_state','list_characters','create_character','claim_character','save_character_state','release_character','get_inventory_schema_migration_history') }
    for ($i = 0; $i -lt $routineNames.Count; ++$i) {
        $set = $Snapshot.Sets[7 + $i]
        Assert-Condition ($set.Rows.Count -eq 1) 'Invalid SHOW CREATE row count.'
        $row = $set.Rows[0]
        $present = $i -eq 0 -or ($i -eq 1 -and $Head -ge 2) -or ($i -eq 2 -and $Head -ge 3) -or ($i -eq 3 -and $Head -ge 4) -or ($i -ge 4 -and $Head -ge 5)
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

# A recovery confirmation is valid only for the sole, completed V0 failure.
# Recheck the actual deployment checksum and original audit before any write.
function Assert-RecoverableBootstrap($Snapshot) {
    Assert-Condition ($Snapshot.Sets[1].Rows.Count -eq 1) 'Bootstrap recovery requires exactly one failed initial audit; inspect existing confirmations/other attempts.'
    $row = $Snapshot.Sets[1].Rows[0]
    Assert-Condition ($row[0] -cmatch '^[1-9][0-9]*$' -and $row[1] -ceq '0' -and
        $row[2] -ceq 'migration_history' -and $row[3] -ceq 'UP' -and $row[4] -ceq $script:bootstrap.Hash -and
        $null -eq $row[5] -and $row[6] -ceq 'FAILED') 'Bootstrap recovery audit/checksum mismatch.'
    [void][uint64]::Parse($row[0], [System.Globalization.CultureInfo]::InvariantCulture)
    Assert-RecoveryAuditTime $row
}

function Assert-RecoveryAuditTime($Row) {
    foreach ($index in @(7, 8)) {
        Assert-Condition ($Row[$index] -cmatch '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}Z$') 'Invalid recovery audit time.'
        [void][datetime]::ParseExact($Row[$index], "yyyy-MM-dd'T'HH:mm:ss.ffffff'Z'", [System.Globalization.CultureInfo]::InvariantCulture)
    }
    Assert-Condition ([string]::CompareOrdinal($Row[8], $Row[7]) -ge 0) 'Invalid recovery audit time order.'
}

function Assert-RecoveryObjects([ValidateRange(0, 1)][int]$Version) {
    Assert-Lock
    $counts = Read-Sets 'SELECT (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()), (SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE()), (SELECT COUNT(*) FROM information_schema.EVENTS WHERE EVENT_SCHEMA = DATABASE()), (SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA = DATABASE())'
    Assert-Condition ($counts.Sets.Count -eq 1 -and $counts.Sets[0].Columns -eq 4 -and
        $counts.Sets[0].Rows.Count -eq 1) 'Invalid recovery object inventory.'
    $row = $counts.Sets[0].Rows[0]
    $tableCount = if ($Version -eq 0) { '1' } else { '3' }
    Assert-Condition ($row[0] -ceq $tableCount -and $row[1] -ceq '1' -and $row[2] -ceq '0' -and $row[3] -ceq '0') "Recovery requires exactly $tableCount tables and the inspection procedure, without other objects."
}

function Invoke-BootstrapRecovery {
    $script:stage = 'bootstrap recovery verification'
    $snapshot = Get-Snapshot
    Assert-RecoverableBootstrap $snapshot
    Assert-Structure $snapshot 0
    Assert-RecoveryObjects 0
    $failed = $snapshot.Sets[1].Rows[0]
    Write-Host "Plan: preserve FAILED bootstrap execution $($failed[0]); append a verified recovery confirmation only."
    Assert-Lock
    $script:stage = 'bootstrap recovery confirmation'
    # A single autocommitted INSERT records completed verification, without DDL or
    # modifying the failed row. The self-join refuses any additional audit row.
    $insertSql = @'
INSERT INTO schema_migrations (version, name, direction, up_checksum, down_checksum, state, started_at, finished_at)
SELECT 0, 'migration_history_recovery', 'UP', failed.up_checksum, NULL, 'SUCCEEDED', UTC_TIMESTAMP(6), UTC_TIMESTAMP(6)
FROM schema_migrations AS failed
LEFT JOIN schema_migrations AS other ON other.execution_id <> failed.execution_id
WHERE other.execution_id IS NULL AND failed.execution_id = ? AND failed.version = 0
    AND failed.name = 'migration_history' AND failed.direction = 'UP'
    AND CAST(failed.up_checksum AS BINARY) = CAST(? AS BINARY) AND failed.down_checksum IS NULL
    AND failed.state = 'FAILED' AND failed.finished_at IS NOT NULL
    AND UTC_TIMESTAMP(6) >= failed.finished_at
'@
    Assert-Condition ((Invoke-Write $insertSql @($failed[0], $script:bootstrap.Hash)) -eq 1) 'Cannot append bootstrap recovery confirmation; inspect history before retrying.'
    $script:stage = 'bootstrap recovery final verification'
    $final = Get-Snapshot
    Assert-Condition ($final.Sets[1].Rows.Count -eq 2 -and
        (ConvertTo-Json -InputObject $final.Sets[1].Rows[0] -Compress) -ceq
        (ConvertTo-Json -InputObject $failed -Compress)) 'Bootstrap recovery changed/unexpected audit history; inspect before proceeding.'
    Assert-Condition ((Get-Head $final) -eq 0) 'Bootstrap recovery head mismatch.'
    Assert-Structure $final 0
    Assert-RecoveryObjects 0
    Write-Host 'Recovery complete. Active version: V000000. Original FAILED audit preserved; run a separate normal Up to apply V000001..V000005.'
}

function Invoke-AccountsRecovery {
    $script:stage = 'account-table recovery verification'
    $snapshot = Get-Snapshot
    $rows = $snapshot.Sets[1].Rows
    Assert-Condition ($rows.Count -eq 2 -or $rows.Count -eq 3) 'Account-table recovery requires a V0-only history followed by one initial V1 failure.'
    $prefixRows = [System.Collections.Generic.List[object]]::new()
    for ($i = 0; $i -lt $rows.Count - 1; ++$i) { $prefixRows.Add($rows[$i]) }
    $prefix = [pscustomobject]@{ Sets = @($null, [pscustomobject]@{ Rows = $prefixRows }) }
    Assert-Condition ((Get-Head $prefix) -eq 0) 'Account-table recovery requires a verified V0 prefix.'
    $failed = $rows[$rows.Count - 1]
    $file = $script:catalog[1]
    Assert-Condition ($failed[0] -cmatch '^[1-9][0-9]*$' -and $failed[1] -ceq '1' -and
        $failed[2] -ceq $file.Name -and $failed[3] -ceq 'UP' -and $failed[4] -ceq $file.Up.Hash -and
        $failed[5] -ceq $file.Down.Hash -and $failed[6] -ceq 'FAILED') 'Account-table recovery audit/checksum mismatch.'
    Assert-Condition ([uint64]$failed[0] -gt [uint64]$prefixRows[$prefixRows.Count - 1][0]) 'Invalid account-table recovery execution order.'
    Assert-RecoveryAuditTime $failed
    Assert-Structure $snapshot 1
    Assert-RecoveryObjects 1
    Assert-Condition ((Read-Scalar 'SELECT NOT EXISTS (SELECT 1 FROM accounts) AND NOT EXISTS (SELECT 1 FROM account_identities)') -ceq '1') 'Account-table recovery refused: account data exists.'
    Write-Host "Plan: preserve FAILED account-table execution $($failed[0]); append a verified recovery confirmation only."
    Assert-Lock
    $script:stage = 'account-table recovery confirmation'
    # Only the last, checksum-bound V1 failure can append a confirmation. Check
    # emptiness again in the same atomic INSERT; no failed row or DDL is changed.
    $insertSql = @'
INSERT INTO schema_migrations (version, name, direction, up_checksum, down_checksum, state, started_at, finished_at)
SELECT 1, 'create_login_accounts_recovery', 'UP', failed.up_checksum, failed.down_checksum, 'SUCCEEDED', UTC_TIMESTAMP(6), UTC_TIMESTAMP(6)
FROM schema_migrations AS failed
LEFT JOIN schema_migrations AS later ON later.execution_id > failed.execution_id
WHERE later.execution_id IS NULL AND failed.execution_id = ? AND failed.version = 1
    AND failed.name = 'create_login_accounts' AND failed.direction = 'UP'
    AND CAST(failed.up_checksum AS BINARY) = CAST(? AS BINARY)
    AND CAST(failed.down_checksum AS BINARY) = CAST(? AS BINARY)
    AND failed.state = 'FAILED' AND failed.finished_at IS NOT NULL
    AND UTC_TIMESTAMP(6) >= failed.finished_at
    AND NOT EXISTS (SELECT 1 FROM accounts) AND NOT EXISTS (SELECT 1 FROM account_identities)
'@
    Assert-Condition ((Invoke-Write $insertSql @($failed[0], $file.Up.Hash, $file.Down.Hash)) -eq 1) 'Cannot append account-table recovery confirmation; inspect history before retrying.'
    $script:stage = 'account-table recovery final verification'
    $final = Get-Snapshot
    Assert-Condition ($final.Sets[1].Rows.Count -eq $rows.Count + 1) 'Unexpected account-table recovery audit count.'
    for ($i = 0; $i -lt $rows.Count; ++$i) {
        Assert-Condition ((ConvertTo-Json -InputObject $final.Sets[1].Rows[$i] -Compress) -ceq
            (ConvertTo-Json -InputObject $rows[$i] -Compress)) 'Account-table recovery changed existing history; inspect before proceeding.'
    }
    Assert-Condition ((Get-Head $final) -eq 1) 'Account-table recovery head mismatch.'
    Assert-Structure $final 1
    Assert-RecoveryObjects 1
    Assert-Condition ((Read-Scalar 'SELECT NOT EXISTS (SELECT 1 FROM accounts) AND NOT EXISTS (SELECT 1 FROM account_identities)') -ceq '1') 'Account data changed during recovery; inspect before proceeding.'
    Write-Host 'Recovery complete. Active version: V000001. Original FAILED audit preserved; run a separate normal Up to apply V000002..V000005.'
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
        if ($Action -eq 'DOWN' -and $Version -eq 4) {
            [void](Invoke-Write 'LOCK TABLES characters WRITE, character_skills WRITE, schema_migrations WRITE')
            $tableLocks = $true
            Assert-Condition ((Read-Scalar 'SELECT COUNT(*) FROM characters') -ceq '0' -and
                (Read-Scalar 'SELECT COUNT(*) FROM character_skills') -ceq '0') 'Down 000004 refused: character data exists.'
        }
        if ($Action -eq 'DOWN' -and $Version -eq 5) {
            [void](Invoke-Write 'LOCK TABLES character_operations WRITE, character_items WRITE, character_state WRITE, schema_migrations WRITE')
            $tableLocks = $true
            Assert-Condition ((Read-Scalar 'SELECT COUNT(*) FROM character_operations') -ceq '0' -and
                (Read-Scalar 'SELECT COUNT(*) FROM character_items') -ceq '0' -and
                (Read-Scalar 'SELECT COUNT(*) FROM character_state') -ceq '0') 'Down 000005 refused: inventory, ownership or request data exists.'
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
            # The empty pair is gone atomically. CREATE/DROP routines must not run
            # with explicit table locks; the named migration lock remains owned.
            if ($tableLocks -and $Action -eq 'DOWN' -and $Version -in @(4, 5) -and $i -eq 0) {
                [void](Invoke-Write 'UNLOCK TABLES'); $tableLocks = $false
            }
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
    $script:stage = 'operator confirmation'
    if ([string]::IsNullOrWhiteSpace($Database)) { $Database = Read-MigrationValue 'Target database' 'actionrpg' }
    Assert-Condition ($Database.Length -ge 1 -and $Database.Length -le 64) 'Database name must contain 1..64 characters.'
    if (-not $ServicesStopped.IsPresent) {
        Write-Host "Requested direction: $Direction / database: $Database"
        $confirmation = Read-Host 'Stop all Auth/Town/Room and other DB-using services. Type Y or y to confirm'
        Assert-Condition ($confirmation -ieq 'Y') 'Cancelled: DB-using services must be stopped; no database connection was opened.'
    }
    Assert-Condition (-not $RecoverAccounts.IsPresent -or ($Direction -eq 'Up' -and -not $RecoverBootstrap.IsPresent -and
        -not $CreateDatabase.IsPresent -and -not $InspectOnly.IsPresent)) '-RecoverAccounts requires Up without other recovery, creation or inspection options.'
    Assert-Condition (-not $RecoverBootstrap.IsPresent -or ($Direction -eq 'Up' -and
        -not $CreateDatabase.IsPresent -and -not $InspectOnly.IsPresent)) '-RecoverBootstrap requires Up without -CreateDatabase or -InspectOnly.'
    Assert-Condition (-not $InspectOnly.IsPresent -or ($Direction -eq 'Up' -and -not $CreateDatabase.IsPresent)) '-InspectOnly requires Up without -CreateDatabase.'
    Assert-Condition ($PSBoundParameters.ContainsKey('InspectVersion') -eq $InspectOnly.IsPresent) 'Supply -InspectOnly and -InspectVersion 0..5 together; the comparison version is not an active-version claim.'
    Assert-Condition (-not $CreateDatabase.IsPresent -or $Direction -eq 'Up') '-CreateDatabase is only supported with Up.'
    if ($CreateDatabase) {
        Assert-Condition ($Database -cmatch '\A[a-z][a-z0-9_]{0,63}\z' -and
            $Database -cnotin @('mysql', 'information_schema', 'performance_schema', 'sys')) 'Database creation requires a lowercase ASCII application name: [a-z][a-z0-9_]{0,63}.'
    }
    Assert-Condition ([Environment]::Is64BitProcess) 'Use 64-bit Windows PowerShell and a matching 64-bit MySQL ODBC driver.'
    $script:stage = 'local file validation'
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
    Assert-Condition ($script:catalog.Count -eq 5) 'Current schema inspection supports versions 000001..000005; extend the contract with new migrations.'
    for ($i = 1; $i -le $script:catalog.Count; ++$i) { Assert-Condition ($script:catalog.ContainsKey($i)) 'Migration version gap.' }
    Assert-Condition ((@(Get-ChildItem -LiteralPath (Join-Path $root 'Down') -Filter '*.sql' -File)).Count -eq $script:catalog.Count) 'Unexpected Down files.'
    Assert-Condition ((Normalize-Sql $script:catalog[1].Down.Statements[0]) -ceq 'drop table account_identities , accounts' -and
        $script:catalog[1].Down.Statements.Count -eq 1) 'Unsafe Down 000001 contract.'
    Assert-Condition ($script:catalog[4].Up.Statements.Count -eq 3 -and
        $script:catalog[4].Down.Statements.Count -eq 2 -and
        (Normalize-Sql $script:catalog[4].Down.Statements[0]) -ceq 'drop table character_skills , characters' -and
        (Normalize-Sql $script:catalog[4].Down.Statements[1]) -ceq 'drop procedure get_character_schema_migration_history') 'Unsafe Down 000004 contract.'
    Assert-Condition ($script:catalog[5].Up.Statements.Count -eq 10 -and
        $script:catalog[5].Down.Statements.Count -eq 8 -and
        (Normalize-Sql $script:catalog[5].Down.Statements[0]) -ceq 'drop table character_operations , character_items , character_state' -and
        (Normalize-Sql $script:catalog[5].Down.Statements[7]) -ceq 'drop procedure get_inventory_schema_migration_history') 'Unsafe Down 000005 contract.'
    $script:stage = 'connection input'
    $secret = Get-MigrationConnectionString
    if ($CreateDatabase) {
        $builder = [System.Data.Odbc.OdbcConnectionStringBuilder]::new($secret)
        # Driver/DSN are built-in keys even when unset; inspect stored values.
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($builder.Driver) -and -not $builder.ShouldSerialize('DSN') -and
            -not $builder.ShouldSerialize('FILEDSN') -and -not $builder.ShouldSerialize('SAVEFILE')) 'Database creation requires a DSN-less DRIVER connection.'
        if ($builder.ContainsKey('DATABASE')) {
            Assert-Condition ([string]$builder['DATABASE'] -ceq $Database) 'Connection database must match -Database.'
            [void]$builder.Remove('DATABASE')
        }
        $secret = $builder.ConnectionString
        $builder.Clear()
        $builder = $null
    }
    $script:stage = 'ODBC connection/target verification'
    $script:connection = [System.Data.Odbc.OdbcConnection]::new($secret)
    $secret = $null
    $handler = [System.Data.Odbc.OdbcInfoMessageEventHandler]{ param($sender, $eventArgs) $script:warningSeen = $true }
    $script:connection.add_InfoMessage($handler)
    $script:connection.Open()
    Assert-Condition (-not $script:warningSeen) 'ODBC connection warning.'
    $target = Get-Target
    if ($CreateDatabase) {
        Assert-Condition ($null -eq $target.Database) 'Database creation connection must not preselect a schema.'
    } else { Assert-Condition ($target.Database -ceq $Database) 'Connection database must match -Database.' }
    Write-Host "Target: $($target.Host):$($target.Port) / $Database / MySQL $($target.Version)"
    $script:lockName = if ($CreateDatabase) {
        Read-Scalar "SELECT CONCAT('actionrpg:migrate:', LEFT(SHA2(?, 256), 40))" @($Database)
    } else { Read-Scalar "SELECT CONCAT('actionrpg:migrate:', LEFT(SHA2(DATABASE(), 256), 40))" }
    Assert-Condition ((Read-Scalar 'SELECT GET_LOCK(?, 0)' @($script:lockName)) -ceq '1') 'Another migrator/schema inspector owns this DB lock.'
    $script:ownsLock = $true
    if ($CreateDatabase) {
        Initialize-Database
        $target = Get-Target
        Assert-Condition ($target.Database -ceq $Database) 'Selected database must match -Database.'
        Assert-Condition ((Read-Scalar "SELECT CONCAT('actionrpg:migrate:', LEFT(SHA2(DATABASE(), 256), 40))") -ceq
            $script:lockName) 'Selected database migration lock does not match.'
    }
    $script:stage = 'history/actual schema verification'
    $hasHistory = Read-Scalar "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'schema_migrations'"
    if ($InspectOnly) {
        # This branch must precede bootstrap/audit writes and Get-Head's success-only gate.
        # The existing inspection routine reads metadata/audit and takes the same named lock.
        Assert-Condition ($hasHistory -ceq '1') 'Inspection requires existing migration infrastructure; nothing was created.'
        $script:stage = 'read-only schema inspection'
        Write-Host "Inspection only: compare structure with V$('{0:D6}' -f $InspectVersion); no migrations or audit updates."
        $snapshot = Get-Snapshot
        foreach ($row in $snapshot.Sets[1].Rows) { Write-Host ('Audit: ' + (ConvertTo-Json -InputObject $row -Compress)) }
        Assert-Structure $snapshot $InspectVersion
        Write-Host 'Inspection complete: structure matched the requested comparison version. Audit validity, active version and recovery are not established.'
        return
    }
    if ($RecoverBootstrap) {
        Assert-Condition ($hasHistory -ceq '1') 'Bootstrap recovery requires existing migration infrastructure; nothing was created.'
        Invoke-BootstrapRecovery
        return
    }
    if ($RecoverAccounts) {
        Assert-Condition ($hasHistory -ceq '1') 'Account-table recovery requires existing migration infrastructure; nothing was created.'
        Invoke-AccountsRecovery
        return
    }
    if ($hasHistory -ceq '0') {
        Assert-Condition ($Direction -eq 'Up') 'Unmanaged schema: Down requires a verified migration history.'
        $objects = Read-Scalar 'SELECT (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()) + (SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE()) + (SELECT COUNT(*) FROM information_schema.EVENTS WHERE EVENT_SCHEMA = DATABASE()) + (SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA = DATABASE())'
        Assert-Condition ($objects -ceq '0') 'Refusing to baseline a nonempty/unmanaged schema.'
        Write-Host 'Plan: bootstrap V000000, then Up V000001 -> V000005.'
        Invoke-Migration 0 'migration_history' 'UP' $script:bootstrap $null 0 -Bootstrap
    }
    $snapshot = Get-Snapshot
    $head = Get-Head $snapshot
    Assert-Structure $snapshot $head
    if ($Direction -eq 'Up') {
        Write-Host "Plan: active V$('{0:D6}' -f $head) -> V000005; Up each pending version in order."
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
    $operation = if ($InspectOnly) { 'Inspection' } elseif ($RecoverBootstrap -or $RecoverAccounts) { 'Recovery' } else { 'Migration' }
    [Console]::Error.WriteLine("$operation stopped at $script:stage. $detail")
    if ($CreateDatabase) { [Console]::Error.WriteLine('Any successfully created database remains; database creation is not rolled back or recorded as a versioned migration.') }
    [Console]::Error.WriteLine('Do not replay partial DDL or edit audit rows. Inspect actual schema and RUNNING/FAILED audit before an approved recovery.')
    exit 1
} finally {
    $secret = $null
    if ($null -ne $script:connection) {
        if ($script:ownsLock -and $script:connection.State -eq [System.Data.ConnectionState]::Open) {
            try { [void](Read-Scalar 'SELECT RELEASE_LOCK(?)' @($script:lockName)) } catch {}
        }
        $script:connection.Dispose()
    }
}
