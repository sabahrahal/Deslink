# Fase 3: monitor virtual

Objetivo: que Windows vea la tablet como una segunda pantalla (extender el escritorio), no como una copia del monitor principal.

## Qué hay

- `windows/driver/idd/`: driver IddCx que crea el monitor «DeskLink» cuando el host lo pide.
- `windows/driver/setup/`: instalador (`instalar-driver.bat` / `desinstalar-driver.bat`).
- `desklink-host --virtual`: enchufa el monitor, lo captura y asigna el táctil a su posición en el escritorio.

## Cómo probarlo

1. Si tenías SuperDisplay u otro programa de pantalla virtual, desinstálalo antes (Configuración, Aplicaciones), para que sus drivers no choquen.
2. Instala `DeskLink-Setup.exe` (ver [Instalar y probar](instalar-y-probar.md)): trae el host y el driver. Windows pide permiso de administrador.
3. (Sin el instalador: descarga `desklink-driver-windows` y `desklink-host-windows` de Actions y ejecuta `instalar-driver.bat`.)
4. Abre «DeskLink» en el menú Inicio (es `desklink-host.exe --virtual`) y conecta la tablet como siempre.
5. En Configuración, Pantalla, aparece una pantalla nueva. Arrástrala para colocarla a un lado del monitor principal y ajusta la escala (por ejemplo 200 %) para que el texto no se vea diminuto. Windows lo recuerda para la próxima vez.

Al cerrar el host, la pantalla desaparece sola. Para quitarlo todo: desinstala DeskLink desde Configuración, Aplicaciones (o `desinstalar-driver.bat` si usaste el paquete suelto).

## Opciones

| Opción | Por defecto | Qué hace |
| --- | --- | --- |
| `--virtual` | apagado | Crea el monitor virtual y captura ese en vez de `--output` |
| `--size` | `2944x1840` | Resolución del monitor virtual (la de la Lenovo Idea Tab Pro) |
| `--fps` | 60 | También es la frecuencia del monitor virtual (hasta 240) |

## Limitaciones

- La resolución inicial se elige al arrancar el host (`--size`). Que la tablet la mande sola al conectar queda para más adelante.
- Si cambias la resolución de la pantalla DeskLink en Windows, el host reinicia la captura solo y avisa a la tablet (hay un parpadeo de un segundo). La última resolución se guarda en `%APPDATA%\DeskLink\virtual-size.txt` y se usa la próxima vez que arranques sin `--size`.
- El driver está firmado con un certificado propio, válido para tu PC (ver `windows/driver/README.md`).
