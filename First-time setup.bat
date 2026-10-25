@echo off
rem First-time setup: installs the build tools, finds the PC release, checks your disc image, builds the game.
rem Safe to run again. Log: logs\setup.log
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\setup.ps1 %*
