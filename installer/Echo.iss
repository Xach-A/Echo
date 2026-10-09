; Echo installer (Inno Setup 6). Built by installer/package.ps1.
;
; Installs per user by default (no administrator rights). It installs ONLY
; the Echo application: no drivers, no services, no background updaters.
; Virtual audio cable drivers are separate products with their own
; installers; see docs\VIRTUAL_DEVICES.md.

#ifndef AppName
  #define AppName "Echo"
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef StageDir
  #define StageDir "..\build\package\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\package"
#endif

[Setup]
AppId={{CAB4B4EF-1163-4A4A-9293-11E8A2D26499}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Echo contributors
AppPublisherURL=https://github.com/Xach-A/Echo
AppSupportURL=https://github.com/Xach-A/Echo/issues
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename={#AppName}-{#AppVersion}-setup-win64
SetupIconFile=..\resources\echo.ico
UninstallDisplayIcon={app}\{#AppName}.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
InfoBeforeFile=installer-info.txt
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppName}.exe"
Name: "{group}\{#AppName} (safe mode)"; Filename: "{app}\{#AppName}.exe"; Parameters: "--safe-mode"
Name: "{group}\{#AppName} documentation"; Filename: "{app}\docs"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppName}.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppName}.exe"; Description: "Start {#AppName} now"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Files Echo may create next to the executable (e.g. a user-installed ONNX Runtime).
Type: filesandordirs; Name: "{app}\ai"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  SettingsDir, LocalDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    SettingsDir := ExpandConstant('{userappdata}\{#AppName}');
    LocalDir := ExpandConstant('{localappdata}\{#AppName}');
    if DirExists(SettingsDir) or DirExists(LocalDir) then
      if SuppressibleMsgBox('Also delete your Echo settings, presets, voice models and logs?' + #13#10 +
                '(' + SettingsDir + ')' + #13#10#13#10 +
                'Recordings in your Music folder are never deleted.', mbConfirmation, MB_YESNO, IDNO) = IDYES then
      begin
        DelTree(SettingsDir, True, True, True);
        DelTree(LocalDir, True, True, True);
      end;
    { Remove the optional "start with Windows" entry if the user enabled it. }
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', '{#AppName}');
  end;
end;
