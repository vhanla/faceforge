; Inno Setup script for FaceForge.
;
; Build with packaging\windows\build_installer.ps1 (or directly):
;   iscc /DVersion=0.2.0 /DSourceDir=C:\path\to\zig-out\bin ^
;        /DOutputDir=C:\path\to\dist /DRepoRoot=C:\path\to\repo faceforge.iss
;
; The installer is per-user by default (no admin, installs under
; %LOCALAPPDATA%\Programs\FaceForge). The user can switch to an all-users
; install in the wizard, which elevates and uses %ProgramFiles%.

#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\zig-out\bin"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif
#ifndef RepoRoot
  #define RepoRoot "..\.."
#endif

[Setup]
AppId={{7E2F5C1A-9B4D-4E3A-8C21-2F6B0A9D4E77}
AppName=FaceForge
AppVersion={#Version}
AppVerName=FaceForge {#Version}
AppPublisher=vhanla
AppPublisherURL=https://github.com/vhanla/faceforge
AppSupportURL=https://github.com/vhanla/faceforge/issues
AppUpdatesURL=https://github.com/vhanla/faceforge/releases
DefaultDirName={autopf}\FaceForge
DefaultGroupName=FaceForge
DisableProgramGroupPage=yes
; Per-user by default (writable, no elevation). "dialog" lets the user pick an
; all-users install, which prompts for UAC and installs under Program Files.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
; 64-bit only; the ONNX Runtime and OpenCV builds we ship are x64.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=faceforge-{#Version}-windows-x64-setup
SetupIconFile=..\icons\faceforge.ico
UninstallDisplayIcon={app}\faceforge.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableWelcomePage=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "fetchmodels"; Description: "Download model weights now (needs Python with onnx and numpy)"; GroupDescription: "Model weights:"; Flags: unchecked

[Files]
; Application binaries (faceforge-gui.exe, faceforge.dll, onnxruntime*.dll,
; opencv_*.dll) produced by zig build.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; Launch wrapper that starts the app in a per-user, writable data folder.
Source: "{#RepoRoot}\packaging\windows\faceforge.cmd"; DestDir: "{app}"; Flags: ignoreversion
; App icon for shortcuts and the uninstall entry.
Source: "{#RepoRoot}\packaging\icons\faceforge.ico"; DestDir: "{app}"; Flags: ignoreversion
; Per-user data directory: model fetch helper and the README.
Source: "{#RepoRoot}\scripts\fetch_models.ps1"; DestDir: "{localappdata}\FaceForge\scripts"; Flags: ignoreversion
Source: "{#RepoRoot}\scripts\extract_emap.py"; DestDir: "{localappdata}\FaceForge\scripts"; Flags: ignoreversion
Source: "{#RepoRoot}\README.md"; DestDir: "{localappdata}\FaceForge"; Flags: ignoreversion

[Icons]
Name: "{group}\FaceForge"; Filename: "{app}\faceforge.cmd"; IconFilename: "{app}\faceforge.ico"
Name: "{group}\FaceForge - Download models"; Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -NoProfile -File ""{localappdata}\FaceForge\scripts\fetch_models.ps1"""; WorkingDir: "{localappdata}\FaceForge"; IconFilename: "{app}\faceforge.ico"
Name: "{autodesktop}\FaceForge"; Filename: "{app}\faceforge.cmd"; IconFilename: "{app}\faceforge.ico"; Tasks: desktopicon

[Run]
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -NoProfile -File ""{localappdata}\FaceForge\scripts\fetch_models.ps1"""; WorkingDir: "{localappdata}\FaceForge"; Description: "Download model weights"; Flags: postinstall skipifsilent; Tasks: fetchmodels
Filename: "{app}\faceforge.cmd"; Description: "{cm:LaunchProgram,FaceForge}"; Flags: nowait postinstall skipifsilent

; Model weights live in %LOCALAPPDATA%\FaceForge and are intentionally left
; behind on uninstall, like any other user data.
