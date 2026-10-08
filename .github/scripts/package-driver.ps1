# Empaqueta y firma el driver ya compilado (MSBuild) junto con desklink-driver.exe.
# Lo usan windows-driver.yml (paquete suelto) y release.yml (instalador).
#
#   package-driver.ps1 -Out pkg
#
# Además deja driver-id.txt: identifica el código del driver (no la compilación). El instalador lo compara con el
# del driver ya instalado y sólo reinstala el driver cuando cambia.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'

$pkg = New-Item -ItemType Directory -Force $Out
$built = 'windows/driver/idd/x64/Release'
Copy-Item "$built/desklink-idd.dll" $pkg
# El .inf ya sellado (versión y arquitectura) que deja MSBuild junto a la DLL.
Copy-Item "$built/desklink-idd.inf" $pkg

$kits = 'C:\Program Files (x86)\Windows Kits\10\bin'
$ver = Get-ChildItem $kits -Directory | Where-Object Name -like '10.*' | Sort-Object Name -Descending |
       Where-Object { Test-Path "$($_.FullName)\x64\signtool.exe" } | Select-Object -First 1
$signtool = "$($ver.FullName)\x64\signtool.exe"
$inf2cat = "$($ver.FullName)\x86\Inf2Cat.exe"

# Certificado nuevo en cada compilación. La clave privada nunca sale de esta máquina de la CI
# (no se puede exportar y la máquina se destruye al terminar): nadie puede firmar otra cosa con él.
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=DeskLink Driver $env:GITHUB_RUN_NUMBER" `
        -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(5)
Export-Certificate -Cert $cert -FilePath "$pkg/desklink-idd.cer" -Type CERT | Out-Null

& $signtool sign /fd sha256 /sha1 $cert.Thumbprint "$pkg/desklink-idd.dll"
if ($LASTEXITCODE) { throw 'signtool (dll)' }
& $inf2cat /driver:$pkg /os:10_X64 /uselocaltime
if ($LASTEXITCODE) { throw 'Inf2Cat' }
& $signtool sign /fd sha256 /sha1 $cert.Thumbprint "$pkg/desklink-idd.cat"
if ($LASTEXITCODE) { throw 'signtool (cat)' }
Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey

Copy-Item build/setup/Release/desklink-driver.exe $pkg
Copy-Item windows/driver/setup/*.bat $pkg

# Hash git del código del driver y de su interfaz: igual en todas las compilaciones mientras no cambie.
$idd = git rev-parse HEAD:windows/driver/idd
$inc = git rev-parse HEAD:windows/driver/include
if ($LASTEXITCODE -or -not $idd -or -not $inc) { throw 'git rev-parse (driver-id)' }
Set-Content -NoNewline -Encoding ascii "$pkg/driver-id.txt" "$($idd.Substring(0, 12))-$($inc.Substring(0, 12))"

Get-ChildItem $pkg
