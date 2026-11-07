; gsmacro one-click installer (Inno Setup 6). Build with build-installer.ps1:
;   ISCC.exe /DSrc=<package folder> /DAppVer=<version> gsmacro-setup.iss
;
; One click on "Install" does everything:
;   1. closes a running gsmacro, copies gsmacro to Program Files\gsmacro
;   2. installs ViGEmBus and HidHide if missing (setup-drivers.ps1: official
;      GitHub release, signature checked, silent)
;   3. hides the controller from games (hidhide-setup.ps1 -Mode hide). If
;      that can't be done yet (no controller plugged in, or HidHide needs the
;      restart first), gsmacro does it by itself later (hide-pending flag)
;   4. "Start with Windows" (per-user Run entry) + Start menu / desktop icons
;   5. restarts if a driver needs it, otherwise starts gsmacro in the tray
; Uninstall: stops gsmacro, unhides the controller, removes files, startup
; entry and icons. The drivers stay installed (other apps may use them).

#ifndef Src
  #error Pass /DSrc=<folder with gsmacro.exe>
#endif
#ifndef AppVer
  #define AppVer "1.0"
#endif

[Setup]
AppId={{B6F2D9A4-6C1E-4F7B-9E4A-3D8C2A7F51E0}
AppName=gsmacro
AppVersion={#AppVer}
AppPublisher=az-musned
AppComments=Controller macros (wall dash) for Rocket League
DefaultDirName={autopf}\gsmacro
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=admin
UsedUserAreasWarning=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=gsmacro-setup
SetupIconFile=..\..\app\gsmacro.ico
UninstallDisplayIcon={app}\gsmacro.exe
UninstallDisplayName=gsmacro
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
RestartIfNeededByRun=no

[Messages]
WelcomeLabel1=Install gsmacro
WelcomeLabel2=gsmacro adds the wall-dash macros to your controller in Rocket League:%n%n    hold Square + tap R1  =  wall dash right%n    hold Square + tap L1  =  wall dash left%n%nBefore you click Install:%n    1. plug your DualSense in with a USB cable%n    2. stay connected to the internet%n%nEverything else happens by itself. Click Yes if Windows asks for permission.

[Files]
Source: "{#Src}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion
Source: "setup-drivers.ps1"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\gsmacro"; Filename: "{app}\gsmacro.exe"
Name: "{autodesktop}\gsmacro"; Filename: "{app}\gsmacro.exe"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "gsmacro"; \
  ValueData: """{app}\gsmacro.exe"" --autostart"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\gsmacro.exe"; Parameters: "--autostart"; Description: "Start gsmacro now"; \
  Flags: nowait postinstall runasoriginaluser; Check: not RestartNeeded

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/im gsmacro.exe /f"; Flags: runhidden; RunOnceId: "StopGsmacro"
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\hidhide-setup.ps1"" -Mode unhide -Quiet"; \
  Flags: runhidden; RunOnceId: "UnhideController"

[UninstallDelete]
Type: files; Name: "{app}\setup.log"

[Code]
var
  Reboot: Boolean;

function RestartNeeded(): Boolean;
begin
  Result := Reboot;
end;

function NeedRestart(): Boolean;
begin
  Result := Reboot;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  rc: Integer;
begin
  { an older gsmacro may be running in the tray: stop it so its files can be replaced }
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/im gsmacro.exe /f', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Result := '';
end;

procedure RunPs(const Script, Args, Status: String; var rc: Integer);
begin
  WizardForm.StatusLabel.Caption := Status;
  WizardForm.Refresh;
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\') + Script + '" ' + Args,
      '', SW_HIDE, ewWaitUntilTerminated, rc) then
    rc := -1;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  rc: Integer;
  marker, flagDir: String;
  err: AnsiString;
begin
  if CurStep <> ssPostInstall then Exit;
  marker := ExpandConstant('{tmp}\reboot.txt');
  RunPs('setup-drivers.ps1', '-Log "' + ExpandConstant('{app}\setup.log') + '" -Marker "' + marker + '"',
        'Installing the drivers gsmacro needs (ViGEmBus, HidHide). This can take a minute...', rc);
  if rc <> 0 then
  begin
    if not LoadStringFromFile(marker + '.error', err) then err := 'see ' + ExpandConstant('{app}\setup.log');
    MsgBox('The drivers gsmacro needs could not be installed:' + #13#10 + String(err) + #13#10#13#10 +
           'Check the internet connection, then run gsmacro-setup again.', mbError, MB_OK);
  end;
  Reboot := FileExists(marker);

  { gsmacro hides the controller by itself later if this step can't finish now }
  flagDir := ExpandConstant('{userappdata}\gsmacro');
  ForceDirectories(flagDir);
  SaveStringToFile(flagDir + '\hide-pending', 'hide the controller from games on the next start', False);
  RunPs('hidhide-setup.ps1', '-Mode hide -Quiet', 'Hiding the controller from games...', rc);
end;
