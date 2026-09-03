#!/usr/bin/env python3
"""Упаковывает web/* в zip и генерирует src/web_assets_data.cpp с байтовым массивом.
Запуск: python3 tools/embed_assets.py  (из корня проекта)
"""
import os, zipfile, io, textwrap, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
OUT = ROOT / "src" / "web_assets_data.cpp"

def main():
    if not WEB.is_dir():
        print(f"web/ не найдена: {WEB}")
        return 1
    files = sorted([p for p in WEB.rglob("*") if p.is_file()])
    if not files:
        print("web/ пуста")
        return 1
    # zip в памяти
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for p in files:
            rel = p.relative_to(WEB).as_posix()
            zf.write(p, rel)
    data = buf.getvalue()
    # генерация cpp
    lines = []
    lines.append("// Автогенерация: не править вручную. Сгенерировано tools/embed_assets.py")
    lines.append("#include \"web_assets.h\"")
    lines.append("#include <cstddef>")
    lines.append("namespace web_assets {")
    lines.append(f"const size_t zip_size = {len(data)};")
    lines.append("const unsigned char zip_data[] = {")
    # по 12 байт в строке
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    lines.append("} // namespace web_assets")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Записано {OUT} ({len(data)} байт zip, {len(files)} файлов)")
    for p in files:
        print(f"  - {p.relative_to(WEB)}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
