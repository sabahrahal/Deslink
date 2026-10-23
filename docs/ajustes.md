# Ajustes: dónde se elige cada cosa

**El PC manda.** La ventana de Extendesk en el PC es el único sitio donde se eligen la imagen, el sonido y el táctil.
La tablet dice al conectar lo que puede hacer (su pantalla, su frecuencia y si decodifica HEVC) y enseña lo que el
PC decide. Así no hay dos sitios con la misma opción ni ajustes que se pisan.

## En el PC (extendesk.exe)

A la vista, lo que casi todo el mundo toca:

| Ajuste | Opciones | Qué hace |
| --- | --- | --- |
| Modo | Pantalla extra (de serie) · Duplicar | Monitor virtual a la medida de la tablet, o copia de un monitor |
| Calidad | Alta (de serie) · Equilibrada · Ahorro | Alta: los fps y la tasa más altos que aguanten tablet y PC. Equilibrada: hasta 60 fps y 30 Mbps. Ahorro: 30 fps, gasta menos en la tablet |
| Sonido | En el PC (de serie) · En la tablet | Dónde suena el PC. En la tablet, los altavoces del PC se silencian mientras dura y vuelven al terminar |
| Táctil | Táctil (de serie) · Solo ver | Toques reales de Windows, o los toques no llegan al PC |

En **Configuración avanzada**: fluidez (fps), calidad de vídeo (Mbps), códec (todo «Automático» de serie),
«bajar la calidad si la batería de la tablet no aguanta» (de serie, no),
cable USB, abrir Extendesk en la tablet al enchufarla (de serie, sí), abrir con Windows, transmitir nada más abrir, el
puerto y el registro. El puntero del ratón siempre se dibuja en la imagen. El monitor que se duplica, con «Duplicar»,
se elige a la vista, en el sitio de «Resolución».

La **resolución de la pantalla extra** no tiene ajuste propio: «Resolución», a la vista bajo el modo, abre Configuración
de pantalla y el host sigue lo que se elija ahí. La recuerda para esa tablet (por su resolución nativa) y manda sobre
la de la tablet; con otra tablet se vuelve a usar su resolución nativa.

**Abrir al enchufar**: con la depuración USB aceptada, el host lanza la app en la tablet
(`adb shell am start -a app.extendesk.action.USB_ATTACHED`) y esta conecta sola por el cable. No usa el modo accesorio
USB, así que no deja nada registrado en la tablet ni muestra «ninguna aplicación funciona con este accesorio».

Con la transmisión en marcha la ventana enseña también dónde se quedan los fps: «Captura · Codificación · Tablet».

Casi todo se aplica en marcha, sin cortar la imagen. Solo el modo (se reinicia solo), el monitor y el
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
al abrir, escribir la IP del PC, estadísticas y el diagnóstico. La pantalla se mantiene siempre encendida mientras
hay sesión.

Durante la sesión, el **botón azul ☰** abre el menú rápido (se puede arrastrar si tapa algo, sin salirse de la pantalla;
recuerda dónde lo dejas): estadísticas y desconectar. El táctil (Táctil o Solo ver) se elige en el PC; si Windows no
acepta los toques, el host los convierte solo en ratón.

## Cómo viaja cada cosa

| Qué | Paquete | Cuándo |
| --- | --- | --- |
| Lo que puede la tablet (pantalla, Hz, dpi, HEVC) | HELLO (payload) | Cada HELLO |
| Lo que decide el PC (calidad, sonido, táctil, batería, fps y tasa de ahora, si va reducida) | SESSION_CONFIG (17) | Tras el STREAM_INFO, al cambiar y cada 2 s |
| Ajustes cambiados en la ventana del PC | línea `@ajustes …` por la entrada del host | Al cambiarlos |
| Batería de la tablet (si en el PC está activado) | QUALITY_REQUEST | Al cambiar y cada 5 s |

Ver [el protocolo](protocol-dlp.md) y [la fluidez automática](battery.md#fluidez-automática).

## Guía de inicio (dispositivo)

La primera vez que se abre la app de la tablet sale una guía corta: instalar Extendesk en el PC, conectar por Wi-Fi (sin
configurar nada) o por cable (botones a «Anclaje USB», sin modo desarrollador, y a la depuración USB, opcional para más
fluidez; si ya está activa lo dice), subir la «Escala» de Windows en vez de bajar la
resolución para ver todo más grande, y el botón ☰ durante la sesión. Quien ya había conectado con una versión
anterior no la ve de golpe; está siempre en Configuración avanzada > Ayuda > Guía de inicio. En el PC, el mismo
consejo de la escala va bajo «Resolución».

## «¿Necesitas más velocidad por USB?»

Si se pasa la guía deprisa, la pantalla de inicio de la app lo recuerda con una tarjeta: se muestra cuando la
depuración USB está apagada, no hay cable conectado y no se ha cerrado antes. «Ver cómo» enseña los pasos con botones
a «Anclaje USB» y a los ajustes de depuración USB; «No mostrar más» la oculta para siempre. Los textos hablan de
«dispositivo» porque la app sirve igual para un móvil que para una tablet.
