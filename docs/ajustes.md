# Ajustes: dónde se elige cada cosa

**El PC manda.** La ventana de Extendesk en el PC es el único sitio donde se eligen la imagen, el sonido y el táctil.
La tablet dice al conectar lo que puede hacer (su pantalla, su frecuencia y si decodifica HEVC) y enseña lo que el
PC decide. Así no hay dos sitios con la misma opción ni ajustes que se pisan.

**Pocas opciones y todo automático.** Lo que el usuario no sabría elegir no se pregunta: fps, Mbps y códec los
decide «Calidad» para cada dispositivo (H.264 en los MediaTek, que lo decodifican antes; 90 fps estables en una Idea
Tab con Alta), el cable USB se usa solo si está, y la fluidez baja sola si la batería del dispositivo no aguanta. Cada
opción que queda dice en una línea qué hace, y la ayuda de la web (`https://extendesk.app/ayuda`, un «?» junto a la
opción) lo cuenta entero.

## En el PC (extendesk.exe)

A la vista, lo que casi todo el mundo toca:

| Ajuste | Opciones | Qué hace |
| --- | --- | --- |
| Modo | Pantalla extra (de serie) · Duplicar | Monitor virtual a la medida de la tablet, o copia de un monitor |
| Resolución | (enlace) · con Duplicar, el monitor | Abre Configuración de pantalla de Windows (ver abajo) |
| Calidad | Alta (de serie) · Equilibrada · Ahorro | Alta: los fps y la tasa más altos que aguanten tablet y PC. Equilibrada: hasta 60 fps y 30 Mbps. Ahorro: 30 fps, gasta menos en la tablet |
| Sonido | En el PC (de serie) · En la tablet | Dónde suena el PC. En la tablet, los altavoces del PC se silencian mientras dura y vuelven al terminar |

En **Configuración avanzada**: táctil (Táctil de serie, o Solo ver: los toques no llegan al PC), la red Wi-Fi propia
del PC, abrir con Windows (de serie, sí), el idioma (Automático, el de Windows, de serie; o English o Español),
«Ayuda» (la página de la web, en el idioma de la ventana) y el registro. El puntero del ratón siempre se
dibuja en la imagen.

**Ya no se eligen** (desde que se simplificó la ventana):

| Antes | Ahora |
| --- | --- |
| Fluidez (fps), calidad de vídeo (Mbps), códec | Los decide «Calidad»; quien quiera fijarlos, en el `.ini` (`fps`, `bitrate`, `codec`) |
| Bajar la calidad si la batería no aguanta | Siempre activo (ver [batería](battery.md)): sin cargador baja un escalón por debajo del 20 %, y con el cable si la batería baja igualmente. La ventana dice «Fluidez reducida: la batería del dispositivo no aguanta» |
| Conexión por cable USB, abrir la app de la tablet al enchufarla | Siempre activos. `extra=--no-usb` en el `.ini` para quitar el túnel |
| Transmitir nada más abrir | Siempre escucha: la pantalla extra solo aparece cuando un dispositivo se conecta ([desconexión](architecture.md#desconexión)) |
| Puerto | Solo en el `.ini` (`port`) |

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

La pantalla de inicio enseña una tarjeta por PC, con un solo botón Conectar y nada que elegir: si llega por cable USB
usa el cable, si no el Wi-Fi. Para el usuario solo hay un «cable USB»: por dentro va por el anclaje de red si está
activo (menos picos que el túnel de adb: el peor frame ~55 ms frente a 75-95 ms en una Lenovo con MediaTek) y si no por
la depuración USB. Al conectarse sola, si el último PC llega también por cable, usa el cable. En **Configuración
avanzada** solo está lo que es de la tablet: conectar solo al abrir, escribir la IP del PC y Ayuda (la página de la web,
el diagnóstico y la guía de inicio). La pantalla se mantiene siempre encendida mientras hay sesión.

Durante la sesión, el **botón azul ☰** abre el menú rápido (se puede arrastrar si tapa algo, sin salirse de la pantalla;
recuerda dónde lo dejas): estadísticas (se recuerdan para la próxima vez) y desconectar. El táctil (Táctil o Solo ver)
se elige en el PC; si Windows no acepta los toques, el host los convierte solo en ratón. Al desconectar, bloquear la
tablet o detener el PC, los dos lados se enteran: ver [desconexión](architecture.md#desconexión).

## Cómo viaja cada cosa

| Qué | Paquete | Cuándo |
| --- | --- | --- |
| Lo que puede la tablet (pantalla, Hz, dpi, HEVC) | HELLO (payload) | Cada HELLO |
| Lo que decide el PC (calidad, sonido, táctil, batería, fps y tasa de ahora, si va reducida) | SESSION_CONFIG (17) | Tras el STREAM_INFO, al cambiar y cada 2 s |
| Ajustes cambiados en la ventana del PC | línea `@ajustes …` por la entrada del host | Al cambiarlos |
| Batería de la tablet | QUALITY_REQUEST | Al cambiar y cada 5 s |
| Desconectar, bloquear la tablet o detener el PC | BYE (20) | Al irse, 3 veces |

Ver [el protocolo](protocol-dlp.md) y [la fluidez automática](battery.md#fluidez-automática).

## Guía de inicio (dispositivo)

La primera vez que se abre la app de la tablet sale una guía corta: instalar Extendesk en el PC, conectar por Wi-Fi (sin
configurar nada) o por cable (botones a «Anclaje USB», sin modo desarrollador, y a la depuración USB, opcional para que
conecte solo al enchufar; si ya está activa lo dice), subir la «Escala» de Windows en vez de bajar la
resolución para ver todo más grande, y el botón ☰ durante la sesión. Quien ya había conectado con una versión
anterior no la ve de golpe; está siempre en Configuración avanzada > Ayuda > Guía de inicio. En el PC, el mismo
consejo de la escala va bajo «Resolución».

## Cable USB: un solo paso

Si la tablet está enchufada y el PC aún no llega por el cable, la pantalla de inicio enseña «Activa la conexión por
cable» con un botón a «Anclaje USB» (sin modo desarrollador). La depuración USB queda debajo, plegada, como consejo
opcional: con ella el cable conecta solo al enchufar, sin repetir el paso, y el PC abre la app al ver la tablet. La
tarjeta desaparece en cuanto el PC llega por el cable. Los textos hablan de «dispositivo» porque la app sirve igual para
un móvil que para una tablet.
