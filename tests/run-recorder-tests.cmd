@echo off
setlocal
if "%~2"=="" (
  echo Usage: run-recorder-tests.cmd existing-probe-capture new-output-directory
  exit /b 2
)
set "fixture=%~f1"
set "resultRoot=%~f2"
if exist "%resultRoot%" (
  echo Output directory must not already exist.
  exit /b 2
)
if not defined VSCMD_VER call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
if not exist build mkdir build
cl /nologo /W4 /std:c++17 /EHsc /D_CRT_SECURE_NO_WARNINGS /I src tests\recorder-replay.cpp /Fe:build\recorder-replay.exe /Fo:build\recorder-replay.obj
if errorlevel 1 exit /b 1
for %%m in (natural pause quick-pause stop-paused size time long missing timestamp rename-failure) do (
  build\recorder-replay.exe "%fixture%" "%resultRoot%\%%m" %%m
  if errorlevel 1 exit /b 1
)
node tests\validate-mkv.mjs "%resultRoot%"
if errorlevel 1 exit /b 1
popd
endlocal
