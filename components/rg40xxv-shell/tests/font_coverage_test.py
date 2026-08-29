#!/usr/bin/env python3
import sys

from fontTools.ttLib import TTFont


def missing_characters(cmap, text):
    return sorted({character for character in text if ord(character) not in cmap})


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: font_coverage_test.py FONT")
    font = TTFont(sys.argv[1], lazy=True)
    cmap = font.getBestCmap() or {}
    traditional = "繁體中文遊戲庫應用程式設定重新啟動開機選單原廠維修螢幕搖桿"
    simplified = "简体中文游戏设置应用程序启动菜单屏幕摇杆传奇龙门后备"
    missing_traditional = missing_characters(cmap, traditional)
    missing_simplified = missing_characters(cmap, simplified)
    if missing_traditional or missing_simplified:
        raise SystemExit(
            "missing glyphs: zh_TW=%r zh_CN_titles=%r"
            % (missing_traditional, missing_simplified)
        )
    print(
        "FONT_COVERAGE_TEST PASS zh_TW=%d simplified-title=%d glyphs=%d"
        % (len(set(traditional)), len(set(simplified)), len(cmap))
    )


if __name__ == "__main__":
    main()
