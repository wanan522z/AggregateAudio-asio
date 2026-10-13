#ifndef PayloadDir
  #error Supply PayloadDir with the verified release files.
#endif
#ifndef OutputDir
  #error Supply OutputDir.
#endif
#ifndef ReleaseNumber
  #define ReleaseNumber 19
#endif
#define DriverClsid "{D3AA79B2-9D91-4B6F-A2B1-55CE84C2A43D}"
#ifdef PackagingTest
  #define RegistryRoot "HKCU64"
  #define RegistryBase "Software\AggregateAudioPackagingTest\Registry\"
  #define InstallerAppId "{C784C799-ED9C-4DCE-9F51-52B9C670EA41}"
  #define InstallerName "Aggregate Audio packaging fixture"
  #define InstallerFilename "AggregateAudio-PackagingFixture"
#else
  #define RegistryRoot "HKLM64"
  #define RegistryBase ""
  #define InstallerAppId "{930E7EB1-4D72-4577-A167-04617163A305}"
  #define InstallerName "Aggregate Audio"
  #define InstallerFilename "AggregateAudio-v" + Str(ReleaseNumber) + "-x64-Setup"
#endif

[Setup]
AppId={{#InstallerAppId}
AppName={#InstallerName}
AppVersion={#ReleaseNumber}.0
AppVerName=Aggregate Audio v{#ReleaseNumber} (test build)
VersionInfoVersion={#ReleaseNumber}.0.0.0
VersionInfoDescription=Aggregate Audio x64 installer
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.10240
#ifdef PackagingTest
DefaultDirName={#TestInstallDir}
PrivilegesRequired=lowest
CreateUninstallRegKey=no
#else
DefaultDirName={autopf}\Aggregate Audio
PrivilegesRequired=admin
CreateUninstallRegKey=yes
#endif
DefaultGroupName=Aggregate Audio
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename={#InstallerFilename}
SetupIconFile=..\assets\aggregate-audio.ico
UninstallDisplayIcon={app}\AggregateAudioConfig.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
RestartApplications=no
AllowNoIcons=yes
Uninstallable=yes
InfoBeforeFile=INSTALL-zh-CN.txt

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesesimplified.DesktopIcon=创建桌面快捷方式
english.DesktopIcon=Create a desktop shortcut
chinesesimplified.OpenPanel=打开 Aggregate Audio 设置面板
english.OpenPanel=Open the Aggregate Audio settings panel
chinesesimplified.Guide=安装与测试说明
english.Guide=Installation and testing guide
chinesesimplified.Busy=Aggregate Audio 文件正在使用或不可写入。请关闭使用此 ASIO 的 DAW、设置面板及诊断工具后重试。
english.Busy=Aggregate Audio files are in use or not writable. Close DAWs using this ASIO, the settings panel, and diagnostics, then retry.
chinesesimplified.RuntimeError=微软 Visual C++ x64 运行库安装失败（错误码 %1）。安装已停止，请查看安装日志。
english.RuntimeError=Microsoft Visual C++ x64 runtime installation failed (code %1). Setup has stopped; see the setup log.

[Tasks]
#ifndef PackagingTest
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
#endif

[Files]
Source: "{#PayloadDir}\AggregateAudio.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\AggregateAudioConfig.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\AggregateAudioDiagnostics.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\vc_redist.x64.exe"; Flags: dontcopy
Source: "INSTALL-zh-CN.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\third_party\asio-sdk\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "Steinberg-ASIO-SDK.txt"; Flags: ignoreversion
Source: "Inno-Setup-LICENSE.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#PayloadDir}\binaries.sha256"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: {#RegistryRoot}; Subkey: "{#RegistryBase}SOFTWARE\ASIO\Aggregate Audio"; ValueType: string; ValueName: "CLSID"; ValueData: "{{#DriverClsid}"
Root: {#RegistryRoot}; Subkey: "{#RegistryBase}SOFTWARE\ASIO\Aggregate Audio"; ValueType: string; ValueName: "Description"; ValueData: "Aggregate Audio"
Root: {#RegistryRoot}; Subkey: "{#RegistryBase}SOFTWARE\Classes\CLSID\{{#DriverClsid}"; ValueType: string; ValueName: ""; ValueData: "Aggregate Audio"
Root: {#RegistryRoot}; Subkey: "{#RegistryBase}SOFTWARE\Classes\CLSID\{{#DriverClsid}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\AggregateAudio.dll"
Root: {#RegistryRoot}; Subkey: "{#RegistryBase}SOFTWARE\Classes\CLSID\{{#DriverClsid}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"

[Icons]
#ifndef PackagingTest
Name: "{group}\Aggregate Audio"; Filename: "{app}\AggregateAudioConfig.exe"
Name: "{group}\{cm:Guide}"; Filename: "{app}\INSTALL-zh-CN.txt"
Name: "{autodesktop}\Aggregate Audio"; Filename: "{app}\AggregateAudioConfig.exe"; Tasks: desktopicon
#endif

[Run]
#ifndef PackagingTest
Filename: "{app}\AggregateAudioConfig.exe"; Description: "{cm:OpenPanel}"; Flags: nowait postinstall skipifsilent runasoriginaluser unchecked
#endif

[Code]
const
  AsioKey = '{#RegistryBase}SOFTWARE\ASIO\Aggregate Audio';
  ComKey = '{#RegistryBase}SOFTWARE\Classes\CLSID\{#DriverClsid}';

function OpenExistingFile(const FileName: String; Access, Sharing: LongWord;
  Security: NativeUInt; Creation, Attributes: LongWord; Template: NativeUInt): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseFileHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function FilesAvailable: Boolean;
var
  Names: TArrayOfString;
  I: Integer;
  Handle: THandle;
  Path: String;
begin
  Result := True;
  Names := ['AggregateAudio.dll', 'AggregateAudioConfig.exe', 'AggregateAudioDiagnostics.exe'];
  for I := 0 to GetArrayLength(Names) - 1 do begin
    Path := ExpandConstant('{app}\') + Names[I];
    if FileExists(Path) then begin
      { Sharing=0 detects both running EXEs and DLLs mapped into a DAW. }
      Handle := OpenExistingFile(Path, $C0000000, 0, 0, 3, $80, 0);
      if Handle = THandle(-1) then begin
        Log('Cannot exclusively open ' + Path);
        Result := False;
        Exit;
      end;
      CloseFileHandle(Handle);
    end;
  end;
end;

function RuntimeInView(RootKey: Integer): Boolean;
var
  Installed, Major, Minor, Build: Cardinal;
  Key: String;
begin
  Key := 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  Result := RegQueryDWordValue(RootKey, Key, 'Installed', Installed) and
    (Installed = 1) and RegQueryDWordValue(RootKey, Key, 'Major', Major) and
    RegQueryDWordValue(RootKey, Key, 'Minor', Minor) and
    RegQueryDWordValue(RootKey, Key, 'Bld', Build);
  if Result then
    Result := (Major > 14) or ((Major = 14) and
      ((Minor > 44) or ((Minor = 44) and (Build >= 35112))));
end;

function RuntimePresent: Boolean;
begin
  { Redistributable versions have used both registry views for x64 detection. }
  Result := RuntimeInView(HKLM64) or RuntimeInView(HKLM32);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Result := '';
  if not FilesAvailable then begin
    Result := CustomMessage('Busy');
    Exit;
  end;
#ifndef PackagingTest
  if not RuntimePresent then begin
    ExtractTemporaryFile('vc_redist.x64.exe');
    if not Exec(ExpandConstant('{tmp}\vc_redist.x64.exe'),
      '/install /quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, Code) then begin
      Result := FmtMessage(CustomMessage('RuntimeError'), [IntToStr(Code)]);
      Exit;
    end;
    Log('Visual C++ runtime exit code: ' + IntToStr(Code));
    if (Code <> 0) and (Code <> 3010) and (Code <> 1638) then
      Result := FmtMessage(CustomMessage('RuntimeError'), [IntToStr(Code)])
    else if not RuntimePresent then
      Result := FmtMessage(CustomMessage('RuntimeError'), [IntToStr(Code)])
    else if Code = 3010 then
      NeedsRestart := True;
  end;
#endif
end;

function InitializeUninstall: Boolean;
begin
  Result := FilesAvailable;
  if not Result then SuppressibleMsgBox(CustomMessage('Busy'), mbError, MB_OK, IDOK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  RegisteredPath, RegisteredClsid: String;
begin
  if CurUninstallStep = usUninstall then begin
    { A later manual registration must not be removed by this uninstaller. }
    if RegQueryStringValue({#RegistryRoot}, ComKey + '\InprocServer32', '', RegisteredPath) and
      (CompareText(RegisteredPath, ExpandConstant('{app}\AggregateAudio.dll')) = 0) then begin
      if RegQueryStringValue({#RegistryRoot}, AsioKey, 'CLSID', RegisteredClsid) and
        (CompareText(RegisteredClsid, '{#DriverClsid}') = 0) then
        RegDeleteKeyIncludingSubkeys({#RegistryRoot}, AsioKey);
      RegDeleteKeyIncludingSubkeys({#RegistryRoot}, ComKey);
    end;
  end;
end;
