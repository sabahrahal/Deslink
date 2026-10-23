; Instalador de Extendesk para Windows (Inno Setup 6.3+).
;
; Instala el host, adb y el monitor virtual (driver) en una sola pasada, crea la entrada del menú Inicio y lo quita
; todo al desinstalar. Al actualizar encima de una versión anterior sólo reinstala el driver si su código cambió
; (compara driver-id.txt con el que guardó la instalación anterior).
;
; Lo compila la CI (.github/workflows/release.yml):
;   iscc /DAppVersion=0.1.123 /DStage=<carpeta> /DDriverId=<driver-id.txt> windows\installer\extendesk.iss
; donde <carpeta> contiene host\, adb\, driver\ y tools\ (ver release.yml).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Stage
  #define Stage "..\..\build\installer"
#endif
#ifndef DriverId
  #error Falta /DDriverId (contenido de driver-id.txt)
#endif
; Ejecutable y argumentos del acceso directo: la app de bandeja (windows/app, extendesk.exe) si viene en el paquete;
; si no, el host de consola. Los dos van en la raíz de la carpeta de instalación, junto a adb.
#define HostExe "extendesk-host.exe"
#ifexist Stage + "\host\extendesk.exe"
  #define MainExe "extendesk.exe"
  #define MainArgs ""
#else
  #define MainExe HostExe
  #define MainArgs "--virtual"
#endif

[Setup]
; No cambiar AppId: es lo que hace que una versión nueva se instale encima de la anterior.
AppId={{6B1E3C0A-5D2F-4C7B-9E21-D35C1A7F0B42}
AppName=Extendesk
AppVersion={#AppVersion}
AppVerName=Extendesk {#AppVersion}
AppPublisher=Extendesk
DefaultDirName={autopf}\Extendesk
DefaultGroupName=Extendesk
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputBaseFilename=Extendesk-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MainExe}
UninstallDisplayName=Extendesk
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
ShowLanguageDialog=no

[Languages]
; Inglés y español, según el idioma de Windows y sin preguntar (ShowLanguageDialog=no); en cualquier otro idioma, el
; inglés. Para añadir otro: su línea aquí y su versión de cada texto de [CustomMessages].
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"

[CustomMessages]
en.DesktopIcon=Create a desktop shortcut
es.DesktopIcon=Crear un acceso directo en el escritorio
en.ReinstallDriver=Reinstall the virtual monitor even if it hasn't changed
es.ReinstallDriver=Reinstalar el monitor virtual aunque no haya cambiado
en.OpenApp=Open Extendesk
es.OpenApp=Abrir Extendesk
en.RemovingDriver=Removing the previous virtual monitor...
es.RemovingDriver=Quitando el monitor virtual anterior...
en.InstallingDriver=Installing the virtual monitor...
es.InstallingDriver=Instalando el monitor virtual...
en.DriverFailed=Couldn't install the virtual monitor (code %1).%nExtendesk still works by mirroring the screen. The details are in:%n%2
es.DriverFailed=No se pudo instalar el monitor virtual (código %1).%nExtendesk funciona igual duplicando la pantalla. El detalle está en:%n%2

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; Flags: unchecked
Name: "reinstalardriver"; Description: "{cm:ReinstallDriver}"; Flags: unchecked; Check: DriverIsCurrent

[InstallDelete]
; Los ejecutables de antes del nombre Extendesk (desklink*.exe): ahora son extendesk*.exe.
Type: files; Name: "{app}\desklink.exe"
Type: files; Name: "{app}\desklink-host.exe"
Type: files; Name: "{app}\driver\desklink-driver.exe"

[Files]
Source: "{#Stage}\host\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs
Source: "{#Stage}\adb\*"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\tools\*"; DestDir: "{app}\tools"; Flags: ignoreversion
; El driver sólo se copia cuando se va a (re)instalar: así {app}\driver siempre es el que está instalado y
; desinstalar quita exactamente ese.
Source: "{#Stage}\driver\*"; DestDir: "{app}\driver"; Flags: ignoreversion; Check: DriverNeedsInstall
; La herramienta sí se actualiza siempre: con «estado» comprueba que el driver instalado funciona.
Source: "{#Stage}\driver\extendesk-driver.exe"; DestDir: "{app}\driver"; Flags: ignoreversion

[Icons]
Name: "{group}\Extendesk"; Filename: "{app}\{#MainExe}"; Parameters: "{#MainArgs}"; WorkingDir: "{app}"
Name: "{group}\Actualizar el dispositivo"; Filename: "{app}\tools\actualizar-tablet.bat"; WorkingDir: "{app}\tools"
Name: "{group}\Actualizar Extendesk en el PC"; Filename: "{app}\tools\actualizar-pc.bat"; WorkingDir: "{app}\tools"
Name: "{group}\Desinstalar Extendesk"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Extendesk"; Filename: "{app}\{#MainExe}"; Parameters: "{#MainArgs}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; Windows guarda en caché los iconos de los programas: sin esto la barra de tareas seguía con el logo de antes.
Filename: "{sys}\ie4uinit.exe"; Parameters: "-show"; Flags: runhidden waituntilterminated skipifdoesntexist
; El cortafuegos deja entrar a la tablet en cualquier red, también en la red Wi-Fi propia del PC (que Windows trata
; como pública y para la que el aviso de la primera vez no daba permiso). Se borra antes por si ya estaba, también la
; regla con el nombre antiguo (DeskLink).
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""DeskLink"""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Extendesk"""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Extendesk"" dir=in action=allow program=""{app}\{#HostExe}"" protocol=udp profile=any enable=yes"; Flags: runhidden waituntilterminated
Filename: "{app}\{#MainExe}"; Parameters: "{#MainArgs}"; WorkingDir: "{app}"; Description: "{cm:OpenApp}"; Flags: postinstall nowait skipifsilent unchecked

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Extendesk"""; Flags: runhidden waituntilterminated; RunOnceId: "ExtendeskFirewall"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""DeskLink"""; Flags: runhidden waituntilterminated; RunOnceId: "DeskLinkFirewall"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\driver"

[Code]
const
  RegKey = 'Software\Extendesk';
  // Antes del nombre Extendesk: el registro, la entrada de «Arrancar con Windows» y la herramienta del driver.
  LegacyRegKey = 'Software\DeskLink';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  LegacyRunValue = 'DeskLink';
  NewDriverId = '{#DriverId}';

function InstalledDriverId(): String;
begin
  if not RegQueryStringValue(HKLM, RegKey, 'DriverId', Result) and
     not RegQueryStringValue(HKLM, LegacyRegKey, 'DriverId', Result) then
    Result := '';
end;

// Hay un driver instalado por este instalador y es el mismo que trae esta versión. (Sólo mira el registro: se usa
// también en [Tasks], antes de que {app} esté disponible.)
function DriverIsCurrent(): Boolean;
begin
  Result := InstalledDriverId() = NewDriverId;
end;

function DriverNeedsInstall(): Boolean;
begin
  Result := (not DriverIsCurrent()) or WizardIsTaskSelected('reinstalardriver');
end;

// La herramienta del driver instalada: extendesk-driver.exe o, en una instalación de antes del nombre Extendesk,
// desklink-driver.exe. Vacío si no hay ninguna.
function DriverTool(): String;
begin
  Result := ExpandConstant('{app}\driver\extendesk-driver.exe');
  if not FileExists(Result) then
    Result := ExpandConstant('{app}\driver\desklink-driver.exe');
  if not FileExists(Result) then
    Result := '';
end;

// Ejecuta la herramienta del driver sin ventana y deja su salida en driver\<orden>.log.
function RunDriverTool(const Command: String; var Log: String): Integer;
var
  Exe: String;
  Code: Integer;
begin
  Exe := DriverTool();
  Log := ExpandConstant('{app}\driver\' + Command + '.log');
  Result := -1;
  if Exe = '' then
    Exit;
  if Exec(ExpandConstant('{cmd}'), '/c ""' + Exe + '" ' + Command + ' --sin-pausa > "' + Log + '" 2>&1"',
          ExpandConstant('{app}\driver'), SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := Code;
end;

// Cierra la app de bandeja y el host (tiene abierto el monitor virtual) y el servidor de adb (bloquea adb.exe).
procedure StopExtendesk();
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM extendesk.exe /IM {#HostExe} /IM desklink.exe /IM desklink-host.exe',
       '', SW_HIDE, ewWaitUntilTerminated, Code);
  if FileExists(ExpandConstant('{app}\adb.exe')) then
    Exec(ExpandConstant('{app}\adb.exe'), 'kill-server', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Log: String;
  LegacyId: String;
begin
  Result := '';
  // El driver que apuntó una instalación de antes del nombre Extendesk pasa a la clave nueva.
  if RegQueryStringValue(HKLM, LegacyRegKey, 'DriverId', LegacyId) then begin
    if not RegValueExists(HKLM, RegKey, 'DriverId') then
      RegWriteStringValue(HKLM, RegKey, 'DriverId', LegacyId);
    RegDeleteKeyIncludingSubkeys(HKLM, LegacyRegKey);
  end;
  StopExtendesk();
  // Driver nuevo: primero se quita el anterior con su propia herramienta (dispositivo, paquete y certificado).
  if DriverNeedsInstall() and (DriverTool() <> '') then begin
    WizardForm.PreparingLabel.Caption := CustomMessage('RemovingDriver');
    RunDriverTool('desinstalar', Log);
    RegDeleteValue(HKLM, RegKey, 'DriverId');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Log: String;
  Code: Integer;
  Reinstall: Boolean;
  RunCmd: String;
begin
  // «Arrancar con Windows» de antes del nombre Extendesk abría desklink.exe, que ya no existe: pasa a la app nueva.
  if (CurStep = ssPostInstall) and RegQueryStringValue(HKCU, RunKey, LegacyRunValue, RunCmd) then begin
    RegWriteStringValue(HKCU, RunKey, 'Extendesk', '"' + ExpandConstant('{app}\{#MainExe}') + '" --tray');
    RegDeleteValue(HKCU, RunKey, LegacyRunValue);
  end;
  // Mismo driver que el instalado: se comprueba que de verdad funciona (pudo fallar, o quitarse a mano) y, si no,
  // se vuelve a instalar con la copia que ya está en {app}\driver.
  Reinstall := False;
  if (CurStep = ssPostInstall) and not DriverNeedsInstall() then
    Reinstall := RunDriverTool('estado', Log) <> 0;
  if (CurStep = ssPostInstall) and (DriverNeedsInstall() or Reinstall) then begin
    WizardForm.StatusLabel.Caption := CustomMessage('InstallingDriver');
    Code := RunDriverTool('instalar', Log);
    if Code = 0 then
      RegWriteStringValue(HKLM, RegKey, 'DriverId', NewDriverId)
    else
      SuppressibleMsgBox(FmtMessage(CustomMessage('DriverFailed'), [IntToStr(Code), Log]), mbError, MB_OK, IDOK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Log: String;
begin
  if CurUninstallStep = usUninstall then begin
    StopExtendesk();
    RunDriverTool('desinstalar', Log);
  end;
  if CurUninstallStep = usPostUninstall then begin
    RegDeleteKeyIncludingSubkeys(HKLM, RegKey);
    RegDeleteKeyIncludingSubkeys(HKLM, LegacyRegKey);
    // «Arrancar con Windows» de la app de bandeja.
    RegDeleteValue(HKCU, RunKey, 'Extendesk');
    RegDeleteValue(HKCU, RunKey, LegacyRunValue);
  end;
end;
