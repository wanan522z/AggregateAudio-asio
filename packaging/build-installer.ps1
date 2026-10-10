[CmdletBinding()]
param(
    [ValidateRange(1, 65535)][int]$ReleaseNumber = 12,
    [string]$BinaryDirectory,
    [string]$VerifiedManifest,
    [string]$Compiler,
    [string]$RuntimeInstaller
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not $BinaryDirectory) { $BinaryDirectory = Join-Path $taskRoot "build\release-v$ReleaseNumber\Release" }
if (-not $VerifiedManifest) { $VerifiedManifest = Join-Path $PSScriptRoot "verified-v$ReleaseNumber.psd1" }
if (-not $Compiler) { $Compiler = Join-Path $taskRoot 'build\packaging-tools\inno-7.1.0\tools\ISCC.exe' }
if (-not $RuntimeInstaller) {
    $RuntimeInstaller = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\vc_redist.x64.exe'
}
$BinaryDirectory = (Resolve-Path -LiteralPath $BinaryDirectory).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$RuntimeInstaller = (Resolve-Path -LiteralPath $RuntimeInstaller).Path
$taskCompilerSig = Get-AuthenticodeSignature -LiteralPath $Compiler
if ($taskCompilerSig.Status -ne 'Valid' -or $taskCompilerSig.SignerCertificate.Subject -notmatch 'Pyrsys B.V.') {
    throw 'Use the authentic signed Inno Setup compiler.'
}
$taskCompilerVersion = (& $Compiler '--version' | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $taskCompilerVersion -ne '7.1.0') { throw 'Use Inno Setup 7.1.0 for this package.' }
$taskRuntimeSig = Get-AuthenticodeSignature -LiteralPath $RuntimeInstaller
if ($taskRuntimeSig.Status -ne 'Valid' -or $taskRuntimeSig.SignerCertificate.Subject -notmatch 'CN=Microsoft Corporation,') {
    throw 'The runtime installer must have a valid Microsoft signature.'
}
if ([version](Get-Item -LiteralPath $RuntimeInstaller).VersionInfo.FileVersion -lt [version]'14.44.35112.0') {
    throw 'The x64 Visual C++ runtime installer is too old.'
}

function Get-PeMachine([string]$Path) {
    $taskReader = [IO.BinaryReader]::new([IO.File]::OpenRead($Path))
    try {
        if ($taskReader.ReadUInt16() -ne 0x5A4D) { throw "Invalid PE file: $Path" }
        $taskReader.BaseStream.Position = 0x3C
        $taskPeOffset = $taskReader.ReadUInt32()
        $taskReader.BaseStream.Position = $taskPeOffset
        if ($taskReader.ReadUInt32() -ne 0x00004550) { throw "Invalid PE signature: $Path" }
        return $taskReader.ReadUInt16()
    } finally { $taskReader.Dispose() }
}

$taskManifest = Import-PowerShellDataFile $VerifiedManifest
$taskPayload = Join-Path $taskRoot "build\installer-v$ReleaseNumber\payload"
$taskOutput = Join-Path $taskRoot 'dist'
New-Item -ItemType Directory -Path $taskPayload, $taskOutput -Force | Out-Null
$taskHashLines = foreach ($taskName in ($taskManifest.Keys | Sort-Object)) {
    $taskSource = Join-Path $BinaryDirectory $taskName
    $taskHash = (Get-FileHash -LiteralPath $taskSource -Algorithm SHA256).Hash
    if ($taskHash -ne $taskManifest[$taskName]) { throw "Unverified release file: $taskName" }
    if ((Get-PeMachine $taskSource) -ne 0x8664) { throw "Not an x64 release file: $taskName" }
    Copy-Item -LiteralPath $taskSource -Destination (Join-Path $taskPayload $taskName) -Force
    if ((Get-FileHash -LiteralPath (Join-Path $taskPayload $taskName) -Algorithm SHA256).Hash -ne $taskHash) {
        throw "Payload copy mismatch: $taskName"
    }
    "$taskHash  $taskName"
}
$taskHashLines | Set-Content -LiteralPath (Join-Path $taskPayload 'binaries.sha256') -Encoding ascii
Copy-Item -LiteralPath $RuntimeInstaller -Destination (Join-Path $taskPayload 'vc_redist.x64.exe') -Force
if ((Get-FileHash -LiteralPath $RuntimeInstaller).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $taskPayload 'vc_redist.x64.exe')).Hash) { throw 'Runtime copy mismatch.' }

& $Compiler '--quiet' "--define=ReleaseNumber=$ReleaseNumber" "--define=PayloadDir=$taskPayload" "--define=OutputDir=$taskOutput" (Join-Path $PSScriptRoot 'AggregateAudio.iss')
if ($LASTEXITCODE -ne 0) { throw "Installer compiler failed: $LASTEXITCODE" }
$taskInstaller = Join-Path $taskOutput "AggregateAudio-v$ReleaseNumber-x64-Setup.exe"
$taskInstallerHash = (Get-FileHash -LiteralPath $taskInstaller -Algorithm SHA256).Hash
"$taskInstallerHash  AggregateAudio-v$ReleaseNumber-x64-Setup.exe" |
    Set-Content -LiteralPath (Join-Path $taskOutput "AggregateAudio-v$ReleaseNumber-x64-Setup.sha256") -Encoding ascii
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'INSTALL-zh-CN.txt') -Destination $taskOutput -Force
[ordered]@{
    Version = "v$ReleaseNumber"; Architecture = 'x64'; Installer = $taskInstaller
    InstallerSHA256 = $taskInstallerHash; InstallerBytes = (Get-Item -LiteralPath $taskInstaller).Length
    Payload = $taskManifest; RuntimeVersion = (Get-Item -LiteralPath $RuntimeInstaller).VersionInfo.FileVersion
    RuntimeSHA256 = (Get-FileHash -LiteralPath $RuntimeInstaller).Hash
    CompilerVersion = $taskCompilerVersion
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $taskOutput 'package-manifest.json') -Encoding utf8
Get-Item -LiteralPath $taskInstaller | Select-Object FullName, Length
Write-Output "SHA256: $taskInstallerHash"
