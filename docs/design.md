# Diseño visual de DeskLink

Estilo limpio, parecido al de macOS: fondo gris claro (u oscuro, si el sistema está en modo oscuro), tarjetas
blancas con un borde fino, un solo color de acento azul y nada de brillos, halos ni degradados. La app de Android y
la del PC usan los mismos colores y tamaños de texto para que se sientan una sola aplicación.

## Una sola fuente

Los colores, el texto y las formas están en [`design/tokens.json`](../design/tokens.json). De ahí salen, con
`python3 tools/design/gen_tokens.py`:

- Android: `android/app/src/main/kotlin/com/desklink/ui/Tokens.kt` (`LightColors`, `DarkColors`, `DlType`, `DlShape`).
- Windows: `windows/app/src/tokens.h` (`tokens::kLight`, `tokens::kDark`…).

`gen_tokens.py --check` (en la CI, flujo `design`) falla si esos archivos no están al día o si algún par de colores
de `contrast_pairs` no llega al contraste AA (4,5:1). Para cambiar un color: se edita el JSON y se regenera.

## Colores

| Token | Claro | Oscuro | Uso |
| --- | --- | --- | --- |
| `background` | `#F5F5F7` | `#1C1C1E` | Fondo de pantalla |
| `surface` | `#FFFFFF` | `#2C2C2E` | Tarjetas |
| `surface_secondary` | `#ECECF0` | `#3A3A3C` | Campos, controles segmentados, botones secundarios |
| `separator` | `#D2D2D7` | `#48484A` | Bordes y líneas |
| `text` | `#1D1D1F` | `#F5F5F7` | Texto y títulos |
| `text_secondary` | `#5E5E63` | `#AEAEB2` | Explicaciones |
| `accent` | `#0066CC` | `#409CFF` | Botón principal, selección, enlaces |
| `on_accent` | `#FFFFFF` | `#000000` | Texto sobre el acento |
| `success` | `#1E7D34` | `#30D158` | Conectado |
| `warning` | `#A84B00` | `#FF9F0A` | Avisos (calidad reducida, falta algo) |
| `error` | `#D70015` | `#FF6961` | Errores |

## Texto y formas

| Estilo | Tamaño | Peso | Uso |
| --- | --- | --- | --- |
| Large title | 28 | 600 | Nombre de la pantalla («DeskLink», «Configuración avanzada») |
| Title | 20 | 600 | Estado («Conectado por cable USB») |
| Headline | 17 | 600 | Nombre de un PC |
| Section | 13 | 600 | Encabezado de sección, en gris |
| Body | 15 | 400 | Texto normal |
| Note | 13 | 400 | Explicaciones bajo una opción |

Tarjetas con esquinas de 12, controles (botones, segmentados) de 8. Tipografía del sistema (Segoe UI en Windows).

## Reglas

- Cada pantalla tiene pocas cosas a la vista; lo demás va en **Configuración avanzada**, plegado o en otra pantalla.
- Opciones excluyentes (modo, calidad, sonido, táctil, cable USB o Wi-Fi) con un control segmentado.
- Un solo botón principal por pantalla, con el acento. El resto, secundarios en gris.
- Todo texto lleva color del tema: en Android, `DeskLinkTheme` fija `LocalContentColor` para que un `Text` sin color
  no salga negro sobre fondo oscuro.

## Pruebas

- Android (Robolectric, `./gradlew :app:testDebugUnitTest`): `ui/HomeScreenTest.kt` (una tarjeta por PC con cable
  USB y Wi-Fi, ayuda plegada, enlace a Configuración avanzada) y `ui/ThemeTest.kt` (color del texto y contraste).
- Windows (cualquier sistema, `ctest`): `windows/app/tests/test_view_model.cpp` comprueba qué enseña la ventana en
  cada estado.
