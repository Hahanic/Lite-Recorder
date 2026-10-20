param([Parameter(Mandatory=$true)][string]$DevkitPro)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$env:DEVKITPRO = ([IO.Path]::GetFullPath($DevkitPro)).Replace('\','/')
$compiler = Join-Path $DevkitPro 'devkitA64/bin/aarch64-none-elf-g++.exe'
$toolRoot = Join-Path $DevkitPro 'tools/bin'
function RunChecked([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed: $LASTEXITCODE" }
}
$buildRoot = Join-Path $projectRoot 'build'
$stageRoot = Join-Path $projectRoot 'stage'
$exefsRoot = Join-Path $buildRoot 'exefs'
$moduleDir = Join-Path $stageRoot 'atmosphere/contents/00FF4C5250524231'
$overlayDir = Join-Path $stageRoot 'switch/.overlays'
New-Item -ItemType Directory -Force -Path $buildRoot,$exefsRoot,$moduleDir,$overlayDir | Out-Null
$common = @('-std=gnu++17','-O2','-g','-Wall','-Wextra','-Wno-unused-parameter','-Wno-missing-field-initializers',
    '-march=armv8-a','-mtune=cortex-a57','-mtp=soft','-fPIE','-ffunction-sections','-fdata-sections',
    '-fno-exceptions','-fno-rtti','-D__SWITCH__',"-I$env:DEVKITPRO/libnx/include", "-I$projectRoot/src")
$link = @("-specs=$env:DEVKITPRO/libnx/switch.specs", "-L$env:DEVKITPRO/libnx/lib",'-lnx')
RunChecked $compiler ($common + @("$projectRoot/src/probe.cpp",'-o',"$buildRoot/probe.elf") + $link)
RunChecked "$toolRoot/elf2nso.exe" @("$buildRoot/probe.elf","$exefsRoot/main")
RunChecked "$toolRoot/npdmtool.exe" @("$projectRoot/module.json","$exefsRoot/main.npdm")
RunChecked "$toolRoot/build_pfs0.exe" @($exefsRoot,"$moduleDir/exefs.nsp")
RunChecked $compiler ($common + @('-std=gnu++20','-frtti','-Os',"-I$projectRoot/vendor/libtesla/include",'-DAPP_TITLE="Lite Recorder"',
    '-DAPP_VERSION="0.2.1"', "$projectRoot/src/overlay.cpp",'-o',"$buildRoot/overlay.elf") + $link)
RunChecked "$toolRoot/nacptool.exe" @('--create','Lite Recorder','Lite Recorder contributors','0.2.1',"$buildRoot/control.nacp")
RunChecked "$toolRoot/elf2nro.exe" @("$buildRoot/overlay.elf","$overlayDir/ovl-lite-probe.ovl","--nacp=$buildRoot/control.nacp")
Copy-Item -LiteralPath "$projectRoot/toolbox.json" -Destination $moduleDir -Force
Write-Host 'Build complete. No boot2.flag is installed; load the module from the overlay.'
