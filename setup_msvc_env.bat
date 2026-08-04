@echo off
:: ============================================================================
:: setup_msvc_env.bat — op 库 nmake 增量重编所需的 MSVC + Windows SDK 环境
:: ============================================================================
:: 用途: 在新开的 cmd 里 `call setup_msvc_env.bat` 后即可 `cd build\nmake-x64-Release && nmake op_x64`
:: 覆盖: MSVC 14.44.35207 / WinSDK 10.0.26100.0 / ATL(atlmfc) / winrt / cppwinrt
::       （OpAutomation.cpp 含 atlbase.h 需 atlmfc；DisplayHook 需 winrt/wrl）
:: 注意: 路径为本机配置，换机器请调整 VS_PATH / MSVC_VER / WINSDK_VER
:: ============================================================================

setlocal EnableDelayedExpansion

:: --- 版本与路径（按本机实际填写）---
set "VS_PATH=D:\Program Files\Microsoft Visual Studio\2022\Professional"
set "MSVC_VER=14.44.35207"
set "WINSDK_VER=10.0.26100.0"
set "MSVC_PATH=%VS_PATH%\VC\Tools\MSVC\%MSVC_VER%"
set "WINSDK=C:\Program Files (x86)\Windows Kits\10"

echo === 设置 MSVC 编译环境 ===
echo   MSVC   = %MSVC_VER%
echo   WinSDK = %WINSDK_VER%

:: --- PATH: cl/link/nmake/rc ---
set "PATH=%MSVC_PATH%\bin\Hostx64\x64;%WINSDK%\bin\%WINSDK_VER%\x64;%PATH%"

:: --- INCLUDE: MSVC + ATL + WinSDK(ucrt/um/shared/winrt/cppwinrt) ---
set "INCLUDE=%MSVC_PATH%\include;%MSVC_PATH%\atlmfc\include;%VS_PATH%\VC\Auxiliary\VS\include;%WINSDK%\Include\%WINSDK_VER%\ucrt;%WINSDK%\Include\%WINSDK_VER%\um;%WINSDK%\Include\%WINSDK_VER%\shared;%WINSDK%\Include\%WINSDK_VER%\winrt;%WINSDK%\Include\%WINSDK_VER%\cppwinrt"

:: --- LIB: MSVC + ATL + WinSDK(um/ucrt) ---
set "LIB=%MSVC_PATH%\lib\x64;%MSVC_PATH%\atlmfc\lib\x64;%WINSDK%\Lib\%WINSDK_VER%\um\x64;%WINSDK%\Lib\%WINSDK_VER%\ucrt\x64"

:: --- 验证关键工具 ---
where cl  >nul 2>&1 && (echo   [OK] cl.exe)   || (echo   [FAIL] cl.exe)
where link >nul 2>&1 && (echo   [OK] link.exe) || (echo   [FAIL] link.exe)
where nmake >nul 2>&1 && (echo   [OK] nmake.exe) || (echo   [FAIL] nmake.exe)
where rc  >nul 2>&1 && (echo   [OK] rc.exe)   || (echo   [FAIL] rc.exe)

:: --- 验证 ATL 头（OpAutomation.cpp 依赖）---
if exist "%MSVC_PATH%\atlmfc\include\atlbase.h" (echo   [OK] atlbase.h) else (echo   [FAIL] atlbase.h — atlmfc 未安装)

echo === 环境就绪: cd build\nmake-x64-Release ^&^& nmake op_x64 ===
endlocal
