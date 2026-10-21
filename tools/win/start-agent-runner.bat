@echo off
rem Starts the KH2 Co-op agent runner. Close its window to stop it.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0agent-runner.ps1"
pause
