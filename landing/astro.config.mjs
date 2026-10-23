// @ts-check
import { defineConfig } from 'astro/config';

// Se publica en GitHub Pages: https://sabahrahal.github.io/deslink/
// Con un dominio propio, cambia `site` por el dominio y quita `base`.
export default defineConfig({
  site: 'https://sabahrahal.github.io',
  base: '/deslink',
});
