# Landing de DeskLink

Página de presentación hecha con [Astro](https://astro.build), sin dependencias aparte de Astro.

```sh
cd landing
npm install
npm run dev      # http://localhost:4321/deslink/
npm run build    # deja la web estática en dist/
```

- Enlaces de descarga, correo de contacto y repositorio: [`src/config.ts`](src/config.ts). Con `email` vacío, el
  formulario de contacto abre un issue en GitHub con el mensaje.
- Una sección por componente en [`src/components/`](src/components/); la página los junta en `src/pages/index.astro`.
- Colores y fuentes de la marca (`design/`): `src/styles/global.css`. Las animaciones respetan
  «reducir movimiento» del sistema.
- Las cifras salen de la documentación del repo (`README.md`, `docs/ajustes.md`, `docs/battery.md`, `docs/usb.md`).

## Publicación

[`.github/workflows/landing.yml`](../.github/workflows/landing.yml) la compila en cada PR que la toca y la publica en
GitHub Pages en cada cambio en `main`: <https://sabahrahal.github.io/deslink/>. Hace falta activarlo una vez en
Settings → Pages → Source: «GitHub Actions». Con un dominio propio, cambia `site` y quita `base` en
`astro.config.mjs`.
