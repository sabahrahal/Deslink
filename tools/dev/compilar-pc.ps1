# Compila en este PC el instalador de Windows (driver firmado, host, app de bandeja y adb), igual que release.yml,
# sin GitHub Actions. Se lanza desde una copia del repo:
#
#   powershell -ExecutionPolicy Bypass -File tools\dev\compilar-pc.ps1            prepara el PC (la 1.ª vez) y compila
#   powershell -ExecutionPolicy Bypass -File tools\dev\compilar-pc.ps1 -Instalar  además lo instala encima
#
# La primera vez instala con winget lo que falte: Git, CMake, Visual Studio 2022 Build Tools (C++), el WDK (para el
# driver) e Inno Setup. Son unos 10 GB y tarda un buen rato; después cada compilación es de pocos minutos.
# El driver se firma con un certificado nuevo de este PC (como en la CI): el instalador lo añade a los de confianza.
param([switch]$Instalar, [string]$Version = '0.2.0')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
Set-Location $repo

function Test-Admin {
  ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Install-Winget([string]$Id, [string]$Extra = '') {
  Write-Host "Instalando $Id (puede tardar)..."
  $wargs = @('install', '--id', $Id, '-e', '--accept-package-agreements', '--accept-source-agreements', '--silent')
  if ($Extra) { $wargs += @('--override', $Extra) }
  & winget @wargs
  # 0 = instalado; -1978335189 = ya estaba y no hay nada nuevo.
  if ($LASTEXITCODE -and $LASTEXITCODE -ne -1978335189) { throw "winget no pudo instalar $Id (código $LASTEXITCODE)." }
}

function Find-VS {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { return $null }
  $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($path) { return $path } else { return $null }
}

function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}

# --- 1. Herramientas ------------------------------------------------------------------------------------------------
$kits = 'C:\Program Files (x86)\Windows Kits\10'
$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
$needs = @()
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { $needs += 'git' }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { $needs += 'cmake' }
if (-not (Find-VS)) { $needs += 'vs' }
if (-not (Test-Path "$kits\Include\*\km\iddcx")) { $needs += 'wdk' }
if (-not (Test-Path $iscc)) { $needs += 'inno' }
if ($needs) {
  if (-not (Test-Admin)) { throw 'Falta instalar herramientas: vuelve a lanzarlo como administrador (solo hace falta la primera vez).' }
  if ($needs -contains 'git') { Install-Winget 'Git.Git' }
  if ($needs -contains 'cmake') { Install-Winget 'Kitware.CMake' }
  if ($needs -contains 'vs') {
    Install-Winget 'Microsoft.VisualStudio.2022.BuildTools' ('--quiet --wait --norestart ' +
      '--add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ' +
      '--add Microsoft.VisualStudio.Component.VC.ATL --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --includeRecommended')
  }
  # El WDK trae la extensión de Visual Studio que compila drivers (PlatformToolset WindowsUserModeDriver10.0).
  if ($needs -contains 'wdk') { Install-Winget 'Microsoft.WindowsWDK.10.0.26100' }
  if ($needs -contains 'inno') { Install-Winget 'JRSoftware.InnoSetup' }
  Refresh-Path
}
$vs = Find-VS
if (-not $vs) { throw 'No encuentro Visual Studio 2022 con C++ después de instalarlo.' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'

# --- 2. Compilar (lo mismo que el trabajo «windows» de release.yml) -----------------------------------------------
Write-Host '== Driver'
& $msbuild windows/driver/idd/desklink-idd.vcxproj /p:Configuration=Release /p:Platform=x64 /m /v:minimal
if ($LASTEXITCODE) {
  throw ('No compiló el driver. Si el error habla de «WindowsUserModeDriver10.0», falta la extensión del WDK para ' +
         'Visual Studio: instálala desde el instalador del WDK (o reinstala Microsoft.WindowsWDK.10.0.26100).')
}

Write-Host '== Herramienta del driver'
cmake -S windows/driver/setup -B build/setup -A x64
cmake --build build/setup --config Release
if ($LASTEXITCODE) { throw 'No compiló desklink-driver.exe.' }

Write-Host '== Firmar el driver'
Remove-Item -Recurse -Force build/installer -ErrorAction SilentlyContinue
& "$repo\.github\scripts\package-driver.ps1" -Out build/installer/driver

Write-Host '== Host'
cmake -S windows/host -B build/host -A x64
cmake --build build/host --config Release
if ($LASTEXITCODE) { throw 'No compiló el host.' }

Write-Host '== App de bandeja'
cmake -S windows/app -B build/app -A x64
cmake --build build/app --config Release
if ($LASTEXITCODE) { throw 'No compiló la app de bandeja.' }

Write-Host '== Preparar los archivos'
$stage = 'build/installer'
New-Item -ItemType Directory -Force "$stage/host", "$stage/adb", "$stage/tools" | Out-Null
Get-ChildItem build/host/Release, build/app/Release -File | Where-Object Extension -in '.exe', '.dll' |
  Copy-Item -Destination "$stage/host"
Copy-Item tools/dev/actualizar.ps1, tools/dev/actualizar-tablet.bat, tools/dev/actualizar-pc.bat "$stage/tools"
$pt = Join-Path $env:TEMP 'desklink-platform-tools'
if (-not (Test-Path "$pt/platform-tools/adb.exe")) {
  Invoke-WebRequest https://dl.google.com/android/repository/platform-tools-latest-windows.zip -OutFile "$pt.zip"
  Expand-Archive "$pt.zip" -DestinationPath $pt -Force
}
Copy-Item "$pt/platform-tools/adb.exe", "$pt/platform-tools/AdbWinApi.dll", "$pt/platform-tools/AdbWinUsbApi.dll" "$stage/adb"

Write-Host '== Instalador'
$stageFull = (Resolve-Path $stage).Path
$driverId = Get-Content -Raw "$stage/driver/driver-id.txt"
& $iscc "/DAppVersion=$Version" "/DStage=$stageFull" "/DDriverId=$driverId" "/O$repo\build" windows/installer/desklink.iss
if ($LASTEXITCODE) { throw 'Inno Setup no pudo armar el instalador.' }
$setup = "$repo\build\Extendesk-Setup.exe"
Write-Host "Listo: $setup"

if ($Instalar) {
  Write-Host 'Instalando encima (Windows pedirá permiso de administrador)...'
  $p = Start-Process $setup -ArgumentList '/SILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
  if ($p.ExitCode) { throw "El instalador terminó con el código $($p.ExitCode)." }
  Write-Host 'PC actualizado.'
}
