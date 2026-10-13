[CmdletBinding()]
param([ValidateRange(1, 65535)][int]$ReleaseNumber = 19, [string]$Compiler)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not $Compiler) { $Compiler = Join-Path $taskRoot 'build\packaging-tools\inno-7.1.0\tools\ISCC.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
if ((Get-AuthenticodeSignature -LiteralPath $Compiler).Status -ne 'Valid') { throw 'Unsigned compiler.' }
$taskSmoke = Join-Path $taskRoot "build\installer-v$ReleaseNumber\smoke"
$taskInstall = Join-Path $taskSmoke '测试安装'
$taskFixtureRegistry = 'Software\AggregateAudioPackagingTest\Registry'
$taskClsid = '{D3AA79B2-9D91-4B6F-A2B1-55CE84C2A43D}'
$taskAsio = "$taskFixtureRegistry\SOFTWARE\ASIO\Aggregate Audio"
$taskCom = "$taskFixtureRegistry\SOFTWARE\Classes\CLSID\$taskClsid"
$taskCurrentUser = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)
function Assert-That([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Get-ProductionRegistration {
    $taskMachine = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $taskAsioKey = $taskMachine.OpenSubKey('SOFTWARE\ASIO\Aggregate Audio')
        $taskComKey = $taskMachine.OpenSubKey("SOFTWARE\Classes\CLSID\$taskClsid\InprocServer32")
        try {
            [ordered]@{
                CLSID = if ($taskAsioKey) { $taskAsioKey.GetValue('CLSID') } else { $null }
                Description = if ($taskAsioKey) { $taskAsioKey.GetValue('Description') } else { $null }
                Path = if ($taskComKey) { $taskComKey.GetValue('') } else { $null }
                ThreadingModel = if ($taskComKey) { $taskComKey.GetValue('ThreadingModel') } else { $null }
            } | ConvertTo-Json -Compress
        } finally {
            if ($taskAsioKey) { $taskAsioKey.Dispose() }
            if ($taskComKey) { $taskComKey.Dispose() }
        }
    } finally { $taskMachine.Dispose() }
}
function Read-FixtureValue([string]$Key, [string]$Name) {
    $taskOpenedKey = $taskCurrentUser.OpenSubKey($Key)
    try { if ($taskOpenedKey) { return $taskOpenedKey.GetValue($Name) }; return $null }
    finally { if ($taskOpenedKey) { $taskOpenedKey.Dispose() } }
}
function Invoke-Fixture([string]$Path, [string]$LogName, [switch]$Installing) {
    $taskArguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/LOG="' + (Join-Path $taskSmoke $LogName) + '"'))
    if ($Installing) { $taskArguments += @('/LANG=english', ('/DIR="' + $taskInstall + '"')) }
    $taskProcess = Start-Process -FilePath $Path -ArgumentList $taskArguments -WindowStyle Hidden -PassThru -Wait
    return $taskProcess.ExitCode
}

$taskOriginal = Get-ProductionRegistration
$taskPreexistingKey = $taskCurrentUser.OpenSubKey($taskFixtureRegistry)
if ($taskPreexistingKey) { $taskPreexistingKey.Dispose(); throw 'Fixture registry already exists; inspect it first.' }
if (Test-Path -LiteralPath $taskInstall) { throw 'Fixture install folder already exists; inspect it first.' }
New-Item -ItemType Directory -Path $taskSmoke -Force | Out-Null
try {
    & $Compiler '--quiet' '--define=PackagingTest' "--define=ReleaseNumber=$ReleaseNumber" "--define=TestInstallDir=$taskInstall" `
        "--define=PayloadDir=$(Join-Path $taskRoot "build\installer-v$ReleaseNumber\payload")" `
        "--define=OutputDir=$taskSmoke" (Join-Path $PSScriptRoot 'AggregateAudio.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Fixture compilation failed.' }
    $taskSetup = Join-Path $taskSmoke 'AggregateAudio-PackagingFixture.exe'
    Assert-That ((Invoke-Fixture $taskSetup 'install.log' -Installing) -eq 0) 'Fixture installation failed.'
    $taskManifest = Import-PowerShellDataFile (Join-Path $PSScriptRoot "verified-v$ReleaseNumber.psd1")
    foreach ($taskName in $taskManifest.Keys) {
        Assert-That ((Get-FileHash -LiteralPath (Join-Path $taskInstall $taskName)).Hash -eq $taskManifest[$taskName]) "Installed hash mismatch: $taskName"
    }
    Assert-That ((Read-FixtureValue $taskAsio 'CLSID') -eq $taskClsid) 'ASIO CLSID mismatch.'
    Assert-That ((Read-FixtureValue $taskAsio 'Description') -eq 'Aggregate Audio') 'Host-visible ASIO name differs from the product name.'
    Assert-That ((Read-FixtureValue $taskCom '') -eq 'Aggregate Audio') 'COM display name differs from the product name.'
    Assert-That ((Read-FixtureValue "$taskCom\InprocServer32" '') -eq (Join-Path $taskInstall 'AggregateAudio.dll')) 'COM path mismatch (Unicode path test).'
    Assert-That ((Read-FixtureValue "$taskCom\InprocServer32" 'ThreadingModel') -eq 'Apartment') 'COM threading mismatch.'
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $taskInstall 'vc_redist.x64.exe'))) 'Prerequisite left in application directory.'
    Write-Output "PASS: install, exact v$ReleaseNumber payload hashes, Unicode COM registration, no prerequisite leftovers"

    $taskUninstaller = Join-Path $taskInstall 'unins000.exe'
    $taskLockedFile = [IO.File]::Open((Join-Path $taskInstall 'AggregateAudio.dll'), 'Open', 'Read', 'None')
    try {
        Assert-That ((Invoke-Fixture $taskSetup 'locked-update.log' -Installing) -ne 0) 'Update accepted a locked DLL.'
        Assert-That ((Invoke-Fixture $taskUninstaller 'locked-uninstall.log') -ne 0) 'Uninstall accepted a locked DLL.'
        Assert-That ((Read-FixtureValue "$taskCom\InprocServer32" '') -eq (Join-Path $taskInstall 'AggregateAudio.dll')) 'Lock failure changed registration.'
    } finally { $taskLockedFile.Dispose() }
    Write-Output 'PASS: upgrade and uninstall reject locked driver files without closing applications'

    $taskLegacyAsio = $taskCurrentUser.OpenSubKey($taskAsio, $true)
    $taskLegacyCom = $taskCurrentUser.OpenSubKey($taskCom, $true)
    try {
        $taskLegacyAsio.SetValue('Description', 'Multi-device audio driver for ASIO hosts')
        $taskLegacyCom.SetValue('', 'Multi-device audio driver for ASIO hosts')
    } finally { $taskLegacyAsio.Dispose(); $taskLegacyCom.Dispose() }
    Assert-That ((Invoke-Fixture $taskSetup 'update.log' -Installing) -eq 0) 'Unlocked update failed.'
    Assert-That ((Read-FixtureValue $taskAsio 'Description') -eq 'Aggregate Audio') 'Upgrade kept the legacy ASIO display name.'
    Assert-That ((Read-FixtureValue $taskCom '') -eq 'Aggregate Audio') 'Upgrade kept the legacy COM display name.'
    Assert-That ((Invoke-Fixture $taskUninstaller 'uninstall.log') -eq 0) 'Uninstall failed.'
    Assert-That ($null -eq (Read-FixtureValue $taskAsio 'CLSID')) 'ASIO registration survived uninstall.'
    Assert-That ($null -eq (Read-FixtureValue "$taskCom\InprocServer32" '')) 'COM registration survived uninstall.'
    foreach ($taskName in $taskManifest.Keys) {
        Assert-That (-not (Test-Path -LiteralPath (Join-Path $taskInstall $taskName))) "Uninstall left $taskName"
    }
    Write-Output 'PASS: update and normal uninstall remove installed binaries and owned registration'

    Assert-That ((Invoke-Fixture $taskSetup 'reinstall.log' -Installing) -eq 0) 'Reinstall failed.'
    $taskAlternatePath = Join-Path $taskSmoke 'another-install\AggregateAudio.dll'
    $taskReassigned = $taskCurrentUser.OpenSubKey("$taskCom\InprocServer32", $true)
    try { $taskReassigned.SetValue('', $taskAlternatePath, [Microsoft.Win32.RegistryValueKind]::String) }
    finally { $taskReassigned.Dispose() }
    Assert-That ((Invoke-Fixture $taskUninstaller 'reassigned-uninstall.log') -eq 0) 'Reassigned uninstall failed.'
    Assert-That ((Read-FixtureValue "$taskCom\InprocServer32" '') -eq $taskAlternatePath) 'Uninstall deleted a different installation registration.'
    Assert-That ((Read-FixtureValue $taskAsio 'CLSID') -eq $taskClsid) 'Uninstall deleted the different installation ASIO entry.'
    Write-Output 'PASS: uninstall preserves registration reassigned to another installation'
    Assert-That ((Get-ProductionRegistration) -eq $taskOriginal) 'Production ASIO registration changed.'
    Write-Output 'PASS: production ASIO registration unchanged'
} finally {
    # This exact HKCU test subtree is owned by this invocation; never delete production keys.
    $taskCurrentUser.DeleteSubKeyTree($taskFixtureRegistry, $false)
    $taskCurrentUser.Dispose()
}
