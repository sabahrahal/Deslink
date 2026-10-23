# Protocolo DLP v1

Protocolo propio sobre UDP, el mismo para WiFi y USB. Por cable USB (`adb reverse`, ver `docs/usb.md`) los mismos paquetes viajan por TCP, cada uno precedido de su tamaño (u16 little-endian). Implementado en `protocol/` (C++, usado por el host de Windows) y en `android/app/.../Dlp.kt` y `FrameReassembler.kt` (Kotlin, fase 1). Las pruebas de ambos cubren los mismos casos.

## Cabecera (24 bytes, little-endian)

| Bytes | Campo | Significado |
| --- | --- | --- |
| 0–1 | magic | `0x4C44` ("DL") |
| 2 | version | 1 |
| 3 | type | tipo de paquete (tabla siguiente) |
| 4 | flags | bit 0: frame clave; bit 1: último paquete del frame |
| 5 | fec_group | en VIDEO y VIDEO_FEC: paquetes que cubre cada paridad del frame (0 = sin FEC); en los demás, 0 |
| 6–9 | frame_id | número de frame; crece de uno en uno |
| 10–11 | packet_index | posición del paquete dentro del frame |
| 12–13 | packet_count | paquetes del frame |
| 14–21 | timestamp_us | reloj del emisor, en microsegundos |
| 22–23 | payload_size | bytes de payload que siguen a la cabecera |

Tamaño máximo de paquete: 1200 bytes (1176 de payload), para no fragmentar en UDP. Todos los paquetes de un frame salvo el último van llenos.

## Tipos

| Valor | Nombre | Sentido | Payload |
| --- | --- | --- | --- |
| 1 | VIDEO | host a cliente | trozo del frame codificado (H.264 o HEVC, formato Annex B) |
| 2 | HELLO | cliente a host | Registra al cliente como destino y pide frame clave. Payload opcional: configuración de la tablet (ver abajo); vacío = no cambia nada |
| 3 | KEYFRAME_REQUEST | cliente a host | vacío |
| 4 | PING | los dos sentidos | vacío; el timestamp es el de quien lo envía |
| 5 | PONG | los dos sentidos | devuelve el timestamp del PING. El host añade su hora al contestar (u64, µs de su reloj) para que la tablet calcule la diferencia de relojes; un host antiguo lo manda vacío y una tablet antigua no lee el payload. La tablet contesta los PING del host vacío |
| 6 | STREAM_INFO | host a cliente | ancho u16, alto u16, fps u16, códec u8 (1 H.264, 2 HEVC). Hosts nuevos añaden: banderas u8 (bit 0: aplica la configuración de HELLO), longitud u8 y nombre del PC en UTF-8 (máx. 64). Los clientes antiguos solo leen los 7 primeros bytes |
| 7 | BURST_REQUEST | cliente a host | count u16, size u16, pace_mbps u16 (0 = de golpe). El timestamp sirve de nonce |
| 9 | RATE_REPORT | cliente a host | frames recibidos u16, frames perdidos u16 frames mostrados u16 (los que sacó el decodificador) en el último segundo y lo que tardó de media cada frame en decodificarse, en décimas de ms u16 (0 si no mostró ninguno); `payload_size` 4 en clientes antiguos, 6 desde que se añadió el tercer campo y 8 desde el cuarto, y el host lee cada campo solo si `payload_size` lo cubre. Las tablets nuevas mandan 18 bytes: añaden el retardo por etapas en décimas de ms (0 = no se sabe): total u16 (de entrar al codificador a verse en pantalla), llegar u16 (hasta tener el frame entero), esperar reenvíos u16, pantalla u16 (de decodificado a encendido) y el peor total del segundo u16. Ver «Retardo de punta a punta». El host baja la tasa un 30 % si hay pérdidas (o si la tablet pide un frame clave), recuerda un techo por debajo de la tasa que falló y la sube un 5 % tras 8 s limpios (máximo `--bitrate`) |
| 10 | NACK | cliente a host | `frame_id` en la cabecera; payload: u16 por cada `packet_index` que falta (máx. 128). El host guarda los últimos 4096 paquetes y los reenvía al ritmo de `--pace`. La tablet reclama a ~2 RTT (mín. 4 ms), hasta 4 veces por paquete, y retiene un frame completo (15–60 ms según el RTT) mientras recupera uno anterior; si no llega, se descarta y se pide frame clave |
| 11 | TOUCH | cliente a host | estado táctil completo: u8 n, y por dedo u8 id, u16 x, u16 y (0–65535 sobre la pantalla capturada), u16 presión (0–1024). La tablet lo reenvía cada 100 ms mientras haya dedos y repite el «sin dedos» final; el host compara con el estado anterior para decidir qué dedos bajan, se mueven o suben, y los suelta tras 700 ms de silencio. Por defecto se inyectan toques reales de Windows (punteros sintéticos, o `InjectTouchInput` en Windows anteriores a 1809) sobre el monitor capturado, sin mover el cursor; el host repite los dedos quietos cada 33 ms para que Windows no los cancele. En modo ratón los gestos se traducen a clics y rueda. `--no-touch` lo desactiva |
| 12 | DISCOVER | cliente a host | vacío. El host responde STREAM_INFO al remitente sin tomarlo como destino del vídeo (búsqueda de PCs). Un host antiguo lo ignora: la tablet manda HELLO si nadie responde en 1 s |
| 13 | INPUT_MODE | ambos | cliente a host: u8 modo pedido (0 táctil directo, 1 ratón); el host lo aplica en marcha y lo guarda en `%APPDATA%\DeskLink\input-mode.txt`. Host a cliente: u8 modo pedido, u8 modo en uso (distintos si Windows no aceptó los toques y se pasó a ratón); se envía tras cada HELLO y cada vez que cambia |
| 14 | AUDIO | host a cliente | sonido del PC: Opus 48 kHz estéreo, 10 ms por trozo (RESTRICTED_LOWDELAY, 128 kbps por defecto). `frame_id` = número de trozo. Payload: u8 n (1 o 2) y n x {u16 tamaño, bytes}; la entrada i es el trozo `frame_id - i` (el anterior va repetido para cubrir pérdidas por WiFi). Si en el PC no suena nada, no se envía nada |
| 15 | AUDIO_CONTROL | cliente a host | obsoleto: el host lo ignora. Dónde suena el sonido lo elige el PC (`--audio-tablet`); el host captura (WASAPI loopback de la salida predeterminada) y lo envía solo si está elegido y no se arrancó con `--no-audio` |
| 8 | BURST_DATA | host a cliente | relleno; `packet_index`/`packet_count` numeran la ráfaga y el timestamp devuelve el nonce |
| 16 | QUALITY_REQUEST | cliente a host | petición de la batería: fps máx. u16, Mbps máx. u16 en unidades de 100 kbps, escala u8, perfil u8 (0 alta, 1 equilibrado, 2 ahorro, 3 mínimo). La tablet solo lo manda si SESSION_CONFIG dice que el PC lo acepta (bit 1), al cambiar y cada 5 s. El host solo baja fps y, como mucho, la tasa al 60 % de la elegida; nunca cambia la escala por esto. Ver [batería](battery.md) |
| 17 | SESSION_CONFIG | host a cliente | lo que decidió el PC, 10 bytes: calidad u8 (0 alta, 1 equilibrada, 2 ahorro), banderas u8 (bit 0 sonido en la tablet, bit 1 acepta QUALITY_REQUEST de la batería), táctil u8 (0 táctil, 1 ratón, 2 solo ver), reducido u8 (0 no, 1 Wi-Fi, 2 batería, 3 carga de la gráfica), fps u16 de ahora, tasa u16 de ahora y tasa elegida u16 (en 100 kbps). Tras cada STREAM_INFO, al cambiar y cada 2 s. Ver `protocol/include/dlp/session.h` |
| 18 | CLIENT_INFO | cliente a host | texto UTF-8 (hasta 1176 bytes) que el host copia en su log: modelo de la tablet, versión de Android y qué decodificador abrió, con o sin modo de baja latencia. La tablet lo repite cada 5 s mientras hay vídeo; el host solo lo escribe cuando cambia. Con el sonido en la tablet manda además otro que empieza por «Sonido en la tablet:» (trozos que llegan y que suenan, volumen multimedia y el último error), que el host escribe siempre |
| 19 | VIDEO_FEC | host a cliente | por WiFi o anclaje (no por el cable de adb), tras cada grupo de `fec_group` paquetes VIDEO de un frame (10 de serie, `--fec`): `packet_index` = número de grupo, `packet_count` = paquetes del frame, `flags` = el de frame clave. Payload: XOR de las longitudes de los payloads del grupo u16 + XOR de los payloads rellenos con ceros (hasta 1176 bytes; es el único paquete que puede pasar 2 bytes de `kMaxPayload`). Si de un grupo falta un solo paquete, la tablet lo rehace sin reclamarlo; los huecos de un grupo no se reclaman hasta que llega su paridad o paquetes del grupo siguiente. Un cliente antiguo lo ignora |

## Configuración de la tablet (payload de HELLO)

14 bytes, little-endian. La tablet dice lo que **puede** hacer; lo que se usa lo decide el PC (ver [ajustes](ajustes.md)).

| Bytes | Campo | Significado |
| --- | --- | --- |
| 0–1 | width | ancho de la pantalla de la tablet en horizontal (el monitor virtual se crea así salvo que el PC fije otra resolución) |
| 2–3 | height | alto |
| 4–5 | fps | sin uso (0) |
| 6–7 | bitrate_mbps | sin uso (0) |
| 8 | codec | qué decodifica: 0 no lo dice, 1 solo H.264, 2 también HEVC, 3 también HEVC pero su H.264 es más rápido (en automático el PC elige H.264) |
| 9 | reservado | 0 |
| 10–11 | density_dpi | informativo: densidad de la pantalla |
| 12–13 | refresh_hz | frecuencia máxima de la pantalla: con fluidez automática el PC usa 120 fps si llega a 119 Hz, 90 si llega a 89, si no 60 |

Campos nuevos se añaden al final: el host ignora los bytes que no conoce y toma como 0 los que falten. La tablet manda la misma configuración en cada HELLO; el host solo actúa cuando cambia. Entonces deja de responder a los HELLO, rehace el pipeline (y, con `--virtual`, vuelve a crear el monitor con el tamaño pedido salvo que se arrancara con `--size`) y manda el STREAM_INFO nuevo.

## Flujo

1. El cliente envía HELLO (con su configuración) cada segundo hasta recibir STREAM_INFO.
2. El host responde STREAM_INFO, fuerza un frame clave y empieza a enviar VIDEO a la dirección de origen del HELLO.
3. El cliente reensambla. Sin colas: al completarse un frame se descartan los anteriores incompletos.
4. Si se pierde un frame (o hay un hueco en `frame_id`), el cliente pide KEYFRAME_REQUEST y no decodifica frames hasta recibir uno clave.
5. El cliente envía PING cada segundo; el RTT es la diferencia entre el reloj local y el timestamp devuelto.
   Con `--status` (la app de bandeja) el host también envía un PING por segundo al cliente para mostrar el RTT en el PC;
   el cliente que lo conteste con PONG (mismo timestamp) lo hace visible. Un cliente que no lo conteste sigue funcionando.

## Retardo de punta a punta

La tablet mide cuánto tarda cada frame desde que el PC lo manda al codificador (su `timestamp_us`) hasta que se enciende en su pantalla, y lo reparte por etapas. Para comparar la hora del PC con la suya usa los PONG: con cada uno calcula `hora del PC − (envío + RTT/2)` y se queda con la muestra de menor RTT de las últimas 10, así que el error es como mucho medio RTT de esa muestra (menos de 1 ms por cable y normalmente 1–3 ms por WiFi). El momento en pantalla lo da Android (`MediaCodec.OnFrameRenderedListener`); en los dispositivos que no lo avisan, el total acaba al salir del decodificador y falta la etapa «pantalla».

- En la tablet: las estadísticas (botón del menú) muestran «retardo X ms (peor Y): PC y red + esperar reenvíos + decodificar + pantalla».
- En el log del PC, cada 5 s: la línea `Latencia:` (espera de captura, en cola, codificador, envío) y la línea `Retardo de punta a punta:`, que suma la espera de captura del PC a lo que mide la tablet y estima la red como «llegar» menos lo que el PC tarda en cola, codificar y enviar.

## Pendiente (no está en la fase 1)

- FEC para pérdidas sueltas sin esperar a un frame clave.
- Cifrado AES-GCM y emparejamiento por QR. **Ahora el tráfico va en claro y el host acepta a cualquier cliente de la red**: usar sólo en redes de confianza.
- Canal de entrada (toque y lápiz) y control de tasa adaptativo.
