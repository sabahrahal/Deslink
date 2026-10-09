# Fluidez automática, calidad y batería

## Fluidez automática

El PC decide y ajusta la imagen solo, sin que haya que tocar nada (lógica en `protocol/src/session.cpp`, probada en
`protocol/tests/test_session.cpp`):

- **Al conectar**: fps según la pantalla de la tablet (120, 90 o 60), tasa según la resolución y los fps, HEVC si la
  tablet puede (ver [ajustes](ajustes.md#qué-es-automático)).
- **Wi-Fi justo** (`RateController`): solo baja la tasa un 15 % si en un segundo se pierde más del 2 % de los frames,
  nunca por debajo de la mitad de la elegida, y vuelve a subir un 10 % por segundo tras 3 s sin pérdidas. Por cable
  USB no baja nunca. Antes se bajaba un 30 % por cada pérdida hasta el 10 % y tardaba mucho en volver: esa era la
  caída de calidad repentina.
- **Gráfica del PC cargada** (`FpsGovernor`): si codificar un frame tarda más del 85 % del tiempo de un frame durante
  3 s, baja un escalón de fps (120, 90, 60, 45, 30); si sobra (menos del 50 % durante 10 s), vuelve a subir.
- **En la tablet**: cada frame se pinta en el siguiente refresco de pantalla y, si llega otro antes, se descarta el
  viejo (`VideoDecoder.kt`): no se acumula retraso con el Wi-Fi a ráfagas.

Cuando la imagen va por debajo de lo elegido, la ventana del PC y la tablet dicen por qué («el Wi-Fi va justo», «la
gráfica va cargada», «la batería no aguanta»).

## Perfiles

La calidad se elige en el PC (Alta, Equilibrada, Ahorro) y viaja a la tablet en `SESSION_CONFIG`; la tablet aplica
su parte (brillo, Wi-Fi, frecuencia de pantalla). Con **«bajar la calidad si la batería de la tablet no aguanta»**
activado en la Configuración avanzada del PC (de serie, no), la tablet puede pedir un perfil menor con
`QUALITY_REQUEST` (ver [protocolo](protocol-dlp.md)); el PC lo atiende bajando antes los fps, nunca la resolución ni
la tasa por debajo del 60 % de la elegida. Sin esa opción, la batería de la tablet no cambia la imagen.

| Perfil | fps | Mbps | Resolución codificada | Brillo | WiFi | Pantalla |
| --- | --- | --- | --- | --- | --- | --- |
| Alta (Máxima calidad) | automáticos | automáticos | 100 % | sin tocar | baja latencia | frecuencia libre, rendimiento sostenido |
| **Equilibrado (recomendado)** | 60 | 30 | 100 % | sin tocar | baja latencia | modo de 60 Hz |
| Ahorro | 30 | 12 | 75 % (2208x1380 en la Idea Tab Pro) | tope 50 % | alto rendimiento (deja dormir la radio) | modo de 60 Hz |
| Mínimo (solo automático) | 30 | 8 | 60 % | tope 35 % | alto rendimiento | modo de 60 Hz |

- **De serie: Alta, sin bajar por batería.** Es lo que se pidió: la imagen más fluida. Equilibrada o la opción de
  batería son para quien prefiera que la tablet aguante más.
- El tope de brillo solo actúa con el brillo en manual y más alto que el tope; en automático no se toca (al sol
  quedaría ilegible).
- «Modo de 60 Hz»: se pide a Android el modo de pantalla de la misma resolución con la menor frecuencia que muestra
  los fps del stream sin saltos (30 fps → 60 Hz; en una pantalla de 144 Hz esto evita refrescar a 144 Hz). Máxima
  calidad solo da la pista de fps (`Surface.setFrameRate`) y deja decidir al sistema.
- Cambiar fps o Mbps no corta la imagen. Cambiar la escala rehace la captura en el PC (un parpadeo de una fracción de
  segundo) porque el codificador cambia de tamaño.
- El códec sigue siendo HEVC en todos: a igual calidad necesita menos Mbps que H.264 y la tablet lo decodifica por
  hardware.

En el host: `--profile alta|equilibrada|ahorro` y `--battery-adapt` (los pone la ventana del PC).

## Bajar si la batería no aguanta (con cable USB)

Solo con la opción activada en el PC.

Cada segundo la app mide la potencia neta de la batería (lo que entra por el cable menos lo que gasta la tablet) y
`ChargeGovernor.kt` decide:

1. **Con cable y la batería baja** (neto por debajo de −0,15 W durante 10 s, y al menos 40 s desde el último cambio
   para que la medida se asiente): baja un escalón (Máxima → Equilibrado → Ahorro → Mínimo).
2. **Con cable y sobra margen** (más de +1,5 W durante 2 min): sube un escalón, nunca por encima del perfil elegido y
   sin volver durante 15 min a uno que ya descargó la batería.
3. **Ya en Mínimo (o con «adaptar» apagado) y sigue bajando** 30 s: aviso en rojo «Se descarga pese al cable» y un
   mensaje con qué hacer (puerto USB-C o USB 3, cargador de pared con WiFi, bajar el brillo).
4. **Sin cable**: el perfil elegido; con un 20 % o menos, un escalón menos (vuelve al subir del 25 %).
5. Con la batería al 98 % o más se toleran pequeñas bajadas (hasta −1 W): algunos aparatos dejan bajar la batería
   llena antes de rellenarla y no es una descarga real.

Si todavía no hay cifra de potencia, que el porcentaje baje con el cable puesto también cuenta como descarga.

### Indicador

Arriba a la derecha, unos segundos al empezar y tras cada cambio, y fijo mientras haya un problema:

- verde «Cargando · neto +2,1 W»
- naranja «Se descarga con el cable −0,6 W · bajando calidad»
- rojo «Se descarga pese al cable −0,4 W · bajado a Mínimo»
- «Batería · consumo neto −4,2 W» sin cable

Las estadísticas (menú de cuatro dedos) muestran además el %, por dónde entra la carga (USB, cargador), de dónde sale
la cifra (medida o estimada, ver abajo) y el perfil activo con sus fps, Mbps y escala.

## Qué se mide y qué se estima

**Medido en la tablet, en vivo** (`DeviceProbe.kt`):

| Cifra | Fuente | Fiabilidad |
| --- | --- | --- |
| Potencia neta, «medido: corriente de la batería» | `CURRENT_NOW` × voltaje, con el signo aprendido del propio aparato | Buena y rápida (suavizada ~5 s). El signo se aprende cuando no hay cable (la batería solo puede bajar) o cuando el contador de carga se mueve claramente, y se guarda |
| Potencia neta, «medido: contador de carga» | Variación de `CHARGE_COUNTER` en 20–60 s | Buena pero lenta; algunas tablets solo lo mueven a saltos de 1 % |
| Potencia neta, «estimado: signo según el estado de carga» | `CURRENT_NOW` con el signo de `EXTRA_STATUS` | **Estimación**: con cable Android suele decir «cargando» aunque la batería baje. Solo se usa hasta aprender el signo |
| %, cable, temperatura | `ACTION_BATTERY_CHANGED` | Exacto |

**Medido por Sabah con la app** (Lenovo Idea Tab Pro, antes de los perfiles, 60 fps / 50 Mbps): por el cable USB
del PC unos **+4 W** netos (carga), y por WiFi sin cable unos **−4 W** (consumo). Son lecturas de las estadísticas,
no de un medidor externo.

**Estimado (sin medir aún en la tablet)** — el reparto del consumo y lo que ahorra cada palanca:

| Palanca | Ahorro estimado | Por qué |
| --- | --- | --- |
| Brillo (tope 50 %) | 0,5–1,5 W | La pantalla de 12,7" es lo que más gasta |
| 144 Hz → 60 Hz | 0,3–0,8 W | Menos refrescos del panel y de la composición |
| 60 → 30 fps y 75 % de resolución | 0,2–0,6 W | El decodificador trabaja con ~4 veces menos píxeles por segundo |
| 30 → 12 Mbps | 0,1–0,4 W por WiFi; casi nada por cable | La radio pasa más tiempo dormida |
| WiFi de alto rendimiento en vez de baja latencia | 0,1–0,5 W | Sin modo de baja latencia la radio puede dormir entre ráfagas |

En el PC el perfil también reduce trabajo: menos frames convertidos y codificados y, con escala, menos píxeles.

### Cómo medirlo de verdad (para afinar la tabla)

1. Sin cable y con el brillo fijo en manual, abre el stream y deja las estadísticas a la vista (menú de 4 dedos).
2. Espera a que la cifra diga «medido: corriente de la batería» (aprende el signo en unos segundos sin cable).
3. Con el escritorio quieto y luego moviendo una ventana, anota los W de cada perfil (cambia la calidad en la ventana del PC). Unos 2 min por perfil.
4. Repite con el cable en el puerto que uses para ver cuánto da (neto con cable − neto sin cable ≈ lo que entra).

Los umbrales están en `ChargeGovernor.kt` y los perfiles en `PowerProfile.kt` y `protocol/src/packet.cpp`
(`ProfileDefaults`); se ajustarán con estas mediciones.
