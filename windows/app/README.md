# windows/app

`desklink.exe`: la app de DeskLink para Windows. Vive en la bandeja (junto al reloj) y maneja `desklink-host.exe`
sin consola: lo arranca con los ajustes guardados, enseña el estado y explica los errores. Win32 nativo en C++
(sin .NET ni WinUI): un solo .exe pequeño, sin instalar nada más.

La ventana se dibuja entera con GDI+ con el estilo oscuro «gamer» de `docs/design.md` (los mismos colores que la app
de Android, en `namespace color` de `src/main.cpp`): tarjeta de estado con halo, botón principal con el degradado
cian-magenta, opciones en pastillas e interruptores. Barra de título y menús oscuros.

## Qué hace

- **Iniciar / Detener** la transmisión, desde la ventana o desde el menú del icono (clic derecho).
- **Estado**: conectado por cable USB o por WiFi (con la IP de la tablet), resolución, fps, Mbps, tiempo de
  codificación, RTT (si la tablet contesta el PING del host) y frames perdidos; si hay tablets enchufadas por USB con
  la depuración autorizada; y las IP de este PC, para escribirlas en la tablet si hiciera falta.
- **Ajustes**: la calidad, los fps, el códec, la resolución, el sonido y el táctil se eligen en la tablet y llegan
  al conectar (ver [ajustes](../../docs/ajustes.md)); la ventana lo dice y, conectada, enseña lo que pide la tablet.
  Plegado en **Avanzado** queda lo que la tablet no puede saber: pantalla extra (monitor virtual) o duplicar un
  monitor, qué monitor, dibujar el puntero, cable USB y el puerto (solo lectura). Se guardan en
  `%APPDATA%\DeskLink\desklink.ini`. Con la transmisión en marcha aparece **Aplicar cambios**, que reinicia el host.
- **Abrir al iniciar Windows** (registro del usuario, `HKCU\...\Run`, sin permisos de administrador) y **empezar a
  transmitir al abrir**: con los dos, al encender el PC el monitor virtual y el túnel USB ya están listos.
- **Errores en claro**: falta el driver, puerto ocupado, la GPU no puede codificar, el monitor ya no existe…
  El texto completo del host está en **Registro…** (con «Copiar todo» para mandarlo).
- Si el host se cae después de funcionar un rato, se vuelve a arrancar solo. Al cerrar la ventana la app sigue en la
  bandeja; para cerrarla del todo: clic derecho en el icono > **Salir** (quita el monitor virtual y el túnel de adb).

Opciones que solo están en el `.ini`: `port`, `adb` (ruta de adb.exe), `extra` (opciones más para el host, p. ej.
`--refine 0 --pace 80`) y, para fijar algo desde el PC por encima de la tablet, `fps`, `bitrate` (Mbps), `codec`
(`hevc` o `h264`) y `size` (`2560x1600`); vacíos o 0 = lo decide la tablet, y si están puestos la ventana lo avisa.
Los `.ini` de versiones anteriores (sin `config=2`) guardaban fps, calidad, códec, resolución y táctil desde la
ventana: al abrir la app se borran para que mande la tablet.

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

El icono (`res/desklink.ico`, degradado cian-magenta) sale de `res/make_icon.py`; la bandeja dibuja la misma tableta
en gris (detenido), ámbar (esperando), verde (conectado) o rojo (error).
