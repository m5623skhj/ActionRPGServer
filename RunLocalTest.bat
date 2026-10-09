@echo off
setlocal
set "ACTIONRPG_LOCAL_LAUNCHER=%~f0"
set "LOCAL_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if defined PROCESSOR_ARCHITEW6432 set "LOCAL_POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%LOCAL_POWERSHELL%" -NoProfile -Command "$source = [IO.File]::ReadAllText($env:ACTIONRPG_LOCAL_LAUNCHER); & ([scriptblock]::Create($source.Substring($source.LastIndexOf('# POWERSHELL START'))))"
set "LAUNCH_RESULT=%ERRORLEVEL%"
pause
exit /b %LAUNCH_RESULT%

# POWERSHELL START
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $env:ACTIONRPG_LOCAL_LAUNCHER
$serverDirectory = Join-Path $root 'ActionRPGServer\x64\Debug'
$roomDirectory = Join-Path $root 'artifacts\bin\x64\Debug'
$clientDirectory = [IO.Path]::GetFullPath((Join-Path $root '..\ActionRPGClient\ActionRPGClient\artifacts\bin\x64\Debug'))
$script:startedServers = @()
$script:failureMessage = ''
$script:launchStage = 'Checking executable/runtime prerequisites'
$script:googleDesktopClientId = $env:ACTIONRPG_GOOGLE_CLIENT_ID
$script:googleDesktopClientSecret = $env:ACTIONRPG_GOOGLE_DESKTOP_CLIENT_SECRET
# Only Start-Client receives this value; server processes must not inherit it.
[Environment]::SetEnvironmentVariable('ACTIONRPG_GOOGLE_DESKTOP_CLIENT_SECRET', $null, 'Process')
$script:required = @('ACTIONRPG_AUTH_HOST', 'ACTIONRPG_AUTH_TLS_CERT', 'ACTIONRPG_AUTH_TLS_KEY', 'ACTIONRPG_AUTH_CA_FILE', 'ACTIONRPG_GOOGLE_CLIENT_ID', 'ACTIONRPG_AUTH_TOWN_REGISTRY', 'ACTIONRPG_TOWN_ID', 'ACTIONRPG_TOWN_AUTH_KEY', 'ACTIONRPG_TOWN_TLS_CERT', 'ACTIONRPG_TOWN_TLS_KEY', 'ACTIONRPG_DB_CONNECTION_STRING', 'ACTIONRPG_TOWN_DB_CONNECTION_STRING', 'ACTIONRPG_DB_SCHEMA', 'ACTIONRPG_DB_MIGRATIONS_DIRECTORY')

function Fail([string] $message) {
    $script:failureMessage = $message
    throw [InvalidOperationException]::new('Local launcher stopped.')
}
function Require-File([string] $path, [string] $name) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { Fail "File required: $name" }
}
function Require-AbsolutePath([string] $path, [string] $name) {
    if ($path -notmatch '^(?:[A-Za-z]:[\\/]|\\\\)') { Fail "Absolute path required: $name" }
}
function Find-MSBuild($installations, [string] $toolset) {
    foreach ($installation in $installations) {
        $buildTool = Join-Path $installation.installationPath 'MSBuild\Current\Bin\MSBuild.exe'
        $toolsetPattern = Join-Path $installation.installationPath ('MSBuild\Microsoft\VC\*\Platforms\x64\PlatformToolsets\' + $toolset + '\Toolset.props')
        if ((Test-Path -LiteralPath $buildTool -PathType Leaf) -and @(Get-ChildItem -Path $toolsetPattern -File -ErrorAction SilentlyContinue).Count) { return $buildTool }
    }
    Fail ("Install Visual Studio C++ toolset $toolset, MSBuild and Windows SDK before running this launcher.")
}
function Build-LocalProjects {
    $script:launchStage = 'Finding Visual Studio build tools'
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    Require-File $vswhere 'Visual Studio Installer/vswhere.exe; install Visual Studio C++ build tools'
    $serverSolution = Join-Path $root 'ActionRPGServer\ActionRPGServer.slnx'
    $clientProject = [IO.Path]::GetFullPath((Join-Path $root '..\ActionRPGClient\ActionRPGClient\ActionRPGClient\ActionRPGClient.vcxproj'))
    Require-File $serverSolution 'server solution'
    Require-File $clientProject 'sibling ActionRPGClient project'
    # Build tools and their children do not need authentication or database credentials.
    $savedEnvironment = @{}
    try {
        foreach ($entry in Get-ChildItem Env: | Where-Object { $_.Name.StartsWith('ACTIONRPG_', [StringComparison]::OrdinalIgnoreCase) }) {
            $savedEnvironment[$entry.Name] = $entry.Value
            [Environment]::SetEnvironmentVariable($entry.Name, $null, 'Process')
        }
        $metadata = & $vswhere -all -products '*' -requires Microsoft.Component.MSBuild -format json
        if ($LASTEXITCODE -ne 0) { Fail 'Visual Studio build tool discovery failed.' }
        $installed = ($metadata -join [Environment]::NewLine) | ConvertFrom-Json
        $installations = @($installed | Where-Object { $_.isComplete -and $_.isLaunchable } | Sort-Object { [version]$_.installationVersion } -Descending)
        $serverBuildTool = Find-MSBuild $installations 'v145'
        $clientBuildTool = Find-MSBuild $installations 'v143'
        $script:launchStage = 'Building servers Debug x64'
        Write-Host 'Building servers Debug x64 (incremental; project targets prepare runtime data and DLLs)...'
        & $serverBuildTool $serverSolution /t:Build /m /nologo /verbosity:minimal /p:Configuration=Debug /p:Platform=x64 /p:VcpkgEnableManifest=true
        if ($LASTEXITCODE -ne 0) { Fail 'Server Debug x64 build failed. Resolve the build errors above; no server was started.' }
        $script:launchStage = 'Building client Debug x64'
        Write-Host 'Building client Debug x64 (incremental; project targets prepare runtime assets)...'
        & $clientBuildTool $clientProject /t:Build /m /nologo /verbosity:minimal /p:Configuration=Debug /p:Platform=x64 /p:VcpkgEnableManifest=true
        if ($LASTEXITCODE -ne 0) { Fail 'Client Debug x64 build failed. Resolve the build errors above; no server was started.' }
    } finally {
        foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
        $savedEnvironment.Clear()
    }
}

function New-RandomKey {
    $bytes = New-Object byte[] 32
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes); return [BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant() }
    finally { $rng.Dispose() }
}
function Read-Secret([string] $prompt) {
    $secure = Read-Host $prompt -AsSecureString
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer); $secure.Dispose() }
}
function Assert-GoogleDesktopSecret($value) {
    if ($value -isnot [string] -or $value -cnotmatch '\A[\x21-\x7E]{1,1024}\z') {
        Fail 'Google Desktop client secret must be 1..1024 printable ASCII characters without whitespace. Enter it locally; do not share it.'
    }
}
function Initialize-GoogleDesktopCredentials($secrets, [string] $secretPath) {
    $script:launchStage = 'Preparing protected Google Desktop client credentials'
    if ($env:ACTIONRPG_GOOGLE_CLIENT_ID -cnotmatch '\A[A-Za-z0-9._-]{1,256}\.apps\.googleusercontent\.com\z' -or $env:ACTIONRPG_GOOGLE_CLIENT_ID.Length -gt 256) {
        Fail 'A valid Google Desktop client ID is required before adding protected credentials.'
    }
    $hasSecret = $null -ne $secrets.PSObject.Properties['googleDesktopClientSecret']
    $hasClientId = $null -ne $secrets.PSObject.Properties['googleDesktopClientId']
    if ($hasSecret -ne $hasClientId) { Fail 'Incomplete saved Google Desktop credentials. Review the protected local profile.' }
    if ($hasSecret) {
        Assert-GoogleDesktopSecret $secrets.googleDesktopClientSecret
        if ($secrets.googleDesktopClientId -isnot [string] -or $secrets.googleDesktopClientId -cne $env:ACTIONRPG_GOOGLE_CLIENT_ID) {
            Fail 'Saved Google Desktop credentials belong to a different client ID. Review the protected local profile.'
        }
    } else {
        Write-Host 'Enter the client secret from the SAME Google Desktop OAuth client as the saved client ID.'
        Write-Host 'It is used only for Google token exchange. Do not paste it into chat or source files.'
        $value = Read-Secret 'Google Desktop client secret (hidden input; blank cancels)'
        try {
            Assert-GoogleDesktopSecret $value
            $secrets | Add-Member -NotePropertyName googleDesktopClientId -NotePropertyValue ([string]$env:ACTIONRPG_GOOGLE_CLIENT_ID)
            $secrets | Add-Member -NotePropertyName googleDesktopClientSecret -NotePropertyValue $value
            $plainBytes = $null
            $protectedBytes = $null
            $temporaryPath = Join-Path (Split-Path -Parent $secretPath) ([Guid]::NewGuid().ToString('N') + '.dpapi.tmp')
            try {
                $plainBytes = [Text.Encoding]::UTF8.GetBytes(($secrets | ConvertTo-Json -Compress -Depth 6))
                $protectedBytes = [Security.Cryptography.ProtectedData]::Protect($plainBytes, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
                [IO.File]::WriteAllBytes($temporaryPath, $protectedBytes)
                Assert-PrivatePath $temporaryPath
                # Replace only after encryption completes; preserve all existing credential fields.
                # PowerShell 5.1 converts ordinary $null to an empty string argument.
                [IO.File]::Replace($temporaryPath, $secretPath, [System.Management.Automation.Language.NullString]::Value)
                Assert-PrivatePath $secretPath
            } finally {
                if ($null -ne $plainBytes) { [Array]::Clear($plainBytes, 0, $plainBytes.Length) }
                if ($null -ne $protectedBytes) { [Array]::Clear($protectedBytes, 0, $protectedBytes.Length) }
                if ([IO.File]::Exists($temporaryPath)) { [IO.File]::Delete($temporaryPath) }
            }
        } finally { $value = $null }
        Write-Host 'Google Desktop credentials saved with DPAPI CurrentUser; existing DB and town credentials preserved.'
    }
    $script:googleDesktopClientId = [string]$secrets.googleDesktopClientId
    $script:googleDesktopClientSecret = [string]$secrets.googleDesktopClientSecret
}
function Get-MySqlDrivers {
    if (!(Get-Command Get-OdbcDriver -ErrorAction SilentlyContinue)) { Fail '64-bit ODBC driver discovery is unavailable. See the DB preparation guide.' }
    try { $drivers = @(Get-OdbcDriver -Platform '64-bit' | Where-Object { $_.Name -match 'MySQL.*Unicode' }) }
    catch { Fail '64-bit ODBC driver discovery failed. Driver readiness is unknown.' }
    if (!$drivers.Count) { Fail 'Install the official Windows 64-bit MySQL Unicode ODBC driver before setup. See docs/workflows/DATABASE_MIGRATIONS.md.' }
    return $drivers
}
function Assert-OdbcReady($connection) {
    if (![Environment]::Is64BitProcess) { Fail '64-bit Windows PowerShell is required.' }
    # ODBC retains brace quoting when a saved connection string is parsed again.
    $driverName = [string]$connection.Driver
    if ($driverName -cmatch '\A\{(?:[^}]|}})*\}\z') {
        $driverName = $driverName.Substring(1, $driverName.Length - 2).Replace('}}', '}')
    }
    $driver = @(Get-MySqlDrivers | Where-Object { $_.Name -ceq $driverName })
    if ($driver.Count -ne 1) { Fail 'The configured 64-bit MySQL Unicode ODBC driver is not registered.' }
    $registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    $key = $null
    try {
        $key = $registry.OpenSubKey('SOFTWARE\ODBC\ODBCINST.INI\' + $driverName)
        if (!$key -or ![IO.File]::Exists([string]$key.GetValue('Driver'))) { Fail 'The configured 64-bit MySQL ODBC driver DLL is unavailable.' }
    } finally { if ($key) { $key.Dispose() }; $registry.Dispose() }
}
function Read-DatabaseSettings {
    $script:launchStage = 'Reading database settings'
    $schema = $env:ACTIONRPG_DB_SCHEMA
    if (!$schema) { $schema = Read-Host 'Existing account database schema name (no database is created)' }
    if ([string]::IsNullOrWhiteSpace($schema) -or $schema.Length -gt 64) { Fail 'A valid existing database schema is required.' }
    if ($env:ACTIONRPG_DB_CONNECTION_STRING) {
        try { $connection = [Data.Odbc.OdbcConnectionStringBuilder]::new($env:ACTIONRPG_DB_CONNECTION_STRING) }
        catch { Fail 'Invalid existing DB connection string format.' }
    } else {
        $drivers = @(Get-MySqlDrivers)
        for ($index = 0; $index -lt $drivers.Count; $index++) { Write-Host (($index + 1).ToString() + ': ' + $drivers[$index].Name) }
        $choice = 0
        if (![int]::TryParse((Read-Host 'Choose the installed 64-bit Unicode ODBC driver number'), [ref]$choice) -or $choice -lt 1 -or $choice -gt $drivers.Count) { Fail 'Invalid ODBC driver selection.' }
        $dbHost = Read-Host 'Existing MySQL host'
        $dbPort = 0
        if ([string]::IsNullOrWhiteSpace($dbHost) -or ![int]::TryParse((Read-Host 'Existing MySQL port'), [ref]$dbPort) -or $dbPort -lt 1 -or $dbPort -gt 65535) { Fail 'A valid existing MySQL host and port are required.' }
        $dbUser = Read-Host 'Runtime database user (not the migration account)'
        if ([string]::IsNullOrWhiteSpace($dbUser)) { Fail 'A runtime database user is required.' }
        $script:launchStage = 'Creating ODBC connection string builder'
        $connection = [Data.Odbc.OdbcConnectionStringBuilder]::new()
        $script:launchStage = 'Assigning ODBC driver'
        $connection.set_Driver([string]$drivers[$choice - 1].Name)
        $script:launchStage = 'Assigning database host, port, schema and user'
        $connection['SERVER'] = [string]$dbHost
        $connection['PORT'] = [string]$dbPort
        $connection['DATABASE'] = [string]$schema
        $connection['UID'] = [string]$dbUser
        $script:launchStage = 'Reading database password'
        $connection['PWD'] = [string](Read-Secret 'Database password')
        $script:launchStage = 'Reading additional ODBC options'
        $options = Read-Secret 'Actual ODBC TLS/CA and other options as key=value pairs; blank uses driver defaults'
        if ($options) {
            try { $extra = [Data.Odbc.OdbcConnectionStringBuilder]::new($options) }
            catch { Fail 'Invalid additional ODBC options format.' }
            foreach ($key in $extra.Keys) {
                if (!$extra.ShouldSerialize($key)) { continue }
                if ($key -in @('DRIVER','DSN','SERVER','PORT','DATABASE','UID','USER','PWD','PASSWORD')) { Fail 'Additional ODBC options must not override the database identity or credentials.' }
                $connection[$key] = [string]$extra[$key]
            }
        }
        $options = $null
    }
    if (!$connection.ContainsKey('DATABASE') -or [string]$connection['DATABASE'] -cne $schema) { Fail 'The DB connection database must match ACTIONRPG_DB_SCHEMA.' }
    if ($connection.ConnectionString.Length -gt 32766) { Fail 'The DB connection string exceeds the supported environment limit.' }
    $script:launchStage = 'Checking installed ODBC driver'
    Assert-OdbcReady $connection
    return @{ Schema = $schema; Connection = $connection.ConnectionString }
}

function Read-TownDatabaseSettings([string] $authConnection) {
    $script:launchStage = 'Preparing Town database connection'
    $auth = [Data.Odbc.OdbcConnectionStringBuilder]::new($authConnection)
    if ($env:ACTIONRPG_TOWN_DB_CONNECTION_STRING) {
        try { $town = [Data.Odbc.OdbcConnectionStringBuilder]::new($env:ACTIONRPG_TOWN_DB_CONNECTION_STRING) }
        catch { Fail 'Invalid Town database connection string.' }
    } else {
        $town = [Data.Odbc.OdbcConnectionStringBuilder]::new($authConnection)
    }
    foreach ($key in @('SERVER','PORT','DATABASE')) {
        if (!$town.ContainsKey($key) -or [string]$town[$key] -cne [string]$auth[$key]) {
            Fail 'Auth and Town must target the same configured database host, port and schema.'
        }
    }
    $townUser = if ($town.ContainsKey('UID')) { [string]$town['UID'] } else { [string]$town['USER'] }
    if ([string]::IsNullOrWhiteSpace($townUser)) { Fail 'Town requires a valid runtime database user.' }
    if ($town.ConnectionString.Length -gt 32766) { Fail 'Town DB connection string exceeds the environment limit.' }
    Assert-OdbcReady $town
    return $town.ConnectionString
}

# Settings, encrypted credentials and TLS key files belong to this Windows user.
# Existing profiles are validated, never silently replaced or rekeyed.
function Assert-PrivatePath([string] $path) {
    $item = Get-Item -LiteralPath $path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { Fail 'Local settings must not use reparse points.' }
    $acl = Get-Acl -LiteralPath $path
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    if ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -ne $sid.Value) { Fail 'Local settings must be owned by the current Windows user.' }
    foreach ($rule in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and $rule.IdentityReference.Value -ne $sid.Value) { Fail 'Local settings permissions must allow only the current Windows user.' }
    }
}
function Write-Pem([string] $path, [byte[]] $bytes) {
    $pem = "-----BEGIN CERTIFICATE-----" + [Environment]::NewLine + [Convert]::ToBase64String($bytes, [Base64FormattingOptions]::InsertLineBreaks) + [Environment]::NewLine + "-----END CERTIFICATE-----" + [Environment]::NewLine
    [IO.File]::WriteAllText($path, $pem, [Text.Encoding]::ASCII)
}
function Assert-OpenSslReady([string] $openssl) {
    # Inspect only version/provider metadata before creating any local certificate material.
    foreach ($arguments in @('version','list -providers -provider default -provider legacy')) {
        $process = [Diagnostics.Process]::new()
        $process.StartInfo.FileName = $openssl
        $process.StartInfo.Arguments = $arguments
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.CreateNoWindow = $true
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        foreach ($name in @($process.StartInfo.EnvironmentVariables.Keys)) {
            if ($name.StartsWith('ACTIONRPG_', [StringComparison]::OrdinalIgnoreCase)) { $process.StartInfo.EnvironmentVariables.Remove($name) }
        }
        $modulePath = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $openssl) '..\lib\ossl-modules'))
        if (Test-Path -LiteralPath $modulePath -PathType Container) { $process.StartInfo.EnvironmentVariables['OPENSSL_MODULES'] = $modulePath }
        try {
            [void]$process.Start()
            $output = $process.StandardOutput.ReadToEndAsync()
            $errors = $process.StandardError.ReadToEndAsync()
            if (!$process.WaitForExit(5000)) { Fail 'OpenSSL prerequisite check timed out; no local certificates were created.' }
            if ($process.ExitCode -ne 0) { Fail 'OpenSSL version/provider check failed; OpenSSL 3 or newer with its default and legacy providers is required.' }
            if ($arguments -eq 'version') {
                $match = [regex]::Match($output.Result, '^OpenSSL ([0-9]+)\.')
                if (!$match.Success -or [int]$match.Groups[1].Value -lt 3) { Fail 'OpenSSL 3 or newer is required before local certificate setup.' }
            } elseif ($output.Result -notmatch '(?m)^\s*default\s*$' -or $output.Result -notmatch '(?m)^\s*legacy\s*$') {
                Fail 'OpenSSL default/legacy providers are unavailable; no local certificates were created.'
            }
        } catch {
            if (!$script:failureMessage) { Fail 'OpenSSL prerequisite check failed; check its executable/runtime/provider files before setup.' }
            throw
        } finally { $process.Dispose() }
    }
}
function Export-TlsKey($certificate, [string] $path, [string] $openssl) {
    $password = New-RandomKey
    $pfx = $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $password)
    $process = [Diagnostics.Process]::new()
    $process.StartInfo.FileName = $openssl
    # Windows PFX exports may use RC2; OpenSSL 3's legacy provider handles that input.
    $process.StartInfo.Arguments = 'pkcs12 -legacy -nocerts -nodes -passin env:ACTIONRPG_LOCAL_PFX_PASSWORD'
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardInput = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    foreach ($name in @($process.StartInfo.EnvironmentVariables.Keys)) {
        if ($name.StartsWith('ACTIONRPG_', [StringComparison]::OrdinalIgnoreCase)) { $process.StartInfo.EnvironmentVariables.Remove($name) }
    }
    $process.StartInfo.EnvironmentVariables['ACTIONRPG_LOCAL_PFX_PASSWORD'] = $password
    $modulePath = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $openssl) '..\lib\ossl-modules'))
    if (Test-Path -LiteralPath $modulePath -PathType Container) { $process.StartInfo.EnvironmentVariables['OPENSSL_MODULES'] = $modulePath }
    try {
        [void]$process.Start()
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.BaseStream.Write($pfx, 0, $pfx.Length)
        $process.StandardInput.Close()
        if (!$process.WaitForExit(10000)) { Fail 'OpenSSL key export timed out. No server was started.' }
        if ($process.ExitCode -ne 0 -or $output.Result -notmatch '-----BEGIN PRIVATE KEY-----') { Fail 'OpenSSL 3 private-key export failed. Check its runtime/provider files.' }
        [IO.File]::WriteAllText($path, $output.Result, [Text.Encoding]::ASCII)
    } finally {
        [Array]::Clear($pfx, 0, $pfx.Length)
        $password = $null
        $process.Dispose()
    }
}
function Get-ExistingBrokerCertificates {
    $certificates = @(Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -like '*DevServerCert*' })
    $now = Get-Date
    # Room selects by subject, so every possible match must be usable.
    foreach ($certificate in $certificates) {
        if ($certificate.Subject -cne 'CN=DevServerCert' -or !$certificate.HasPrivateKey -or $certificate.NotBefore -gt $now -or $certificate.NotAfter -le $now) {
            Fail 'An existing DevServerCert is invalid or ambiguous. Review it manually; no certificate was replaced.'
        }
    }
    return $certificates
}
function Create-LocalSettings([string] $directory, [string] $settingsPath, [string] $secretPath) {
    $script:launchStage = 'Reading Google Desktop client ID'
    Write-Host 'First local setup: use a real Google Desktop client ID and the existing MySQL database.'
    $googleId = $env:ACTIONRPG_GOOGLE_CLIENT_ID
    if (!$googleId) { $googleId = Read-Host 'Google Desktop client ID from Google Cloud Console (blank cancels)' }
    if ($googleId.Length -gt 256 -or $googleId -cnotmatch '^[A-Za-z0-9._-]+\.apps\.googleusercontent\.com\z') { Fail 'Create a real Google Desktop OAuth client ID before setup; no placeholder is accepted.' }
    $database = Read-DatabaseSettings
    $script:launchStage = 'Checking SQL files and OpenSSL prerequisites'
    $schema = $database.Schema
    $sqlDirectory = $env:ACTIONRPG_DB_MIGRATIONS_DIRECTORY
    if (!$sqlDirectory) { $sqlDirectory = Join-Path $root 'ActionRPGServer\Database\Migrations\MySQL' }
    Require-AbsolutePath $sqlDirectory 'ACTIONRPG_DB_MIGRATIONS_DIRECTORY'
    Assert-SqlFiles $sqlDirectory
    $opensslCandidates = @((Join-Path $env:ProgramFiles 'Git\mingw64\bin\openssl.exe'), (Join-Path $env:ProgramFiles 'OpenSSL-Win64\bin\openssl.exe'))
    $openssl = $opensslCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (!$openssl) { Fail 'OpenSSL 3 is required for first-time TLS key export; no tool is installed automatically.' }
    Assert-OpenSslReady $openssl
    if (!(Get-Command New-SelfSignedCertificate -ErrorAction SilentlyContinue)) { Fail 'Windows PKI certificate creation is unavailable.' }
    $existingBroker = @(Get-ExistingBrokerCertificates)
    $script:launchStage = 'Creating private local settings directory'
    if (Test-Path -LiteralPath $directory) {
        Assert-PrivatePath $directory
        if (@(Get-ChildItem -LiteralPath $directory -Force).Count) { Fail 'Incomplete local settings already exist. Review them manually; setup does not overwrite keys or settings.' }
    }
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $directoryAcl = [Security.AccessControl.DirectorySecurity]::new()
    $directoryAcl.SetOwner($sid)
    $directoryAcl.SetAccessRuleProtection($true, $false)
    $directoryAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
    [void][IO.Directory]::CreateDirectory($directory, $directoryAcl)
    # CreateDirectory applies the ACL at creation; only verify it afterward.
    Assert-PrivatePath $directory
    $keyAcl = [Security.AccessControl.FileSecurity]::new()
    $keyAcl.SetOwner($sid)
    $keyAcl.SetAccessRuleProtection($true, $false)
    $keyAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'Allow'))
    $common = @{ Type = 'Custom'; CertStoreLocation = 'Cert:\CurrentUser\My'; KeyAlgorithm = 'RSA'; KeyLength = 3072; HashAlgorithm = 'SHA256'; SecurityDescriptor = $keyAcl; NotBefore = (Get-Date).AddMinutes(-5) }
    $script:launchStage = 'Creating local CA certificate'
    $ca = New-SelfSignedCertificate @common -Subject ('CN=ActionRPG Local CA ' + [Guid]::NewGuid().ToString('N')) -FriendlyName 'ActionRPG Local CA' -KeyExportPolicy NonExportable -KeyUsage CertSign,CRLSign -TextExtension '2.5.29.19={critical}{text}ca=1' -NotAfter (Get-Date).AddYears(2)
    $certificates = @{}
    foreach ($name in @('Auth','Town','Broker')) {
        $script:launchStage = 'Preparing ' + $name + ' TLS certificate and key'
        if ($name -eq 'Broker' -and $existingBroker.Count -gt 0) {
            $certificates[$name] = $existingBroker[0]
            Write-Host 'Reusing existing valid DevServerCert certificates; none were replaced.'
            continue
        }
        $subject = 'CN=localhost'
        if ($name -eq 'Broker') { $subject = 'CN=DevServerCert' }
        $certificate = New-SelfSignedCertificate @common -Subject $subject -FriendlyName ('ActionRPG Local ' + $name) -Signer $ca -KeyExportPolicy ExportableEncrypted -KeyUsage DigitalSignature,KeyEncipherment -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.1','2.5.29.17={text}DNS=localhost&IPAddress=127.0.0.1') -NotAfter (Get-Date).AddYears(1)
        $certificates[$name] = $certificate
        if ($name -ne 'Broker') {
            Write-Pem (Join-Path $directory ($name.ToLowerInvariant() + '.cert.pem')) $certificate.RawData
            Export-TlsKey $certificate (Join-Path $directory ($name.ToLowerInvariant() + '.key.pem')) $openssl
        }
    }
    $script:launchStage = 'Writing CA bundles and installing local CA trust'
    $caPath = Join-Path $directory 'local-ca.pem'
    Write-Pem $caPath $ca.RawData
    # Auth uses this bundle for both local Auth HTTPS and Google's public JWKS HTTPS.
    $bundle = [Text.StringBuilder]::new([IO.File]::ReadAllText($caPath))
    $seen = @{}
    foreach ($store in @('Cert:\CurrentUser\Root','Cert:\LocalMachine\Root')) {
        foreach ($certificate in Get-ChildItem -LiteralPath $store) {
            if (!$seen.ContainsKey($certificate.Thumbprint) -and $certificate.NotBefore -le (Get-Date) -and $certificate.NotAfter -gt (Get-Date)) {
                [void]$bundle.AppendLine('-----BEGIN CERTIFICATE-----')
                [void]$bundle.AppendLine([Convert]::ToBase64String($certificate.RawData, [Base64FormattingOptions]::InsertLineBreaks))
                [void]$bundle.AppendLine('-----END CERTIFICATE-----')
                $seen[$certificate.Thumbprint] = $true
            }
        }
    }
    if (!$seen.Count) { Fail 'No Windows trusted public roots are available for Google HTTPS verification.' }
    $bundlePath = Join-Path $directory 'auth-ca-bundle.pem'
    [IO.File]::WriteAllText($bundlePath, $bundle.ToString(), [Text.Encoding]::ASCII)
    $rootStore = [Security.Cryptography.X509Certificates.X509Store]::new('Root', 'CurrentUser')
    try {
        $rootStore.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $rootStore.Add([Security.Cryptography.X509Certificates.X509Certificate2]::new($ca.RawData))
    } finally { $rootStore.Close() }
    $script:launchStage = 'Saving protected credentials and public settings'
    $townKey = New-RandomKey
    $townDatabaseConnection = Read-TownDatabaseSettings $database.Connection
    $secretBytes = [Text.Encoding]::UTF8.GetBytes((@{ dbConnection = $database.Connection; townDbConnection = $townDatabaseConnection; townKey = $townKey } | ConvertTo-Json -Compress))
    try { [IO.File]::WriteAllBytes($secretPath, [Security.Cryptography.ProtectedData]::Protect($secretBytes, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)) }
    finally { [Array]::Clear($secretBytes, 0, $secretBytes.Length); $database = $null; $townKey = $null }
    $settings = @{
        schemaVersion = 1
        environment = @{
            ACTIONRPG_AUTH_HOST = 'localhost'; ACTIONRPG_AUTH_TLS_CERT = (Join-Path $directory 'auth.cert.pem'); ACTIONRPG_AUTH_TLS_KEY = (Join-Path $directory 'auth.key.pem')
            ACTIONRPG_AUTH_CA_FILE = $bundlePath; ACTIONRPG_GOOGLE_CLIENT_ID = $googleId; ACTIONRPG_TOWN_ID = 'local-town'
            ACTIONRPG_TOWN_TLS_CERT = (Join-Path $directory 'town.cert.pem'); ACTIONRPG_TOWN_TLS_KEY = (Join-Path $directory 'town.key.pem')
            ACTIONRPG_DB_SCHEMA = $schema; ACTIONRPG_DB_MIGRATIONS_DIRECTORY = $sqlDirectory
        }
        client = @{ authUrl = 'https://localhost:8443'; googleClientId = $googleId; townCaFile = $caPath; playerName = ''; characterId = 1; servers = @(@{serverId='local-town';name='Local Town';hostname='localhost';port=7777}) }
        certificates = @{ root = $ca.Thumbprint; auth = $certificates.Auth.Thumbprint; town = $certificates.Town.Thumbprint; broker = $certificates.Broker.Thumbprint }
    }
    [IO.File]::WriteAllText($settingsPath, ($settings | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
    Write-Host 'Local settings saved for this Windows user. DB readiness is checked by Auth and Town; no migration was applied.'
}
function Assert-SqlFiles([string] $directory) {
    foreach ($file in @('Infrastructure\V000000__migration_history.sql','V000001__create_login_accounts.sql','V000002__create_google_login_procedure.sql','V000003__create_auth_account_status_procedure.sql','V000004__create_characters_and_skills.sql','V000005__create_character_inventory_persistence.sql','Down\V000001__create_login_accounts.sql','Down\V000002__create_google_login_procedure.sql','Down\V000003__create_auth_account_status_procedure.sql','Down\V000004__create_characters_and_skills.sql','Down\V000005__create_character_inventory_persistence.sql')) {
        Require-File (Join-Path $directory $file) ('ACTIONRPG_DB_MIGRATIONS_DIRECTORY/' + $file)
    }
}
function Initialize-LocalSettings {
    Add-Type -AssemblyName System.Data
    Add-Type -AssemblyName System.Security
    $directory = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'ActionRPG\LocalTest'))
    if ($directory.StartsWith(($root.TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase)) { Fail 'Local settings must be outside the repository.' }
    $ancestor = $directory
    while ($ancestor) {
        if (Test-Path -LiteralPath (Join-Path $ancestor '.git')) { Fail 'Local settings must not be inside a Git checkout.' }
        $ancestor = Split-Path -Parent $ancestor
    }
    $parent = Split-Path -Parent $directory
    if ((Test-Path -LiteralPath $parent) -and ((Get-Item -LiteralPath $parent).Attributes -band [IO.FileAttributes]::ReparsePoint)) { Fail 'Local settings parent must not be a reparse point.' }
    $settingsPath = Join-Path $directory 'settings.json'
    $secretPath = Join-Path $directory 'credentials.dpapi'
    if (!(Test-Path -LiteralPath $settingsPath)) {
        if (!$env:ACTIONRPG_TOWN_DB_CONNECTION_STRING -and $env:ACTIONRPG_DB_CONNECTION_STRING) {
            $env:ACTIONRPG_TOWN_DB_CONNECTION_STRING = $env:ACTIONRPG_DB_CONNECTION_STRING
        }
        $missing = @($script:required | Where-Object { [string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($_)) })
        if (!$missing.Count) {
            if ($null -ne $script:googleDesktopClientSecret) { Assert-GoogleDesktopSecret $script:googleDesktopClientSecret }
            return
        }
        Create-LocalSettings $directory $settingsPath $secretPath
    }
    Assert-PrivatePath $directory
    $script:launchStage = 'Loading and validating saved local settings'
    foreach ($path in @($settingsPath,$secretPath)) {
        Require-File $path 'local settings file'
        Assert-PrivatePath $path
        if ((Get-Item -LiteralPath $path).Length -gt 65536) { Fail 'Local settings file is too large.' }
    }
    try {
        $settings = [IO.File]::ReadAllText($settingsPath) | ConvertFrom-Json
        if ($settings.schemaVersion -ne 1) { Fail 'Unsupported local settings version.' }
        $bytes = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($secretPath), $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
        try { $secrets = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json }
        finally { [Array]::Clear($bytes, 0, $bytes.Length) }
    } catch {
        if (!$script:failureMessage) { Fail 'Local settings cannot be read by this Windows user. Review or re-enter them locally; no secret details are displayed.' }
        throw
    }
    foreach ($name in $script:required | Where-Object { $_ -notin @('ACTIONRPG_DB_CONNECTION_STRING','ACTIONRPG_TOWN_DB_CONNECTION_STRING','ACTIONRPG_TOWN_AUTH_KEY','ACTIONRPG_AUTH_TOWN_REGISTRY') }) {
        $value = $settings.environment.$name
        if ($value -isnot [string] -or [string]::IsNullOrWhiteSpace($value)) { Fail "Missing saved configuration: $name" }
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
    if ($secrets.dbConnection -isnot [string] -or $secrets.townKey -isnot [string]) { Fail 'Protected local credentials are invalid.' }
    $env:ACTIONRPG_DB_CONNECTION_STRING = $secrets.dbConnection
    # Explicit environment wins; legacy profiles reuse their protected Auth connection without rewriting the profile.
    if (!$env:ACTIONRPG_TOWN_DB_CONNECTION_STRING) {
        $env:ACTIONRPG_TOWN_DB_CONNECTION_STRING = if ($secrets.PSObject.Properties['townDbConnection']) {
            if ($secrets.townDbConnection -isnot [string] -or [string]::IsNullOrWhiteSpace($secrets.townDbConnection)) {
                Fail 'Protected Town database credentials are invalid.'
            }
            $secrets.townDbConnection
        } else { $secrets.dbConnection }
    }
    $env:ACTIONRPG_TOWN_AUTH_KEY = $secrets.townKey
    $env:ACTIONRPG_AUTH_TOWN_REGISTRY = @{ $env:ACTIONRPG_TOWN_ID = $secrets.townKey } | ConvertTo-Json -Compress
    foreach ($name in @('root','auth','town','broker')) {
        if ($settings.certificates.$name -notmatch '^[0-9A-Fa-f]{40}\z') { Fail 'Invalid saved local certificate identity.' }
        $certificatePath = 'Cert:\CurrentUser\My\' + $settings.certificates.$name
        if (!(Test-Path -LiteralPath $certificatePath)) { Fail 'A saved local certificate is missing. Review the local profile; certificates are not silently regenerated.' }
        $certificate = Get-Item -LiteralPath $certificatePath
        if (!$certificate.HasPrivateKey -or $certificate.NotBefore -gt (Get-Date) -or $certificate.NotAfter -le (Get-Date)) { Fail 'A saved local certificate is not currently valid or lacks its private key.' }
    }
    $brokerCertificates = @(Get-ExistingBrokerCertificates)
    if (!@($brokerCertificates | Where-Object { $_.Thumbprint -ceq $settings.certificates.broker }).Count) { Fail 'The saved broker certificate does not match DevServerCert.' }
    if (!(Test-Path -LiteralPath ('Cert:\CurrentUser\Root\' + $settings.certificates.root))) { Fail 'The local CA is not trusted in CurrentUser/Root.' }
    foreach ($name in @('ACTIONRPG_AUTH_TLS_KEY','ACTIONRPG_TOWN_TLS_KEY')) { Assert-PrivatePath ([Environment]::GetEnvironmentVariable($name)) }
    if ($settings.client.googleClientId -isnot [string] -or $settings.client.googleClientId -cne $env:ACTIONRPG_GOOGLE_CLIENT_ID) {
        Fail 'Saved client Google ID does not match this local server profile.'
    }
    try { Initialize-GoogleDesktopCredentials $secrets $secretPath }
    finally { $secrets = $null }
    $script:clientSettings = $settings.client
}
function Supply-ClientSettings([switch] $ValidateOnly) {
    $path = Join-Path $clientDirectory 'Assets\Data\AuthClient.json'
    foreach ($target in @($clientDirectory,(Join-Path $clientDirectory 'Assets'),(Split-Path -Parent $path),$path)) {
        if ((Test-Path -LiteralPath $target) -and ((Get-Item -LiteralPath $target).Attributes -band [IO.FileAttributes]::ReparsePoint)) { Fail 'Client runtime settings must not use reparse points.' }
    }
    if (!$script:clientSettings) {
        Require-File $path 'client runtime Assets/Data/AuthClient.json'
        if ((Get-Item -LiteralPath $path).Length -gt 32768) { Fail 'Client runtime authentication settings are too large.' }
        try { $script:clientSettings = [IO.File]::ReadAllText($path) | ConvertFrom-Json }
        catch { Fail 'Client runtime authentication settings cannot be read. Supply the public settings locally before using the environment-only launch path.' }
    }
    $client = $script:clientSettings
    $servers = @($client.servers)
    if ($client.authUrl -isnot [string] -or $client.googleClientId -isnot [string] -or $client.townCaFile -isnot [string]) { Fail 'Invalid saved client connection fields.' }
    if ($client.authUrl -cne ('https://{0}:8443' -f $env:ACTIONRPG_AUTH_HOST) -or $client.googleClientId -cne $env:ACTIONRPG_GOOGLE_CLIENT_ID -or $servers.Count -ne 1 -or $servers[0].serverId -cne $env:ACTIONRPG_TOWN_ID -or $servers[0].hostname -cne 'localhost' -or $servers[0].port -ne 7777) { Fail 'Saved public client settings do not match this local server profile.' }
    if ($servers[0].name -isnot [string] -or !$servers[0].name -or $servers[0].name.Length -gt 80 -or ($servers[0].port -isnot [int] -and $servers[0].port -isnot [long])) { Fail 'Invalid saved client town entry.' }
    if ($client.playerName -isnot [string] -or [Text.Encoding]::UTF8.GetByteCount($client.playerName) -gt 32 -or ($client.characterId -isnot [int] -and $client.characterId -isnot [long]) -or $client.characterId -notin @(1,2,3)) { Fail 'Invalid saved client player name or character ID.' }
    Require-AbsolutePath $client.townCaFile 'saved client townCaFile'
    Require-File $client.townCaFile 'saved client townCaFile'
    if ($ValidateOnly) { return }
    if (!(Test-Path -LiteralPath (Split-Path -Parent $path) -PathType Container)) { Fail 'Client runtime Assets/Data directory is missing. Build/supply client assets first.' }
    # Only public client fields go to the client's existing runtime file; source assets stay untouched.
    $public = @{ authUrl=$client.authUrl; googleClientId=$client.googleClientId; townCaFile=$client.townCaFile; servers=$servers; playerName=$client.playerName; characterId=$client.characterId }
    [IO.File]::WriteAllText($path, ($public | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
}
function Start-Client {
    $process = [Diagnostics.Process]::new()
    $process.StartInfo.FileName = Join-Path $clientDirectory 'ActionRPGClient.exe'
    $process.StartInfo.WorkingDirectory = $clientDirectory
    $process.StartInfo.UseShellExecute = $false
    foreach ($name in @($process.StartInfo.EnvironmentVariables.Keys)) {
        if ($name.StartsWith('ACTIONRPG_', [StringComparison]::OrdinalIgnoreCase)) { $process.StartInfo.EnvironmentVariables.Remove($name) }
    }
    try {
        if ($script:googleDesktopClientSecret) {
            $process.StartInfo.EnvironmentVariables['ACTIONRPG_GOOGLE_CLIENT_ID'] = [string]$script:googleDesktopClientId
            $process.StartInfo.EnvironmentVariables['ACTIONRPG_GOOGLE_DESKTOP_CLIENT_SECRET'] = [string]$script:googleDesktopClientSecret
        }
        [void]$process.Start()
    } finally {
        $process.StartInfo.EnvironmentVariables.Remove('ACTIONRPG_GOOGLE_DESKTOP_CLIENT_SECRET')
        $process.Dispose()
    }
}
function Assert-ServersAlive {
    foreach ($server in $script:startedServers) {
        $server.Process.Refresh()
        if ($server.Process.HasExited) {
            Fail ($server.Name + ' exited. Check its console window for the startup error.')
        }
    }
}
function Assert-PortsFree([int[]] $ports) {
    $listeners = [Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
    foreach ($port in $ports) {
        if ($listeners | Where-Object { $_.Port -eq $port }) { Fail "TCP port already in use: $port" }
    }
}

# Keep actual server output in its console after exit. Only its PID crosses an
# in-memory pipe; credentials are inherited through the process environment.
function Start-Server([string] $name, [string] $directory, [string[]] $arguments, [int[]] $ports) {
    Assert-ServersAlive
    Assert-PortsFree $ports
    if (Get-Process -Name $name -ErrorAction SilentlyContinue) { Fail "$name is already running." }
    $pipeName = 'ActionRPGLocal-' + [Guid]::NewGuid().ToString('N')
    $pipe = [IO.Pipes.NamedPipeServerStream]::new($pipeName, [IO.Pipes.PipeDirection]::In, 1, [IO.Pipes.PipeTransmissionMode]::Byte, [IO.Pipes.PipeOptions]::Asynchronous)
    $keep = @('ACTIONRPG_ROOM_CONTROL_KEY')
    if ($name -eq 'AuthServer') { $keep = @('ACTIONRPG_AUTH_TLS_CERT','ACTIONRPG_AUTH_TLS_KEY','ACTIONRPG_AUTH_CA_FILE','ACTIONRPG_GOOGLE_CLIENT_ID','ACTIONRPG_AUTH_TOWN_REGISTRY','ACTIONRPG_DB_CONNECTION_STRING','ACTIONRPG_DB_SCHEMA','ACTIONRPG_DB_MIGRATIONS_DIRECTORY') }
    if ($name -eq 'TownServer') { $keep += @('ACTIONRPG_TOWN_TLS_CERT','ACTIONRPG_TOWN_TLS_KEY','ACTIONRPG_AUTH_HOST','ACTIONRPG_AUTH_CA_FILE','ACTIONRPG_TOWN_ID','ACTIONRPG_TOWN_AUTH_KEY','ACTIONRPG_TOWN_DB_CONNECTION_STRING','ACTIONRPG_DB_SCHEMA','ACTIONRPG_DB_MIGRATIONS_DIRECTORY') }
    $payload = @{ Name = $name; Executable = (Join-Path $directory ($name + '.exe')); Directory = $directory; Arguments = $arguments; Pipe = $pipeName; ManagedEnvironment = ($script:required + @('ACTIONRPG_ROOM_CONTROL_KEY')); KeepEnvironment = $keep } | ConvertTo-Json -Compress
    $payload64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($payload))
    $runner = @'
$ErrorActionPreference = 'Stop'
$launch = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('PAYLOAD')) | ConvertFrom-Json
$Host.UI.RawUI.WindowTitle = $launch.Name
foreach ($name in $launch.ManagedEnvironment) {
    if ($name -notin $launch.KeepEnvironment) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
}
$pipe = $null
try {
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', $launch.Pipe, [IO.Pipes.PipeDirection]::Out)
    $pipe.Connect(5000)
    $options = @{ FilePath = $launch.Executable; WorkingDirectory = $launch.Directory; NoNewWindow = $true; PassThru = $true }
    if ($launch.Arguments.Count -gt 0) { $options.ArgumentList = $launch.Arguments }
    $native = Start-Process @options
    # Cache the handle while alive so Windows PowerShell can read the exit code afterward.
    [void]$native.Handle
    $writer = [IO.StreamWriter]::new($pipe)
    $writer.WriteLine($native.Id)
    $writer.Flush()
    $pipe.Dispose()
    $pipe = $null
    $native.WaitForExit()
    Write-Host ('[ERROR] ' + $launch.Name + ' exited with code ' + $native.ExitCode + '. See the server output above.')
} catch {
    Write-Host ('[ERROR] Unable to start or monitor ' + $launch.Name + '. Check the executable and runtime dependencies.')
} finally {
    if ($pipe) { $pipe.Dispose() }
    [void](Read-Host 'Press Enter to close this server window')
}
'@
    $runner = $runner.Replace('PAYLOAD', $payload64)
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($runner))
    $reader = $null
    try {
        $connection = $pipe.WaitForConnectionAsync()
        [void](Start-Process (Join-Path $PSHOME 'powershell.exe') -ArgumentList @('-NoProfile', '-EncodedCommand', $encoded) -WindowStyle Normal -PassThru)
        if (!$connection.Wait(8000)) { Fail "$name console did not respond. Check its window before retrying." }
        $reader = [IO.StreamReader]::new($pipe)
        $pidRead = $reader.ReadLineAsync()
        if (!$pidRead.Wait(5000)) { Fail "$name did not report startup. Check its console window." }
        $nativeId = 0
        if (![int]::TryParse($pidRead.Result, [ref] $nativeId)) { Fail "$name could not start. Check its console window." }
        try {
            $native = [Diagnostics.Process]::GetProcessById($nativeId)
            # Acquire the native handle before PID reuse; the console wrapper stays alive after exit.
            [void]$native.Handle
        } catch { Fail "$name exited during startup. Check its console window." }
        $script:startedServers += @{ Name = $name; Process = $native }
    } catch {
        if (!$script:failureMessage) { Fail "$name could not start. Check its console window." }
        throw
    } finally {
        if ($reader) { $reader.Dispose() }
        $pipe.Dispose()
    }
}
function Wait-Server([string] $name, [int[]] $ports, [int] $seconds, [string] $authUrl = '') {
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Assert-ServersAlive
        $listening = @([Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners() | ForEach-Object { $_.Port })
        $missing = @($ports | Where-Object { $_ -notin $listening })
        if ($missing.Count -eq 0) {
            if (!$authUrl) { return }
            # Local development certificates have no revocation distribution points.
            # Keep CA/hostname verification and reject known revocations; discard challenge bodies.
            $status = & $script:curl --disable --silent --ipv4 --output NUL --write-out '%{http_code}' --noproxy '*' --cacert $env:ACTIONRPG_AUTH_CA_FILE --ssl-revoke-best-effort --connect-timeout 1 --max-time 2 --request POST --header 'Content-Type: application/json' --data '{}' $authUrl
            $curlResult = $LASTEXITCODE
            Assert-ServersAlive
            if ($curlResult -eq 0 -and $status -eq '200') { return }
            if ($status -eq '503') { Fail 'AuthServer DB verification failed. Check the AuthServer console and deployed DB/SQL configuration.' }
            if ($curlResult -eq 60 -or $curlResult -eq 77) { Fail 'AuthServer TLS verification failed. Check ACTIONRPG_AUTH_HOST and ACTIONRPG_AUTH_CA_FILE.' }
            if ($curlResult -eq 0) { Fail 'AuthServer readiness request was rejected. Check its console window.' }
        }
        Start-Sleep -Milliseconds 200
    }
    Fail "$name was not ready within $seconds seconds. Check its console window."
}

try {
    $script:curl = (Get-Command curl.exe -CommandType Application -ErrorAction SilentlyContinue).Source
    if (!$script:curl) { Fail 'curl.exe is required for the verified Auth HTTPS readiness check.' }

    $required = $script:required
    Assert-PortsFree @(8443,7777,7780)
    foreach ($name in @('AuthServer','TownServer','GameRoomServer','ActionRPGClient')) {
        if (Get-Process -Name $name -ErrorAction SilentlyContinue) { Fail "$name is already running. Close it manually before setup, build or launch." }
    }
    Initialize-LocalSettings
    $invalid = $false
    foreach ($name in $required) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ([string]::IsNullOrWhiteSpace($value) -or $value.Length -gt 32766) {
            Write-Host "[ERROR] Missing or invalid configuration: $name"
            $invalid = $true
        }
    }
    if ($invalid) { Fail 'Provide the listed environment settings in this launcher process. Values are never displayed.' }
    foreach ($name in @('ACTIONRPG_AUTH_TLS_CERT', 'ACTIONRPG_AUTH_TLS_KEY', 'ACTIONRPG_AUTH_CA_FILE', 'ACTIONRPG_TOWN_TLS_CERT', 'ACTIONRPG_TOWN_TLS_KEY')) {
        $path = [Environment]::GetEnvironmentVariable($name)
        Require-AbsolutePath $path $name
        Require-File $path $name
    }
    if ($env:ACTIONRPG_GOOGLE_CLIENT_ID.Length -gt 256 -or $env:ACTIONRPG_GOOGLE_CLIENT_ID -cnotmatch '^[A-Za-z0-9._-]+\.apps\.googleusercontent\.com\z') { Fail 'Invalid configuration: ACTIONRPG_GOOGLE_CLIENT_ID' }
    if ([Text.Encoding]::UTF8.GetByteCount($env:ACTIONRPG_TOWN_ID) -gt 64) { Fail 'Invalid configuration: ACTIONRPG_TOWN_ID' }
    if ($env:ACTIONRPG_TOWN_AUTH_KEY -cnotmatch '^[0-9a-f]{64}\z') { Fail 'Invalid configuration: ACTIONRPG_TOWN_AUTH_KEY' }
    try { $registry = $env:ACTIONRPG_AUTH_TOWN_REGISTRY | ConvertFrom-Json }
    catch { Fail 'Invalid JSON: ACTIONRPG_AUTH_TOWN_REGISTRY' }
    if ($registry -isnot [Management.Automation.PSCustomObject]) { Fail 'JSON object required: ACTIONRPG_AUTH_TOWN_REGISTRY' }
    $entries = @($registry.PSObject.Properties)
    if ($entries.Count -eq 0 -or $entries.Count -gt 128) { Fail 'Invalid entry count: ACTIONRPG_AUTH_TOWN_REGISTRY' }
    foreach ($entry in $entries) {
        if (!$entry.Name -or [Text.Encoding]::UTF8.GetByteCount($entry.Name) -gt 64 -or $entry.Value -isnot [string] -or $entry.Value -cnotmatch '^[0-9a-f]{64}\z') { Fail 'Invalid town identity: ACTIONRPG_AUTH_TOWN_REGISTRY' }
    }
    $matching = @($entries | Where-Object { $_.Name -ceq $env:ACTIONRPG_TOWN_ID -and $_.Value -ceq $env:ACTIONRPG_TOWN_AUTH_KEY })
    if ($matching.Count -ne 1) { Fail 'ACTIONRPG_TOWN_ID/ACTIONRPG_TOWN_AUTH_KEY do not match ACTIONRPG_AUTH_TOWN_REGISTRY.' }
    if ($env:ACTIONRPG_DB_SCHEMA.Length -gt 64) { Fail 'Invalid configuration: ACTIONRPG_DB_SCHEMA' }
    Add-Type -AssemblyName System.Data
    try { $connection = [Data.Odbc.OdbcConnectionStringBuilder]::new($env:ACTIONRPG_DB_CONNECTION_STRING) }
    catch { Fail 'Invalid connection string format: ACTIONRPG_DB_CONNECTION_STRING' }
    Assert-OdbcReady $connection
    $null = Read-TownDatabaseSettings $env:ACTIONRPG_DB_CONNECTION_STRING
    $sqlDirectory = $env:ACTIONRPG_DB_MIGRATIONS_DIRECTORY
    Require-AbsolutePath $sqlDirectory 'ACTIONRPG_DB_MIGRATIONS_DIRECTORY'
    if (!(Test-Path -LiteralPath $sqlDirectory -PathType Container)) { Fail 'Directory required: ACTIONRPG_DB_MIGRATIONS_DIRECTORY' }
    Assert-SqlFiles $sqlDirectory
    # This launcher starts a local Auth instance; never accidentally probe a remote one.
    if ($env:ACTIONRPG_AUTH_HOST -notmatch '^[A-Za-z0-9.-]{1,253}\z') { Fail 'Hostname or IPv4 address required: ACTIONRPG_AUTH_HOST' }
    try {
        $resolution = [Net.Dns]::GetHostAddressesAsync($env:ACTIONRPG_AUTH_HOST)
        if (!$resolution.Wait(3000)) { Fail 'ACTIONRPG_AUTH_HOST resolution timed out.' }
        $addresses = $resolution.Result
    } catch {
        if (!$script:failureMessage) { Fail 'Unable to resolve ACTIONRPG_AUTH_HOST.' }
        throw
    }
    if (!$addresses -or @($addresses | Where-Object { ![Net.IPAddress]::IsLoopback($_) }).Count -gt 0 -or !@($addresses | Where-Object { $_.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork }).Count) { Fail 'ACTIONRPG_AUTH_HOST must resolve to local loopback IPv4 for this launcher.' }
    $authUrl = 'https://{0}:8443/v1/challenges' -f $env:ACTIONRPG_AUTH_HOST

    $script:launchStage = 'Preserving validated public client settings before build'
    Supply-ClientSettings -ValidateOnly
    try { Build-LocalProjects }
    finally {
        # A partially failed build can already have copied the source's empty AuthClient.json.
        if (Test-Path -LiteralPath (Join-Path $clientDirectory 'Assets\Data') -PathType Container) { Supply-ClientSettings }
    }
    $script:launchStage = 'Checking executable/runtime files after build'
    foreach ($name in @('AuthServer', 'TownServer')) { Require-File (Join-Path $serverDirectory ($name + '.exe')) ($name + ' Debug x64 executable') }
    Require-File (Join-Path $roomDirectory 'GameRoomServer.exe') 'GameRoomServer Debug x64 executable'
    foreach ($dependency in @('libssl-3-x64.dll','libcrypto-3-x64.dll')) {
        Require-File (Join-Path $roomDirectory $dependency) ('GameRoomServer runtime ' + $dependency + '; rebuild GameRoomServer Debug x64')
    }
    Require-File (Join-Path $clientDirectory 'ActionRPGClient.exe') 'ActionRPGClient Debug x64 executable'
    Require-File (Join-Path $roomDirectory 'ServerOptionFile\CoreOption.txt') 'room runtime ServerOptionFile/CoreOption.txt'
    $brokerPath = Join-Path $roomDirectory 'ServerOptionFile\SessionBrokerOption.txt'
    Require-File $brokerPath 'room runtime ServerOptionFile/SessionBrokerOption.txt'
    $brokerBytes = [IO.File]::ReadAllBytes($brokerPath)
    if ($brokerBytes.Length -lt 2 -or $brokerBytes[0] -ne 255 -or $brokerBytes[1] -ne 254) { Fail 'Room SessionBrokerOption.txt requires UTF-16 LE BOM.' }
    $brokerText = [Text.Encoding]::Unicode.GetString($brokerBytes, 2, $brokerBytes.Length - 2)
    $portMatch = [regex]::Match($brokerText, '(?m)^\s*SESSION_BROKER_PORT\s*=\s*([0-9]+)\s*$')
    $hostMatch = [regex]::Match($brokerText, '(?m)^\s*CORE_IP\s*=\s*"(127\.0\.0\.1)"\s*$')
    $brokerPort = 0
    if (!$hostMatch.Success -or !$portMatch.Success -or ![int]::TryParse($portMatch.Groups[1].Value, [ref] $brokerPort) -or $brokerPort -lt 1 -or $brokerPort -gt 65535) { Fail 'Room broker options must provide local CORE_IP and a valid SESSION_BROKER_PORT.' }
    $ports = @(8443, 7777, 7780, $brokerPort)
    if (@($ports | Select-Object -Unique).Count -ne 4) { Fail 'Auth, Town, RoomControl and room broker TCP ports must be distinct.' }
    Assert-PortsFree $ports
    foreach ($name in @('AuthServer', 'TownServer', 'GameRoomServer')) {
        if (Get-Process -Name $name -ErrorAction SilentlyContinue) { Fail "$name is already running. Close it manually before using this launcher." }
    }

    # Fresh shared secret stays in memory and is inherited by Town and Room.
    $env:ACTIONRPG_ROOM_CONTROL_KEY = New-RandomKey
    $script:launchStage = 'Supplying public client settings'
    Supply-ClientSettings

    Write-Host '[1/5] Starting AuthServer...'
    $script:launchStage = 'Starting AuthServer and checking readiness'
    Start-Server 'AuthServer' $serverDirectory @() @(8443)
    Wait-Server 'AuthServer' @(8443) 25 $authUrl
    Write-Host '[2/5] Starting TownServer...'
    $script:launchStage = 'Starting TownServer and checking readiness'
    Start-Server 'TownServer' $serverDirectory @('7777', '4', '7780') @(7777, 7780)
    Wait-Server 'TownServer' @(7777, 7780) 10
    Write-Host '[3/5] Starting GameRoomServer...'
    $script:launchStage = 'Starting GameRoomServer and checking readiness'
    Start-Server 'GameRoomServer' $roomDirectory @('127.0.0.1', '7780', '1', '1000', '4') @($brokerPort)
    Wait-Server 'GameRoomServer' @($brokerPort) 10
    foreach ($number in @(1, 2)) {
        $script:launchStage = 'Starting client ' + $number
        Assert-ServersAlive
        Write-Host ('[' + ($number + 3) + '/5] Starting client ' + $number + '...')
        Start-Client
    }
    Assert-ServersAlive
    Write-Host 'Servers and two clients started. Log in manually with two different Google accounts.'
    Write-Host 'Close each server/client window manually when finished.'
    exit 0
} catch {
    # Only fixed launcher diagnostics are reported; parser/driver exceptions may contain secrets.
    if ($script:failureMessage) { Write-Host ('[ERROR] ' + $script:failureMessage) }
    else {
        Write-Host ('[ERROR] Launcher failed at stage: ' + $script:launchStage)
        # Exception messages, source lines and argument values can contain credentials.
        Write-Host ('[ERROR] PowerShell line: ' + $_.InvocationInfo.ScriptLineNumber + '; exception type: ' + $_.Exception.GetType().FullName)
        $cause = $_.Exception.GetBaseException()
        Write-Host ('[ERROR] Underlying exception type: ' + $cause.GetType().FullName + '; HRESULT: ' + ('0x{0:X8}' -f $cause.HResult))
        Write-Host '[ERROR] Check settings, executable/runtime files and server console windows.'
    }
    Write-Host 'Any servers already started are left running. Close their windows manually before retrying.'
    exit 1
} finally {
    $script:googleDesktopClientSecret = $null
}
