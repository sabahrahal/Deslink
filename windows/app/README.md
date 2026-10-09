# windows/app

`desklink.exe`: la app de DeskLink para Windows. Vive en la bandeja (junto al reloj) y maneja `desklink-host.exe`
sin consola: lo arranca con los ajustes guardados, enseña el estado y explica los errores. Win32 nativo en C++
(sin .NET ni WinUI): un solo .exe pequeño, sin instalar nada más.

La ventana se dibuja entera con GDI+, en una columna y con el estilo limpio de `docs/design.md`: los colores de
`design/tokens.json` (`src/tokens.h`, generado), en claro u oscuro según Windows. Lo que enseña en cada estado sale
de `src/view_model.cpp`, sin Windows, y lo prueba `tests/test_view_model.cpp`.

## Qué hace

- **Estado**: conectado por cable USB o por Wi-Fi (con la IP de la tablet), fps, Mbps y retardo, y por qué la imagen
  va reducida si es así. Esperando, enseña las dos formas de conectar: el cable USB (enchufada y lista, o qué falta)
  y el Wi-Fi (la IP de este PC).
- **Iniciar / Detener** la transmisión, desde la ventana o desde el menú del icono (clic derecho).
- **Ajustes a la vista**: modo (pantalla extra o duplicar), calidad (Alta, Equilibrada, Ahorro), sonido (en el PC o
  en la tablet) y táctil (táctil, ratón o solo ver). El PC manda: la tablet usa lo que se elija aquí (ver
  [ajustes](../../docs/ajustes.md)).
- **Configuración avanzada**, plegada: resolución, fluidez (fps), calidad de vídeo (Mbps), códec, monitor que se
  duplica, bajar la calidad si la batería de la tablet no aguanta, puntero, cable USB, abrir con Windows,
  transmitir al abrir, el puerto y **Ver registro** (la salida completa del host, con «Copiar todo»).
- Casi todo se aplica en marcha: la app manda al host una línea `@ajustes …` por su entrada. El monitor, el puntero
  y el cable USB necesitan reiniciar la transmisión: aparece **Aplicar cambios**. El modo reinicia solo.
- Se guarda en `%APPDATA%\DeskLink\desklink.ini` (`config=3`; los `.ini` anteriores se pasan solos).
- **Errores en claro**: falta el driver, puerto ocupado, la GPU no puede codificar, el monitor ya no existe…
- Si el host se cae después de funcionar un rato, se vuelve a arrancar solo. Al cerrar la ventana la app sigue en la
  bandeja; para cerrarla del todo: clic derecho en el icono > **Salir** (quita el monitor virtual y el túnel de adb).

Opciones que solo están en el `.ini`: `port`, `adb` (ruta de adb.exe) y `extra` (opciones más para el host, p. ej.
`--refine 0 --pace 80`).

## Cómo se habla con el host

`desklink.exe` lanza `desklink-host.exe` (de la misma carpeta) con las opciones de siempre más:

- `--status`: además del texto normal, una línea por segundo `@estado enlace=usb|wifi|ninguno ip=… codificados=…
  mbps=… cod_ms=… rtt_ms=… perdidos=… size=AxB fps_obj=… mbps_obj=… codec=… reducido=no|wifi|bateria|carga adb=N
  adb_pendientes=N`, `@listo` al empezar a escuchar y `@ajuste tactil=…` si la tablet cambia el táctil desde su menú.
  Con `--status` el host lee además su entrada: cada línea `@ajustes calidad=… fps=… mbps=… codec=… sonido=… tactil=…
  bateria=… size=…` se aplica en marcha.
- `--stop-event nombre`: evento con nombre que la app activa para que el host pare limpiamente.

El host va en un «job» de Windows: si la app muere de golpe, el host se cierra con ella (y el driver quita el monitor
virtual a los pocos segundos). La captura y la codificación no cambian: son las mismas que desde la consola.

## Compilar

```
cmake -S windows/app -B build/app -A x64
cmake --build build/app --config Release
```

Sale en `build/app/Release/desklink.exe`. `ctest --test-dir build/app -C Release` corre las pruebas de la ventana
(también en Linux: sin Windows solo se compilan las pruebas). Tiene que ir en la misma carpeta que `desklink-host.exe` (y, para el cable
USB, `adb.exe`, `AdbWinApi.dll` y `AdbWinUsbApi.dll`). La CI (`windows-app`) publica las dos juntas en el artefacto
`desklink-windows`.

El icono (`res/desklink.ico`, degradado azul) sale de `res/make_icon.py`; la bandeja dibuja la misma tableta
en gris (detenido), ámbar (esperando), verde (conectado) o rojo (error).
