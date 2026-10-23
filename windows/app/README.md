# windows/app

`extendesk.exe`: la app de Extendesk para Windows. Vive en la bandeja (junto al reloj) y maneja `extendesk-host.exe`
sin consola: lo arranca con los ajustes guardados, enseña el estado y explica los errores. Win32 nativo en C++
(sin .NET ni WinUI): un solo .exe pequeño, sin instalar nada más.

La ventana se dibuja entera con GDI+, en una columna y con el estilo limpio de `docs/design.md`: los colores de
`design/tokens.json` (`src/tokens.h`, generado), en claro u oscuro según Windows. Lo que enseña en cada estado sale
de `src/view_model.cpp`, sin Windows, y lo prueba `tests/test_view_model.cpp`.

## Qué hace

- **Estado**, corto: conectado por cable USB (por anclaje o por depuración, es el mismo «cable») o por Wi-Fi, y por
  qué la imagen va reducida si es por algo que el usuario entiende y puede arreglar (Wi-Fi, batería, gráfica; con la
  imagen quieta no se avisa). Esperando, solo lo que hace falta: el cable ya detectado, aceptar la depuración USB,
  falta adb o el PC sin red. Tras un corte, «Reconectando…» con los segundos que la pantalla extra espera.
- **Al registro, no a la ventana**: fps, Mbps y retardo; los fps de cada etapa («Fluidez: Captura · Codificación ·
  Dispositivo fps» con Mbps, RTT, resolución y códec, cada 10 s conectado), los avisos técnicos del dispositivo
  («Aviso: …» al aparecer y «Aviso resuelto: …» al irse), cada conexión y desconexión y las IP de este PC («IP de este
  PC: …», al cambiar). Se ven en **Ver registro**.
- **La ventana se puede agrandar** hasta 720 px de ancho y el alto del contenido (sin maximizar), y estrechar hasta
  480 × 360. Mientras no se toque, toma el alto del contenido; si se cambia el tamaño, se recuerda en el `.ini`
  (`window_w`, `window_h`, a 96 ppp).
- **Iniciar / Detener** la transmisión, desde la ventana o desde el menú del icono (clic derecho).
- **Ajustes a la vista**: modo (pantalla extra o duplicar), la resolución (abre la de Windows; con duplicar, el
  monitor que se duplica), calidad (Alta, Equilibrada, Ahorro) y sonido (en el PC o en la tablet), cada uno con una
  línea que dice qué hace. El PC manda: la tablet usa lo que se elija aquí (ver [ajustes](../../docs/ajustes.md)).
- **Configuración avanzada**, plegada: táctil (táctil o solo ver), red Wi-Fi del PC, abrir con Windows, **Ayuda** y
  **Ver registro** (la salida completa del host, con «Copiar todo»).
- **Ayuda**: un «?» junto a Modo, Calidad y Red Wi-Fi del PC abre su sección de `https://extendesk.app/ayuda`
  (`HelpUrl`); «Ayuda» en avanzada abre la página entera.
- Siempre escucha nada más abrirse (ya no es una opción): la pantalla extra solo aparece cuando un dispositivo se
  conecta, así que no cuesta nada. El cable USB, abrir la app de la tablet al enchufarla y bajar la fluidez si la
  batería de la tablet no aguanta también van siempre.
- Casi todo se aplica en marcha: la app manda al host una línea `@ajustes …` por su entrada. El monitor necesita
  reiniciar la transmisión: aparece **Aplicar cambios**. El modo reinicia solo.
- Se guarda en `%APPDATA%\Extendesk\extendesk.ini` (`config=3`; los `.ini` anteriores, también el `%APPDATA%\DeskLink\desklink.ini`
  de antes del nombre Extendesk, se pasan solos).
- **Errores en claro**: falta el driver, puerto ocupado, la GPU no puede codificar, el monitor ya no existe…
- Si el host se cae después de funcionar un rato, se vuelve a arrancar solo. Al cerrar la ventana la app sigue en la
  bandeja; para cerrarla del todo: clic derecho en el icono > **Salir** (quita el monitor virtual y el túnel de adb).

Opciones que solo están en el `.ini`: `fps`, `bitrate` (Mbps) y `codec` (`hevc` o `h264`; 0 o vacío = automático),
`port`, `adb` (ruta de adb.exe) y `extra` (opciones más para el host, p. ej. `--refine 0 --pace 80`, o `--no-usb` para
no abrir el túnel del cable). Las claves `usb`, `open_tablet`, `stream_on_open` y `battery_adapt` de `.ini` viejos ya no
se leen.

## Cómo se habla con el host

`extendesk.exe` lanza `extendesk-host.exe` (de la misma carpeta) con las opciones de siempre más:

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

Sale en `build/app/Release/extendesk.exe`. `ctest --test-dir build/app -C Release` corre las pruebas de la ventana
(también en Linux: sin Windows solo se compilan las pruebas). Tiene que ir en la misma carpeta que `extendesk-host.exe` (y, para el cable
USB, `adb.exe`, `AdbWinApi.dll` y `AdbWinUsbApi.dll`). La CI (`windows-app`) publica las dos juntas en el artefacto
`extendesk-windows`.

El icono (`res/extendesk.ico`, los dos eslabones de `design/logo/`) sale de `tools/design/gen_logo.py --ico`; la
cabecera de la ventana y la bandeja dibujan el mismo logo con GDI+ (`src/logo.h`), la bandeja en gris (detenido),
ámbar (esperando), verde (conectado) o rojo (error). Las fuentes del logo (Montserrat) van como recursos.
