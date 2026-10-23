#!/usr/bin/env python3
"""Genera los tokens de diseño de Extendesk para Android y Windows a partir de design/tokens.json.

    python3 tools/design/gen_tokens.py          # escribe los archivos generados
    python3 tools/design/gen_tokens.py --check  # falla si están desfasados o si algún par no se lee bien

Los archivos generados no se editan a mano (lo dice su cabecera): un color cambiado solo en un lado hace fallar
--check en la CI.
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "design" / "tokens.json"
KOTLIN = ROOT / "android/app/src/main/kotlin/com/desklink/ui/Tokens.kt"
CPP = ROOT / "windows/app/src/tokens.h"

HEADER = "Generado por tools/design/gen_tokens.py desde design/tokens.json. No editar a mano."


def camel(name: str) -> str:
    parts = name.split("_")
    return parts[0] + "".join(p.capitalize() for p in parts[1:])


def pascal(name: str) -> str:
    return "".join(p.capitalize() for p in name.split("_"))


def rgb(hex_color: str):
    h = hex_color.lstrip("#")
    return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)


def luminance(hex_color: str) -> float:
    def channel(c: int) -> float:
        c = c / 255
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4

    r, g, b = rgb(hex_color)
    return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b)


def contrast(a: str, b: str) -> float:
    la, lb = sorted([luminance(a), luminance(b)], reverse=True)
    return (la + 0.05) / (lb + 0.05)


def entries(section):
    """Las entradas de una sección, sin las notas («_comment»)."""
    return [(k, v) for k, v in section.items() if not k.startswith("_")]


def kotlin(t) -> str:
    colors = t["colors"]
    lines = [
        f"// {HEADER}",
        "package com.desklink.ui",
        "",
        "import androidx.compose.ui.graphics.Color",
        "import androidx.compose.ui.text.font.FontWeight",
        "import androidx.compose.ui.unit.dp",
        "import androidx.compose.ui.unit.sp",
        "",
        "/** Colores de un modo (claro u oscuro). */",
        "data class DlColors(",
    ]
    for name, c in colors.items():
        lines.append(f"    /** {c['use']}. */")
        lines.append(f"    val {camel(name)}: Color,")
    lines.append(")")
    lines.append("")
    for mode in ("light", "dark"):
        lines.append(f"val {mode.capitalize()}Colors = DlColors(")
        for name, c in colors.items():
            lines.append(f"    {camel(name)} = Color(0xFF{c[mode].lstrip('#').upper()}),")
        lines.append(")")
        lines.append("")
    lines.append("/** Tamaños, pesos y fuente del texto (Brand = Montserrat, la del logo; si no, la del sistema). */")
    lines.append("object DlType {")
    for name, s in entries(t["type"]):
        lines.append(f"    /** {s['use']}. */")
        lines.append(f"    val {pascal(name)}Size = {s['size']}.sp")
        lines.append(f"    val {pascal(name)}Weight = FontWeight({s['weight']})")
        lines.append(f"    const val {pascal(name)}Brand = {'true' if s['family'] == 'brand' else 'false'}")
    lines.append("}")
    lines.append("")
    lines.append("/** Colores fijos del icono (los mismos en claro y oscuro). */")
    lines.append("object DlLogo {")
    for name, c in entries(t["logo"]):
        lines.append(f"    /** {c['use']}. */")
        lines.append(f"    val {pascal(name)} = Color(0xFF{c['color'].lstrip('#').upper()})")
    lines.append("}")
    lines.append("")
    shape = t["shape"]
    lines.append("object DlShape {")
    lines.append(f"    val CardRadius = {shape['card_radius']}.dp")
    lines.append(f"    val ControlRadius = {shape['control_radius']}.dp")
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def cpp(t) -> str:
    colors = t["colors"]
    lines = [
        f"// {HEADER}",
        "#pragma once",
        "",
        "#include <windows.h>",
        "",
        "namespace tokens {",
        "",
        "struct Colors {",
    ]
    for name, c in colors.items():
        lines.append(f"  COLORREF {name};  // {c['use']}")
    lines.append("};")
    lines.append("")
    for mode in ("light", "dark"):
        lines.append(f"constexpr Colors k{mode.capitalize()} = {{")
        for name, c in colors.items():
            r, g, b = rgb(c[mode])
            lines.append(f"    RGB(0x{r:02X}, 0x{g:02X}, 0x{b:02X}),  // {name}")
        lines.append("};")
        lines.append("")
    lines.append("// Texto (px a 96 ppp), peso y fuente (Brand = Montserrat, la del logo; si no, Segoe UI).")
    for name, s in entries(t["type"]):
        lines.append(f"constexpr float k{pascal(name)}Px = {s['size']};")
        lines.append(f"constexpr int k{pascal(name)}Weight = {s['weight']};")
        lines.append(f"constexpr bool k{pascal(name)}Brand = {'true' if s['family'] == 'brand' else 'false'};")
    lines.append("")
    lines.append("// Colores fijos del icono (los mismos en claro y oscuro).")
    for name, c in entries(t["logo"]):
        r, g, b = rgb(c["color"])
        lines.append(f"constexpr COLORREF kLogo{pascal(name)} = RGB(0x{r:02X}, 0x{g:02X}, 0x{b:02X});  // {c['use']}")
    lines.append("")
    shape = t["shape"]
    lines.append(f"constexpr float kCardRadius = {shape['card_radius']};")
    lines.append(f"constexpr float kControlRadius = {shape['control_radius']};")
    lines.append("")
    lines.append("}  // namespace tokens")
    lines.append("")
    return "\n".join(lines)


def check_contrast(t) -> list:
    errors = []
    for fg, bg, minimum in t["contrast"]["pairs"]:
        for mode in ("light", "dark"):
            ratio = contrast(t["colors"][fg][mode], t["colors"][bg][mode])
            if ratio < minimum:
                errors.append(f"{mode}: {fg} sobre {bg} = {ratio:.2f} (mínimo {minimum})")
    return errors


def main() -> int:
    t = json.loads(SOURCE.read_text(encoding="utf-8"))
    outputs = {KOTLIN: kotlin(t), CPP: cpp(t)}
    errors = check_contrast(t)
    if "--check" in sys.argv:
        for path, text in outputs.items():
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                errors.append(f"{path.relative_to(ROOT)} no coincide con design/tokens.json: ejecuta tools/design/gen_tokens.py")
    else:
        for path, text in outputs.items():
            path.write_text(text, encoding="utf-8")
            print(f"escrito {path.relative_to(ROOT)}")
    for e in errors:
        print("ERROR:", e, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
