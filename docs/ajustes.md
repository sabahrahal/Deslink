# Ajustes: dónde se elige cada cosa

**El PC manda.** La ventana de DeskLink en el PC es el único sitio donde se eligen la imagen, el sonido y el táctil.
La tablet dice al conectar lo que puede hacer (su pantalla, su frecuencia y si decodifica HEVC) y enseña lo que el
PC decide. Así no hay dos sitios con la misma opción ni ajustes que se pisan.

## En el PC (desklink.exe)

A la vista, lo que casi todo el mundo toca:

| Ajuste | Opciones | Qué hace |
| --- | --- | --- |
| Modo | Pantalla extra (de serie) · Duplicar | Monitor virtual a la medida de la tablet, o copia de un monitor |
| Calidad | Alta (de serie) · Equilibrada · Ahorro | Alta: los fps y la tasa más altos que aguanten tablet y PC. Equilibrada: hasta 60 fps y 30 Mbps. Ahorro: 30 fps, gasta menos en la tablet |
| Sonido | En el PC (de serie) · En la tablet | Dónde suena el PC |
| Táctil | Táctil (de serie) · Ratón · Solo ver | Toques reales de Windows, el dedo mueve el puntero, o los toques no llegan al PC |

En **Configuración avanzada**: fluidez (fps), calidad de vídeo (Mbps), códec (todo «Automático» de serie), el
monitor que se duplica, «bajar la calidad si la batería de la tablet no aguanta» (de serie, no), dibujar el puntero,
cable USB, abrir DeskLink en la tablet al enchufarla (de serie, sí), abrir con Windows, transmitir nada más abrir, el
puerto y el registro.

La **resolución de la pantalla extra** no tiene ajuste propio: «Cambiar la resolución en Windows» abre Configuración
de pantalla y el host sigue lo que se elija ahí. La recuerda para esa tablet (por su resolución nativa) y manda sobre
la de la tablet; con otra tablet se vuelve a usar su resolución nativa.

**Abrir al enchufar**: con la depuración USB aceptada, el host lanza la app en la tablet
(`adb shell am start -a com.desklink.action.USB_ATTACHED`) y esta conecta sola por el cable. No usa el modo accesorio
USB, así que no deja nada registrado en la tablet ni muestra «ninguna aplicación funciona con este accesorio».

Con la transmisión en marcha la ventana enseña también dónde se quedan los fps: «Captura · Codificación · Tablet».

Casi todo se aplica en marcha, sin cortar la imagen. Solo el modo (se reinicia solo), el monitor, el puntero y el
cable USB reinician la transmisión; para esos aparece «Aplicar cambios».

### Qué es «Automático»

- **Fluidez**: 120 fps si la tablet llega a 120 Hz, 90 si llega a 90, si no 60.
- **Calidad de vídeo**: unos 0,08 bits por píxel en HEVC (0,12 en H.264), entre 20 y 100 Mbps. 2944×1840 a 120 fps
  en HEVC son unos 52 Mbps.
- **Códec**: HEVC si la tablet lo decodifica, si no H.264.
- **Resolución**: la de la pantalla de la tablet, o la que se deje en Configuración de pantalla de Windows.

## En la tablet

La pantalla de inicio enseña una tarjeta por PC; si llega por cable USB y por Wi-Fi a la vez, la misma tarjeta tiene
las dos opciones (de serie, el cable). El anclaje USB cuenta como cable, no como Wi-Fi. Al conectarse sola, si el
último PC llega también por cable, usa el cable. En **Configuración avanzada** solo está lo que es de la tablet: conectar solo
al abrir, escribir la IP del PC, estadísticas, mantener la pantalla encendida y el diagnóstico.

Durante la sesión, el **botón ••• de la esquina** abre el menú rápido (se puede arrastrar si tapa algo; recuerda dónde lo dejas): táctil o ratón (el PC lo recuerda),
estadísticas y desconectar.

## Cómo viaja cada cosa

| Qué | Paquete | Cuándo |
| --- | --- | --- |
| Lo que puede la tablet (pantalla, Hz, dpi, HEVC) | HELLO (payload) | Cada HELLO |
| Lo que decide el PC (calidad, sonido, táctil, batería, fps y tasa de ahora, si va reducida) | SESSION_CONFIG (17) | Tras el STREAM_INFO, al cambiar y cada 2 s |
| Ajustes cambiados en la ventana del PC | línea `@ajustes …` por la entrada del host | Al cambiarlos |
| Táctil o ratón desde el menú rápido | INPUT_MODE | Al tocarlo; el host avisa a la ventana con `@ajuste tactil=…` |
| Batería de la tablet (si en el PC está activado) | QUALITY_REQUEST | Al cambiar y cada 5 s |

Ver [el protocolo](protocol-dlp.md) y [la fluidez automática](battery.md#fluidez-automática).
