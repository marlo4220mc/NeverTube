; ── Inno Setup script for nevertube x86 (Windows 10/11 32-bit) ───────────────
; The THIRD installer. The three of them partition the Windows space, and each
; one refuses the other two (see InitializeSetup in [Code]):
;
;   dist.iss              → 32-bit Windows BEFORE 10      → nevertube-setup-win7-x86.exe
;   dist_x86_win10.iss    → Windows 10/11, 32-bit only    → nevertube-setup-win10-x86.exe
;   dist64.iss            → Windows 10/11, 64-bit only    → nevertube-setup-x64.exe
;
; This one exists because the x86 payload is NOT the same on both sides of the
; Windows 10 line: the Win7 build has no mpv (its only engine is the
; FfmpegBackend, and its UCRT/imports are pinned to Win7 SP1), while Win10 x86
; CAN link an i686 libmpv (mpv-player-windows / mpv-winbuild-cmake, pinned to the
; newest build whose TLS init is clean) and therefore uses the same
; engine as x64 (MpvBackend, HwProbe, hwdec=auto). Shipping the Win7 bundle on
; Win10 x86 would give those users the degraded engine for no reason.
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
; Same AppName (and no explicit AppId) on the three setups ON PURPOSE: Inno
; derives the AppId from AppName, so an install of this build lands on the same
; uninstall entry and the same folder as a previous x86 install. That is what
; makes "upgrade Windows 7 → Windows 10, run the new installer" an in-place
; upgrade (the Win7 setup installed on 32-bit Win10 before this split existed)
; instead of two NeverTube entries in Programs and Features.
AppName=NeverTube
AppVersion={#AppVersion}
AppPublisher=NeverTube
AppPublisherURL=https://github.com/marlo4220mc/NeverTube
DefaultDirName={autopf}\NeverTube
DefaultGroupName=NeverTube
DisableProgramGroupPage=yes
OutputDir=setup-out
OutputBaseFilename=nevertube-setup-win10-x86
Compression=lzma2
SolidCompression=yes
; Icon of the setup itself (the first thing seen when launching it) and of the
; shortcuts. Paths are relative to the directory of the .iss, which is where
; iscc runs. The same nt.ico is embedded in the .exe (resources/nt.rc), so the
; shortcut icon and the binary icon are the same file.
SetupIconFile=resources\icon\nt.ico
; 32-bit payload: it always installs as 32-bit ({autopf}=Program Files (x86)
; on a 64-bit machine, Program Files on 32-bit). No
; ArchitecturesInstallIn64BitMode.
; ArchitecturesAllowed is the 32-bit default and it does NOT exclude 64-bit:
; `x86compatible` means "can run x86 binaries", which is every x64 Windows too.
; The exclusion is done in [Code] (IsWin64), on purpose: Inno has no
; "32-bit Windows only" value, and a [Code] guard can NAME the installer that
; does work for the user instead of a generic "requires 32-bit Windows".
ArchitecturesAllowed=x86compatible
; NO MinVersion: the version check lives in InitializeSetup so that a Win7 user
; is pointed to the Win7 installer instead of Inno's generic "requires Windows
; 10" page, which leaves them with no way out.
; admin: needed so ssPostInstall installs VC_redist.x86.exe (Microsoft CRT.
; On Win10 the UCRT comes with the system, so there is no Win7-style problem
; with it: the redist is the CRT and the safety net for a trimmed machine).
PrivilegesRequired=admin

[Tasks]
; Desktop shortcut with the checkbox marked BY DEFAULT (without Flags
; unchecked). It is created in the icons phase: the app is already installed,
; and the C++ runtime runs later in ssPostInstall. Groups.
Name: "desktopicon"; Description: "Crear un acceso directo en el escritorio"; GroupDescription: "Acceso directo:"

[Files]
Source: "dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Excludes: "VC_redist.x86.exe"
; nt.ico: the shortcuts of [Icons] need it (IconFilename). It cannot come
; from dist\* because the CI copies only the exe and the DLLs there; it is
; taken from the repo. It is installed flat in {app} (not in a subfolder)
; because that is what the IconFilename entries below reference.
Source: "resources\icon\nt.ico"; DestDir: "{app}"; Flags: ignoreversion
; The redist goes to {tmp}: its INSTALLATION does not run here (AfterInstall),
; it is launched from ssPostInstall, AFTER copying the app and creating the
; icons. It runs and blocks with ewWaitUntilTerminated so its exit code can be
; checked: a redist that fails would leave an app that does not start. No
; deleteafterinstall: the exe must remain in {tmp} when ssPostInstall runs
; ({tmp} is purged when the installer finishes anyway).
; vs/17 (Visual C++ 2015-2022): on the Win10 line there is no Win7 import
; restriction, so the modern redist is the right one (the Win7 setup pins
; vs/16 precisely because 2022+ imports W8+ symbols).
Source: "dist\VC_redist.x86.exe"; DestDir: "{tmp}"

; NO [InstallDelete]: unlike the Win7 setup, this bundle ships the UCRT that
; windeployqt deploys (it WORKS on Win10, where the W8+ APIs exist), so there is
; no app-local UCRT to purge.

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
; Program group icon + desktop shortcut (checkbox marked by default, see
; [Tasks] desktopicon). Both are created in this phase, already with the app
; on disk; the C++ runtime runs later in ssPostInstall.
; IconFilename on both: without this the shortcuts come out with the default
; icon and the app is not recognizable on the desktop. nt.ico comes from
; [Files] and is the same one embedded in the .exe (resources/nt.rc).
Name: "{group}\NeverTube"; Filename: "{app}\nevertube.exe"; IconFilename: "{app}\nt.ico"
Name: "{autodesktop}\NeverTube"; Filename: "{app}\nevertube.exe"; Tasks: desktopicon; IconFilename: "{app}\nt.ico"

[Code]
// TARGET GUARD (before touching disk). This setup is the x86 build FOR
// WINDOWS 10/11, so it refuses exactly the two machines that have their own
// installer:
//   - 64-bit Windows  → dist64.iss (nevertube-setup-x64.exe): that one runs the
//     x64 libmpv with VP9/AV1 hardware and 4K, which this 32-bit binary cannot.
//   - Windows before 10 (i.e. Win7/Win8.x, 32-bit) → dist.iss
//     (nevertube-setup-win7-x86.exe): that is the FfmpegBackend build, the only
//     one that loads on Win7 SP1 (its CRT and imports are pinned to it).
// GetWindowsVersion: Major shl 24 | Minor shl 16 | Build -> Win10/Win11 =
// $0A000000. Windows 11 has no 32-bit edition, so "10+" here means Win10 22H2
// in practice.
// The messages do NOT just say "cancelled": whoever reaches here has a machine
// where a DIFFERENT installer does work, and cancelling without saying which
// one leaves the user stranded. Plain ASCII, like the rest of the setup strings.
function InitializeSetup(): Boolean;
var
  V: LongWord;
begin
  V := GetWindowsVersion;
  Result := True;

  if IsWin64 then begin
    MsgBox('Este instalador es solo para Windows 10 de 32 bits.'
           + #13#10 + #13#10
           + 'Tu equipo tiene Windows de 64 bits.'
           + #13#10 + #13#10
           + 'Para Windows 10/11 de 64 bits usa el instalador x64'
           + #13#10
           + '(nevertube-setup-x64.exe) de la pagina de releases.'
           + #13#10 + #13#10
           + 'Instalacion cancelada.', mbCriticalError, MB_OK);
    Result := False;
  end else if V < $0A000000 then begin
    MsgBox('Este instalador es solo para Windows 10 de 32 bits.'
           + #13#10 + #13#10
           + 'Tu equipo tiene una version anterior de Windows.'
           + #13#10 + #13#10
           + 'Para Windows 7 de 32 bits usa el instalador'
           + #13#10
           + '(nevertube-setup-win7-x86.exe) de la pagina de releases.'
           + #13#10 + #13#10
           + 'Instalacion cancelada.', mbCriticalError, MB_OK);
    Result := False;
  end;
end;

// Microsoft CRT for the target machine (Win10+). On Win10 the UCRT is part of
// the system, so this is the Visual C++ 2015-2022 redistributable (aka.ms/vs/17)
// and its only job is msvcp140/vcruntime140 on a trimmed install.
// /install /passive /norestart avoids reboots and shows progress (it is not
// silent). Acceptable codes: 0 = installed, 1638 = a newer version was already
// present, 3010 = restart required (installed anyway).
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

// INSTALLATION ORDER, from start to finish:
//   1) copy the full app to {app}
//   2) create icons: menu + desktop shortcut (checkbox marked)
//   3) Microsoft CRT (VC_redist.x86.exe)
// The runtime goes last (ssPostInstall) because its installation asks for UAC
// and may ask for a restart: it must run with the app already complete on disk
// and the icons created, not in the middle of the file copy.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    InstallVcRedist();
  end;
end;
