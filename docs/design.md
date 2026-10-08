# Diseño visual de DeskLink

Estilo oscuro «gamer»: fondo casi negro azulado, acentos neón cian y magenta, tarjetas con borde fino y halo en lo
que se puede tocar. La app de Android y la del PC usan los mismos colores para que se sientan una sola aplicación.

## Colores

| Token | Hex | Uso |
| --- | --- | --- |
| `background` | `#0A0E17` | Fondo de las pantallas |
| `surface` | `#121826` | Tarjetas |
| `surface-raised` | `#1A2233` | Opciones no seleccionadas, campos |
| `outline` | `#263049` | Bordes y separadores |
| `accent` | `#00E5FF` | Acción principal, selección, foco (cian neón) |
| `accent-alt` | `#FF2BD6` | Segundo color de los degradados y detalles (magenta) |
| `success` | `#3DFF9A` | Conectado, cable USB recomendado |
| `warning` | `#FFB547` | Sin respuesta, reconectando |
| `error` | `#FF4D6D` | Errores |
| `text` | `#E8EEFF` | Texto principal |
| `text-muted` | `#8A96B8` | Texto secundario |

Degradado de marca: `accent` → `accent-alt` (de izquierda a derecha o en diagonal). Se usa en el logotipo, el botón
principal y el borde de la tarjeta destacada.

## Formas y texto

- Tarjetas con esquinas de 20 dp/px, borde de 1 (1,5 si están destacadas, con el degradado y un halo cian).
- Botón principal: esquinas de 14, degradado de marca, texto oscuro `#05070C` en negrita.
- Títulos de sección en mayúsculas, 12 sp, espaciado 2, color `accent`.
- Tipografía del sistema; logotipo «DESKLINK» en negrita máxima con el degradado y espaciado 4.

## Dónde está

- Android: `android/app/src/main/kotlin/com/desklink/ui/Theme.kt` (`Palette`) y `res/values/colors.xml`.
- Windows: `windows/app/src/main.cpp` (`namespace color`).
