# Baja lo último de la versión «latest» de GitHub Releases (la CI la rehace con cada cambio en main) y lo instala.
#
#   actualizar.ps1 -Tablet   APK en la tablet conectada por USB (adb install -r: conserva datos y ajustes)
#   actualizar.ps1 -PC       instalador de Windows (actualiza encima; sólo reinstala el driver si cambió)
#   actualizar.ps1 -Tablet -PC
#
# El repo es privado: hace falta un token de GitHub con permiso de lectura del repo. Si tienes la CLI `gh` con
# sesión iniciada se usa esa; si no, la primera vez pide un token y lo guarda cifrado para tu usuario de Windows en
# %APPDATA%\Extendesk\github-token.txt (bórralo para cambiarlo).
param([switch]$Tablet, [switch]$PC, [string]$Repo = 'sabahrahal/Extendesk', [string]$Tag = 'latest')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest va mucho más rápido sin barra de progreso
if (-not $Tablet -and -not $PC) { $Tablet = $true }

$dir = Join-Path $env:TEMP 'Extendesk-actualizar'
New-Item -ItemType Directory -Force $dir | Out-Null

function Get-Token {
  if ($env:GITHUB_TOKEN) { return $env:GITHUB_TOKEN }
  $file = Join-Path $env:APPDATA 'Extendesk\github-token.txt'
  # Antes del nombre Extendesk se guardaba en %APPDATA%\DeskLink.
  $legacy = Join-Path $env:APPDATA 'DeskLink\github-token.txt'
  if (-not (Test-Path $file) -and (Test-Path $legacy)) {
    New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
    Move-Item $legacy $file
  }
  if (Test-Path $file) {
    $secure = Get-Content $file | ConvertTo-SecureString
  } else {
    Write-Host 'Hace falta un token de GitHub para bajar de un repo privado (sólo la primera vez).'
    Write-Host 'Créalo en https://github.com/settings/personal-access-tokens/new con acceso de solo lectura'
    Write-Host "a «Contents» del repo $Repo."
    $secure = Read-Host 'Token' -AsSecureString
    New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
    $secure | ConvertFrom-SecureString | Set-Content $file
  }
  return [System.Net.NetworkCredential]::new('', $secure).Password
}

function Get-Asset([string]$Name) {
  $out = Join-Path $dir $Name
  Remove-Item $out -ErrorAction SilentlyContinue
  if (Get-Command gh -ErrorAction SilentlyContinue) {
    & gh release download $Tag -R $Repo -p $Name -D $dir --clobber 2>$null
    if (-not $LASTEXITCODE -and (Test-Path $out)) { return $out }
  }
  $headers = @{ Authorization = "Bearer $(Get-Token)"; 'X-GitHub-Api-Version' = '2022-11-28' }
  $release = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/tags/$Tag" -Headers $headers
  $asset = $release.assets | Where-Object name -eq $Name
  if (-not $asset) { throw "La versión '$Tag' no tiene $Name." }
  Write-Host "Bajando $Name ($([math]::Round($asset.size / 1MB, 1)) MB) de $($release.name)..."
  Invoke-WebRequest $asset.url -Headers ($headers + @{ Accept = 'application/octet-stream' }) -OutFile $out
  return $out
}

function Find-Adb {
  foreach ($p in @("$PSScriptRoot\..\adb.exe", "$PSScriptRoot\adb.exe",
                   "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe")) {
    if (Test-Path $p) { return (Resolve-Path $p).Path }
  }
  $cmd = Get-Command adb.exe -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  throw 'No encuentro adb.exe (instala Extendesk o pon adb en el PATH).'
}

if ($Tablet) {
  $apk = Get-Asset 'Extendesk.apk'
  $adb = Find-Adb
  Write-Host 'Instalando en la tablet...'
  & $adb install -r $apk
  if ($LASTEXITCODE) {
    Write-Host ''
    Write-Host 'Si dice INSTALL_FAILED_UPDATE_INCOMPATIBLE: la app de la tablet está firmada con otra clave'
    Write-Host '(las APK anteriores a este cambio). Desinstálala una vez y vuelve a ejecutar esto.'
    throw 'adb install falló.'
  }
  Write-Host 'Tablet actualizada.'
}

if ($PC) {
  $setup = Get-Asset 'Extendesk-Setup.exe'
  Write-Host 'Instalando en el PC (Windows pedirá permiso de administrador)...'
  $p = Start-Process $setup -ArgumentList '/SILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
  if ($p.ExitCode) { throw "El instalador terminó con el código $($p.ExitCode)." }
  Write-Host 'PC actualizado.'
}
