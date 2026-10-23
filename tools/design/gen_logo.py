#!/usr/bin/env python3
"""Genera el icono de Extendesk (los dos eslabones) para Android, Windows y design/logo/ desde un solo dibujo.

    python3 tools/design/gen_logo.py          # escribe los archivos generados
    python3 tools/design/gen_logo.py --check  # falla si alguno está desfasado
    python3 tools/design/gen_logo.py --ico    # además rehace windows/app/res/desklink.ico (pip install resvg-py)

El dibujo (abajo, en las coordenadas de design/logo/desklink-icon.png, 1254x1254) calca ese PNG; los colores salen
de la sección «logo» de design/tokens.json. Los archivos generados no se editan a mano.
"""
import io
import json
import math
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOKENS = ROOT / "design" / "tokens.json"
SVG = ROOT / "design/logo/desklink-icon.svg"
ANDROID_LOGO = ROOT / "android/app/src/main/res/drawable/ic_logo.xml"
ANDROID_LAUNCHER = ROOT / "android/app/src/main/res/drawable/ic_launcher_foreground.xml"
CPP = ROOT / "windows/app/src/logo.h"
ICO = ROOT / "windows/app/res/desklink.ico"

HEADER = "Generado por tools/design/gen_logo.py. No editar a mano."

# Caja del dibujo dentro del PNG original.
BOX_X, BOX_Y, BOX_W, BOX_H = 119, 316, 1034, 662

# Eslabón de atrás: abierto abajo a la izquierda, donde lo tapa el de delante.
BACK = ("M467,600 V492 A176,176 0 0 1 643,316 H977 A176,176 0 0 1 1153,492 V644 A176,176 0 0 1 977,820 H836 V697 "
        "H970 A48,48 0 0 0 1018,649 V494 A48,48 0 0 0 970,446 H655 A48,48 0 0 0 607,494 V600 Z")
# Eslabón de delante con la diagonal que lo une al de atrás; el segundo trazo es el hueco.
FRONT = ("M119,690 A155,155 0 0 1 274,535 H628 Q694,535 735,582 L925,797 Q945,820 975,820 H850 Q806,820 775,786 "
         "L733,740 V823 A155,155 0 0 1 578,978 H274 A155,155 0 0 1 119,823 Z "
         "M286,656 A34,34 0 0 0 252,690 V823 A34,34 0 0 0 286,857 H571 A34,34 0 0 0 605,823 V656 Z")
# Sombra (degradado de arriba abajo) donde el lado derecho del de delante pasa bajo la diagonal.
SHADOW = "M605,656 H652 L733,744 V830 H605 Z"
SHADOW_TOP, SHADOW_BOTTOM = 656, 830

KAPPA = 0.5522847498  # cuarto de círculo con una curva cúbica


def parse(d):
    """Pasa un trazo SVG (solo M H V L Q A Z absolutos y arcos de 90°) a M, L, C y Z."""
    tokens = re.findall(r"[MHVLQAZ]|-?\d+(?:\.\d+)?", d)
    out, i, cur, start = [], 0, (0.0, 0.0), (0.0, 0.0)

    def num():
        nonlocal i
        i += 1
        return float(tokens[i - 1])

    while i < len(tokens):
        cmd = tokens[i]
        i += 1
        if cmd == "M":
            cur = start = (num(), num())
            out.append(("M", cur))
        elif cmd in "HVL":
            if cmd == "H":
                cur = (num(), cur[1])
            elif cmd == "V":
                cur = (cur[0], num())
            else:
                cur = (num(), num())
            out.append(("L", cur))
        elif cmd == "Q":
            q, end = (num(), num()), (num(), num())
            c1 = (cur[0] + 2 / 3 * (q[0] - cur[0]), cur[1] + 2 / 3 * (q[1] - cur[1]))
            c2 = (end[0] + 2 / 3 * (q[0] - end[0]), end[1] + 2 / 3 * (q[1] - end[1]))
            out.append(("C", c1, c2, end))
            cur = end
        elif cmd == "A":
            r, _, _, _, sweep, end = num(), num(), num(), num(), num(), (num(), num())
            # Cuarto de círculo con los ejes rectos: el centro es una de las dos esquinas.
            for c in ((cur[0], end[1]), (end[0], cur[1])):
                u = (cur[0] - c[0], cur[1] - c[1])
                v = (end[0] - c[0], end[1] - c[1])
                cross = u[0] * v[1] - u[1] * v[0]
                if (cross > 0) == (sweep == 1):
                    break
            assert abs(math.hypot(*u) - r) < 1e-6 and abs(math.hypot(*v) - r) < 1e-6, f"arco no recto: {d}"
            c1 = (cur[0] + KAPPA * v[0], cur[1] + KAPPA * v[1])
            c2 = (end[0] + KAPPA * u[0], end[1] + KAPPA * u[1])
            out.append(("C", c1, c2, end))
            cur = end
        elif cmd == "Z":
            out.append(("Z",))
            cur = start
    return out


def transform(cmds, scale, dx, dy):
    """Lleva el dibujo a otras coordenadas: (x - BOX_X) * scale + dx."""
    def tp(p):
        return ((p[0] - BOX_X) * scale + dx, (p[1] - BOX_Y) * scale + dy)
    return [(c[0],) + tuple(tp(p) for p in c[1:]) for c in cmds]


def fmt(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


def cfloat(v):
    s = fmt(v)
    return (s if "." in s else s + ".0") + "f"


def path_data(cmds):
    parts = []
    for c in cmds:
        parts.append(c[0] + " ".join(f"{fmt(p[0])},{fmt(p[1])}" for p in c[1:]))
    return " ".join(parts)


def colors():
    logo = json.loads(TOKENS.read_text(encoding="utf-8"))["logo"]
    return {k: v["color"].upper() for k, v in logo.items() if not k.startswith("_")}


def svg(c):
    return f"""<?xml version="1.0" encoding="utf-8"?>
<!-- {HEADER} -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="{BOX_X} {BOX_Y} {BOX_W} {BOX_H}" width="{BOX_W}" height="{BOX_H}">
  <defs>
    <linearGradient id="shadow" x1="0" y1="{SHADOW_TOP}" x2="0" y2="{SHADOW_BOTTOM}" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="{c['front_shadow']}"/>
      <stop offset="1" stop-color="{c['front']}"/>
    </linearGradient>
  </defs>
  <path fill="{c['back']}" d="{BACK}"/>
  <path fill="{c['front']}" fill-rule="evenodd" d="{FRONT}"/>
  <path fill="url(#shadow)" d="{SHADOW}"/>
</svg>
"""


def android_vector(c, comment, size_dp, viewport, scale, dx, dy, mono=False):
    back = path_data(transform(parse(BACK), scale, dx, dy))
    front = path_data(transform(parse(FRONT), scale, dx, dy))
    shadow = path_data(transform(parse(SHADOW), scale, dx, dy))
    top = fmt((SHADOW_TOP - BOX_Y) * scale + dy)
    bottom = fmt((SHADOW_BOTTOM - BOX_Y) * scale + dy)
    w, h = viewport
    return f"""<?xml version="1.0" encoding="utf-8"?>
<!-- {HEADER} {comment} -->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:aapt="http://schemas.android.com/aapt"
    android:width="{fmt(size_dp[0])}dp"
    android:height="{fmt(size_dp[1])}dp"
    android:viewportWidth="{fmt(w)}"
    android:viewportHeight="{fmt(h)}">
    <path
        android:fillColor="#FF{c['back'][1:]}"
        android:pathData="{back}" />
    <path
        android:fillColor="#FF{c['front'][1:]}"
        android:fillType="evenOdd"
        android:pathData="{front}" />
    <path android:pathData="{shadow}">
        <aapt:attr name="android:fillColor">
            <gradient
                android:type="linear"
                android:startX="0"
                android:startY="{top}"
                android:endX="0"
                android:endY="{bottom}"
                android:startColor="#FF{c['front_shadow'][1:]}"
                android:endColor="#FF{c['front'][1:]}" />
        </aapt:attr>
    </path>
</vector>
"""


def launcher_scale():
    """Escala para que el dibujo quepa en el círculo de 66 dp del icono adaptativo (108 dp), con margen."""
    pts = []
    for d in (BACK, FRONT):
        for c in parse(d):
            pts.extend(c[1:])
    cx, cy = BOX_X + BOX_W / 2, BOX_Y + BOX_H / 2
    far = max(math.hypot(x - cx, y - cy) for x, y in pts)
    return 31 / far


def cpp(c):
    def table(name, d):
        lines = [f"constexpr Cmd {name}[] = {{"]
        for cmd in transform(parse(d), 1, 0, 0):
            op = {"M": "kMove", "L": "kLine", "C": "kCubic", "Z": "kClose"}[cmd[0]]
            nums = [v for p in cmd[1:] for v in p] + [0] * (6 - 2 * (len(cmd) - 1))
            lines.append(f"    {{{op}, {{{', '.join(cfloat(v) for v in nums)}}}}},")
        lines.append("};")
        return lines

    lines = [
        f"// {HEADER}",
        "// El icono de Extendesk (los dos eslabones) para dibujarlo con GDI+ a cualquier tamaño.",
        "#pragma once",
        "",
        "namespace logo {",
        "",
        "enum Op { kMove, kLine, kCubic, kClose };",
        "struct Cmd {",
        "  Op op;",
        "  float p[6];  // kMove/kLine: x, y; kCubic: x1, y1, x2, y2, x, y",
        "};",
        "",
        f"constexpr float kWidth = {BOX_W};",
        f"constexpr float kHeight = {BOX_H};",
        "",
        "// Eslabón de atrás (tokens::kLogoBack).",
    ]
    lines += table("kBack", BACK)
    lines += ["", "// Eslabón de delante (tokens::kLogoFront); el segundo trazo es el hueco (relleno alterno)."]
    lines += table("kFront", FRONT)
    lines += ["", "// Sombra: degradado vertical de tokens::kLogoFrontShadow (arriba) a tokens::kLogoFront (abajo)."]
    lines += table("kShadow", SHADOW)
    lines += [
        f"constexpr float kShadowTop = {SHADOW_TOP - BOX_Y};",
        f"constexpr float kShadowBottom = {SHADOW_BOTTOM - BOX_Y};",
        "",
        "}  // namespace logo",
        "",
    ]
    return "\n".join(lines)


def ico(c):
    import resvg_py
    from PIL import Image

    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    pad = 0.04  # margen alrededor, en fracción del lado
    source = svg(c)
    images = []
    for s in sizes:
        inner = s * (1 - 2 * pad)
        scale = inner / BOX_W
        w, h = round(BOX_W * scale), round(BOX_H * scale)
        png = bytes(resvg_py.svg_to_bytes(svg_string=source, width=w, height=h))
        canvas = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        canvas.paste(Image.open(io.BytesIO(png)), ((s - w) // 2, (s - h) // 2))
        buf = io.BytesIO()
        canvas.save(buf, format="PNG", optimize=True)
        images.append(buf.getvalue())
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, img in zip(sizes, images):
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    return out + b"".join(images)


def main() -> int:
    c = colors()
    ls = launcher_scale()
    outputs = {
        SVG: svg(c),
        ANDROID_LOGO: android_vector(c, "Icono para dentro de la app (cabecera).", (BOX_W / BOX_H * 28, 28),
                                     (BOX_W, BOX_H), 1, 0, 0),
        ANDROID_LAUNCHER: android_vector(c, "Primer plano del icono adaptativo, centrado en 108 dp.", (108, 108),
                                         (108, 108), ls, 54 - BOX_W * ls / 2, 54 - BOX_H * ls / 2),
        CPP: cpp(c),
    }
    errors = []
    if "--check" in sys.argv:
        for path, text in outputs.items():
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                errors.append(f"{path.relative_to(ROOT)} no coincide con el dibujo: ejecuta tools/design/gen_logo.py")
    else:
        for path, text in outputs.items():
            path.write_text(text, encoding="utf-8")
            print(f"escrito {path.relative_to(ROOT)}")
        if "--ico" in sys.argv:
            ICO.write_bytes(ico(c))
            print(f"escrito {ICO.relative_to(ROOT)}")
    for e in errors:
        print("ERROR:", e, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
