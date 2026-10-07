# Fase 3: monitor virtual

Objetivo: que Windows vea la tablet como una segunda pantalla (extender el escritorio), no como una copia del monitor principal.

## Qué hay

- `windows/driver/idd/`: driver IddCx que crea el monitor «DeskLink» cuando el host lo pide.
- `windows/driver/setup/`: instalador (`instalar-driver.bat` / `desinstalar-driver.bat`).
- `desklink-host --virtual`: enchufa el monitor, lo captura y asigna el táctil a su posición en el escritorio.

## Cómo probarlo

1. Si tenías SuperDisplay u otro programa de pantalla virtual, desinstálalo antes (Configuración, Aplicaciones), para que sus drivers no choquen.
2. En GitHub, pestaña Actions, descarga `desklink-driver-windows` (el driver) y `desklink-host-windows` (el host) de la última ejecución.
3. Descomprime el driver y haz doble clic en `instalar-driver.bat`. Windows pide permiso de administrador. Hay que hacerlo una vez (y otra vez cada vez que se actualice el driver).
4. Arranca `desklink-host.exe --virtual` y conecta la tablet como siempre.
5. En Configuración, Pantalla, aparece una pantalla nueva. Arrástrala para colocarla a un lado del monitor principal y ajusta la escala (por ejemplo 200 %) para que el texto no se vea diminuto. Windows lo recuerda para la próxima vez.

Al cerrar el host, la pantalla desaparece sola. Para quitarlo todo: `desinstalar-driver.bat`.

## Opciones

| Opción | Por defecto | Qué hace |
| --- | --- | --- |
| `--virtual` | apagado | Crea el monitor virtual y captura ese en vez de `--output` |
| `--size` | `2944x1840` | Resolución del monitor virtual (la de la Lenovo Idea Tab Pro) |
| `--fps` | 60 | También es la frecuencia del monitor virtual (hasta 240) |

## Limitaciones

- La resolución se elige al arrancar el host (`--size`). Que la tablet la mande sola al conectar queda para más adelante.
- Si cambias la resolución de la pantalla DeskLink en Windows, el host se detiene y hay que volver a arrancarlo.
- El driver está firmado con un certificado propio, válido para tu PC (ver `windows/driver/README.md`).
