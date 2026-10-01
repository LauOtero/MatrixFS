; matrixfs.iss — instalador Inno Setup de MatrixFS «ATLAS» (Windows).
;
; Requisitos:
;   · Inno Setup 6 (https://jrsoftware.org/isinfo.php) — `iscc` en el PATH.
;   · Binarios compilados antes con ..\build.ps1  ->  ..\dist\Release\*.exe
;
; Uso (desde platform\windows):
;   iscc installer\matrixfs.iss
;   ; -> installer\Output\MatrixFS-Ultra-Setup-1.0.0.exe
;
; El instalador:
;   1. Copia matrixfs_winfsp.exe, matrixfs-ctl.exe y matrixfs_automount.exe.
;   2. Instala WinFsp en modo silencioso si el medio no lo tiene y se ha
;      colocado `redist\winfsp.msi` junto a este script (opcional).
;   3. Registra el servicio de automontaje MatrixFS-Automount y lo arranca.
;
; La desinstalación detiene y elimina el servicio y borra los binarios, pero
; NO toca NUNCA los volúmenes del usuario: los datos viven en el propio medio.

#define AppName        "MatrixFS"
#define AppVersion     "1.0.0"
#define AppPublisher   "MatrixFS Project"
#define AppURL         "https://example.invalid/matrixfs"
#define ServiceName    "MatrixFS-Automount"
#define ServiceDisplay "MatrixFS Automount"

[Setup]
; AppId fijo: identifica el producto entre versiones (no cambiar).
AppId={{8F3C1A64-9D42-4E7B-A5C1-2B7E6D0F91AC}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
DefaultDirName={autopf}\MatrixFS
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=MatrixFS-Ultra-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; El servicio y el montaje requieren privilegios de administrador.
PrivilegesRequired=admin
; WinFsp sólo existe para x64 y ARM64; se admite x64 (equivalente moderno:
; x64compatible en Inno 6.3+, que también acepta ARM64 vía emulación).
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
UninstallDisplayName={#AppName} {#AppVersion}

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\dist\Release\matrixfs_winfsp.exe";     DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\Release\matrixfs-ctl.exe";        DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\Release\matrixfs_automount.exe";  DestDir: "{app}"; Flags: ignoreversion
; WinFsp opcional: sólo se incluye si el paquete se ha colocado en redist\.
Source: "redist\winfsp.msi"; DestDir: "{tmp}"; Flags: deleteafterinstall skipifsourcedoesntexist

[Icons]
Name: "{group}\MatrixFS — diagnóstico"; Filename: "{app}\matrixfs-ctl.exe"; Parameters: "probe"; Flags: runascurrentuser
Name: "{group}\MatrixFS — desinstalar"; Filename: "{uninstallexe}"

[Run]
; 1. WinFsp (sólo si falta y el MSI se ha distribuido con el instalador).
Filename: "msiexec.exe"; Parameters: "/i ""{tmp}\winfsp.msi"" /qn /norestart"; \
    StatusMsg: "Instalando WinFsp..."; Check: NeedsWinFsp; Flags: waituntilterminated
; 2. Registro del servicio de automontaje (mismos parámetros que install.ps1).
Filename: "{sys}\sc.exe"; Parameters: "create {#ServiceName} binPath= ""{app}\matrixfs_automount.exe"" start= auto DisplayName= ""{#ServiceDisplay}"""; \
    Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "description {#ServiceName} ""Detecta volúmenes MatrixFS y los monta en la primera letra libre (D:, E:, ...)."""; \
    Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "failure {#ServiceName} reset= 86400 actions= restart/5000/restart/10000/restart/30000"; \
    Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "start {#ServiceName}"; \
    StatusMsg: "Arrancando el servicio de automontaje..."; Flags: runhidden waituntilterminated

[UninstallRun]
; Inversos, en orden: parar y eliminar el servicio antes de borrar los binarios.
Filename: "{sys}\sc.exe"; Parameters: "stop {#ServiceName}";  Flags: runhidden; RunOnceId: "StopAutomount"
Filename: "{sys}\sc.exe"; Parameters: "delete {#ServiceName}"; Flags: runhidden; RunOnceId: "DeleteAutomount"

[Code]
{ ¿Está WinFsp presente en el sistema? El SDK/tiempo de ejecución instala la
  DLL winfsp-x64.dll (o winfsp-a64.dll en ARM64) bajo su carpeta de programa y
  registra el servicio de lanzador. Se comprueban ambos indicios. }
function WinFspPresent: Boolean;
begin
  Result :=
    FileExists(ExpandConstant('{commonpf32}\WinFsp\bin\winfsp-x64.dll')) or
    FileExists(ExpandConstant('{commonpf}\WinFsp\bin\winfsp-x64.dll')) or
    FileExists(ExpandConstant('{commonpf}\WinFsp\bin\winfsp-a64.dll'));
end;

{ Sólo se lanza el MSI si falta WinFsp Y se distribuyó el paquete. }
function NeedsWinFsp: Boolean;
begin
  Result := (not WinFspPresent) and
            FileExists(ExpandConstant('{tmp}\winfsp.msi'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    if not WinFspPresent then
      MsgBox('No se ha detectado WinFsp en este equipo.' + #13#10 + #13#10 +
             'MatrixFS lo necesita para montar volúmenes. Instálelo desde ' +
             'https://winfsp.dev y vuelva a ejecutar el instalador, o coloque ' +
             'winfsp.msi en installer\redist\ antes de generarlo.' + #13#10 + #13#10 +
             'Los binarios se han copiado igualmente: puede instalar WinFsp más ' +
             'tarde sin reinstalar MatrixFS.', mbInformation, MB_OK);
  end;
end;
