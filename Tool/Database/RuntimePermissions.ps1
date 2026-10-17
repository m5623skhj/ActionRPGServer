# Shared by the explicit DB preparation tool and the read-only launcher preflight.
# Dot-sourcing only defines functions. Never execute an application CALL here.
function Get-RuntimeSchemaVersion { return 8 }

function Get-RuntimeProcedures([int]$Version, [ValidateSet('Auth','Town')][string]$Role) {
    if ($Version -lt 0 -or $Version -gt (Get-RuntimeSchemaVersion)) {
        Stop-RuntimePermissions 'Unsupported permission contract version; update the public procedure list with the migration.'
    }
    $inspectors = @('get_schema_migration_history','get_schema_migration_history','get_schema_migration_history',
        'get_schema_migration_history','get_character_schema_migration_history','get_inventory_schema_migration_history',
        'get_item_use_schema_migration_history','get_quest_schema_migration_history','get_experience_schema_migration_history')
    $rows = [ordered]@{}
    $rows[$inspectors[$Version]] = 0
    if ($Role -eq 'Auth') {
        if ($Version -ge 2) { $rows['login_google_account'] = 1 }
        if ($Version -ge 3) { $rows['get_auth_account_status'] = 1 }
    } else {
        if ($Version -ge 5) {
            $rows['list_characters'] = 1; $rows['create_character'] = 6; $rows['claim_character'] = 4
            $rows['save_character_state'] = 9; $rows['release_character'] = 4
        }
        if ($Version -ge 6) {
            $rows['reserve_item_use'] = 12; $rows['get_item_use'] = 5
            $rows['complete_item_use'] = 8; $rows['cancel_item_use'] = 10
        }
        if ($Version -ge 7) { $rows['get_character_quests'] = 5; $rows['save_character_quests'] = 12 }
    }
    foreach ($name in $rows.Keys) { [pscustomobject]@{ Name = [string]$name; Parameters = [int]$rows[$name] } }
}

function Stop-RuntimePermissions([string]$Message) {
    $failure = [InvalidOperationException]::new($Message)
    $failure.Data['RuntimePermissionMessage'] = $Message
    $failure.Data['SafeMigrationMessage'] = $Message
    throw $failure
}
function Get-RuntimePermissionFailure([Exception]$Exception) {
    while ($null -ne $Exception) {
        if ($Exception.Data.Contains('RuntimePermissionMessage')) { return [string]$Exception.Data['RuntimePermissionMessage'] }
        $Exception = $Exception.InnerException
    }
    return 'Runtime permission preparation failed; connection strings and driver messages are suppressed.'
}
function Get-RuntimeDatabaseError([Exception]$Exception) {
    while ($null -ne $Exception) {
        if ($Exception -is [Data.Odbc.OdbcException] -and $Exception.Errors.Count -gt 0) {
            $errorRecord = $Exception.Errors[0]
            $state = [string]$errorRecord.SQLState
            if ($state -cnotmatch '\A[A-Z0-9]{5}\z') { $state = 'unknown' }
            return "SQLSTATE=$state; native_code=$($errorRecord.NativeError)"
        }
        $Exception = $Exception.InnerException
    }
    return 'SQLSTATE=unavailable; native_code=unavailable'
}
function Assert-RuntimeDatabaseName([string]$Database) {
    if ($Database -cnotmatch '\A[A-Za-z_][A-Za-z0-9_]{0,63}\z' -or
        $Database -iin @('mysql','sys','information_schema','performance_schema')) {
        Stop-RuntimePermissions 'Runtime permission setup requires an ASCII application database identifier.'
    }
}
function Assert-RuntimeConnectionDatabase([string]$ConnectionString, [string]$Database, [string]$Label) {
    $builder = $null
    try {
        $builder = [Data.Odbc.OdbcConnectionStringBuilder]::new($ConnectionString)
        if (!$builder.ContainsKey('DATABASE') -or [string]$builder['DATABASE'] -cne $Database) {
            Stop-RuntimePermissions "$Label connection must explicitly select the requested application database; no GRANT was issued."
        }
    } finally { if ($null -ne $builder) { $builder.Clear() } }
}
function Invoke-RuntimeSql($Connection, [string]$Sql) {
    $command = $Connection.CreateCommand()
    try { $command.CommandText = $Sql; $command.CommandTimeout = 15; [void]$command.ExecuteNonQuery() }
    finally { $command.Dispose() }
}
function Get-RuntimeDatabaseIdentity($Connection, [string]$Database, [switch]$AllowUnselected) {
    Assert-RuntimeDatabaseName $Database
    $command = $Connection.CreateCommand(); $reader = $null
    try {
        $command.CommandText = 'SELECT DATABASE(), @@server_uuid, CURRENT_USER(), USER(), @@session.sql_mode'
        $command.CommandTimeout = 15; $reader = $command.ExecuteReader()
        if (!$reader.Read() -or $reader.FieldCount -ne 5) { Stop-RuntimePermissions 'Invalid database identity result.' }
        $selected = if ($reader.IsDBNull(0)) { '' } else { [string]$reader.GetValue(0) }
        $uuid = [string]$reader.GetValue(1)
        $principal = [string]$reader.GetValue(2); $login = [string]$reader.GetValue(3); $mode = [string]$reader.GetValue(4)
        if ($reader.Read() -or $reader.NextResult() -or ($selected -cne $Database -and !($AllowUnselected -and !$selected)) -or
            $uuid -cnotmatch '\A[0-9a-fA-F-]{36}\z' -or $mode -match '(^|,)(NO_BACKSLASH_ESCAPES|ANSI_QUOTES)(,|$)' -or
            $principal -cnotmatch '\A([A-Za-z0-9_.-]{1,32})@([A-Za-z0-9_.:%/-]{1,255})\z') {
            Stop-RuntimePermissions 'Database/schema/account identity is unsupported or does not match the requested target.'
        }
        $split = $principal.LastIndexOf('@'); $user = $principal.Substring(0, $split); $hostName = $principal.Substring($split + 1)
        if ($login.LastIndexOf('@') -lt 1 -or $login.Substring(0, $login.LastIndexOf('@')) -cne $user) {
            Stop-RuntimePermissions 'Anonymous/proxy account mapping is unsupported; configure the intended runtime account explicitly.'
        }
        return [pscustomobject]@{ Database = $Database; Uuid = $uuid; Principal = $principal; User = $user; HostName = $hostName }
    } finally { if ($null -ne $reader) { $reader.Dispose() }; $command.Dispose() }
}
function Open-RuntimeDatabase([string]$ConnectionString, [string]$Label, [switch]$WithoutDatabase) {
    $connection = $null; $builder = $null
    try {
        $builder = [Data.Odbc.OdbcConnectionStringBuilder]::new($ConnectionString)
        if ($builder.ShouldSerialize('SAVEFILE') -or $builder.ShouldSerialize('FILEDSN')) {
            Stop-RuntimePermissions 'Permission checks must not create/read connection-string files.'
        }
        if ($WithoutDatabase -and $builder.ContainsKey('DATABASE')) { [void]$builder.Remove('DATABASE') }
        # Own the probe session, leave roles unchanged and deallocate every probe.
        $connection = [Data.Odbc.OdbcConnection]::new($builder.ConnectionString)
        $connection.Open()
        return ,$connection
    } catch {
        if ($null -ne $connection) { $connection.Dispose() }
        $diagnostic = Get-RuntimeDatabaseError $_.Exception
        Stop-RuntimePermissions "$Label connection failed ($diagnostic). Check this account's credentials, host mapping and TLS locally."
    } finally { if ($null -ne $builder) { $builder.Clear() } }
}

# Explicit SQL PREPARE makes the server check CALL permissions and argument count,
# including when the ODBC driver disables native prepared statements. There is no
# EXECUTE: login, inventory, item-use and quest procedure bodies are never run.
function Assert-RuntimeProcedureAccess($Connection, $Identity, [int]$Version, [string]$Role) {
    foreach ($routine in Get-RuntimeProcedures $Version $Role) {
        $arguments = (@('NULL') * $routine.Parameters) -join ','
        $call = 'CALL `{0}`.`{1}`({2})' -f $Identity.Database, $routine.Name, $arguments
        $prepared = $false
        try {
            Invoke-RuntimeSql $Connection ("PREPARE actionrpg_permission_probe FROM '$call'")
            $prepared = $true
        } catch {
            $diagnostic = Get-RuntimeDatabaseError $_.Exception
            Stop-RuntimePermissions ("$Role runtime account $($Identity.Principal) cannot prepare $($Identity.Database).$($routine.Name) ($diagnostic). " +
                'Run Tool\Database\UpMigration.bat -Database ' + $Identity.Database + ' -PermissionsOnly with services stopped; if the procedure is missing, run normal Up first.')
        } finally {
            if ($prepared) { Invoke-RuntimeSql $Connection 'DEALLOCATE PREPARE actionrpg_permission_probe' }
        }
    }
}
function Assert-RuntimeDatabaseAccess([string]$Database, [string]$AuthConnectionString, [string]$TownConnectionString,
    [int]$Version = (Get-RuntimeSchemaVersion), [string]$ExpectedUuid = '') {
    $auth = $null; $town = $null
    try {
        Assert-RuntimeConnectionDatabase $AuthConnectionString $Database 'Auth runtime'
        Assert-RuntimeConnectionDatabase $TownConnectionString $Database 'Town runtime'
        $auth = Open-RuntimeDatabase $AuthConnectionString 'Auth runtime' -WithoutDatabase
        $authIdentity = Get-RuntimeDatabaseIdentity $auth $Database -AllowUnselected
        if ($ExpectedUuid -and $authIdentity.Uuid -ine $ExpectedUuid) { Stop-RuntimePermissions 'Auth runtime targets a different MySQL instance.' }
        $town = Open-RuntimeDatabase $TownConnectionString 'Town runtime' -WithoutDatabase
        $townIdentity = Get-RuntimeDatabaseIdentity $town $Database -AllowUnselected
        if ($townIdentity.Uuid -ine $authIdentity.Uuid) { Stop-RuntimePermissions 'Auth and Town runtime accounts target different MySQL instances.' }
        Assert-RuntimeProcedureAccess $auth $authIdentity $Version 'Auth'
        Assert-RuntimeProcedureAccess $town $townIdentity $Version 'Town'
        Invoke-RuntimeSql $auth ('USE `{0}`' -f $Database)
        Invoke-RuntimeSql $town ('USE `{0}`' -f $Database)
        Write-Host "Runtime EXECUTE verified: Auth=$($authIdentity.Principal); Town=$($townIdentity.Principal); database=$Database. No application procedure was executed."
    } finally { if ($null -ne $town) { $town.Dispose() }; if ($null -ne $auth) { $auth.Dispose() } }
}
function Read-RuntimePermissionConnection([string]$Template, [string]$Label, [string]$DefaultUser, [string]$Database) {
    $builder = [Data.Odbc.OdbcConnectionStringBuilder]::new($Template)
    $secure = $null; $pointer = [IntPtr]::Zero
    try {
        $user = ([string](Read-Host "$Label account [$DefaultUser]")).Trim()
        if (!$user) { $user = $DefaultUser }
        if ($user -cnotmatch '\A[A-Za-z0-9_.-]{1,32}\z') { Stop-RuntimePermissions 'Enter a supported MySQL account name without a host or quotes.' }
        foreach ($key in @('UID','USER','PWD','PASSWORD')) { if ($builder.ContainsKey($key)) { [void]$builder.Remove($key) } }
        $builder['DATABASE'] = $Database; $builder['UID'] = $user
        $secure = Read-Host "$Label password (hidden; never saved)" -AsSecureString
        if ($null -eq $secure -or $secure.Length -eq 0) { Stop-RuntimePermissions 'Password input cancelled; no runtime permission preparation completed.' }
        $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
        $builder['PWD'] = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
        return $builder.ConnectionString
    } finally {
        if ($pointer -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer) }
        if ($null -ne $secure) { $secure.Dispose() }; $builder.Clear()
    }
}

function Sync-RuntimeDatabasePermissions($MigrationConnection, [string]$Database, [int]$Version, [scriptblock]$AssertLock) {
    $admin = $null; $auth = $null; $town = $null
    $adminText = $null; $authText = $null; $townText = $null; $template = $null
    try {
        & $AssertLock
        $migrationIdentity = Get-RuntimeDatabaseIdentity $MigrationConnection $Database
        $template = $MigrationConnection.ConnectionString
        $authText = [Environment]::GetEnvironmentVariable('ACTIONRPG_DB_CONNECTION_STRING')
        if ([string]::IsNullOrWhiteSpace($authText)) { $authText = Read-RuntimePermissionConnection $template 'Auth runtime (not migration/admin)' 'actionrpg_auth' $Database }
        $townText = [Environment]::GetEnvironmentVariable('ACTIONRPG_TOWN_DB_CONNECTION_STRING')
        if ([string]::IsNullOrWhiteSpace($townText)) { $townText = $authText }
        Assert-RuntimeConnectionDatabase $authText $Database 'Auth runtime'
        Assert-RuntimeConnectionDatabase $townText $Database 'Town runtime'
        # A newly created runtime account may not yet have permission to USE this schema.
        # Authenticate without preselecting it to learn the actual matched account, then grant.
        $auth = Open-RuntimeDatabase $authText 'Auth runtime' -WithoutDatabase
        $authIdentity = Get-RuntimeDatabaseIdentity $auth $Database -AllowUnselected
        $town = Open-RuntimeDatabase $townText 'Town runtime' -WithoutDatabase
        $townIdentity = Get-RuntimeDatabaseIdentity $town $Database -AllowUnselected
        foreach ($identity in @($authIdentity,$townIdentity)) {
            if ($identity.Uuid -ine $migrationIdentity.Uuid) { Stop-RuntimePermissions 'Runtime connection targets a different MySQL instance; no GRANT was issued.' }
            if ($identity.Principal -ceq $migrationIdentity.Principal) { Stop-RuntimePermissions 'Runtime and migration accounts must be separate; no GRANT was issued.' }
        }
        $auth.Dispose(); $auth = $null; $town.Dispose(); $town = $null
        $adminText = [Environment]::GetEnvironmentVariable('ACTIONRPG_PERMISSION_CONNECTION_STRING')
        if ([string]::IsNullOrWhiteSpace($adminText)) { $adminText = Read-RuntimePermissionConnection $template 'Permission administrator (EXECUTE with GRANT OPTION required)' 'root' $Database }
        $admin = Open-RuntimeDatabase $adminText 'Permission administrator'
        $adminIdentity = Get-RuntimeDatabaseIdentity $admin $Database
        if ($adminIdentity.Uuid -ine $migrationIdentity.Uuid -or $adminIdentity.Principal -cin @($authIdentity.Principal,$townIdentity.Principal)) {
            Stop-RuntimePermissions 'Permission administrator must target the same MySQL instance and be separate from runtime accounts.'
        }
        $granted = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($profile in @(@{ Role = 'Auth'; Identity = $authIdentity }, @{ Role = 'Town'; Identity = $townIdentity })) {
            foreach ($routine in Get-RuntimeProcedures $Version $profile.Role) {
                & $AssertLock
                $identity = $profile.Identity; $key = $identity.Principal + ':' + $routine.Name
                if (!$granted.Add($key)) { continue }
                # Names come from the fixed public allowlist; principals were obtained from CURRENT_USER().
                # No database-wide EXECUTE, private emit/apply, account creation, REVOKE or audit writes.
                $sql = 'GRANT EXECUTE ON PROCEDURE `{0}`.`{1}` TO ''{2}''@''{3}''' -f $Database,$routine.Name,$identity.User,$identity.HostName
                try { Invoke-RuntimeSql $admin $sql }
                catch {
                    $diagnostic = Get-RuntimeDatabaseError $_.Exception
                    Stop-RuntimePermissions "GRANT failed for $($identity.Principal), $Database.$($routine.Name) ($diagnostic). Supply a permission administrator with EXECUTE and GRANT OPTION; rerun -PermissionsOnly."
                }
            }
        }
        & $AssertLock
        Assert-RuntimeDatabaseAccess $Database $authText $townText $Version $migrationIdentity.Uuid
    } finally {
        if ($null -ne $town) { $town.Dispose() }; if ($null -ne $auth) { $auth.Dispose() }; if ($null -ne $admin) { $admin.Dispose() }
        $adminText = $null; $authText = $null; $townText = $null; $template = $null
    }
}
