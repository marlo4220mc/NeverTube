; ── Script Inno Setup de nevertube_qt2 x64 (Win10+) ───────────────────────────
; Gemelo de `dist.iss` (que es el del build x86 / Win7 de 32 bits) con las
; diferencias que exige el target x64. NO se puede reutilizar el de x86 tal cual:
; ver las DIFERENCIA 1..5 de abajo ([Setup], [Files] y la ausencia de guard de
; arquitectura en [Code]).
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
; DIFERENCIA 1 vs x86: en x64 se instala en Program Files de 64 bits. Inno
; necesita ArchitecturesInstallIn64BitMode para que {autopf} signifique
; "C:\Program Files" (64 bits) y no "C:\Program Files (x86)".
DefaultDirName={autopf}\nevertube_qt2
DefaultGroupName=nevertube_qt2
DisableProgramGroupPage=yes
OutputDir=setup-out
OutputBaseFilename=nevertube-setup-x64
Compression=lzma2
SolidCompression=yes
; DIFERENCIA 2 vs x86: este target es Win10+ en 64 bits, así que se declara
; explícitamente. ArchitecturesAllowed=x64compatible acepta x64 de 64 bits
; (WoW no es un objetivo) y Win11 ARM vía emulación, donde la app correría
; emulada. MinVersion=10.0 con la forma 10.0 de Inno: el build x64 usa mpv
; moderno y Vulkan, que no existen antes de Win10. OJO: Win10/Win11 se reportan
; como 6.3 a cualquier app sin el manifiesto de compatibilidad; el setup de
; Inno 6 sí lo lleva (se añadió en la 6.0), así que 10.0 es lo que ve y lo que
; se puede comparar — con un Inno 5.5.5 este MinVersion rechazaría el propio
; Win10.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; DIFERENCIA 3 vs x86: aquí NO hay problema de UCRT. El target es Win10+, donde
; la UCRT viene en el sistema, así que NO se empaqueta ucrtbase.dll ni
; api-ms-win-crt-*.dll app-local (en x86 era obligatorio purgarlos porque la
; UCRT de la Windows SDK rompe Win7). El CRT sí lo instala VC_redist.x64.exe
; (aka.ms/vs/17, la variante 2022+: en x64 no hay restricción de imports Win7).
PrivilegesRequired=admin

[Tasks]
; Igual que el setup x86: acceso directo al escritorio con la casilla marcada
; POR DEFECTO (sin Flags unchecked). Se crea en la fase de iconos, ya con la
; app en disco; el runtime C++ corre después en ssPostInstall.
Name: "desktopicon"; Description: "Crear un acceso directo en el escritorio"; GroupDescription: "Acceso directo:"

[Files]
; DIFERENCIA 4 vs x86: el nombre del redist. En x86 es VC_redist.x86.exe
; (2015-2019, aka.ms/vs/16) porque el 2022+ trae imports W8+ que rompen Win7;
; aquí es el x64 moderno de vs/17. windeployqt --compiler-runtime deja el CRT
; junto al exe cuando puede, pero en el runner de CI no (viene del VS del host),
; así que el setup lo instala y es la red de seguridad.
Source: "dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Excludes: "VC_redist.x64.exe"
; El redist va a {tmp}: su INSTALACIÓN no corre aquí (AfterInstall), se lanza
; desde ssPostInstall, DESPUÉS de copiar la app y crear los iconos. Se ejecuta
; y bloquea con swWaitUntilTerminated para poder comprobar el código de salida: un
; redist que falla dejaría una app que no arranca. Sin deleteafterinstall: el exe
; debe seguir en {tmp} cuando corra ssPostInstall ({tmp} se purga al terminar el
; instalador igualmente).
Source: "dist\VC_redist.x64.exe"; DestDir: "{tmp}"

; (Aquí NO hay [InstallDelete] de la UCRT: solo hacía falta en x86 para borrar la
; UCRT app-local de instalaciones antiguas que rompía Win7. En Win10+ la UCRT del
; sistema está siempre presente y gana. Ver dist.iss para el detalle.)

[UninstallDelete]
; Desinstalar DEBE dejar la máquina sin rastro de la app: la BD local y las
; cachés viven FUERA de {app} (Inno solo borra {app} por defecto; sin esto,
; reinstalar devolvería una app con los datos antiguos).
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
; Icono del grupo de programas + acceso directo al escritorio (casilla marcada
; por defecto, ver [Tasks] desktopicon). Ambos se crean en esta fase, ya con la
; app en disco; el runtime C++ corre después en ssPostInstall.
Name: "{group}\nevertube_qt2"; Filename: "{app}\nevertube_qt2.exe"
Name: "{autodesktop}\nevertube_qt2"; Filename: "{app}\nevertube_qt2.exe"; Tasks: desktopicon

[Code]
// DIFERENCIA 5 vs x86: aquí NO hay guard de arquitectura que cancele. El de x86
// (InitializeSetup en dist.iss) rechazaba Windows de 64 bits 10/11 y desviaba al
// build x64; en este setup el 64 bits ES el objetivo, así que un equipo de 64
// bits con Win10+ es exactamente el caso para el que existe este instalador, y
// no hay nada que redirigir. Un Windows de 32 bits no puede instalar un binario
// de 64 bits, pero no lo decide este código: lo decide ArchitecturesAllowed
// (x64compatible), que lo impide ANTES de escribir en disco, con su propio
// mensaje de error. Por eso aquí no hay ningún InitializeSetup: sería un
// `Result := True` que no hace nada.
//
// OJO con los comentarios de esta sección: aquí valen los `//` de Pascal
// Script, NO el `;` del resto del .iss (dentro de [Code] el `;` es separador de
// sentencias y el compilador lo rechaza). Es lo que hace dist.iss.

// Runtime de Microsoft (CRT) para el target. En x64 es el Visual C++ 2022
// (aka.ms/vs/17) — aquí no hay que tener cuidado con los imports W8+ como en
// x86, porque el target es Win10+. /install /passive /norestart evita
// reinicios y muestra progreso (no es silencioso). Códigos aceptables:
// 0 = instalado, 1638 = ya había una versión más nueva, 3010 = requiere
// reinicio (instalado igual).
procedure InstallVcRedist();
var
  R: Integer;
begin
  if not Exec(ExpandConstant('{tmp}\VC_redist.x64.exe'),
              '/install /passive /norestart', '', SW_SHOW,
              ewWaitUntilTerminated, R) then begin
    MsgBox('No se pudo ejecutar el runtime de Microsoft (VC_redist.x64.exe).'
           + #13#10 + 'La app no podrá arrancar sin él.', mbError, MB_OK);
  end else if (R <> 0) and (R <> 1638) and (R <> 3010) then begin
    MsgBox(Format('El runtime de Microsoft (VC_redist.x64.exe) falló con el ' +
                  'código %d. La app no podrá arrancar sin él.', [R]),
           mbError, MB_OK);
  end;
end;

// ORDEN de la instalación, de principio a fin:
//   1) copiar la app completa a {app}
//   2) crear iconos: menú + acceso directo al escritorio (casilla marcada)
//   3) runtime del sistema C++ (VC_redist.x64.exe)
// El runtime va al final (ssPostInstall) porque su instalación pide UAC y puede
// pedir reinicio: debe correr con la app ya completa en disco y los iconos
// creados, no en mitad de la copia de archivos.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    InstallVcRedist();
  end;
end;
