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
; admin: necesario para que ssPostInstall instale VC_redist.x86.exe (UCRT + CRT
; de Microsoft, la única UCRT con la variante compatible con Win7 — la UCRT de
; la Windows SDK importa GetSystemTimePreciseAsFileTime, W8+, y no carga en la
; el equipo de pruebas) y KB2533623 con wusa, ambos elevados. Un prompt de UAC al instalar,
; estándar en Windows.
PrivilegesRequired=admin

[Tasks]
; Acceso directo al escritorio con la casilla marcada POR DEFECTO (sin Flags
; unchecked). Se crea en la fase de iconos: la app ya está instalada, y el
; runtime C++ y la update corren después en ssPostInstall. Grupos.
Name: "desktopicon"; Description: "Crear un acceso directo en el escritorio"; GroupDescription: "Acceso directo:"

[Files]
Source: "dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Excludes: "VC_redist.x86.exe,KB2533623-x86.msu"
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
; KB2533623 = actualización de Win7 que añade a kernel32 AddDllDirectory +
; GetFileInformationByHandleEx + GetFinalPathNameByHandle (las API que importa el
; python314.dll del yt-dlp_x86_win7.exe; sin ella el loader muere con "Entry
; Point Not Found ... KERNEL32.dll"). La máquina de pruebas está SIN updates (no tiene el
; KB): se detecta en caliente (¿exporta kernel32 AddDllDirectory?) y se instala
; con wusa solo cuando falta. El .msu va a {tmp} aquí; su INSTALACIÓN se
; dispara desde ssPostInstall (después del runtime C++) porque la update pide
; reiniciar (wusa suele devolver 3010): debe correr al final, con la app
; completa instalada y los iconos creados, no en mitad de la copia de archivos.
Source: "dist\KB2533623-x86.msu"; DestDir: "{tmp}"

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

[Icons]
; Icono del grupo de programas + acceso directo al escritorio (casilla
; marcada por defecto, ver [Tasks] desktopicon). Ambos se crean en esta fase,
; ya con la app en disco; VCRedist y la update corren después en ssPostInstall.
Name: "{group}\nevertube_qt2"; Filename: "{app}\nevertube_qt2.exe"
Name: "{autodesktop}\nevertube_qt2"; Filename: "{app}\nevertube_qt2.exe"; Tasks: desktopicon

[Code]
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

// KB2533623 (Win7 SIN updates): kernel32 de la máquina de pruebas no exporta
// AddDllDirectory / GetFileInformationByHandleEx / GetFinalPathNameByHandle
// (las añade KB2533623, que nunca se instaló porque la máquina de pruebas está sin
// actualizaciones). El python314.dll del yt-dlp_x86_win7.exe las importa
// estáticamente: sin el KB el loader muere con "The procedure entry point
// AddDllDirectory could not be located in the dynamic link library
// KERNEL32.dll" al lanzar yt-dlp. LoadLibrary/GetProcAddress existen SIEMPRE
// en kernel32 (base Win7, no dependen del KB); se usan las variantes ANSI
// (LoadLibraryA) porque en Inno un `string` de un external se pasa como LPCSTR
// (LoadLibraryW esperaría LPWSTR). Códigos de salida de wusa: 0 = instalado,
// 3010 = instalado pero hay que reiniciar. En Win10+/Win7 actualizado no hace
// nada.
function Kernel32LoadLibraryA(FileName: string): Cardinal;
  external 'LoadLibraryA@kernel32.dll stdcall';
function Kernel32GetProcAddress(Module: Cardinal; ProcName: string): Cardinal;
  external 'GetProcAddress@kernel32.dll stdcall';

function AddDllDirectoryAvailable(): Boolean;
var
  H: Cardinal;
begin
  H := Kernel32LoadLibraryA('kernel32.dll');
  Result := (H <> 0) and (Kernel32GetProcAddress(H, 'AddDllDirectory') <> 0);
end;

// TODO este mecanismo (KB2533623 + polyfill + yt-dlp del fork nicolaasjan)
// SOLO aplica a la máquina de pruebas Windows 7. En versiones superiores (W8/W8.1/W10/W11)
// esas funciones existen y entran los mpv/yt-dlp OFICIALES, así que aquí se
// gatea por versión: el KB solo se toca en Windows 7.
function IsWindows7(): Boolean;
begin
  // GetWindowsVersion (desde Inno 6.x): devuelve major/minor/build empaquetados
  // en un solo Cardinal ($0601xxxx en Win7). Se testea con el ISCC del CI
  // (Inno 6.7.1 por choco) y el 7.x local.
  Result := ((GetWindowsVersion shr 24) = 6) and
            (((GetWindowsVersion shr 16) and $FF) = 1);
end;

procedure InstallKb2533623();
var
  R: Integer;
begin
  if not IsWindows7() then begin
    Log('KB2533623 no aplica: este Windows no es la Win7 (W8 usa '
      + 'mpv/yt-dlp oficiales)');
    Exit;
  end;
  if AddDllDirectoryAvailable() then begin
    Log('KB2533623 no necesaria: AddDllDirectory ya está en kernel32');
    Exit;
  end;
  // Aquí falta la actualización necesaria: el usuario no tiene instalada la
  // update de Windows que la máquina de pruebas necesita. Se explica con claridad ANTES
  // de instalarla para que no parezca un fallo de la app.
  MsgBox('Este equipo tiene instalado Windows 7, pero le falta la '
       + 'actualización de Windows necesaria (KB2533623). Sin ella, yt-dlp no '
       + 'puede arrancar: kernel32 de Windows 7 sin esa actualización no '
       + 'incluye la función AddDllDirectory y el programa falla al buscar '
       + 'vídeos con "Entry Point Not Found".' + #13#10 + #13#10
       + 'El instalador va a instalar ahora KB2533623 automáticamente.',
       mbInformation, MB_OK);
  Log('KB2533623 ausente: instalando Windows6.1-KB2533623-x86.msu con wusa');
  if not Exec(ExpandConstant('{sys}\wusa.exe'),
      ExpandConstant('"{tmp}\KB2533623-x86.msu" /quiet /norestart'),
      '', SW_HIDE, ewWaitUntilTerminated, R) then begin
    MsgBox('No se pudo ejecutar wusa para instalar KB2533623. yt-dlp no '
         + 'funcionará sin ella.', mbError, MB_OK);
    Exit;
  end;
  if R = 3010 then begin
    MsgBox('KB2533623 instalada correctamente. Reinicia Windows para que '
         + 'yt-dlp funcione.', mbInformation, MB_OK);
  end else if R = 0 then begin
    MsgBox('KB2533623 instalada correctamente. yt-dlp ya puede funcionar.',
           mbInformation, MB_OK);
  end else begin
    MsgBox(Format('La instalación de KB2533623 falló con el código %d. '
                 + 'yt-dlp no funcionará sin ella.', [R]), mbError, MB_OK);
  end;
end;

// ORDEN de la instalación, de principio a fin:
//   1) copiar la app completa a {app} (y limpiar UCRT viejas)
//   2) crear iconos: menú + acceso directo al escritorio (casilla marcada)
//   3) runtime del sistema C++ (VC_redist.x86.exe)
//   4) update KB2533623 (solo Win7 sin AddDllDirectory; pide reiniciar)
// La update va al final porque wusa suele devolver 3010 (reinicio): si el
// usuario reinicia a mitad de la copia, la instalación quedaría a medias.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    InstallVcRedist();
    InstallKb2533623();
  end;
end;
