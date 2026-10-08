# Instalar y probar una versión nueva

Cada cambio que entra en `main` lo compila la CI y lo publica como la versión de prueba **latest** de GitHub Releases.
Los enlaces no cambian nunca (el repo es privado: hay que tener la sesión de GitHub iniciada):

- PC: <https://github.com/sabahrahal/Deslink/releases/download/latest/DeskLink-Setup.exe>
- Tablet: <https://github.com/sabahrahal/Deslink/releases/download/latest/DeskLink.apk>

La página de la versión (<https://github.com/sabahrahal/Deslink/releases/tag/latest>) dice de qué commit sale.

## La primera vez

1. **PC**: si tenías el driver de un paquete anterior de la CI, ejecuta una última vez su `desinstalar-driver.bat`.
   Después abre `DeskLink-Setup.exe`. Instala el host, `adb` y el monitor virtual, y crea «DeskLink» en el menú Inicio.
2. **Tablet**: desinstala la app DeskLink que tengas (las APK anteriores se firmaban con otra clave; es la última vez).
   Instala `DeskLink.apk` (abriéndola en la tablet o con «Actualizar la tablet», abajo).

## Cada vez que haya una versión nueva

- **PC**: abre otra vez `DeskLink-Setup.exe` (o «Actualizar DeskLink en el PC» en el menú Inicio). Se instala encima,
  cierra el host si estaba abierto y sólo reinstala el monitor virtual si su código cambió.
- **Tablet**: con la tablet enchufada por USB (depuración USB activada), «Actualizar la tablet» en el menú Inicio.
  Baja la APK y hace `adb install -r`: la app se actualiza sin perder sus ajustes. También vale abrir el enlace de la
  APK en la tablet e instalarla encima.

«Actualizar…» necesita leer el repo privado: usa la CLI `gh` si tiene la sesión iniciada; si no, la primera vez pide un
token de GitHub de solo lectura y lo guarda cifrado para tu usuario (ver `tools/dev/actualizar.ps1`).

## Desinstalar

Configuración → Aplicaciones → DeskLink → Desinstalar (o «Desinstalar DeskLink» en el menú Inicio). Quita el host,
`adb`, el monitor virtual, su driver y su certificado.

## Cómo está hecho

- Instalador: Inno Setup, en [`windows/installer/desklink.iss`](../windows/installer/desklink.iss).
- Publicación: [`.github/workflows/release.yml`](../.github/workflows/release.yml). En los PR también se compila y el
  instalador y la APK quedan como artefactos de la ejecución, sin publicar nada.
- Firma de la APK: [`android/keystore/`](../android/keystore/README.md).
- El driver se firma con un certificado nuevo en cada compilación (ver `windows/driver/README.md`). El instalador
  guarda un identificador del código del driver (`driver-id.txt`, hash git de `windows/driver/idd` y `include`) y,
  si no cambió, no lo toca: el driver instalado sigue funcionando con su certificado.
