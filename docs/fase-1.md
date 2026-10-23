# Fase 1: prototipo de pipeline por WiFi

Objetivo: duplicar un monitor de Windows, codificarlo con la GPU, enviarlo por WiFi y verlo en Android, con estadísticas para medir la latencia base.

## Qué hay

- `protocol/`: formato de paquetes y reensamblado, con pruebas (`ctest`).
- `windows/host/`: captura DXGI Desktop Duplication, conversión BGRA a NV12 en GPU, codificador por hardware (con NVIDIA, NVENC directo; si no, Media Foundation) y envío UDP.
- `android/`: app que recibe, reensambla, decodifica por hardware (`MediaCodec` en modo baja latencia) y pinta en una `SurfaceView`.

## Cómo probarlo

1. En GitHub, pestaña Actions, abre la última ejecución y descarga dos artefactos: `desklink-host-windows` (el `.exe`) y `desklink-android-debug` (la APK).
2. En el PC: ejecuta `desklink-host.exe`. Permite el acceso en redes privadas cuando Windows lo pida.
3. En la tablet: instala la APK, escribe la IP del PC y pulsa Conectar. Un toque en la pantalla oculta o muestra las estadísticas.

Con el PC por cable al router y la tablet en 5 o 6 GHz. Opciones del host: ver `windows/host/README.md`.

## Qué mide

- Host: fps capturados y codificados, Mbps, latencia de codificación.
- Tablet: fps recibidos y decodificados, Mbps, RTT, frames perdidos.

La latencia de cristal a cristal se mide a mano: un cronómetro en milisegundos en el monitor duplicado y una foto con el móvil a la tablet y al monitor a la vez.

## Limitaciones de esta fase

- Probado sólo con pruebas automáticas del protocolo. El host de Windows y la app de Android se compilan en CI, pero **no se han ejecutado todavía en un PC y una tablet reales**: pueden aparecer errores en la primera prueba.
- El receptor de Android está en Kotlin; pasar la red y el reensamblado a C++ (NDK) queda para cuando haya medidas que lo justifiquen.
- Sin cifrado ni FEC (ver `docs/protocol-dlp.md`).
- Se duplica un monitor existente; el monitor virtual (IddCx) es la fase 3.
