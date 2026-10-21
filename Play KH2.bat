@echo off
rem Play KH2 Final Mix on the native build: double-click this file.
rem Settings (F1 in game) are saved to build\win\kh2recomp.ini; the session log is logs\play.log.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\play.ps1 %*
