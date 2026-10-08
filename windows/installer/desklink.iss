; Instalador de DeskLink para Windows (Inno Setup 6.3+).
;
; Instala el host, adb y el monitor virtual (driver) en una sola pasada, crea la entrada del menú Inicio y lo quita
; todo al desinstalar. Al actualizar encima de una versión anterior sólo reinstala el driver si su código cambió
; (compara driver-id.txt con el que guardó la instalación anterior).
;
; Lo compila la CI (.github/workflows/release.yml):
;   iscc /DAppVersion=0.1.123 /DStage=<carpeta> /DDriverId=<driver-id.txt> windows\installer\desklink.iss
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
; Ejecutable y argumentos del acceso directo. El host vive en la raíz de la carpeta de instalación.
#define HostExe "desklink-host.exe"
#define HostArgs "--virtual"

[Setup]
; No cambiar AppId: es lo que hace que una versión nueva se instale encima de la anterior.
AppId={{6B1E3C0A-5D2F-4C7B-9E21-D35C1A7F0B42}
AppName=DeskLink
AppVersion={#AppVersion}
AppVerName=DeskLink {#AppVersion}
AppPublisher=DeskLink
DefaultDirName={autopf}\DeskLink
DefaultGroupName=DeskLink
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputBaseFilename=DeskLink-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#HostExe}
UninstallDisplayName=DeskLink
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "Crear un acceso directo en el escritorio"; Flags: unchecked
Name: "reinstalardriver"; Description: "Reinstalar el monitor virtual aunque no haya cambiado"; Flags: unchecked; Check: DriverIsCurrent

[Files]
Source: "{#Stage}\host\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs
Source: "{#Stage}\adb\*"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\tools\*"; DestDir: "{app}\tools"; Flags: ignoreversion
; El driver sólo se copia cuando se va a (re)instalar: así {app}\driver siempre es el que está instalado y
; desinstalar quita exactamente ese.
Source: "{#Stage}\driver\*"; DestDir: "{app}\driver"; Flags: ignoreversion; Check: DriverNeedsInstall

[Icons]
Name: "{group}\DeskLink"; Filename: "{app}\{#HostExe}"; Parameters: "{#HostArgs}"; WorkingDir: "{app}"
Name: "{group}\Actualizar la tablet"; Filename: "{app}\tools\actualizar-tablet.bat"; WorkingDir: "{app}\tools"
Name: "{group}\Actualizar DeskLink en el PC"; Filename: "{app}\tools\actualizar-pc.bat"; WorkingDir: "{app}\tools"
Name: "{group}\Desinstalar DeskLink"; Filename: "{uninstallexe}"
Name: "{autodesktop}\DeskLink"; Filename: "{app}\{#HostExe}"; Parameters: "{#HostArgs}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#HostExe}"; Parameters: "{#HostArgs}"; WorkingDir: "{app}"; Description: "Abrir DeskLink"; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{app}\driver"

[Code]
const
  RegKey = 'Software\DeskLink';
  NewDriverId = '{#DriverId}';

function InstalledDriverId(): String;
begin
  if not RegQueryStringValue(HKLM, RegKey, 'DriverId', Result) then
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

// Ejecuta desklink-driver.exe sin ventana y deja su salida en driver\<orden>.log.
function RunDriverTool(const Command: String; var Log: String): Integer;
var
  Exe: String;
  Code: Integer;
begin
  Exe := ExpandConstant('{app}\driver\desklink-driver.exe');
  Log := ExpandConstant('{app}\driver\' + Command + '.log');
  Result := -1;
  if not FileExists(Exe) then
    Exit;
  if Exec(ExpandConstant('{cmd}'), '/c ""' + Exe + '" ' + Command + ' --sin-pausa > "' + Log + '" 2>&1"',
          ExpandConstant('{app}\driver'), SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := Code;
end;

// Cierra el host (tiene abierto el monitor virtual) y el servidor de adb (bloquea adb.exe).
procedure StopDeskLink();
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#HostExe}', '', SW_HIDE, ewWaitUntilTerminated, Code);
  if FileExists(ExpandConstant('{app}\adb.exe')) then
    Exec(ExpandConstant('{app}\adb.exe'), 'kill-server', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Log: String;
begin
  Result := '';
  StopDeskLink();
  // Driver nuevo: primero se quita el anterior con su propia herramienta (dispositivo, paquete y certificado).
  if DriverNeedsInstall() and FileExists(ExpandConstant('{app}\driver\desklink-driver.exe')) then begin
    WizardForm.PreparingLabel.Caption := 'Quitando el monitor virtual anterior...';
    RunDriverTool('desinstalar', Log);
    RegDeleteValue(HKLM, RegKey, 'DriverId');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Log: String;
  Code: Integer;
begin
  if (CurStep = ssPostInstall) and DriverNeedsInstall() then begin
    WizardForm.StatusLabel.Caption := 'Instalando el monitor virtual...';
    Code := RunDriverTool('instalar', Log);
    if Code = 0 then
      RegWriteStringValue(HKLM, RegKey, 'DriverId', NewDriverId)
    else
      SuppressibleMsgBox('No se pudo instalar el monitor virtual (código ' + IntToStr(Code) + ').' + #13#10 +
                         'DeskLink funciona igual duplicando la pantalla. El detalle está en:' + #13#10 + Log,
                         mbError, MB_OK, IDOK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Log: String;
begin
  if CurUninstallStep = usUninstall then begin
    StopDeskLink();
    RunDriverTool('desinstalar', Log);
  end;
  if CurUninstallStep = usPostUninstall then
    RegDeleteKeyIncludingSubkeys(HKLM, RegKey);
end;
