; ── Inno Setup script for nevertube x86 (32-bit Windows BEFORE 10) ───────────
; The three installers partition the Windows space and each one refuses the
; other two (see InitializeSetup in [Code]):
;
;   dist.iss              → 32-bit Windows BEFORE 10      → nevertube-setup-win7-x86.exe
;   dist_x86_win10.iss    → Windows 10/11, 32-bit only    → nevertube-setup-win10-x86.exe
;   dist64.iss            → Windows 10/11, 64-bit only    → nevertube-setup-x64.exe
;
; This one is the Win7 build: NO mpv (its only engine is the FfmpegBackend) and
; its CRT/imports are pinned to Win7 SP1. Windows 10 x86 has its own bundle with
; the i686 libmpv, so it is refused here.
; Like a typical Inno setup, the deliverable for the target machine is this
; INSTALLABLE `setup.exe` that is NOT a `dist/` folder with loose DLLs:
; it carries EVERYTHING inside (nevertube.exe + Qt5Widgets/Core/Gui.dll +
; plugins platforms/qwindows.dll + VC runtime + mpv-dev DLLs). A single
; single file to download.
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
DefaultDirName={autopf}\NeverTube
DefaultGroupName=NeverTube
DisableProgramGroupPage=yes
OutputDir=setup-out
OutputBaseFilename=nevertube-setup-win7-x86
Compression=lzma2
SolidCompression=yes
; Icon of the setup itself (the first thing seen when launching it) and of the
; shortcuts. Paths are relative to the directory of the .iss, which is where
; iscc runs. The same nt.ico is embedded in the .exe (resources/nt.rc), so the
; shortcut icon and the binary icon are the same file.
SetupIconFile=resources\icon\nt.ico
; The target machine is Windows x86: the payload is 32-bit and the installer
; always installs as 32-bit ({autopf}=Program Files on x86 / Program Files
; (x86) on a 64-bit machine). No ArchitecturesInstallIn64BitMode.
; TARGET GUARD: this bundle is the Win7 one (FfmpegBackend, Win7-pinned CRT and
; imports), so it refuses EVERY Windows 10 or later. On 64-bit 10/11 the right
; thing is the x64 build (mpv + HwProbe: VP9/AV1 hardware, 4K) and on 32-bit
; 10/11 it is the Win10 x86 build (i686 libmpv, same engine as x64); this x86
; payload on 32-bit Win10 would be the degraded engine for no reason. See
; InitializeSetup in [Code]: it cancels and NAMES the installer that does work.
; Still allowed on 32-bit before 10 and on 64-bit before 10 (WOW64, where the
; x64 build with modern mpv does not run).
; admin: needed so ssPostInstall installs VC_redist.x86.exe (Microsoft UCRT
; + CRT, the only UCRT with the Win7-compatible variant — the UCRT from the
; Windows SDK imports GetSystemTimePreciseAsFileTime, W8+, and does not load on
; the test machine). A UAC prompt during install, standard on Windows.
PrivilegesRequired=admin

[Tasks]
; Desktop shortcut with the checkbox marked BY DEFAULT (without Flags
; unchecked). It is created in the icons phase: the app is already installed,
; and the C++ runtime and the update run later in ssPostInstall. Groups.
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
; icons (order: app -> shortcut -> VCRedist -> update). It runs and blocks with
; swWaitUntilTerminated to be able to check the exit code: a redist that fails
; would leave an app that does not start. No deleteafterinstall: the exe must
; still be in {tmp} when ssPostInstall runs ({tmp} is purged when the installer
; finishes anyway).
; VC_redist.x86.exe = Visual C++ 2015-2019 (the 2022+ version brings the W8+
; imports that break Win7). /passive: shows a progress bar instead of
; installing silently (/quiet). /norestart: does not restart when finished.
Source: "dist\VC_redist.x86.exe"; DestDir: "{tmp}"

; Cleanup of PREVIOUS installs on the target machine: versions <= 840e16c
; packaged the Windows SDK UCRT app-local (ucrtbase.dll +
; api-ms-win-crt-*.dll next to the exe). When installing ON TOP without
; uninstalling, the loader resolves app-local first and loads that SDK UCRT
; (which imports GetSystemTimePreciseAsFileTime, W8+) even if the redist is
; already in System32; it produces exactly the "entry point... in KERNEL32.dll"
; failure. Inno does not delete what is not in [Files], so it is deleted explicitly here.
[InstallDelete]
Type: files; Name: "{app}\ucrtbase.dll"
Type: files; Name: "{app}\api-ms-win-crt-*"

[UninstallDelete]
; Uninstalling MUST leave the machine with no trace of the app: the local DB
; and the caches live OUTSIDE {app} (Inno only deletes {app} by default; without
; this, reinstalling returned an app with the old data — seen on the target
; machine: history/subscriptions intact and the codec profile persisted).
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
; on disk; VCRedist and the update run later in ssPostInstall.
; IconFilename on both: without this the shortcuts come out with the default
; Windows icon and the app is not recognizable on the desktop. nt.ico
; comes from [Files] and is the same one embedded in the .exe (resources/nt.rc).
Name: "{group}\NeverTube"; Filename: "{app}\nevertube.exe"; IconFilename: "{app}\nt.ico"
Name: "{autodesktop}\NeverTube"; Filename: "{app}\nevertube.exe"; Tasks: desktopicon; IconFilename: "{app}\nt.ico"

[Code]
// TARGET GUARD (before touching disk). This setup is the x86 build for 32-bit
// Windows BEFORE 10 (the FfmpegBackend one, Win7-pinned). Windows 10 and later
// have their own installers, one per architecture, so they are refused here:
//   - 64-bit 10/11 → dist64.iss (nevertube-setup-x64.exe)
//   - 32-bit 10/11 → dist_x86_win10.iss (nevertube-setup-win10-x86.exe)
// GetWindowsVersion: Major shl 24 | Minor shl 16 | Build -> Win10/Win11 =
// $0A000000. Still allowed on 32-bit before 10 and on 64-bit before 10 (WOW64,
// where the x64 build with modern mpv would not run).
// The messages do NOT just say "cancelled": whoever reaches here has a machine
// where a DIFFERENT installer does work, and cancelling without saying which one
// leaves the user with no way out. No accents or odd characters, like the rest
// of the setup strings.
function InitializeSetup(): Boolean;
var
  V: LongWord;
begin
  V := GetWindowsVersion;
  Result := True;

  if V >= $0A000000 then begin
    if IsWin64 then begin
      MsgBox('Este instalador es solo para Windows 7 de 32 bits.'
             + #13#10 + #13#10
             + 'Tu equipo tiene Windows 10/11 de 64 bits.'
             + #13#10 + #13#10
             + 'Usa el instalador x64'
             + #13#10
             + '(nevertube-setup-x64.exe) de la pagina de releases.'
             + #13#10 + #13#10
             + 'Instalacion cancelada.', mbCriticalError, MB_OK);
    end else begin
      MsgBox('Este instalador es solo para Windows 7 de 32 bits.'
             + #13#10 + #13#10
             + 'Tu equipo tiene Windows 10 de 32 bits.'
             + #13#10 + #13#10
             + 'Usa el instalador para Windows 10 de 32 bits'
             + #13#10
             + '(nevertube-setup-win10-x86.exe) de la pagina de releases.'
             + #13#10 + #13#10
             + 'Instalacion cancelada.', mbCriticalError, MB_OK);
    end;
    Result := False;
  end;
end;

// Microsoft UCRT + CRT for the target machine (Win7 SP1): the Windows SDK UCRT
// imports GetSystemTimePreciseAsFileTime (W8+) and does not load; the redist
// variant is the one compatible with Win7. /install /passive /norestart avoids
// reboots and shows progress (it is not silent). Acceptable codes:
// 0 = installed, 1638 = a newer version was already present, 3010 = restart
// required (installed anyway). VC_redist.x86.exe = Visual C++ 2015-2019
// (the 2022+ one brings W8+ imports that break Win7).
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
//   1) copy the full app to {app} (and clean up old UCRTs)
//   2) create icons: menu + desktop shortcut (checkbox marked)
//   3) C++ system runtime (VC_redist.x86.exe)
// The runtime goes last (ssPostInstall) because its installation asks for UAC
// and may ask for a restart: it must run with the app already complete on disk
// and the icons created, not in the middle of the file copy.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    InstallVcRedist();
  end;
end;
