// Datos que cambian a menudo: enlaces, contacto y repositorio. Lo demás está en los componentes.

export const REPO = 'https://github.com/sabahrahal/deslink';

export const DOWNLOADS = {
  // Versión «latest» de GitHub Releases: la CI la vuelve a publicar con cada cambio en main y los enlaces no cambian.
  windows: `${REPO}/releases/download/latest/DeskLink-Setup.exe`,
  android: `${REPO}/releases/download/latest/DeskLink.apk`,
  releases: `${REPO}/releases/tag/latest`,
};

export const CONTACT = {
  // Pendiente: el correo de contacto. Mientras esté vacío, el formulario manda a abrir un issue en GitHub.
  email: '',
  issues: `${REPO}/issues/new`,
};
