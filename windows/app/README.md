# windows/app

`desklink.exe`: la app de DeskLink para Windows. Vive en la bandeja (junto al reloj) y maneja `desklink-host.exe`
sin consola: lo arranca con los ajustes guardados, enseña el estado y explica los errores. Win32 nativo en C++
(sin .NET ni WinUI): un solo .exe pequeño, sin instalar nada más.

## Qué hace

- **Iniciar / Detener** la transmisión, desde la ventana o desde el menú del icono (clic derecho).
- **Estado**: conectado por cable USB o por WiFi (con la IP de la tablet), resolución, fps, Mbps, tiempo de
  codificación, RTT (si la tablet contesta el PING del host) y frames perdidos; si hay tablets enchufadas por USB con
  la depuración autorizada; y las IP de este PC, para escribirlas en la tablet si hiciera falta.
- **Ajustes** (se guardan en `%APPDATA%\DeskLink\desklink.ini`): monitor virtual o duplicar un monitor, qué monitor,
  resolución del monitor virtual, fps, Mbps, códec, táctil/ratón, puntero, cable USB.
  Con la transmisión en marcha aparece **Aplicar cambios**, que reinicia el host con los ajustes nuevos.
- **Abrir al iniciar Windows** (registro del usuario, `HKCU\...\Run`, sin permisos de administrador) y **empezar a
  transmitir al abrir**: con los dos, al encender el PC el monitor virtual y el túnel USB ya están listos.
- **Errores en claro**: falta el driver, puerto ocupado, la GPU no puede codificar, el monitor ya no existe…
  El texto completo del host está en **Registro…** (con «Copiar todo» para mandarlo).
- Si el host se cae después de funcionar un rato, se vuelve a arrancar solo. Al cerrar la ventana la app sigue en la
  bandeja; para cerrarla del todo: clic derecho en el icono > **Salir** (quita el monitor virtual y el túnel de adb).

Opciones que solo están en el `.ini`: `port`, `adb` (ruta de adb.exe) y `extra` (opciones más para el host, p. ej.
`--refine 0 --pace 80`).

## Cómo se habla con el host

`desklink.exe` lanza `desklink-host.exe` (de la misma carpeta) con las opciones de siempre más:

- `--status`: además del texto normal, una línea por segundo `@estado enlace=usb|wifi|ninguno ip=… codificados=…
  mbps=… cod_ms=… rtt_ms=… perdidos=… size=AxB adb=N adb_pendientes=N` y `@listo` al empezar a escuchar.
- `--stop-event nombre`: evento con nombre que la app activa para que el host pare limpiamente.

El host va en un «job» de Windows: si la app muere de golpe, el host se cierra con ella (y el driver quita el monitor
virtual a los pocos segundos). La captura y la codificación no cambian: son las mismas que desde la consola.

## Compilar

```
cmake -S windows/app -B build/app -A x64
cmake --build build/app --config Release
```

Sale en `build/app/Release/desklink.exe`. Tiene que ir en la misma carpeta que `desklink-host.exe` (y, para el cable
USB, `adb.exe`, `AdbWinApi.dll` y `AdbWinUsbApi.dll`). La CI (`windows-app`) publica las dos juntas en el artefacto
`desklink-windows`.

El icono (`res/desklink.ico`) sale de `res/make_icon.py`; la bandeja dibuja el mismo icono en gris (detenido),
ámbar (esperando), verde (conectado) o rojo (error).
