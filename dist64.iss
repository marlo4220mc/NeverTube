; ── Inno Setup script for nevertube x64 (Win10+) ──────────────────────────────
; Twin of `dist.iss` (which is the x86 build / 32-bit Win7 one) with the
; differences required by the x64 target. The x86 one cannot be reused as is:
; see DIFFERENCES 1..5 below ([Setup], [Files] and the absence of an
; architecture guard in [Code]).
;
; WARNING: Inno syntax REQUIRES `[Setup]`, `AppName=…`, etc. at column 0.
; That is why the .iss is a REAL hand-written file, NOT a heredoc inside the
; workflow YAML (a heredoc inside `run: |` would end up at column 0 and break
; the GitHub Actions YAML). This .iss lives at the repo root, next to the
; `dist/` produced by the previous steps (windeployqt + DLLs) — that is why the
; paths of `[Files]` and `OutputDir` are relative to the workspace root.
; AppVersion is the version of the RELEASE and arrives from the workflow as
; /DAppVersion=<version> (dist*.iss is compiled by .github/workflows/release.yml).
; The value below is the fallback so a hand-run `iscc dist.iss` still produces a
; usable installer instead of failing on an undefined identifier.
#ifndef AppVersion
#define AppVersion "1.0.0"
#endif

[Setup]
AppName=NeverTube
AppVersion={#AppVersion}
AppPublisher=NeverTube
AppPublisherURL=https://github.com/marlo4220mc/NeverTube
; DIFFERENCE 1 vs x86: on x64 it installs into 64-bit Program Files. Inno
; needs ArchitecturesInstallIn64BitMode so that {autopf} means
; "C:\Program Files" (64-bit) and not "C:\Program Files (x86)".
DefaultDirName={autopf}\NeverTube
DefaultGroupName=NeverTube
DisableProgramGroupPage=yes
OutputDir=setup-out
OutputBaseFilename=nevertube-setup-x64
Compression=lzma2
SolidCompression=yes
; Icon of the setup itself (the first thing seen when launching it) and of the
; shortcuts. Paths are relative to the directory of the .iss, which is where
; iscc runs. The same nt.ico is embedded in the .exe (resources/nt.rc), so the
; shortcut icon and the binary icon are the same file.
SetupIconFile=resources\icon\nt.ico
; ── Pages and metadata (modernization) ──────────────────────────────────────
; WELCOME: the first screen a new user sees; Inno hides it by default.
; LICENSE: the GPL-3 of the project — the SAME LICENSE file that the .deb, the
; .rpm, the PKGBUILD and the Flatpak ship ("GPL-3.0-or-later"): it is the
; project's own text, no term is invented here.
; DIR PAGE: on a fresh install the folder can be chosen. DisableDirPage=auto
; (Inno's default) means the page is SKIPPED when the same AppId is already
; installed, so an upgrade can never land in a second folder: this is what
; keeps the built-in updater and an in-place upgrade going to the same place.
DisableWelcomePage=no
LicenseFile=LICENSE
DisableDirPage=auto
; Inno 6's modern wizard; the default is the classic one.
WizardStyle=modern
; Programs and Features: the name and the icon of the entry. Without
; UninstallDisplayIcon that entry shows the generic Windows icon.
UninstallDisplayName=NeverTube
UninstallDisplayIcon={app}\nevertube.exe
; The version on the wizard's title bar and in the uninstall entry.
AppVerName=NeverTube {#AppVersion}
; DIFFERENCE 2 vs x86: this target is Win10+ on 64-bit, so it is declared
; explicitly. ArchitecturesAllowed=x64compatible accepts 64-bit x64
; (WoW is not a target) and Win11 ARM via emulation, where the app would run
; emulated. MinVersion=10.0 with Inno's 10.0 form: the x64 build uses modern
; mpv and Vulkan, which do not exist before Win10. NOTE: Win10/Win11 report
; themselves as 6.3 to any app without the compatibility manifest; the Inno 6
; setup does carry one (it was added in 6.0), so 10.0 is what it sees and what
; can be checked — with Inno 5.5.5 this MinVersion would reject Win10 itself.
; Win10.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; DIFFERENCE 3 vs x86: here there is NO UCRT problem. The target is Win10+,
; which ships the UCRT in the system, so ucrtbase.dll and api-ms-win-crt-*.dll
; are NOT bundled app-local (on x86 it was mandatory to purge them because the
; Windows SDK UCRT breaks Win7). The CRT is installed by VC_redist.x64.exe
; (aka.ms/vs/17, the 2022+ variant: on x64 there is no Win7 import restriction).
PrivilegesRequired=admin

[Languages]
; The whole wizard in Spanish: the app is Spanish-first (its base catalog is
; es) and every string of this .iss is Spanish. With a single language Inno
; shows no language picker. Spanish.isl ships with every Inno Setup install
; ("compiler:" resolves it inside ISCC's own folder).
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
; Same as the x86 setup: desktop shortcut with the box checked BY DEFAULT
; (no "unchecked" flag). It is created in the icons phase, with the app already
; on disk; the C++ runtime runs later in ssPostInstall.
Name: "desktopicon"; Description: "Crear un acceso directo en el escritorio"; GroupDescription: "Acceso directo:"
; The Start Menu shortcut is optional too: the box can be unchecked, and
; [Icons] below creates that entry only for whoever leaves it checked.
Name: "menuicon"; Description: "Crear un acceso directo en el menu Inicio"; GroupDescription: "Acceso directo:"

[Files]
; DIFFERENCE 4 vs x86: the name of the redist. On x86 it is VC_redist.x86.exe
; (2015-2019, aka.ms/vs/16) because the 2022+ one brings W8+ imports that
; break Win7; here it is the modern x64 one from vs/17. windeployqt
; --compiler-runtime leaves the CRT next to the exe when it can, but on the CI
; runner it cannot (it comes from the host VS), so the setup installs it and it is the safety net.
Source: "dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Excludes: "VC_redist.x64.exe"
; nt.ico: the shortcuts of [Icons] need it (IconFilename). It cannot come
; from dist\* because the CI copies only the exe and the DLLs there; it is
; taken from the repo. It is installed flat in {app} (not in a subfolder)
; because that is what the IconFilename entries below reference.
Source: "resources\icon\nt.ico"; DestDir: "{app}"; Flags: ignoreversion
; The redist goes to {tmp}: its INSTALLATION does not run here (AfterInstall),
; it is launched from ssPostInstall, AFTER copying the app and creating the
; icons. It runs and blocks with swWaitUntilTerminated to be able to check the
; exit code: a redist that fails would leave an app that does not start. No
; deleteafterinstall: the exe must remain in {tmp} when ssPostInstall runs
; ({tmp} is purged when the installer finishes anyway).
Source: "dist\VC_redist.x64.exe"; DestDir: "{tmp}"

; (Here there is NO [InstallDelete] of the UCRT: it was only needed on x86 to
; delete the app-local UCRT of old installs that broke Win7. On Win10+ the
; system UCRT is always present and wins. See dist.iss for the detail.)

[UninstallDelete]
; Uninstalling MUST leave the machine with no trace of the app: the local DB
; and the caches live OUTSIDE {app} (Inno only deletes {app} by default; without
; this, reinstalling returned an app with the old data).
;   - {userdocs}\mini_tube: SQLite DB (settings —including codec_profile—,
;     history, subscriptions, radios) + -wal/-shm. It is ONE directory owned
;     by the app (it never touches other user documents).
;   - {localappdata}\nevertube\nevertube: thumbnail cache (thumbs_*) and HLS
;     resolution cache (hls_resolve_cache.json).
; {userdocs}/{localappdata} are resolved with the profile of the user running
; the uninstaller even if it runs elevated (PrivilegesRequired=admin).
; Anyone who wants to delete ONLY data without uninstalling still has
; "Restablecer aplicación" (Reset application) inside the app.
Type: filesandordirs; Name: "{userdocs}\mini_tube"
Type: filesandordirs; Name: "{localappdata}\nevertube\nevertube"

[Icons]
; Program group icon + desktop shortcut (checkbox marked
; by default, see [Tasks] desktopicon). Both are created in this phase, already
; with the app on disk; the C++ runtime runs later in ssPostInstall.
; IconFilename on both: without this the shortcuts come out with the default
; Windows icon and the app is not recognizable on the desktop. nt.ico
; comes from [Files] and is the same one embedded in the .exe (resources/nt.rc).
Name: "{group}\NeverTube"; Filename: "{app}\nevertube.exe"; Tasks: menuicon; IconFilename: "{app}\nt.ico"
Name: "{autodesktop}\NeverTube"; Filename: "{app}\nevertube.exe"; Tasks: desktopicon; IconFilename: "{app}\nt.ico"
; Uninstall shortcut in the group: without it the program can only be removed
; from Programs and Features.
Name: "{group}\Desinstalar NeverTube"; Filename: "{uninstallexe}"

[Run]
; The last page offers to open the app right away, which is the "come back"
; half of an update: the same installer replaces the files and then relaunches
; NeverTube with one click. nowait: the wizard does not wait for the app;
; postinstall: the entry appears on the Finished page; skipifsilent: a silent
; install (/SILENT) opens no window.
Filename: "{app}\nevertube.exe"; Description: "Ejecutar NeverTube"; Flags: nowait postinstall skipifsilent

[Code]
// DIFFERENCE 5 vs x86: here there is NO architecture guard that cancels. The
// x86 one (InitializeSetup in dist.iss) rejected 64-bit Windows 10/11 and
// redirected to the x64 build; in this setup 64-bit IS the target, so a 64-bit
// machine with Win10+ is exactly the case this installer exists for, and there
// is nothing to redirect. A 32-bit Windows cannot install a 64-bit binary, but
// this code does not decide that: ArchitecturesAllowed decides it
// (x64compatible), which prevents it BEFORE writing to disk, with its own
// error message. That is why there is no InitializeSetup here: it would be a
// `Result := True` that does nothing.
//
// NOTE about the comments in this section: the `//` of Pascal Script apply
// here, NOT the `;` of the rest of the .iss (inside [Code] the `;` is a
// statement separator and the compiler rejects it). This is what dist.iss does.

// Microsoft runtime (CRT) for the target. On x64 it is Visual C++ 2022
// (aka.ms/vs/17) — here there is no need to be careful with the W8+ imports as
// on x86, because the target is Win10+. /install /passive /norestart avoids
// restarts and shows progress (it is not silent). Acceptable codes:
// 0 = installed, 1638 = a newer version was already present, 3010 = restart
// required (installed anyway).
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

// INSTALLATION ORDER, from start to finish:
//   1) copy the full app to {app}
//   2) create icons: menu + desktop shortcut (checkbox marked)
//   3) C++ system runtime (VC_redist.x64.exe)
// The runtime goes last (ssPostInstall) because its installation asks for UAC
// and may ask for a restart: it must run with the app already complete on disk
// and the icons created, not in the middle of the file copy.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    InstallVcRedist();
  end;
end;

// UNSIGNED INSTALLER NOTICE. It is a fact, not a legal term: this setup carries
// no Authenticode signature (the project has no certificate yet), so
// SmartScreen may warn on the first run, and the installation asks Windows for
// administrator rights because it writes to Program Files and installs the
// Microsoft runtime. It is shown once, on the READY page, and it also says what
// is NOT touched: the person's data lives outside {app} (the SQLite database in
// the documents folder, the caches in local app data, the downloads in the
// chosen folder) and this installer only replaces the program's files.
// No accents or odd characters, like the rest of the setup strings.
var
  AvisoMostrado: Boolean;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpReady) and (not AvisoMostrado) then begin
    AvisoMostrado := True;
    MsgBox('Este instalador no esta firmado digitalmente: Windows puede'
           + #13#10
           + 'mostrar un aviso de SmartScreen la primera vez.'
           + #13#10 + #13#10
           + 'Durante la instalacion Windows pedira permisos de administrador.'
           + #13#10 + #13#10
           + 'Tus datos (historial, suscripciones, ajustes y descargas) no se'
           + #13#10
           + 'tocan: el instalador solo reemplaza los archivos del programa.',
           mbInformation, MB_OK);
  end;
end;
