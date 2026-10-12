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
IRQ_SRC = UVPROJ.parents[1] / "src/at32f423_int.c"
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


def adjusted_irq(text: str) -> str:
    # With Workbench I2C Application disabled, its generated dispatcher
    # disappears. Keep both calls in the generator's preserved user sections.
    for label, call in (
        ("I2C1_EVT_IRQ 1", "i2c_evt_irq_handler(&hi2c1);"),
        ("I2C1_ERR_IRQ 1", "i2c_err_irq_handler(&hi2c1);"),
    ):
        begin = f"/* add user code begin {label} */"
        end = f"/* add user code end {label} */"
        start = text.find(begin)
        stop = text.find(end, start + len(begin))
        if start < 0 or stop < 0:
            raise ValueError(f"{label} user region missing: check Workbench IRQ generation")
        region = text[start + len(begin):stop]
        if text.count(call) > 1 or (call in text and call not in region):
            raise ValueError(f"{call} is duplicated or outside retained user code")
        if call not in region:
            text = text[:start + len(begin)] + "\n    " + call + text[start + len(begin):]
    return text


def main() -> int:
    source = UVPROJ.read_text(encoding="utf-8-sig")
    irq_source = IRQ_SRC.read_text(encoding="utf-8-sig")
    fixed = adjusted(source)
    irq_fixed = adjusted_irq(irq_source)
    if "--check" in sys.argv:
        if fixed != source or irq_fixed != irq_source:
            print("Workbench regenerated Keil entries or I2C IRQ dispatch; run without --check.")
            return 1
        print("Keil source list and BSP I2C IRQ handlers OK.")
        return 0
    if fixed != source:
        UVPROJ.write_text(fixed, encoding="utf-8")
        print("Restored BSP-owned I2C and Functions entries in", UVPROJ)
    if irq_fixed != irq_source:
        IRQ_SRC.write_text(irq_fixed, encoding="utf-8")
        print("Restored I2C1 event/error dispatch in preserved user code")
    if fixed == source and irq_fixed == irq_source:
        print("Keil project and I2C IRQ dispatch already up to date.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
