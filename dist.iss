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
PrivilegesRequired=lowest

[Files]
Source: "dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\nevertube_qt2"; Filename: "{app}\nevertube_qt2.exe"
