#!/usr/bin/env python3
"""サブ CPU 側メモリマップの検査 (= make が自動実行、失敗するとビルドが止まる)。

検査する内容は 2 つ:

1. **アドレス定数の突合せ**
   config.mk / src/asm_subprog.s の equ / src/c_subprog.h の #define に
   同じアドレスが書かれている。片方だけ直すと「転送先と参照先がずれる」
   という静かな壊れ方をする。エラーも例外も出ず、描画や実行の異常に
   つながる。よって subprog.bin を作り直すたびに機械的に突き合わせる。
   **正は config.mk。**
   併せて、各領域が順に並び、作業変数 (32 byte) と消し込み表 (208 byte) が
   次の領域の先頭に収まり、全体がコンソール RAM ($C000-$CFFF) の中に
   あることも検査する。

2. **build/subprog.bin のサイズ検査**
   subprog コードは SUB_PROG_ADDR から上へ伸び、その先には描画プログラムが
   自分で張るスタックの領域 (SUB_CODE_END = SUB_STACK_ADDR) がある。
   超えても lwasm も lwlink もエラーを出さない。スタックを踏むとサブ CPU
   ごと暴走するので、ここで機械的に止める。

使い方 (= Makefile 経由で自動実行):
  python3 scripts/check_layout.py build/subprog.bin
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 3 箇所で一致していなければならないアドレス定数。
# (config.mk のキー, asm_subprog.s の equ 名, c_subprog.h の #define 名)
# None は「そのファイルには書かれていない」の意味。
KEYS = [
    ("SUB_WORK_ADDR",   "SUB_WORK_ADDR",   "SUB_WORK_ADDR"),
    ("SUB_TABLE_ADDR",  "SUB_TABLE_ADDR",  "SUB_TABLE_ADDR"),
    ("SUB_PROG_ADDR",   "SUB_PROG_ADDR",   "SUB_PROG_ADDR"),
    ("SUB_CODE_END",    "SUB_CODE_END",    "SUB_CODE_END"),
    ("SUB_STACK_ADDR",  "SUB_STACK_ADDR",  None),
    ("SUB_STACK_TOP",   "SUB_STACK_TOP",   None),
]

# サブ側コンソール RAM の範囲 (SUB_RAM_START 以上、SUB_RAM_END 未満)
SUB_RAM_START = 0xC000
SUB_RAM_END = 0xD000
# 各領域の大きさ (config.mk の注釈の値)
SUB_WORK_SIZE = 32          # 作業変数
SUB_TABLE_SIZE = 2 * 26 * 4  # ページごとの消し込み表 (2 ページ x 26 件 x 4 byte)

# 値として受け付ける書き方 (値の後ろは空白と注釈だけ。$C100+$10 のような式は
# 対応しないのでエラーにする)
RE_C_VALUE = re.compile(r"^(0x[0-9A-Fa-f]+|\d+)\s*(?:/\*.*)?$")
RE_MK_VALUE = re.compile(r"^(0x[0-9A-Fa-f]+|\d+)\s*(?:#.*)?$")
RE_ASM_VALUE = re.compile(r"^\$([0-9A-Fa-f]+)(?:\s+.*)?$")


def die(msg: str) -> None:
    print(f"check_layout: NG — {msg}", file=sys.stderr)
    sys.exit(1)


def parse_config_mk() -> dict:
    vals = {}
    text = (ROOT / "config.mk").read_text(encoding="utf-8")
    for m in re.finditer(r"^\s*(SUB_\w+)\s*=\s*(.*)$", text, re.M):
        v = RE_MK_VALUE.match(m.group(2).strip())
        if not v:
            die(f"config.mk の {m.group(1)} の値 {m.group(2).strip()!r} を読めない")
        vals[m.group(1)] = int(v.group(1), 0)
    return vals


def parse_asm_equ() -> dict:
    vals = {}
    text = (ROOT / "src" / "asm_subprog.s").read_text(encoding="utf-8")
    for m in re.finditer(r"^(SUB_\w+)\s+equ\s+(.*)$", text, re.M):
        v = RE_ASM_VALUE.match(m.group(2).strip())
        if not v:
            die(f"src/asm_subprog.s の equ {m.group(1)} の値 "
                f"{m.group(2).strip()!r} を読めない")
        vals[m.group(1)] = int(v.group(1), 16)
    return vals


def parse_c_header() -> dict:
    vals = {}
    text = (ROOT / "src" / "c_subprog.h").read_text(encoding="utf-8")
    for m in re.finditer(r"^#define\s+(SUB_\w+)\s+(.*)$", text, re.M):
        v = RE_C_VALUE.match(m.group(2).strip())
        if not v:
            # SUB_PARAM_BASE のようなアドレス以外の #define は対象外
            if m.group(1) in {k[2] for k in KEYS}:
                die(f"src/c_subprog.h の #define {m.group(1)} の値 "
                    f"{m.group(2).strip()!r} を読めない")
            continue
        vals[m.group(1)] = int(v.group(1), 0)
    return vals


def main() -> None:
    if len(sys.argv) != 2:
        die("usage: check_layout.py <subprog.bin>")
    binpath = Path(sys.argv[1])

    mk = parse_config_mk()
    asm = parse_asm_equ()
    ch = parse_c_header()

    # 1. アドレス定数の突合せ (正は config.mk)
    for mk_key, asm_key, ch_key in KEYS:
        if mk_key not in mk:
            die(f"config.mk に {mk_key} が無い")
        want = mk[mk_key]
        if asm_key is not None:
            if asm_key not in asm:
                die(f"src/asm_subprog.s に equ {asm_key} が無い")
            if asm[asm_key] != want:
                die(f"{asm_key}: config.mk=${want:04X} だが "
                    f"asm_subprog.s=${asm[asm_key]:04X}")
        if ch_key is not None:
            if ch_key not in ch:
                die(f"src/c_subprog.h に #define {ch_key} が無い")
            if ch[ch_key] != want:
                die(f"{ch_key}: config.mk=${want:04X} だが "
                    f"c_subprog.h=${ch[ch_key]:04X}")

    # 前後関係 (領域が並び順どおりか、コンソール RAM の中か)
    if not (SUB_RAM_START <= mk["SUB_WORK_ADDR"] < mk["SUB_TABLE_ADDR"]
            < mk["SUB_PROG_ADDR"] < mk["SUB_CODE_END"] <= mk["SUB_STACK_ADDR"]
            < mk["SUB_STACK_TOP"] <= SUB_RAM_END):
        die(f"SUB_* の並び順が壊れている (${SUB_RAM_START:04X} <= WORK < TABLE "
            "< PROG < CODE_END <= STACK_ADDR < STACK_TOP <= "
            f"${SUB_RAM_END:04X})")
    # 各領域の大きさが次の領域の先頭に収まるか
    if mk["SUB_WORK_ADDR"] + SUB_WORK_SIZE > mk["SUB_TABLE_ADDR"]:
        die(f"作業変数 {SUB_WORK_SIZE} byte (${mk['SUB_WORK_ADDR']:04X}-) が "
            f"消し込み表の先頭 ${mk['SUB_TABLE_ADDR']:04X} に掛かる")
    if mk["SUB_TABLE_ADDR"] + SUB_TABLE_SIZE > mk["SUB_PROG_ADDR"]:
        die(f"消し込み表 {SUB_TABLE_SIZE} byte (${mk['SUB_TABLE_ADDR']:04X}-) が "
            f"コードの先頭 ${mk['SUB_PROG_ADDR']:04X} に掛かる")
    if mk["SUB_STACK_TOP"] - mk["SUB_STACK_ADDR"] < 32:
        die("自前スタックの領域が 32 byte 未満 (NMI のフレーム 12 byte + 入れ子の"
            " pshs / lbsr を受けられない)")

    # 2. subprog.bin のサイズ検査
    if not binpath.is_file():
        die(f"{binpath} が無い (通常のファイルでない)")
    size = binpath.stat().st_size
    if size <= 0:
        die(f"{binpath} が空")
    top = mk["SUB_PROG_ADDR"] + size
    room = mk["SUB_CODE_END"] - mk["SUB_PROG_ADDR"]
    if top > mk["SUB_CODE_END"]:
        die(f"subprog.bin が {size} byte あり、"
            f"${mk['SUB_PROG_ADDR']:04X}+{size} = ${top:04X} が "
            f"SUB_CODE_END ${mk['SUB_CODE_END']:04X} を超える "
            f"(= サブ CPU のスタックを踏む)。上限 {room} byte。")

    print(f"check_layout: OK — subprog.bin {size} byte "
          f"(${mk['SUB_PROG_ADDR']:04X}-${top - 1:04X}, "
          f"残り {mk['SUB_CODE_END'] - top} byte)")


if __name__ == "__main__":
    main()
