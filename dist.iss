; ── Script Inno Setup de nevertube_qt2 ──────────────
; Como el `win_exe.yml` / `flutter_distributor`, el entregable de la
; `setup.exe` INSTALABLE que NO es un `dist/` con DLLs sueltas
; lleva TODO dentro (nevertube_qt2.exe + Qt5Widgets/Core/Gui.dll + plugins
; platforms/qwindows.dll + runtime VC + DLLs mpv-dev + yt-dlp.exe). Un solo
; archivo único para descargar.
;
; ATENCIÓN: la sintaxis de Inno EXIGE `[Setup]`, `AppName=…`, etc. a columna 0.
; Por eso el .iss es un archivo REAL a mano, NO un heredoc dentro del YAML del
; workflow (un heredoc dentro de `run: |` quedaría a columna 0 y rompería el
; YAML de GitHub Actions). Este .iss vive en la raíz del repo, junto al `dist/`
; que generan los pasos previos (windeployqt + DLLs) — por eso las rutas de
; `[Files]` y `OutputDir` son relativas a la raíz del workspace.
[Setup]
AppName=nevertube_qt2
AppVersion=1.0.0
AppPublisher=nevertube
AppPublisherURL=https://github.com/nevertube
DefaultDirName={autopf}\nevertube_qt2
DefaultGroupName=nevertube_qt2
DisableProgramGroupPage=yes
OutputDir=setup-out
OutputBaseFilename=nevertube-setup
Compression=lzma2
SolidCompression=yes
; El equipo objetivo es Windows x86: el payload es de 32 bits y el instalador
; se instala como 32 bits siempre ({autopf}=Program Files en x86 / Program
; Files (x86) en una máquina de 64 bits). Sin ArchitecturesInstallIn64BitMode.
; admin: necesario para que el [Run] instale VC_redist.x86.exe (UCRT + CRT de
; Microsoft, la única UCRT con la variante compatible con Win7 — la UCRT de la
; Windows SDK importa GetSystemTimePreciseAsFileTime, W8+, y no carga en la
; máquina de pruebas). Un prompt de UAC al instalar, estándar en Windows.
PrivilegesRequired=admin

[Files]
Source: "dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Excludes: "VC_redist.x86.exe"
; El redist va a {tmp}: se ejecuta en AfterInstall (al instalarse) y se limpia
; solo al terminar la instalación (no queda dentro de {app}\ como un exe de 13
; MB suelto). AfterInstall permite comprobar el código de salida: [Run] lo
; ignora y un redist que fallara en silencio dejaría una app que no arranca.
Source: "dist\VC_redist.x86.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; AfterInstall: InstallVcRedist()

[Icons]
Name: "{group}\nevertube_qt2"; Filename: "{app}\nevertube_qt2.exe"

[Code]
// UCRT + CRT de Microsoft para el equipo objetivo (Win7 SP1): la UCRT de la Windows
// SDK importa GetSystemTimePreciseAsFileTime (W8+) y no carga; la variante del
// redist sí es compatible con Win7. /install /quiet /norestart evita diálogos
// y reinicios. Códigos aceptables: 0 = instalado, 1638 = ya había una versión
// más nueva, 3010 = requiere reinicio (instalado igual).
procedure InstallVcRedist();
var
  R: Integer;
begin
  if not Exec(ExpandConstant('{tmp}\VC_redist.x86.exe'),
              '/install /quiet /norestart', '', SW_HIDE,
              ewWaitUntilTerminated, R) then begin
    MsgBox('No se pudo ejecutar el runtime de Microsoft (VC_redist.x86.exe).' +
           #13#10 + 'La app no podrá arrancar sin él.', mbError, MB_OK);
  end else if (R <> 0) and (R <> 1638) and (R <> 3010) then begin
    MsgBox(Format('El runtime de Microsoft (VC_redist.x86.exe) falló con el ' +
                  'código %d. La app no podrá arrancar sin él.', [R]),
           mbError, MB_OK);
  end;
end;
