@echo off
rem Double-click to install what is needed and build. Starting PowerShell here avoids the Windows block on running .ps1 files.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build.ps1" %*
set "code=%errorlevel%"
if "%~1"=="" pause
exit /b %code%
