@echo off
setlocal EnableExtensions
rem Register/query the op COM DLL for release deployment.
rem
rem COM requires an ABSOLUTE path in InprocServer32 / TypeLib win32 / HELPDIR.
rem Those values are filled from the directory where regsvr32 is executed, so
rem copy the DLL to its FINAL (neutral) directory BEFORE registering. Then the
rem registry carries the deployment path instead of a build-machine path.
rem The DLL binary itself contains no build-machine path (verified 2026-11-09).

set "CLSID={6d0fe2a0-f265-4961-9f92-ec22b64fb80c}"
set "TLB={40297cce-e4de-4d31-acfb-81bb8d14d261}"
set "DEFAULT_DLL=%~dp0..\bin\x64\op_x64.dll"

if /I "%~1"=="query" goto :query
if /I "%~1"=="install" goto :install
if /I "%~1"=="uninstall" goto :uninstall
goto :usage

:install
call :resolve "%~2"
if errorlevel 1 exit /b 2
echo [INFO] Registering: %DLL%
regsvr32 /s "%DLL%"
if errorlevel 1 (
  echo [ERR] regsvr32 failed. Run this script as Administrator.
  exit /b 3
)
echo [OK] Registered.
goto :query

:uninstall
call :resolve "%~2"
if errorlevel 1 exit /b 2
echo [INFO] Unregistering: %DLL%
regsvr32 /s /u "%DLL%"
if errorlevel 1 (
  echo [ERR] regsvr32 /u failed. Run this script as Administrator.
  exit /b 3
)
echo [OK] Unregistered.
exit /b 0

:query
echo.
echo [QUERY] HKCR\CLSID\%CLSID%\InprocServer32
reg query "HKCR\CLSID\%CLSID%\InprocServer32" /ve 2>nul
echo [QUERY] HKCR\TypeLib\%TLB%\1.0\0\win32
reg query "HKCR\TypeLib\%TLB%\1.0\0\win32" /ve 2>nul
echo [QUERY] HKCR\TypeLib\%TLB%\1.0\HELPDIR
reg query "HKCR\TypeLib\%TLB%\1.0\HELPDIR" /ve 2>nul
echo.
echo NOTE: absolute paths above are REQUIRED by COM. They are taken from the
echo       directory where regsvr32 was run. Deploy the DLL to its final
echo       neutral directory first; no build path is embedded in the DLL.
exit /b 0

:resolve
set "DLL=%~1"
if "%DLL%"=="" set "DLL=%DEFAULT_DLL%"
if not exist "%DLL%" (
  echo [ERR] DLL not found: %DLL%
  exit /b 2
)
for %%I in ("%DLL%") do set "DLL=%%~fI"
exit /b 0

:usage
echo Usage: %~nx0 install^|uninstall^|query [path\to\op_x64.dll]
echo.
echo   install    regsvr32 /s from the given (or default) DLL path, then show
echo              the absolute paths written to the registry.
echo   uninstall  regsvr32 /s /u
echo   query      show current registered paths (read-only, no elevation).
echo.
echo Default DLL: %DEFAULT_DLL%
echo Run as Administrator for install/uninstall.
exit /b 1
