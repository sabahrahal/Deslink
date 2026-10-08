# windows/installer

Instalador de DeskLink para Windows (Inno Setup 6.3+): host, `adb`, monitor virtual y entradas del menú Inicio en
un solo `DeskLink-Setup.exe`. Lo arma `.github/workflows/release.yml`; cómo usarlo en
[docs/instalar-y-probar.md](../../docs/instalar-y-probar.md).

Para armarlo a mano hace falta la misma carpeta que prepara la CI (`host\`, `adb\`, `driver\` y `tools\`) y:

```
iscc /DAppVersion=0.1.0 /DStage=C:\ruta\carpeta /DDriverId=<contenido de driver\driver-id.txt> windows\installer\desklink.iss
```
