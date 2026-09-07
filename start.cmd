@echo off
rem Compatibility entry point; the Windows launcher lives in window/.
call "%~dp0window\start.cmd" %*
exit /b %errorlevel%
