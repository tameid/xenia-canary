@ECHO OFF
REM ===========================================================================
REM Local NUI spike build for Windows.
REM
REM Everything this needs is scoped to THIS process only:
REM   - the Python venv, activated for this shell and then dropped on exit
REM   - VS's bundled CMake/Ninja, prepended to PATH for this shell only
REM No permanent PATH, registry or environment changes are made.
REM
REM Usage:
REM   tools\nui-spike\build-local.cmd              REM build xenia-app (Release)
REM   tools\nui-spike\build-local.cmd --target=xenia-kernel
REM ===========================================================================
SETLOCAL

SET "REPO=%~dp0..\.."
SET "VENV=C:\Users\sparky\.venvs\xenia"
SET "VSCMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
SET "VSNINJA=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

REM compile_shader_spirv.py needs glslangValidator / spirv-opt / spirv-dis from
REM the Vulkan SDK. Its find_vulkan_tools() reads $VULKAN_SDK then falls back to
REM PATH; the Windows SDK's fxc.exe is not a substitute.
REM Deliberately a single-line IF: inside a parenthesised block %VULKAN_SDK%
REM would be expanded before the FOR had run and yield an empty value.
IF NOT DEFINED VULKAN_SDK FOR /F "delims=" %%D IN ('dir /b /ad "C:\VulkanSDK\*" 2^>nul') DO SET "VULKAN_SDK=C:\VulkanSDK\%%D"

IF NOT EXIST "%VENV%\Scripts\python.exe" (
  ECHO ERROR: venv missing at %VENV%
  ECHO   py -3.14 -m venv "%VENV%"
  EXIT /B 1
)

IF NOT EXIST "%VSCMAKE%\cmake.exe" (
  ECHO ERROR: VS-bundled CMake not found at "%VSCMAKE%"
  ECHO   Point VSCMAKE/VSNINJA in this script at your Visual Studio install.
  EXIT /B 1
)

IF NOT EXIST "%VULKAN_SDK%\Bin\glslangValidator.exe" (
  ECHO ERROR: Vulkan SDK not found - needed for glslangValidator / spirv-opt.
  ECHO   compile_shader_spirv.py cannot build xenia-ui-vulkan without it.
  ECHO   Install it, or set VULKAN_SDK to an existing install.
  EXIT /B 1
)

REM Session-scoped only; ENDLOCAL discards all of this on exit.
SET "PATH=%VULKAN_SDK%\Bin;%VSCMAKE%;%VSNINJA%;%VENV%\Scripts;%PATH%"
SET "PYTHON=%VENV%\Scripts\python.exe"

CD /D "%REPO%"

ECHO === toolchain ===
"%PYTHON%" --version
"%VSCMAKE%\cmake.exe" --version
"%VULKAN_SDK%\Bin\glslangValidator.exe" --version 2>&1 | findstr /C:"Glslang Version"
ECHO.

ECHO === building ===
ECHO   target : %*
ECHO   output : build\bin\Windows\Release\xenia_canary.exe
ECHO.

REM --target is action="append", so injecting a default alongside a caller's own
REM --target would build both. Only supply the default when none was given.
SET "TGT=--target=xenia-app"
FOR %%A IN (%*) DO (
  IF /I "%%~A"=="-target"     SET "TGT="
  IF /I "%%~A"=="--target"    SET "TGT="
  IF /I "%%~A"=="-target="    SET "TGT="
  IF /I "%%~A"=="--target="   SET "TGT="
)

"%PYTHON%" xenia-build.py build --config=Release %TGT% %*

SET "RC=%ERRORLEVEL%"
ECHO.
IF "%RC%"=="0" (
  ECHO === build OK ===
  IF EXIST "build\bin\Windows\Release\xenia_canary.exe" (
    ECHO   build\bin\Windows\Release\xenia_canary.exe
  )
) ELSE (
  ECHO === build FAILED (exit %RC%) ===
)
ENDLOCAL & EXIT /B %RC%