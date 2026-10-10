# Windows x64 installer

The installer packages the hash-verified v12 release without rebuilding audio
code. It embeds the unchanged Microsoft-signed x64 VC++ redistributable so
ASIO loading also works in hosts on a PC without developer tools. Runtime
installation happens before application files and ASIO registration.

Build with the signed Inno Setup 7.1.0 compiler:

```powershell
.\packaging\build-installer.ps1
```

Optional arguments: `-ReleaseNumber`, `-BinaryDirectory`, `-VerifiedManifest`,
`-Compiler`, `-RuntimeInstaller`. The default release directory is
`build\release-v12\Release`, verified against `packaging\verified-v12.psd1`.
The default compiler is the signed binary from the `Tools.InnoSetup` 7.1.0
NuGet package extracted under `build\packaging-tools\inno-7.1.0`.
The package is an unofficial redistribution; the compiler's Authenticode
signature is checked before execution. The original Inno Setup tools are at
<https://github.com/jrsoftware/issrc/releases/tag/is-7_1_0>.

Output: `dist\AggregateAudio-v12-x64-Setup.exe`, installer SHA256, package
manifest and Chinese installation guide. The installer and product binaries
are unsigned. No user audio profile, developer tools, test executables,
kernel drivers, services or autostart entries are installed.

Installation writes the Unicode 64-bit HKLM ASIO and COM registry entries
directly instead of using the SDK's ANSI registration helper. DAWs and the
configurator must be closed before upgrades or uninstall. Setup does not
automatically close applications. Uninstall removes registration only while
it still points to this installation, and preserves user audio profiles and
the shared Microsoft runtime.

The `PackagingTest` define builds a separate, non-elevated fixture using an
isolated HKCU registry subtree, no shortcuts, no uninstall-list entry and no
runtime installation. It is never distributed. This verifies installation,
payload integrity, file-lock protection, and uninstall without changing the
working system ASIO registration.
