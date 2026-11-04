@echo off
rem Launch FaceForge with a per-user, writable working directory.
rem
rem The app resolves the default "models" path relative to the working
rem directory. Starting there keeps the weights under %LOCALAPPDATA%\FaceForge
rem instead of the read-only install folder, for every user account.
set "FACEFORGE_DATA=%LOCALAPPDATA%\FaceForge"
if not exist "%FACEFORGE_DATA%" mkdir "%FACEFORGE_DATA%"
cd /d "%FACEFORGE_DATA%"
start "" "%~dp0faceforge-gui.exe" %*
