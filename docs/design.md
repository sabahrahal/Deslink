# Diseño visual de DeskLink

Estilo limpio, parecido al de macOS: fondo gris claro (u oscuro, si el sistema está en modo oscuro), tarjetas
blancas con un borde fino, un solo color de acento (el morado de la marca) y nada de brillos, halos ni degradados. La app de Android y
la del PC usan los mismos colores y tamaños de texto para que se sientan una sola aplicación.

## Marca

El logo está en [`design/logo/`](../design/logo/): el icono (dos eslabones), el nombre «Desklink» y los dos juntos.
El nombre va en **Montserrat Black** ([`design/fonts/`](../design/fonts/), licencia OFL, recortada a los caracteres
latinos).

| Color | Valor | Dónde |
| --- | --- | --- |
| Morado primario | `#3F0D91` | Nombre del logo y acento en modo claro (botón principal, selección, enlaces) |
| Morado oscuro | `#4D13C7` | Eslabón de atrás del icono |
| Violeta | `#7033FD` | Eslabón de delante del icono (con sombra `#3F11A5` donde pasa por debajo) |
| Lavanda | `#A68CFF` | Acento en modo oscuro: los morados no se leen sobre fondo oscuro |

El icono se dibuja una sola vez, en [`tools/design/gen_logo.py`](../tools/design/gen_logo.py) (calca
`design/logo/desklink-icon.png`), y de ahí salen con `python3 tools/design/gen_logo.py`:

- `design/logo/desklink-icon.svg`.
- Android: `res/drawable/ic_launcher_foreground.xml` (icono adaptativo sobre blanco) y `res/drawable/ic_logo.xml`
  (cabecera, `BrandMark` en `ui/Components.kt`).
- Windows: `windows/app/src/logo.h` (la ventana y la bandeja lo pintan con GDI+) y, con `--ico` (necesita
  `pip install resvg-py`), `windows/app/res/desklink.ico`.

## Una sola fuente

Los colores, el texto y las formas están en [`design/tokens.json`](../design/tokens.json). De ahí salen, con
`python3 tools/design/gen_tokens.py`:

- Android: `android/app/src/main/kotlin/com/desklink/ui/Tokens.kt` (`LightColors`, `DarkColors`, `DlType`, `DlShape`).
- Windows: `windows/app/src/tokens.h` (`tokens::kLight`, `tokens::kDark`…).

`gen_tokens.py --check` (en la CI, flujo `design`, junto con `gen_logo.py --check`) falla si esos archivos no están al día o si algún par de colores
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
| `accent` | `#3F0D91` | `#A68CFF` | Botón principal, selección, enlaces, nombre del logo |
| `on_accent` | `#FFFFFF` | `#000000` | Texto sobre el acento |
| `success` | `#1E7D34` | `#30D158` | Conectado |
| `warning` | `#A84B00` | `#FF9F0A` | Avisos (calidad reducida, falta algo) |
| `error` | `#D70015` | `#FF6961` | Errores |

## Texto y formas

| Estilo | Tamaño | Peso | Fuente | Uso |
| --- | --- | --- | --- | --- |
| Large title | 28 | 900 | Montserrat | Nombre de la pantalla («Desklink», «Configuración avanzada») |
| Title | 20 | 700 | Montserrat | Estado («Conectado por cable USB») |
| Headline | 17 | 600 | Sistema | Nombre de un PC |
| Section | 13 | 700 | Montserrat | Encabezado de sección, en gris |
| Body | 15 | 400 | Sistema | Texto normal |
| Note | 13 | 400 | Sistema | Explicaciones bajo una opción |

Tarjetas con esquinas de 12, controles (botones, segmentados) de 8. Títulos en Montserrat (la del logo); el texto
corrido, en la del sistema (Roboto en Android, Segoe UI en Windows), que se lee mejor en tamaños pequeños. En Windows,
las Montserrat van dentro de `desklink.exe` (`res/desklink.rc`); si no cargan, se usa Segoe UI.

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
