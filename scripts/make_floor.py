#!/usr/bin/env python3
"""床 (奥行き方向に流れる横の縞) のための表を 2 つ焼く。

  build/floor_data.s   … サブ CPU が貼り付ける 6 byte の模様表 (lwasm 用)
  build/floor_table.h  … main 側が「どの行をどちらの模様で塗るか」を決めるための表 (C 用)

両方を 1 つの script が吐くのは、**模様の番号 (index) が 2 つのファイルで
食い違うと、絵が静かに化ける**からである。番号の付け方をここ 1 箇所に
閉じ込め、asm と C は生成物を読むだけにする。

------------------------------------------------------------
なぜ 6 byte 単位なのか
------------------------------------------------------------
サブ側の塗りは PSHU (D/X/Y = 6 byte を 1 命令で書き出す) で行う。描画フレームは
48 byte 幅 = 6 の倍数なので、6 byte の模様を 8 回押せば端数無しに 1 行が
埋まる。模様は横一様 (6 byte とも同じ値) の 2 つだけで、暗い方 (黒) と
明るい方 ($AA の中間調) を奥行きの縞として交互に塗る。

------------------------------------------------------------
遠近との対応
------------------------------------------------------------
地平線から d line 下の床の奥行きは z = ZK / d で、縞の明暗は z_lo[d] (= z の
下位 8 bit) にスクロールの位相 zoff を足した値の bit7 で決める (c_main.c。
zoff を毎フレーム進めると帯が手前へ流れる)。帯 1 本の厚みは画面上で
およそ 128 * d*d / ZK line (d = ZK / z の変化率から求めた近似) で、d が
小さいほど薄い。そこで **厚みが 2 line を切る側 (d < FLR_D_ALIAS) は縞を
描かず黒のまま残す** (2 line を切ると明滅して見えるため)。
"""

import sys

ZK = 30000          # 奥行きの尺度 (小さいほど帯が厚く、黒い遠景が狭くなる)
LINES = 96          # 描画フレームの高さ (line) = z_lo[] の長さ
ROW_BYTES = 6       # PSHU 1 回で書ける byte 数 (= 模様の横周期の上限)


def build_table():
    """entries を返す。entries[i] = 6 byte の模様。
    0 = 暗い帯 (黒)、1 = 明るい帯 ($AA の中間調)。"""
    # 床は 1 行おきに間引いて塗るので、明るい方は $FF (べた白) ではなく
    # $AA (1 dot おきの網) にして、間引いた黒い行との対比を弱める。
    return [[0x00] * ROW_BYTES, [0xAA] * ROW_BYTES]


def d_alias():
    """奥行きの帯の厚み (上の近似式) が 2 line を切る境目 d を返す。"""
    for d in range(1, LINES):
        if 128 * d * d / ZK >= 2.0:
            return d
    return LINES


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_floor.py <out.s> <out.h>")
    s_path, h_path = sys.argv[1], sys.argv[2]

    entries = build_table()
    da = d_alias()

    # ---- サブ側の模様表 (.s) ----
    L = []
    L.append("* ============================================================")
    L.append("* floor_data.s — scripts/make_floor.py が生成 (手で直さない)")
    L.append("*")
    L.append(f"*   床の模様 {ROW_BYTES} byte x {len(entries)} 通り (横一様の無地のみ)。")
    L.append("*   0 = 黒 (暗い帯) / 1 = $AA の網 (明るい帯)")
    L.append("*   サブ側は番号から 6 byte を引いて D/X/Y に載せ、PSHU で貼る。")
    L.append("* ============================================================")
    L.append("")
    L.append("flr_pat:")
    for i, e in enumerate(entries):
        vals = ",".join(f"${v:02X}" for v in e)
        L.append(f"                fcb     {vals}      * {i}")
    with open(s_path, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")

    # ---- main 側の表 (.h) ----
    z = [0] + [(ZK // d) & 0xFF for d in range(1, LINES)]

    H = []
    H.append("/* ============================================================")
    H.append(" * floor_table.h — scripts/make_floor.py が生成 (手で直さない)")
    H.append(" *")
    H.append(f" *   z_lo[d]   = (ZK / d) & 0xFF   (ZK = {ZK})")
    H.append(" *               床の奥行き。帯の明暗は (z_lo[d] + zoff) の bit7。")
    H.append(" *   FLR_D_ALIAS = 奥行きの帯が 2 line を切る境目 (これより地平線に")
    H.append(" *                 近い側は縞を描かず黒のまま残す)")
    H.append(" *   FLR_LINES   = z_lo[] の長さ (描画フレームの高さ。c_main.c の")
    H.append(" *                 FRAME_H_LINES と同じ値)")
    H.append(" * ============================================================ */")
    H.append("")
    H.append("#ifndef FLOOR_TABLE_H")
    H.append("#define FLOOR_TABLE_H")
    H.append("")
    H.append(f"#define FLR_SOLID_DARK  0      /* 暗い帯 (黒) */")
    H.append(f"#define FLR_SOLID_LIGHT 1      /* 明るい帯 ($AA の中間調) */")
    H.append(f"#define FLR_D_ALIAS   {da}")
    H.append(f"#define FLR_LINES     {LINES}")
    H.append("")
    H.append("static const unsigned char z_lo[FLR_LINES] = {")
    for i in range(0, LINES, 12):
        H.append("    " + ", ".join(f"0x{v:02X}" for v in z[i:i + 12]) + ",")
    H.append("};")
    H.append("")
    H.append("#endif")
    with open(h_path, "w", encoding="utf-8") as f:
        f.write("\n".join(H) + "\n")

    print(f"make_floor.py: {s_path} ({len(entries)} 通り x {ROW_BYTES} byte), "
          f"{h_path} (FLR_D_ALIAS={da})")


if __name__ == "__main__":
    main()
