; ── Script Inno Setup de nevertube_qt2 ──────────────
; Como el `win_exe.yml` / `flutter_distributor`, el entregable de la
; `setup.exe` INSTALABLE que NO es un `dist/` con DLLs sueltas
; lleva TODO dentro (nevertube_qt2.exe + Qt5Widgets/Core/Gui.dll + plugins
; platforms/qwindows.dll + runtime VC + DLLs mpv-dev). Un solo
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
; GUARD de arquitectura: en un Windows de 64 bits (10/11) lo correcto es el
; build x64 (mpv + HwProbe: VP9/AV1 por hardware, 4K); el x86 ahí queda capado
; a h264 HW + VP9 software y sin AV1. Ver InitializeSetup en [Code]: cancela
; con aviso. Se permite en Windows de 32 bits y en 64 bits anterior a 10 (donde
; el build x64/mpv moderno no corre).
; admin: necesario para que ssPostInstall instale VC_redist.x86.exe (UCRT + CRT
; de Microsoft, la única UCRT con la variante compatible con Win7 — la UCRT de
; la Windows SDK importa GetSystemTimePreciseAsFileTime, W8+, y no carga en la
; máquina de pruebas). Un prompt de UAC al instalar, estándar en Windows.
PrivilegesRequired=admin

[Tasks]
; Acceso directo al escritorio con la casilla marcada POR DEFECTO (sin Flags
; unchecked). Se crea en la fase de iconos: la app ya está instalada, y el
; runtime C++ y la update corren después en ssPostInstall. Grupos.
Name: "desktopicon"; Description: "Crear un acceso directo en el escritorio"; GroupDescription: "Acceso directo:"

[Files]
Source: "dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Excludes: "VC_redist.x86.exe"
; El redist va a {tmp}: su INSTALACIÓN no corre aquí (AfterInstall), se lanza
; desde ssPostInstall, DESPUÉS de copiar la app y crear los iconos (orden:
; app -> acceso directo -> VCRedist -> update). Se ejecuta y bloquea con
; swWaitUntilTerminated para poder comprobar el código de salida: un redist
; que falla dejaría una app que no arranca. Sin deleteafterinstall: el exe
; debe seguir en {tmp} cuando corra ssPostInstall ({tmp} se purga al terminar
; el instalador igualmente).
; VC_redist.x86.exe = Visual C++ 2015-2019 (la versión 2022+ trae los imports
; W8+ que rompen Win7). /passive: muestra barra de progreso en lugar de
; instalar en silencio (/quiet). /norestart: no reinicia al terminar.
Source: "dist\VC_redist.x86.exe"; DestDir: "{tmp}"

; Limpieza de instalaciones ANTERIORES del equipo objetivo: las versiones <= 840e16c
; empaquetaban la UCRT de la Windows SDK app-local (ucrtbase.dll +
; api-ms-win-crt-*.dll junto al exe). Al instalar ENCIMA sin desinstalar, el
; loader resuelve app-local primero y carga esa UCRT de SDK (que importa
; GetSystemTimePreciseAsFileTime, W8+) aunque el redist ya esté en System32;
; produce exactamente el fallo "punto de entrada... en KERNEL32.dll". Inno no
; borra lo que no está en [Files], así que se elimina explícitamente aquí.
[InstallDelete]
Type: files; Name: "{app}\ucrtbase.dll"
Type: files; Name: "{app}\api-ms-win-crt-*"

[UninstallDelete]
; Desinstalar DEBE dejar la máquina sin rastro de la app: la BD local y las
; cachés viven FUERA de {app} (Inno solo borra {app} por defecto; sin esto,
; reinstalar devolvía una app con los datos antiguos — visto en el equipo objetivo:
; historial/suscripciones intactos y el perfil de códec persistido).
;   - {userdocs}\mini_tube: BD SQLite (ajustes —incluido codec_profile—,
;     historial, suscripciones, radios) + -wal/-shm. Es UN directorio propio
;     de la app (nunca toca otros documentos del usuario).
;   - {localappdata}\nevertube\nevertube: caché de miniaturas (thumbs_*) y de
;     resolución HLS (hls_resolve_cache.json).
; {userdocs}/{localappdata} se resuelven con el perfil del usuario que lanza
; el uninstaller aunque este corra elevado (PrivilegesRequired=admin).
; Quien quiera borrar SOLO datos sin desinstalar sigue teniendo
; "Restablecer aplicación" dentro de la app.
Type: filesandordirs; Name: "{userdocs}\mini_tube"
Type: filesandordirs; Name: "{localappdata}\nevertube\nevertube"

[Icons]
; Icono del grupo de programas + acceso directo al escritorio (casilla
; marcada por defecto, ver [Tasks] desktopicon). Ambos se crean en esta fase,
; ya con la app en disco; VCRedist y la update corren después en ssPostInstall.
Name: "{group}\nevertube_qt2"; Filename: "{app}\nevertube_qt2.exe"
Name: "{autodesktop}\nevertube_qt2"; Filename: "{app}\nevertube_qt2.exe"; Tasks: desktopicon

[Code]
// GUARD de arquitectura (antes de tocar disco): este setup es del build x86
// (Win7 de 32 bits). En un Windows de 64 bits 10/11 se cancela y se
// dirige al build x64. GetWindowsVersion: Major shl 24 | Minor shl 16 | Build
// -> Win10/Win11 = $0A000000. Se permite en 32 bits y en 64 bits < 10 (donde
// el build x64 con mpv moderno no correría).
function InitializeSetup(): Boolean;
begin
  if IsWin64 and (GetWindowsVersion >= $0A000000) then begin
    MsgBox('Este instalador es solo para Windows 7 de 32 bits.'
           + #13#10 + #13#10
           + 'Tu equipo no es un Windows 7 de 32 bits.'
           + #13#10 + #13#10
           + 'Instalacion cancelada.', mbCriticalError, MB_OK);
    Result := False;
  end else
    Result := True;
end;

// UCRT + CRT de Microsoft para el equipo objetivo (Win7 SP1): la UCRT de la Windows
// SDK importa GetSystemTimePreciseAsFileTime (W8+) y no carga; la variante del
// redist sí es compatible con Win7. /install /passive /norestart evita
// reinicios y muestra progreso (no es silencioso). Códigos aceptables:
// 0 = instalado, 1638 = ya había una versión más nueva, 3010 = requiere
// reinicio (instalado igual). VC_redist.x86.exe = Visual C++ 2015-2019
// (la 2022+ trae imports W8+ que rompen Win7).
procedure InstallVcRedist();
var
  R: Integer;
begin
  if not Exec(ExpandConstant('{tmp}\VC_redist.x86.exe'),
              '/install /passive /norestart', '', SW_SHOW,
              ewWaitUntilTerminated, R) then begin
    MsgBox('No se pudo ejecutar el runtime de Microsoft (VC_redist.x86.exe).'
           + #13#10 + 'La app no podrá arrancar sin él.', mbError, MB_OK);
  end else if (R <> 0) and (R <> 1638) and (R <> 3010) then begin
    MsgBox(Format('El runtime de Microsoft (VC_redist.x86.exe) falló con el ' +
                  'código %d. La app no podrá arrancar sin él.', [R]),
           mbError, MB_OK);
  end;
end;

// ORDEN de la instalación, de principio a fin:
//   1) copiar la app completa a {app} (y limpiar UCRT viejas)
//   2) crear iconos: menú + acceso directo al escritorio (casilla marcada)
//   3) runtime del sistema C++ (VC_redist.x86.exe)
// El runtime va al final (ssPostInstall) porque su instalación pide UAC y puede
// pedir reinicio: debe correr con la app ya completa en disco y los iconos
// creados, no en mitad de la copia de archivos.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    InstallVcRedist();
  end;
end;
