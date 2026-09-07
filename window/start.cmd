@echo off
rem Windows launcher; start.ps1 resolves the repository root from this folder.
setlocal
title Avionics Ground Station
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" %*
set "launch_exit=%errorlevel%"
if not "%launch_exit%"=="0" pause
exit /b %launch_exit%
