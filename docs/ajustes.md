# Ajustes: dónde se elige cada cosa

**Todo se elige en la tablet.** La app de Android es el único sitio con ajustes de imagen, sonido y táctil; al
conectar los manda al PC. La app del PC enseña el estado y guarda solo lo que la tablet no puede saber.

## En la tablet (Ajustes)

A la vista, lo que casi todo el mundo toca:

| Ajuste | Opciones | Qué hace |
| --- | --- | --- |
| Perfil | Calidad · Equilibrado (de serie) · Ahorro | fps, Mbps y escala de la imagen, y lo que gasta la tablet (brillo, WiFi, CPU). Ver [batería](battery.md) |
| Sonido | sí / no (de serie, no) | El sonido del PC suena en la tablet |
| Táctil | Táctil (de serie) · Ratón · Solo ver | Toques reales de Windows, el dedo mueve el puntero, o los toques no llegan al PC |

Plegado en **Avanzado**:

- **Imagen**: resolución del monitor (Nativa, Equilibrada, Ligera), calidad (Mbps), fluidez (fps) y códec. En
  «Automática» decide el perfil. Un valor fijo manda sobre el perfil, salvo cuando «adaptar a la carga» baja a un
  perfil menor que el elegido: entonces vuelven los topes de ese perfil para no descargar la batería por cable.
  «Volver a lo automático» lo deja todo como de serie.
- **Batería**: adaptar a la carga por USB.
- **Pantalla y conexión**: estadísticas, mantener la pantalla encendida, conectar solo al abrir.

## En el PC (desklink.exe)

La ventana dice que los ajustes están en la tablet y, conectada, enseña lo que pide («Equilibrado · 60 fps ·
30.0 Mbps · 100 %»). Plegado en **Avanzado**: pantalla extra o duplicar un monitor, qué monitor, dibujar el puntero,
cable USB y el puerto.

`desklink.ini` admite además `fps`, `bitrate`, `codec` y `size` para fijarlos desde el PC (mandan sobre la tablet);
si están puestos, la ventana lo avisa en ámbar.

## Cómo viaja cada ajuste

| Ajuste de la tablet | Paquete | Cuándo |
| --- | --- | --- |
| Resolución, fps, Mbps, códec | HELLO (payload) | Cada HELLO; el host solo rehace el pipeline si cambia |
| Perfil (topes de fps, Mbps y escala) | QUALITY_REQUEST | Tras el HELLO, al cambiar de perfil y cada 5 s. Con fps o Mbps fijos en Avanzado, ese tope va a 0 |
| Sonido | AUDIO_CONTROL | Tras el HELLO y cada segundo |
| Táctil (Táctil o Ratón) | INPUT_MODE | Tras el HELLO y cada 5 s. «Solo ver» no manda nada y la tablet no envía toques |

No hay paquetes nuevos: la APK y el host de la misma versión se entienden, y con un host anterior la tablet sigue
funcionando (INPUT_MODE lo ignora un host que no lo conoce).

Sin `--fps`, `--bitrate` ni `--codec`, el host parte de 60 fps, 50 Mbps y HEVC, y la tablet los cambia en el HELLO.
