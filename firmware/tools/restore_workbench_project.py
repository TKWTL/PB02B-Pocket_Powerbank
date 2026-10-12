#!/usr/bin/env python3
"""Reapply project-owned sources after AT32 Workbench regenerates Keil project.

Usage (repository root):
    python firmware/tools/restore_workbench_project.py
    python firmware/tools/restore_workbench_project.py --check
"""
from pathlib import Path
import re
import sys

UVPROJ = Path(__file__).resolve().parents[1] / "project/MDK_V5/Pocket_PowerBank.uvprojx"
CUSTOM = ("leveler.c", "dinosaur_game.c", "dinosaur_icon.c")


def adjusted(text: str) -> str:
    # Generated files are kept on disk for Workbench but excluded from Keil:
    # bsp_i2c.c owns hi2c1, all used master IRQ operations and crash fixes.
    text = re.sub(
        r"\s*<Group>\s*<GroupName>middlewares/i2c_application</GroupName>[\s\S]*?</Group>",
        "", text,
    )
    text = re.sub(
        r"\s*<File>\s*<FileName>i2c_app\.c</FileName>[\s\S]*?</File>",
        "", text,
    )
    text = text.replace(";../../middlewares/i2c_application_library", "")
    text = text.replace(";..\\..\\middlewares\\i2c_application_library", "")

    group = re.search(
        r"<Group>\s*<GroupName>Functions</GroupName>[\s\S]*?</Group>", text
    )
    if not group:
        raise ValueError("Keil Functions group missing; inspect Workbench output")

    block = group.group()
    for filename in CUSTOM:
        if f"<FileName>{filename}</FileName>" not in block:
            entry = (
                "            <File>\n"
                f"              <FileName>{filename}</FileName>\n"
                "              <FileType>1</FileType>\n"
                f"              <FilePath>..\\..\\Applications\\functions\\{filename}</FilePath>\n"
                "            </File>\n"
            )
            block = block.replace("</Files>", entry + "          </Files>")
    text = text[: group.start()] + block + text[group.end():]
    if "<FileName>bsp_i2c.c</FileName>" not in text:
        raise ValueError("bsp_i2c.c missing from Keil project")
    if "<FileName>i2c_application.c</FileName>" in text or "<FileName>i2c_app.c</FileName>" in text:
        raise ValueError("Generated I2C source was not excluded")
    return text


def main() -> int:
    source = UVPROJ.read_text(encoding="utf-8-sig")
    fixed = adjusted(source)
    if "--check" in sys.argv:
        if fixed != source:
            print("Workbench regeneration changed Keil source ownership. Run this script without --check.")
            return 1
        print("Keil custom I2C and Functions entries OK.")
        return 0
    if fixed != source:
        UVPROJ.write_text(fixed, encoding="utf-8")
        print("Restored BSP-owned I2C and Functions entries in", UVPROJ)
    else:
        print("Keil project already up to date.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
