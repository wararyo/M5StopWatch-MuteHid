"""Convert the pinned Material Design Icons SVGs into 8-bit alpha masks.

The firmware needs no SVG parser at runtime: this emits one C++ header with a
grayscale coverage map per icon, which M5GFX draws through pushGrayscaleImage()
with the foreground and background colours chosen by the UI.

Runs as a PlatformIO pre-script and standalone (``python tools/icons.py``).
"""

import hashlib
import math
import re
from pathlib import Path

try:  # SCons executes build scripts without __file__; it runs from the project.
    ROOT = Path(__file__).resolve().parent.parent
except NameError:
    ROOT = Path.cwd()
SOURCE = ROOT / "assets/icons"
OUTPUT = ROOT / "src/ui/generated/Icons.h"

# (file stem, C++ identifier, pixel size). Sizes are the ones the UI draws;
# scaling at runtime would blur the anti-aliased edges.
ICONS = [
    ("microphone", "microphone", 176),
    ("microphone-off", "microphoneOff", 176),
    ("microphone-question", "microphoneQuestion", 176),
]

SUBSAMPLES = 8  # Sub-rows per pixel row; x coverage is computed analytically.
CURVE_STEPS = 24

TOKENS = re.compile(r"[MmLlHhVvCcSsQqTtAaZz]|-?\d*\.?\d+(?:[eE][-+]?\d+)?")


def tokenize(data):
    for token in TOKENS.findall(data):
        yield token


def cubic(p0, p1, p2, p3, out):
    for step in range(1, CURVE_STEPS + 1):
        t = step / CURVE_STEPS
        u = 1.0 - t
        x = u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0]
        y = u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]
        out.append((x, y))


def quadratic(p0, p1, p2, out):
    cubic(p0, (p0[0] + 2 / 3 * (p1[0] - p0[0]), p0[1] + 2 / 3 * (p1[1] - p0[1])),
          (p2[0] + 2 / 3 * (p1[0] - p2[0]), p2[1] + 2 / 3 * (p1[1] - p2[1])), p2, out)


def arc(start, rx, ry, rotation, large, sweep, end, out):
    """Endpoint to centre parameterisation, per the SVG implementation notes."""
    if rx == 0 or ry == 0 or start == end:
        out.append(end)
        return
    rx, ry = abs(rx), abs(ry)
    phi = math.radians(rotation)
    cos_phi, sin_phi = math.cos(phi), math.sin(phi)
    dx2, dy2 = (start[0] - end[0]) / 2, (start[1] - end[1]) / 2
    x1 = cos_phi * dx2 + sin_phi * dy2
    y1 = -sin_phi * dx2 + cos_phi * dy2
    scale = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry)
    if scale > 1:
        rx *= math.sqrt(scale)
        ry *= math.sqrt(scale)
    denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1
    numerator = max(rx * rx * ry * ry - denominator, 0.0)
    factor = math.sqrt(numerator / denominator) if denominator else 0.0
    if large == sweep:
        factor = -factor
    cx1 = factor * rx * y1 / ry
    cy1 = -factor * ry * x1 / rx
    cx = cos_phi * cx1 - sin_phi * cy1 + (start[0] + end[0]) / 2
    cy = sin_phi * cx1 + cos_phi * cy1 + (start[1] + end[1]) / 2

    def angle(ux, uy, vx, vy):
        dot = ux * vx + uy * vy
        length = math.hypot(ux, uy) * math.hypot(vx, vy)
        value = max(-1.0, min(1.0, dot / length)) if length else 0.0
        result = math.acos(value)
        return -result if ux * vy - uy * vx < 0 else result

    theta = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry)
    delta = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry)
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    elif sweep and delta < 0:
        delta += 2 * math.pi
    steps = max(2, int(CURVE_STEPS * abs(delta) / math.pi))
    for step in range(1, steps + 1):
        t = theta + delta * step / steps
        x = cos_phi * rx * math.cos(t) - sin_phi * ry * math.sin(t) + cx
        y = sin_phi * rx * math.cos(t) + cos_phi * ry * math.sin(t) + cy
        out.append((x, y))


def flatten(path):
    """Return the path as closed polygons in user units."""
    tokens = list(tokenize(path))
    index = 0
    polygons, current = [], []
    position = (0.0, 0.0)
    start = (0.0, 0.0)
    command = ""
    previous_control = None

    def number():
        nonlocal index
        value = float(tokens[index])
        index += 1
        return value

    def point(relative):
        x, y = number(), number()
        return (position[0] + x, position[1] + y) if relative else (x, y)

    while index < len(tokens):
        token = tokens[index]
        if token.isalpha():
            command = token
            index += 1
        elif command in ("M", "m"):
            command = "L" if command == "M" else "l"
        if command in ("M", "m"):
            if len(current) > 1:
                polygons.append(current)
            position = start = point(command == "m")
            current = [position]
            previous_control = None
        elif command in ("L", "l"):
            position = point(command == "l")
            current.append(position)
            previous_control = None
        elif command in ("H", "h"):
            x = number()
            position = (position[0] + x, position[1]) if command == "h" else (x, position[1])
            current.append(position)
            previous_control = None
        elif command in ("V", "v"):
            y = number()
            position = (position[0], position[1] + y) if command == "v" else (position[0], y)
            current.append(position)
            previous_control = None
        elif command in ("C", "c", "S", "s"):
            relative = command in ("c", "s")
            if command in ("C", "c"):
                control1 = point(relative)
            else:
                control1 = position if previous_control is None else (
                    2 * position[0] - previous_control[0], 2 * position[1] - previous_control[1])
            control2 = point(relative)
            end = point(relative)
            cubic(position, control1, control2, end, current)
            position, previous_control = end, control2
        elif command in ("Q", "q", "T", "t"):
            relative = command in ("q", "t")
            if command in ("Q", "q"):
                control = point(relative)
            else:
                control = position if previous_control is None else (
                    2 * position[0] - previous_control[0], 2 * position[1] - previous_control[1])
            end = point(relative)
            quadratic(position, control, end, current)
            position, previous_control = end, control
        elif command in ("A", "a"):
            rx, ry, rotation = number(), number(), number()
            large, sweep = int(number()), int(number())
            end = point(command == "a")
            arc(position, rx, ry, rotation, large, sweep, end, current)
            position, previous_control = end, None
        elif command in ("Z", "z"):
            if len(current) > 1:
                current.append(start)
                polygons.append(current)
            current = [start]
            position = start
            previous_control = None
        else:
            raise ValueError("unsupported path command: %s" % command)
    if len(current) > 1:
        polygons.append(current)
    return polygons


def rasterize(polygons, size, view):
    """Scanline fill with the nonzero winding rule into an 8-bit coverage map."""
    scale = size / view
    edges = []
    for polygon in polygons:
        points = [(x * scale, y * scale) for x, y in polygon]
        if points[0] != points[-1]:
            points.append(points[0])
        for (x0, y0), (x1, y1) in zip(points, points[1:]):
            if y0 != y1:
                edges.append((x0, y0, x1, y1))
    pixels = bytearray(size * size)
    for row in range(size):
        coverage = [0.0] * size
        for sub in range(SUBSAMPLES):
            y = row + (sub + 0.5) / SUBSAMPLES
            crossings = []
            for x0, y0, x1, y1 in edges:
                if (y0 <= y < y1) or (y1 <= y < y0):
                    crossings.append((x0 + (y - y0) / (y1 - y0) * (x1 - x0), 1 if y1 > y0 else -1))
            if not crossings:
                continue
            crossings.sort()
            winding, span_start = 0, 0.0
            for x, direction in crossings:
                if winding == 0:
                    span_start = x
                winding += direction
                if winding == 0 and x > span_start:
                    left = max(0.0, span_start)
                    right = min(float(size), x)
                    first, last = int(left), min(size - 1, int(math.ceil(right)) - 1)
                    for pixel in range(first, last + 1):
                        overlap = min(right, pixel + 1) - max(left, float(pixel))
                        if overlap > 0:
                            coverage[pixel] += overlap
        base = row * size
        for column in range(size):
            value = coverage[column] / SUBSAMPLES
            pixels[base + column] = min(255, int(value * 255 + 0.5))
    return pixels


def convert(stem, size):
    text = (SOURCE / (stem + ".svg")).read_text(encoding="utf-8")
    view = float(re.search(r'viewBox="0 0 (\d+(?:\.\d+)?)', text).group(1))
    body = bytearray(size * size)
    for path in re.findall(r'\sd="([^"]+)"', text):
        mask = rasterize(flatten(path), size, view)
        for index, value in enumerate(mask):
            if value > body[index]:
                body[index] = value
    return body


def render(icons):
    lines = [
        "#pragma once",
        "// Generated by tools/icons.py from assets/icons/*.svg. Do not edit.",
        "// Material Design Icons by Pictogrammers, pinned at commit",
        "// 2424e748e0cc63ab7b9c095a099b9fe239b737c0, Apache License 2.0.",
        "// See assets/icons/LICENSE for the full text.",
        "#include <cstdint>",
        "",
        "namespace icons {",
        "struct Icon { int16_t width, height; const uint8_t* alpha; };",
        "",
    ]
    for stem, name, size, data in icons:
        lines.append("// %s.svg at %dx%d" % (stem, size, size))
        lines.append("inline constexpr uint8_t %sAlpha[] = {" % name)
        for offset in range(0, len(data), 32):
            chunk = data[offset:offset + 32]
            lines.append("    " + "".join("%d," % value for value in chunk))
        lines.append("};")
        lines.append("inline constexpr Icon %s{%d, %d, %sAlpha};" % (name, size, size, name))
        lines.append("")
    lines.append("}  // namespace icons")
    lines.append("")
    return "\n".join(lines)


def generate():
    icons = [(stem, name, size, convert(stem, size)) for stem, name, size in ICONS]
    header = render(icons)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    if OUTPUT.exists() and OUTPUT.read_text(encoding="utf-8") == header:
        return False
    OUTPUT.write_text(header, encoding="utf-8", newline="\n")
    return True


def preview(name, threshold=96):
    for stem, identifier, size in ICONS:
        if identifier != name and stem != name:
            continue
        data = convert(stem, size)
        step = max(1, size // 48)
        for row in range(0, size, step * 2):
            print("".join("#" if data[row * size + column] > threshold else "."
                          for column in range(0, size, step)))


if __name__ == "__main__":
    import sys

    if len(sys.argv) > 2 and sys.argv[1] == "preview":
        preview(sys.argv[2])
    else:
        changed = generate()
        digest = hashlib.sha256(OUTPUT.read_bytes()).hexdigest()[:12]
        print("%s %s (%s)" % ("wrote" if changed else "unchanged", OUTPUT.name, digest))
else:
    try:
        Import("env")  # noqa: F821  PlatformIO injects this only for build scripts.
    except NameError:
        pass
    else:
        if generate():
            print("icons.py: regenerated %s" % OUTPUT.relative_to(ROOT))
