@echo off
rem Update: gets the latest version, updates the libraries and rebuilds what changed.
rem For a new PC, run "First-time setup.bat" instead. Build log: logs\build.log
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\update.ps1 %*
