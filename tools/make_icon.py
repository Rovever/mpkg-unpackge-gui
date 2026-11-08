#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
由与 assets/mpkg-icon.svg 相同的 128x128 几何生成图标位图：
    assets/mpkg-icon.ico  多尺寸图标（16/24/32/48/64/128/256，供 exe 资源使用）
    assets/mpkg-icon.png  256x256 预览（供文档 / 不支持 SVG 的场景使用）

仅「重新生成图标」时需要 Pillow；编译和运行 mpkg_gui.exe 都不需要它。
    python tools/make_icon.py
"""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SS = 4                     # 超采样倍数，缩小后得到平滑边缘
BASE = 128
SIZES = [16, 24, 32, 48, 64, 128, 256]

BG = "#2563EB"
PAPER_BACK, FOLD_BACK = "#BFDBFE", "#93C5FD"
PAPER_FRONT, FOLD_FRONT = "#FFFFFF", "#DBEAFE"
TEXT_LINE = "#BFDBFE"
BOX, RIM, ARROW = "#FFFFFF", "#EFF6FF", "#2563EB"


def render(size):
    s = size * SS
    k = s / BASE
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    def pt(x, y):
        return (int(round(x * k)), int(round(y * k)))

    def rect(box, radius, color):
        d.rounded_rectangle(
            (int(round(box[0] * k)), int(round(box[1] * k)),
             int(round(box[2] * k)), int(round(box[3] * k))),
            radius=max(1, int(round(radius * k))), fill=color)

    def line(points, color, width):
        w = max(1, int(round(width * k)))
        d.line([pt(*p) for p in points], fill=color, width=w, joint="curve")
        r = w / 2.0
        for x, y in (points[0], points[-1]):           # 圆头端点
            cx, cy = pt(x, y)
            d.ellipse((int(cx - r), int(cy - r), int(cx + r), int(cy + r)), fill=color)

    # 背景
    rect((4, 4, 124, 124), 28, BG)

    # 后层文件
    d.rectangle((*pt(56, 18), *pt(92, 66)), fill=PAPER_BACK)
    d.polygon([pt(80, 18), pt(92, 30), pt(80, 30)], fill=FOLD_BACK)

    # 前层文件
    d.rectangle((*pt(36, 30), *pt(72, 78)), fill=PAPER_FRONT)
    d.polygon([pt(60, 30), pt(72, 42), pt(60, 42)], fill=FOLD_FRONT)
    line([(43, 52), (65, 52)], TEXT_LINE, 4)
    line([(43, 60), (58, 60)], TEXT_LINE, 4)

    # 包箱本体与箱口沿（口沿压住文件下缘）
    rect((20, 70, 108, 106), 9, BOX)
    rect((16, 62, 112, 76), 6, RIM)

    # 输出箭头
    line([(64, 80), (64, 96)], ARROW, 6)
    line([(56, 90), (64, 100), (72, 90)], ARROW, 6)

    return im.resize((size, size), Image.LANCZOS)


def main():
    big = render(256)
    ico = ROOT / "assets" / "mpkg-icon.ico"
    png = ROOT / "assets" / "mpkg-icon.png"
    big.save(ico, format="ICO", sizes=[(n, n) for n in SIZES])
    big.save(png, format="PNG")
    print("wrote:", ico, ico.stat().st_size, "bytes,", len(SIZES), "sizes")
    print("wrote:", png, png.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
