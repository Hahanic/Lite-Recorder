@echo off
setlocal
if not defined VSCMD_VER call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
if not exist build mkdir build
cl /nologo /W4 /std:c++17 /EHsc /D_CRT_SECURE_NO_WARNINGS /I src tests\protocol.cpp /Fe:build\protocol-tests.exe /Fo:build\protocol-tests.obj
if errorlevel 1 exit /b 1
cl /nologo /W4 /std:c++17 /EHsc /D_CRT_SECURE_NO_WARNINGS /I src tests\overlay-storage.cpp /Fe:build\overlay-storage-tests.exe /Fo:build\overlay-storage-tests.obj
if errorlevel 1 exit /b 1
cl /nologo /W4 /std:c++17 /EHsc /D_CRT_SECURE_NO_WARNINGS /I src tests\capture-timing.cpp /Fe:build\capture-timing-tests.exe /Fo:build\capture-timing-tests.obj
if errorlevel 1 exit /b 1
cl /nologo /W4 /std:c++17 /EHsc /D_CRT_SECURE_NO_WARNINGS /I src tests\packet-queue.cpp /Fe:build\packet-queue-tests.exe /Fo:build\packet-queue-tests.obj
if errorlevel 1 exit /b 1
pushd build
protocol-tests.exe
if errorlevel 1 exit /b 1
overlay-storage-tests.exe
if errorlevel 1 exit /b 1
capture-timing-tests.exe
if errorlevel 1 exit /b 1
packet-queue-tests.exe
if errorlevel 1 exit /b 1
popd
node --test tests/capture.mjs
if errorlevel 1 exit /b 1
popd
endlocal
