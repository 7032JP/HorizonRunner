#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# このスクリプトは MIT ライセンスのツール D77TOT77WAV (Copyright (c) 2026
# Naomitsu.Tsugiiwa) を本作に取り込んだものです。ライセンス全文は
# 同ディレクトリの D77TOT77WAV.LICENSE.txt を参照。
"""
D77 → T77 変換 (FM-7 F-BASIC 用。機械語ファイル 1 本の形)。

処理の流れ:
    D77 → セクタを IPL が読む順に連結 → 本体 (IPL のセクタに記録された
          セクタ数ぶん)
        → 機械語ファイル 1 本 = [本体][ステージャ]。$1400 に読み込まれる
        → テープの先頭に BASIC のローダー (ASCII 保存)、続けて機械語
          ファイル。T77 のテープイメージと WAV と手順のテキストを書き出す

メモリの割り当て:
    CLEAR ,&H13FF で $1400-$7FFF を空ける。
    $1400-....        本体 (N byte。エントリアドレスに置かれる時と同じ並び)
    ....-....         ステージャ (位置に依存しない。読み込まれた場所で動く)
    末尾は $7FFF 以下でなければならない。

読み込みの仕組み
----------------
F-BASIC が LOADM "CAS0:",,R で機械語ファイルを読み、本体の直後にある
ステージャへ飛ぶ。ステージャは割り込みをマスクし、ROM のオーバレイを
外し ($8000-$FBFF が RAM になる。本作はこの状態で動く)、本体を $1400 から
エントリアドレスへ写し、スタックを張ってエントリへ飛ぶ。コピー先は
コピー元より低いアドレスで、処理はアドレスの昇順 (前方コピー) なので、
コピー先の範囲はステージャより下で終わり、動いているステージャを
書き潰さない。

成立の条件は 2 つ: 本体とステージャが $1400-$7FFF に収まること、
エントリアドレスが $1400 より低いこと (前方コピーの前提)。満たさない時は
読み込めないテープを作らず、理由を出して変換を止める。

書き出す前に、計画の全体を 1 byte ずつ再生して確かめる (simulate_plan)。
組み立てたステージャのバイト列を命令に分解し、転送元・転送先・終端・
ジャンプ先をその命令のオペランドから読み取り (このスクリプトの定数からで
はなく)、LOADM の書き込みとコピーを 64 KiB のメモリ像の上で再生し、結果が
元の本体と一致してエントリアドレスに置かれていることを確かめる。読み取った
オペランドはこのスクリプトが埋めるつもりだった値とも突き合わせるので、
ひな形とこのスクリプトの食い違いは、違う場所へ読み込まれるテープが
黙って出来上がる代わりにエラーとして報告される。

ステージャのひな形は scripts/trampoline.asm 1 つで (Makefile が組み立てる)、
本体の長さとエントリアドレスを埋めて使う。

使い方 (リポジトリの直下で実行する。Makefile が渡す形):
    python3 scripts/d77_to_t77_chunks.py build/game.d77 --addr 0x0400 \\
        --tramp build/trampoline.bin \\
        -o build/game.t77 -w build/game.wav -t build/game.cmt.txt
引数の一覧は --help で出る。
"""

import argparse
import math
import os
import struct
import sys


# ===== FM-7 のメモリの割り当て =====
#
# テープ側のメモリの割り当てはここの値と scripts/trampoline.asm の定数で
# 決まる (config.mk には無い)。対応は次のとおりで、値を変える時は両方を
# 同じ値に揃えること。食い違いは simulate_plan が検出して変換を止める。
#   STAGER_LOAD_ADDR (転送元の先頭) = trampoline.asm の STAGER_LOAD
#   ROM_PORT / ROM_PORT_VALUE       = trampoline.asm の ROM_PORT / LDA #$00
#   IRQ_MASK                        = trampoline.asm の ORCC #$50
#   SENTINEL_*                      = trampoline.asm の PLACEHOLDER_*

CLEAR_VALUE       = 0x13FF
STAGER_LOAD_ADDR  = 0x1400     # LOADM ブロックの先頭 (= 本体の仮置き先)
RAM_TOP           = 0x8000     # ROM モードで RAM として使える上限 (これ未満)
SECTOR_SIZE       = 256
ROM_PORT          = 0xFD0F     # ここへ書くと ROM のオーバレイが外れる (trampoline.asm の ROM_PORT)
ROM_PORT_VALUE    = 0x00       # ステージャが書く値 (trampoline.asm の LDA #$00)
IRQ_MASK          = 0x50       # ORCC # の即値 (I と F の両方をマスク)

# ステージャのひな形に埋めてあるパッチ用のセンチネル
# (= scripts/trampoline.asm の PLACEHOLDER_* と一致させる)
SENTINEL_END      = b'\xDE\xAD'    # 転送元の終端 (1 箇所)
SENTINEL_ENTRY    = b'\xBE\xEF'    # エントリアドレス (2 箇所)

# ステージャが張るスタック (trampoline.asm の ENTRY_STACK) の検査に使う値。
# STACK_RAM_END はスタックとして許す範囲の上限 (これ未満)。メイン側の RAM は
# $FC7F まで ($FC80 から共有 RAM、$FD00 から I/O) だが、検査では余裕を見て
# $FC00 を上限にする。STACK_RESERVE はスタックの先頭から下へ確保しておく量
# (本体は crt0 で自分のスタックを張り直すので、ステージャの JMP までに使う量は
# 無いが、余裕を持たせる)。
STACK_RAM_END     = 0xFC00
STACK_RESERVE     = 32

# IPL のセクタ (C0 H0 R1、256 byte) の先頭 16 byte はメタデータで、BRA $0E
# (16 byte を跨ぐ) に続いて +2 に本体のセクタ数が 1 byte、+3 から +15 までは
# 0 で埋めてある (src/asm_ipl.s の meta。セクタ数は scripts/bin2d77.py が
# 書き込む)。ディスクの IPL はこの数だけセクタを読むので、テープに載せる
# 本体の長さもここから決める。この並びに合わない IPL からは長さを読まない
# (--size で指定する)。
IPL_BRA_OPCODE    = 0x20
IPL_BRA_OPERAND   = 0x0E
IPL_BODY_SECTORS  = 0x02
IPL_META_LEN      = 0x10

# IPL が本体のセクタを読む順 (src/asm_ipl.s の一般式。通し番号 0 が IPL):
#   track = i / 32、side = (i / 16) % 2、sector = i % 16 + 1
SECTORS_PER_TRACK = 16


class CapacityError(RuntimeError):
    """本体とステージャが $1400-$7FFF に収まらない (= 本体を減らす以外に
    手が無い) 失敗。他の RuntimeError とは案内を分ける。"""


# ===== D77 の読み取り =====

def _u16le(data, pos, what):
    if pos + 2 > len(data):
        raise RuntimeError(f'D77 が {what} の途中で終わっている (オフセット {pos})')
    return struct.unpack('<H', data[pos:pos + 2])[0]


def extract_d77_sectors(d77_data):
    """D77 のトラックの表を辿り、セクタを CHR (シリンダ、ヘッド、レコード)
    の順に並べたリスト [(c, h, r, データ), ...] を返す。セクタは全部残し、
    どこまでが本体かは呼び手が決める (IPL に記録されたセクタ数か --size)。
    ヘッダやセクタの途中でファイルが終わっていたら失敗にする (短い入力を
    正常な抽出として返さない)。"""
    if len(d77_data) < 0x20 + 164 * 4:
        raise RuntimeError(
            f'D77 のヘッダ ({0x20 + 164 * 4} byte) より短い ({len(d77_data)} byte)')
    sectors = []
    for ti in range(164):
        off = struct.unpack('<I', d77_data[0x20 + ti * 4:0x24 + ti * 4])[0]
        if off == 0:
            continue
        if off >= len(d77_data):
            raise RuntimeError(
                f'D77 のトラック {ti} のオフセット ${off:X} がファイルの外')
        sec_count = _u16le(d77_data, off + 4, f'トラック {ti} のヘッダ')
        pos = off
        for si in range(sec_count):
            if pos + 16 > len(d77_data):
                raise RuntimeError(
                    f'D77 がトラック {ti} のセクタ {si} のヘッダの途中で終わっている')
            c = d77_data[pos]
            h = d77_data[pos + 1]
            r = d77_data[pos + 2]
            sz = _u16le(d77_data, pos + 14, f'トラック {ti} のセクタ {si} のヘッダ')
            if pos + 16 + sz > len(d77_data):
                raise RuntimeError(
                    f'D77 がトラック {ti} のセクタ {si} (C{c} H{h} R{r}、'
                    f'{sz} byte) の本文の途中で終わっている')
            sectors.append((c, h, r, d77_data[pos + 16:pos + 16 + sz]))
            pos += 16 + sz
    sectors.sort(key=lambda s: (s[0], s[1], s[2]))
    return sectors


def check_sector_order(sectors, nbytes):
    """先頭から nbytes byte ぶんのセクタが、IPL が読む順 (通し番号 0 が
    C0 H0 R1、以後 16 セクタごとにヘッド、32 セクタごとにシリンダが進む)
    に欠けも重複も無く並び、どれも SECTOR_SIZE byte であることを確かめる。
    欠けたトラックの後ろのセクタを詰めて連結した本体を通さないため。"""
    need = (nbytes + SECTOR_SIZE - 1) // SECTOR_SIZE
    if need > len(sectors):
        raise RuntimeError(
            f'必要な {need} セクタに対し D77 には {len(sectors)} セクタしか無い')
    for i in range(need):
        want = (i // (SECTORS_PER_TRACK * 2), (i // SECTORS_PER_TRACK) % 2,
                i % SECTORS_PER_TRACK + 1)
        c, h, r, data = sectors[i]
        if (c, h, r) != want:
            raise RuntimeError(
                f'セクタの並びが IPL の読む順と違う: {i} 番目が C{c} H{h} R{r} '
                f'(C{want[0]} H{want[1]} R{want[2]} のはず。欠けか重複がある)')
        if len(data) != SECTOR_SIZE:
            raise RuntimeError(
                f'セクタ C{c} H{h} R{r} が {len(data)} byte ({SECTOR_SIZE} byte のはず)')


def body_len_from_ipl(payload):
    """IPL のセクタに記録された本体のセクタ数から、本体の長さ (byte) を返す。

    内容から長さを推測しない。末尾がゼロで初期化されたデータで終わる本体
    でも、IPL が読む範囲をそのままテープに載せる。
    """
    if len(payload) < SECTOR_SIZE:
        raise RuntimeError(
            f'D77 に IPL のセクタ ({SECTOR_SIZE} byte) が無い '
            f'(全体 {len(payload)} byte)')
    ipl = payload[:SECTOR_SIZE]
    if (ipl[0] != IPL_BRA_OPCODE or ipl[1] != IPL_BRA_OPERAND
            or any(ipl[IPL_BODY_SECTORS + 1:IPL_META_LEN])):
        raise RuntimeError(
            f'IPL のセクタの先頭が本作のメタデータの並び (BRA ${IPL_BRA_OPERAND:02X}、'
            f'セクタ数、0 x {IPL_META_LEN - IPL_BODY_SECTORS - 1}) でない '
            f'(先頭 {ipl[:4].hex(" ").upper()})。本体のセクタ数を読み取れないので、'
            f'--size で本体の長さを指定する')
    n = ipl[IPL_BODY_SECTORS]
    if n == 0:
        raise RuntimeError('IPL に記録された本体のセクタ数が 0')
    body_len = n * SECTOR_SIZE
    have = len(payload) - SECTOR_SIZE
    if body_len > have:
        raise RuntimeError(
            f'IPL に記録された本体 {n} セクタ ({body_len} byte) が '
            f'D77 の中身 ({have} byte) より長い')
    return body_len


# ===== ステージャのひな形の読み込みとパッチ =====

def _u16_be(v):
    return bytes([(v >> 8) & 0xFF, v & 0xFF])


def _patch_all(buf, sentinel, value, expect):
    """`sentinel` (2 byte) の出現を全部 `value` (16 bit ビッグエンディアン) に
    置き換える。出現回数が `expect` と違えば失敗する (= ひな形と
    このスクリプトの対応が崩れた印)。"""
    if not (0 <= value <= 0xFFFF):
        raise RuntimeError(f'埋める値 ${value:X} が 16 bit に収まらない')
    n = 0
    i = buf.find(sentinel)
    while i >= 0:
        buf[i:i + 2] = _u16_be(value)
        n += 1
        i = buf.find(sentinel, i + 2)
    if n != expect:
        raise RuntimeError(
            f'センチネル {sentinel.hex().upper()} がひな形に {n} 回ある '
            f'({expect} 回のはず)')


def build_stager(template, body_len, entry):
    """ひな形に本体の長さとエントリを埋めて、実行できるステージャに
    する。返すのはパッチ済みのバイト列 (本体の直後に置く)。"""
    if len(template) < 8:
        raise RuntimeError('ステージャのひな形が短すぎる')
    buf = bytearray(template)
    _patch_all(buf, SENTINEL_END, STAGER_LOAD_ADDR + body_len, expect=1)
    _patch_all(buf, SENTINEL_ENTRY, entry, expect=2)
    return bytes(buf)


def check_capacity(body_len, stager_len):
    """本体とステージャが STAGER_LOAD_ADDR から RAM_TOP の手前までに収まるか。
    ステージャにアドレスを埋める前に呼び、収まらない時は容量超過として
    案内する (埋める値の 16 bit の検査に先に掛かって別のエラーにならない
    ように)。"""
    end = STAGER_LOAD_ADDR + body_len + stager_len
    if end > RAM_TOP:
        raise CapacityError(
            f'本体 {body_len} byte + ステージャ {stager_len} byte が '
            f'${STAGER_LOAD_ADDR:04X}-${RAM_TOP - 1:04X} に収まらない '
            f'(末尾 ${end:04X}、超過 {end - RAM_TOP} byte)')


def wrap_block(body, stager):
    """[本体][ステージャ] を 1 つの LOADM ブロックにする。ブロックは
    STAGER_LOAD_ADDR から連続して置かれる (収まりは check_capacity と
    simulate_plan で検査する)。"""
    return bytes(body) + bytes(stager)


# ===== LOADM のブロックと T77 の符号化 =====

def make_loadm_payload(block_addr, block, exec_addr):
    out = bytearray()
    out += struct.pack('>BHH', 0x00, len(block), block_addr)
    out += block
    out += struct.pack('>BHH', 0xFF, 0x0000, exec_addr)
    return bytes(out)


# T77 の時間の単位。1 tick = 16 / 1.794 MHz ≈ 8.92 us (T77 フォーマットが
# 決めている換算で、FM-7 の CPU クロックではない)。
T77_TICK_BASE_HZ  = 1_794_000
TICK_SCALE_CY     = 16

# テープの FSK の半サイクルの長さ (tick)。MARK_HALF = 50 で 1 の 1 周期が
# 100 tick ≈ 892 us (約 1120 Hz)、SPACE_HALF = $1A で 0 の半サイクルが
# 26 tick ≈ 232 us。
MARK_HALF = 50
SPACE_HALF = 0x1A
POL = 0x8000
LEADER_BYTES = 256
GAP_LEADER_BYTES = 40
DATA_PAYLOAD_SIZE = 255
WAV_SILENCE_MARKER = 0x0000


def _uart_bits(b):
    bits = [0]
    for i in range(8):
        bits.append((b >> i) & 1)
    bits.extend([1, 1])
    return bits


def _bits_to_halfcycles(bits):
    out = []
    for b in bits:
        dur = MARK_HALF if b else SPACE_HALF
        out.append(dur | POL)
        out.append(dur)
    return out


def _encode_bytes(bs):
    bits = []
    for b in bs:
        bits.extend(_uart_bits(b))
    return _bits_to_halfcycles(bits)


def _leader(n):
    return _encode_bytes(bytes([0xFF] * n))


def _chksum(b):
    return sum(b) & 0xFF


def _tape_header_block(name, attr=0x02):
    sync = bytearray([0x01, 0x3C])
    content = bytearray([0x00, 0x14])
    fn = name.upper()[:8].encode('ascii').ljust(8, b' ')
    content += fn
    content.append(attr)
    content += bytes(11)
    content.append(_chksum(content))
    return bytes(sync + content)


def _tape_data_block(payload):
    sync = bytearray([0x01, 0x3C])
    content = bytearray([0x01, 0xFF])
    chunk = bytearray(payload)
    if len(chunk) < DATA_PAYLOAD_SIZE:
        chunk += bytes(DATA_PAYLOAD_SIZE - len(chunk))
    content += chunk
    content.append(_chksum(content))
    return bytes(sync + content)


def _tape_end_block():
    sync = bytearray([0x01, 0x3C])
    content = bytearray([0xFF])
    content.append(_chksum(content))
    return bytes(sync + content)


def _build_one_tape_file(loadm_bytes, name):
    hc = []
    hc += _leader(LEADER_BYTES)
    hc += _encode_bytes(_tape_header_block(name, attr=0x02))
    pos = 0
    while pos < len(loadm_bytes):
        chunk = loadm_bytes[pos:pos + DATA_PAYLOAD_SIZE]
        hc += _leader(GAP_LEADER_BYTES)
        hc += _encode_bytes(_tape_data_block(chunk))
        pos += DATA_PAYLOAD_SIZE
    hc += _leader(GAP_LEADER_BYTES)
    hc += _encode_bytes(_tape_end_block())
    return hc


# ---------------------------------------------------------------------------
# ASCII 保存の BASIC ファイル (= テープの先頭に置くローダー)
#
# F-BASIC が SAVE "CAS0:<名前>",A で書き出す形に合わせる。同期バイト
# (01 3C) を除いたブロックの 2 byte 目は **実長**で、チェックサムは
# 識別バイト + 長さバイト + 本文 の総和 & 0xFF。
#
#   ヘッダ : 01 3C | 00 14 | 名前 8 | 属性 00 | FF FF | 00 x9 | チェックサム
#   データ : 01 3C | 01 <実長> | 本文 | チェックサム
#   終端   : 01 3C | FF 00 | チェックサム
#
# 本文は 先頭に CR、各行の後ろにも CR を置く (末尾に $1A は付けない)。
# ブロック間のリーダは 256 byte 取る (機械語ファイルの 40 byte より長い)。
# ---------------------------------------------------------------------------
BASIC_ATTR       = 0x00     # 属性: BASIC プログラム
BASIC_ASCII_FLAG = 0xFF     # ASCII 保存の識別
BASIC_GAP_LEADER = 256      # ブロック間リーダ
LOADER_NAME      = 'LOADER' # テープ先頭の BASIC ローダーのファイル名
LOADM_NAME       = 'C01'    # 本体の機械語ファイルの名前 (8 字以内の任意の名前。
                            # 手順のテキストと BASIC ローダーが同じ名前を使う)

def _basic_header_block(name):
    content = bytearray([0x00, 0x14])
    content += name.upper()[:8].encode('ascii').ljust(8, b' ')
    content += bytes([BASIC_ATTR, BASIC_ASCII_FLAG, BASIC_ASCII_FLAG]) + bytes(9)
    content.append(_chksum(content))
    return bytes(bytearray([0x01, 0x3C]) + content)


def _basic_data_block(payload):
    content = bytearray([0x01, len(payload)]) + bytearray(payload)
    content.append(_chksum(content))
    return bytes(bytearray([0x01, 0x3C]) + content)


def _basic_end_block():
    content = bytearray([0xFF, 0x00])
    content.append(_chksum(content))
    return bytes(bytearray([0x01, 0x3C]) + content)



def _file_seconds(loadm_bytes, name):
    """LOADM ファイル 1 本をテープに流すのに掛かる時間 (= 波形そのものの長さ)。"""
    # 半サイクルの上位 bit は極性フラグなので、長さだけを取り出して足す。
    return (sum(h & ~POL for h in _build_one_tape_file(loadm_bytes, name))
            * TICK_SCALE_CY / T77_TICK_BASE_HZ)


def basic_loader_lines(load_secs=None):
    """BASIC ローダーの行を組む。手で打つ手順と同じ並びである。
    load_secs は本体の機械語ファイルの波形長 (秒)。案内の分数に使う。"""
    lines = [f'10 CLEAR ,&H{CLEAR_VALUE:04X}']
    no = 20
    # プログラムから呼んだ LOADM は Searching / Found: を出さない。
    # 案内を出しておかないと、読み込みの間じゅう画面が無反応に見える。
    if load_secs:
        lines.append(f'{no} PRINT "LOADING - PLEASE WAIT ABOUT '
                     f'{int(load_secs / 60 + 0.5)} MIN"')
        no += 10
    lines.append(f'{no} LOADM "CAS0:",,R')
    return lines

def _build_one_basic_file(lines, name):
    text = b'\r' + b'\r'.join(l.encode('ascii') for l in lines) + b'\r'
    hc = []
    hc += _leader(BASIC_GAP_LEADER)
    hc += _encode_bytes(_basic_header_block(name))
    pos = 0
    while pos < len(text):
        hc += _leader(BASIC_GAP_LEADER)
        hc += _encode_bytes(_basic_data_block(text[pos:pos + DATA_PAYLOAD_SIZE]))
        pos += DATA_PAYLOAD_SIZE
    hc += _leader(BASIC_GAP_LEADER)
    hc += _encode_bytes(_basic_end_block())
    return hc


def _build_file(entry):
    """entry = (name, kind, payload)。kind は 'loadm' か 'basic'。"""
    name, kind, payload = entry
    if kind == 'basic':
        return _build_one_basic_file(payload, name)
    return _build_one_tape_file(payload, name)


def build_t77(files, mark_inter_file_silence=True):
    hc = []
    for i, entry in enumerate(files):
        if i > 0 and mark_inter_file_silence:
            hc.append(WAV_SILENCE_MARKER)
        hc += _build_file(entry)
        hc += _leader(64)
    hc += _leader(32)
    # T77 形式の識別子 (先頭 18 byte、固定)。
    out = bytearray(bytes.fromhex(
        '58 4D 37 20 54 41 50 45 20 49 4D 41 47 45 20 30 00 00'.replace(' ', '')))
    for v in hc:
        out += struct.pack('>H', v)
    return bytes(out)


# ===== WAV の合成 (44.1 kHz / 16 bit 符号付き / モノラル) =====
#
# T77 の半サイクルの項目は、長さ (tick。上の T77_TICK_BASE_HZ の換算) と
# bit 15 の極性フラグを持つ。これを 16 bit 符号付き PCM にする: 無音 = 0、
# 極性が高い半サイクル = +振幅、低い半サイクル = -振幅。半サイクルの長さは
# サンプル数の整数倍でないので、端数を累積して次の半サイクルへ繰り越す。
#
# 極性の切り替わりは -振幅から +振幅への瞬時の段差ではなく、短い余弦の
# 傾斜で書く (切り替わりごとに中間の振幅のサンプルが数個入り、波形に
# 急峻な角が無くなる)。

WAV_SAMPLE_RATE   = 44100
WAV_AMPLITUDE     = 24000      # フルスケールの約 73 % (余裕を残す)
WAV_SILENCE_LVL   = 0
WAV_HIGH_LVL      =  WAV_AMPLITUDE
WAV_LOW_LVL       = -WAV_AMPLITUDE
WAV_RAMP_SAMPLES  = 3          # 余弦の傾斜の幅 (サンプル数)

WAV_SAMPLES_PER_TICK = WAV_SAMPLE_RATE * TICK_SCALE_CY / T77_TICK_BASE_HZ


def _cos_ramp(src, dst, n):
    """`src` から `dst` へ余弦の傾斜で向かう n 個のサンプルを返す。n = 3 なら
    真ん中のサンプルが中点 (-振幅から +振幅の振れなら直流の中心) に来て、
    その前後に部分振幅のサンプルが 1 つずつ付く。"""
    out = []
    for k in range(n):
        t = (k + 1) / (n + 1)
        shape = (1 - math.cos(math.pi * t)) / 2
        out.append(int(round(src + (dst - src) * shape)))
    return out


_RAMP_S_TO_H = struct.pack(f'<{WAV_RAMP_SAMPLES}h',
                           *_cos_ramp(WAV_SILENCE_LVL, WAV_HIGH_LVL, WAV_RAMP_SAMPLES))
_RAMP_S_TO_L = struct.pack(f'<{WAV_RAMP_SAMPLES}h',
                           *_cos_ramp(WAV_SILENCE_LVL, WAV_LOW_LVL,  WAV_RAMP_SAMPLES))
_RAMP_H_TO_L = struct.pack(f'<{WAV_RAMP_SAMPLES}h',
                           *_cos_ramp(WAV_HIGH_LVL,    WAV_LOW_LVL,  WAV_RAMP_SAMPLES))
_RAMP_L_TO_H = struct.pack(f'<{WAV_RAMP_SAMPLES}h',
                           *_cos_ramp(WAV_LOW_LVL,     WAV_HIGH_LVL, WAV_RAMP_SAMPLES))
_BYTES_HIGH  = struct.pack('<h', WAV_HIGH_LVL)
_BYTES_LOW   = struct.pack('<h', WAV_LOW_LVL)
_BYTES_SIL   = struct.pack('<h', WAV_SILENCE_LVL)


def _i16le_run(value_bytes, count):
    """2 byte のサンプル 1 個を `count` 回繰り返す。"""
    return value_bytes * count


def _silence_samples(seconds):
    n = int(round(seconds * WAV_SAMPLE_RATE))
    return _i16le_run(_BYTES_SIL, n)


def _halfcycles_to_samples(hc):
    """T77 の半サイクルの項目の並びを 16 bit 符号付き PCM にする。極性の
    切り替わりごとに短い余弦の傾斜を入れ、矩形の角を丸める。"""
    out = bytearray()
    acc = 0.0
    prev_pol = None        # None = 無音から始まる
    for entry in hc:
        dur = entry & 0x7FFF
        if dur == 0:
            continue                          # 無音の目印は読み飛ばす
        pol = (entry & 0x8000) != 0
        acc += dur * WAV_SAMPLES_PER_TICK
        n = int(acc)
        acc -= n
        if n <= 0:
            continue
        if prev_pol is None:
            ramp = _RAMP_S_TO_H if pol else _RAMP_S_TO_L
        elif prev_pol == pol:
            ramp = b''
        else:
            ramp = _RAMP_L_TO_H if pol else _RAMP_H_TO_L
        body_bytes = _BYTES_HIGH if pol else _BYTES_LOW
        ramp_n = len(ramp) // 2
        if ramp_n and n > ramp_n:
            out += ramp
            out += _i16le_run(body_bytes, n - ramp_n)
        else:
            out += _i16le_run(body_bytes, n)
        prev_pol = pol
    return bytes(out)


def _wrap_wav(samples):
    """生の PCM を RIFF/WAVE の入れ物に包む。"""
    n = len(samples)
    bits_per_sample = 16
    channels = 1
    byte_rate = WAV_SAMPLE_RATE * channels * bits_per_sample // 8
    block_align = channels * bits_per_sample // 8
    fmt = struct.pack('<IHHIIHH',
                      16,                # fmt チャンクの長さ
                      1,                 # PCM
                      channels,
                      WAV_SAMPLE_RATE,
                      byte_rate,
                      block_align,
                      bits_per_sample)
    return (b'RIFF' + struct.pack('<I', 36 + n) + b'WAVE'
            + b'fmt ' + fmt
            + b'data' + struct.pack('<I', n) + samples)


def build_wav(files, head_silence=5.0, gap_silence=5.0, tail_silence=5.0):
    """テープに載せる各ファイル (BASIC ローダーと機械語ファイルの両方) の
    波形を PCM にし、先頭・ファイル間・末尾に直流中心の無音を挟む。"""
    samples = bytearray()
    samples += _silence_samples(head_silence)
    for i, entry in enumerate(files):
        if i > 0:
            samples += _silence_samples(gap_silence)
        samples += _halfcycles_to_samples(_build_file(entry))
    samples += _silence_samples(tail_silence)
    return _wrap_wav(bytes(samples))


# ===== Decoding the assembled stager =====
#
# simulate_plan はこのスクリプトの定数ではなく、組み立てたステージャの
# バイト列を命令に分解し (disassemble_stager)、並びを検査し (decode_stager。
# 検査の条件はその docstring)、その命令を順に実行して転送を再現する
# (run_stager)。分解は scripts/trampoline.asm に現れる命令だけを知る小さな
# 逆アセンブラで行い、知らない命令が現れたら失敗にする。

def _be16(b, i):
    return (b[i] << 8) | b[i + 1]


def disassemble_stager(code):
    """ステージャのバイト列を先頭から JMP まで命令に分解し、
    (オフセット, ニーモニック, オペランド) のリストを返す。JMP で終わる。"""
    ins = []
    i = 0
    n = len(code)

    def need(k, what):
        if i + k > n:
            raise RuntimeError(f'ステージャが {what} の途中で終わっている '
                               f'(オフセット {i})')

    while i < n:
        op = code[i]
        if op == 0x1A:                                          # ORCC #imm
            need(2, 'ORCC #'); ins.append((i, 'ORCC#', code[i + 1])); i += 2
        elif op == 0x86:                                        # LDA #imm
            need(2, 'LDA #'); ins.append((i, 'LDA#', code[i + 1])); i += 2
        elif op == 0xB7:                                        # STA ext
            need(3, 'STA ext'); ins.append((i, 'STAext', _be16(code, i + 1))); i += 3
        elif op == 0x8E:                                        # LDX #imm16
            need(3, 'LDX #'); ins.append((i, 'LDX#', _be16(code, i + 1))); i += 3
        elif op == 0x10 and i + 1 < n and code[i + 1] == 0x8E:  # LDY #imm16
            need(4, 'LDY #'); ins.append((i, 'LDY#', _be16(code, i + 2))); i += 4
        elif op == 0x10 and i + 1 < n and code[i + 1] == 0xCE:  # LDS #imm16
            need(4, 'LDS #'); ins.append((i, 'LDS#', _be16(code, i + 2))); i += 4
        elif op == 0xA6 and i + 1 < n and code[i + 1] == 0x80:  # LDA ,X+
            ins.append((i, 'LDA,X+', None)); i += 2
        elif op == 0xA7 and i + 1 < n and code[i + 1] == 0xA0:  # STA ,Y+
            ins.append((i, 'STA,Y+', None)); i += 2
        elif op == 0x8C:                                        # CMPX #imm16
            need(3, 'CMPX #'); ins.append((i, 'CMPX#', _be16(code, i + 1))); i += 3
        elif op == 0x26:                                        # BNE rel8
            need(2, 'BNE')
            rel = code[i + 1] - 256 if code[i + 1] >= 128 else code[i + 1]
            ins.append((i, 'BNE', i + 2 + rel)); i += 2
        elif op == 0x7E:                                        # JMP ext
            need(3, 'JMP'); ins.append((i, 'JMP', _be16(code, i + 1))); i += 3
            if i != n:
                raise RuntimeError(
                    f'ステージャの JMP の後ろに {n - i} byte 余分なバイトがある '
                    f'(JMP で終わるはず)')
            return ins
        else:
            raise RuntimeError(
                f'ステージャのオフセット {i} の命令 ${op:02X} を解釈できない '
                f'(trampoline.asm とこのスクリプトの対応が崩れている)')
    raise RuntimeError('ステージャが JMP で終わっていない')


LOOP_SHAPE = ('LDA,X+', 'STA,Y+', 'CMPX#', 'BNE')


def decode_stager(code):
    """ステージャを命令に分解し、並びを検査して次の dict を返す。
        ins  : 命令のリスト (disassemble_stager の返り値)
        src  : LDX # の即値 (転送元の先頭)
        dst  : LDY # の即値 (転送先の先頭)
        end  : CMPX # の即値 (転送元の終端。X がここに達したら止まる)
        jump : JMP の絶対アドレス (本体のエントリ)
        loop : 転送ループの先頭 (LDA ,X+ のオフセット)
    検査: LDX # と LDY # が 1 回ずつループの前にあり、ループは
    LDA ,X+ / STA ,Y+ / CMPX # / BNE が間に何も挟まずこの順に並び、
    BNE の飛び先が LDA ,X+ で、それ以外に LDA ,X+ / STA ,Y+ / CMPX # /
    BNE が無く、JMP が最後に 1 回だけある。さらにループの前に、ORCC #
    の即値が IRQ_MASK ($50 = I と F の両方) であること、ROM 切替の
    STA ext の宛先が ROM_PORT ($FD0F) で直前の LDA # の値が ROM_PORT_VALUE
    であること、ORCC # がその STA ext より前にあることを検査する
    (trampoline.asm の実際の値と順序に合わせる)。"""
    ins = disassemble_stager(code)
    mn = [m for _, m, _ in ins]

    # 転送ループの位置 (LDA ,X+ は 1 回だけ)
    starts = [k for k, m in enumerate(mn) if m == 'LDA,X+']
    if len(starts) != 1:
        raise RuntimeError(
            f'ステージャの転送ループ (LDA ,X+) が {len(starts)} 回ある (1 回のはず)')
    k = starts[0]
    if tuple(mn[k:k + 4]) != LOOP_SHAPE:
        raise RuntimeError(
            'ステージャの転送ループが LDA ,X+ / STA ,Y+ / CMPX # / BNE の並び'
            f'でない (実際: {" / ".join(mn[k:k + 4])})')
    for m in ('STA,Y+', 'CMPX#', 'BNE'):
        if mn.count(m) != 1:
            raise RuntimeError(f'ステージャに {m} が {mn.count(m)} 回ある (1 回のはず)')
    loop_off = ins[k][0]
    bne_target = ins[k + 3][2]
    if bne_target != loop_off:
        raise RuntimeError(
            f'ステージャの BNE の飛び先 (オフセット {bne_target}) が '
            f'転送ループの先頭 ({loop_off}) でない')

    # ループの前に LDX # / LDY # が 1 回ずつ
    got = {}
    for key, m in (('src', 'LDX#'), ('dst', 'LDY#')):
        hits = [j for j, mm in enumerate(mn) if mm == m]
        if len(hits) != 1:
            raise RuntimeError(f'ステージャに {m} が {len(hits)} 回ある (1 回のはず)')
        if hits[0] > k:
            raise RuntimeError(f'ステージャの {m} が転送ループより後にある')
        got[key] = ins[hits[0]][2]
    got['end'] = ins[k + 2][2]

    # ループの前の ORCC # (割り込みのマスク) と LDA # / STA ext (ROM 切替)
    orcc = [j for j, mm in enumerate(mn) if mm == 'ORCC#']
    if len(orcc) != 1 or orcc[0] > k:
        raise RuntimeError('ステージャの転送ループの前に ORCC # が 1 回だけある形でない')
    if ins[orcc[0]][2] != IRQ_MASK:
        raise RuntimeError(
            f'ステージャの ORCC # の即値が ${ins[orcc[0]][2]:02X} で、'
            f'${IRQ_MASK:02X} (IRQ と FIRQ のマスク) でない')
    sta = [j for j, mm in enumerate(mn) if mm == 'STAext']
    if len(sta) != 1 or sta[0] > k:
        raise RuntimeError('ステージャの転送ループの前に STA ext が 1 回だけある形でない')
    if ins[sta[0]][2] != ROM_PORT:
        raise RuntimeError(
            f'ステージャの ROM 切替の書き込み先が ${ins[sta[0]][2]:04X} で、'
            f'${ROM_PORT:04X} でない')
    if sta[0] == 0 or mn[sta[0] - 1] != 'LDA#' or ins[sta[0] - 1][2] != ROM_PORT_VALUE:
        raise RuntimeError(
            f'ステージャの ROM 切替の直前が LDA #${ROM_PORT_VALUE:02X} でない')
    if orcc[0] > sta[0]:
        raise RuntimeError(
            'ステージャの ORCC # (割り込みのマスク) が ROM 切替の STA ext より後にある')
    if mn[-1] != 'JMP' or mn.count('JMP') != 1:
        raise RuntimeError('ステージャの JMP が最後に 1 回だけある形でない')
    got['jump'] = ins[-1][2]
    got['loop'] = loop_off
    got['ins'] = ins
    return got


def run_stager(mem, ins, stager_lo, stager_hi, step_limit):
    """分解した命令を、X / Y / A / S とゼロフラグ、CC の I / F ビット、
    ROM_PORT への書き込みを持つ小さな実行器で順に実行する。
    書き込みが走っているステージャ自身 (stager_lo-stager_hi) に掛かれば
    失敗。ROM_PORT への書き込みはメモリ像には書かず値を控え、その時点で
    割り込み (I と F) がマスク済みでなければ失敗。転送ループの最初の書き込み
    の時点で、割り込みがマスクされ ROM_PORT に書き込み済みでなければ失敗。
    JMP に達したら (飛び先, LDS # で張ったスタックの値, ROM_PORT に書いた値)
    を返す。"""
    by_off = {off: idx for idx, (off, _, _) in enumerate(ins)}
    x = y = a = s = 0
    zf = False
    cc = 0                  # CC の I / F ビット (ORCC # で立つ)
    rom = None              # ROM_PORT に書いた値 (書く前は None)
    pc = 0
    steps = 0
    while True:
        if pc not in by_off:
            raise RuntimeError(f'ステージャの実行がオフセット {pc} (命令の境界でない) へ飛んだ')
        _, m, v = ins[by_off[pc]]
        steps += 1
        if steps > step_limit:
            raise RuntimeError('ステージャの転送ループが止まらない')
        nxt = ins[by_off[pc] + 1][0] if by_off[pc] + 1 < len(ins) else None
        if m == 'ORCC#':
            cc |= v
        elif m == 'LDA#':
            a = v; zf = (a == 0)
        elif m == 'STAext':
            if v == ROM_PORT:
                if (cc & IRQ_MASK) != IRQ_MASK:
                    raise RuntimeError(
                        f'ステージャが IRQ と FIRQ をマスクする前に ${ROM_PORT:04X} へ書いている')
                rom = a
            else:
                mem[v] = a
        elif m == 'LDX#':
            x = v; zf = (x == 0)
        elif m == 'LDY#':
            y = v; zf = (y == 0)
        elif m == 'LDS#':
            s = v
        elif m == 'LDA,X+':
            a = mem[x]; zf = (a == 0); x = (x + 1) & 0xFFFF
        elif m == 'STA,Y+':
            if (cc & IRQ_MASK) != IRQ_MASK:
                raise RuntimeError(
                    'ステージャが IRQ と FIRQ をマスクする前にコピーを始めている')
            if rom is None:
                raise RuntimeError(
                    f'ステージャが ${ROM_PORT:04X} へ書く前にコピーを始めている')
            if stager_lo <= y < stager_hi:
                raise RuntimeError(
                    f'コピー先 ${y:04X} が走っているステージャ '
                    f'(${stager_lo:04X}-${stager_hi - 1:04X}) を踏む')
            mem[y] = a; y = (y + 1) & 0xFFFF
        elif m == 'CMPX#':
            zf = (x == v)
        elif m == 'BNE':
            if not zf:
                pc = v
                continue
        elif m == 'JMP':
            return v, s, rom
        else:
            raise RuntimeError(f'実行器が知らない命令 {m}')
        if nxt is None:
            raise RuntimeError('ステージャが JMP に達しないまま終わった')
        pc = nxt


# ===== Byte-for-byte verification of the plan =====

def simulate_plan(start_addr, body, block):
    """計画を 64 KiB のメモリ像の上で再生し、結果を突合せる。

    LOADM がブロックを $1400 から書き、本体の直後に居るステージャが本体を
    前方コピーでエントリへ運ぶ、という 2 手をそのまま再生する。ステージャ
    はブロックの中の実際のバイト列を命令に分解し (decode_stager。転送
    ループの並びもここで検査する)、その命令を順に実行して (run_stager)
    再現する。読み取った転送元・転送先・終端・ジャンプ先がこのスクリプトの
    意図 (仮置き先、エントリ、本体の長さ) と一致することも確かめる。
    返り値は decode_stager の dict。
    """
    POISON = 0xA5
    mem = bytearray([POISON]) * 0x10000

    if start_addr >= STAGER_LOAD_ADDR:
        raise RuntimeError(
            f'エントリ ${start_addr:04X} が仮置き先 ${STAGER_LOAD_ADDR:04X} '
            f'以上にある。前方コピーの前提が崩れる')
    block_end = STAGER_LOAD_ADDR + len(block)
    if block_end > RAM_TOP:
        raise CapacityError(
            f'LOADM ブロックが ${RAM_TOP:04X} を超える (末尾 ${block_end:04X})')
    if len(block) <= len(body):
        raise RuntimeError('ブロックに本体の後ろのステージャが無い')

    # 1) LOADM がブロックを $1400 から書き込む
    mem[STAGER_LOAD_ADDR:block_end] = block

    # 2) ステージャの命令を、メモリ像に置かれた実際のバイト列から分解する
    stager_lo = STAGER_LOAD_ADDR + len(body)
    st = decode_stager(bytes(mem[stager_lo:block_end]))

    # 3) 読み取った値が、このスクリプトが埋めたつもりの値と一致するか
    expect = {'src': STAGER_LOAD_ADDR, 'dst': start_addr,
              'end': STAGER_LOAD_ADDR + len(body), 'jump': start_addr}
    names = {'src': '転送元 (LDX #)', 'dst': '転送先 (LDY #)',
             'end': '転送元の終端 (CMPX #)', 'jump': 'ジャンプ先 (JMP)'}
    for key, want in expect.items():
        if st[key] != want:
            raise RuntimeError(
                f'ステージャの {names[key]} が ${st[key]:04X} で、'
                f'このスクリプトの値 ${want:04X} と食い違う')

    # 4) 分解した命令を順に実行する。転送ループの 1 周は 4 命令なので、
    #    上限は本体の長さ x 4 + 前後の命令ぶんで足りる。
    jump, stack, rom = run_stager(mem, st["ins"], stager_lo, block_end,
                                  step_limit=len(body) * 4 + 64)
    if jump != st['jump']:
        raise RuntimeError('実行した JMP の飛び先が分解した値と違う')
    if rom != ROM_PORT_VALUE:
        raise RuntimeError(
            f'ステージャが ${ROM_PORT:04X} へ書いた値が '
            f'{"無い" if rom is None else f"${rom:02X}"} で、'
            f'${ROM_PORT_VALUE:02X} でない')
    # ステージャが張るスタック (LDS #) の検査。スタックは下へ伸びるので、
    # 先頭だけでなく STACK_RESERVE byte 下までの範囲が、本体にも LOADM
    # ブロックにも掛かっていないこと。さらにその範囲が検査の上限
    # (STACK_RAM_END = $FC00 未満。メイン側の RAM は $FC7F まで、$FC80 から
    # 共有 RAM、$FD00 から I/O) に収まっていること。本作は $FBFF で、
    # 本体 ($0400-) からも LOADM ブロックからも離れている。
    s_lo = stack - STACK_RESERVE + 1
    overlaps = (lambda lo, hi: s_lo <= hi - 1 and stack >= lo)
    if (s_lo < 0 or stack >= STACK_RAM_END or
            overlaps(STAGER_LOAD_ADDR, block_end) or
            overlaps(jump, jump + len(body))):
        raise RuntimeError(
            f'ステージャが張るスタック ${stack:04X} (下へ {STACK_RESERVE} byte) が '
            f'RAM (${STACK_RAM_END - 1:04X} 以下) の外か、本体 (${jump:04X}-'
            f'${jump + len(body) - 1:04X}) または LOADM ブロック '
            f'(${STAGER_LOAD_ADDR:04X}-${block_end - 1:04X}) に掛かっている')

    # 5) ジャンプ先から本体の長さぶんが、本体そのものになっているか
    got = bytes(mem[jump:jump + len(body)])
    if got != body:
        for i, (p_, q_) in enumerate(zip(got, body)):
            if p_ != q_:
                raise RuntimeError(
                    f'転送後のメモリ像が本体と違う: ${jump + i:04X} が '
                    f'${p_:02X} (本体は ${q_:02X})')
        raise RuntimeError('転送後のメモリ像の長さが本体と違う')
    return st


# ===== Procedure text =====

def build_txt(loadm_name, load_secs, t77_name, loader_lines=None,
              total_secs=None):
    """手順のテキストを組む。load_secs は本体の機械語ファイルの波形長 (秒)、
    total_secs は先頭の BASIC ローダーと前後の無音を含めた全体の長さ (秒)。"""
    mins = f"{load_secs / 60:.0f}"
    secs = f"{load_secs:.0f}"
    bar = "──────────────────────────────────────────────────"
    lines = []
    lines.append(f"=== {t77_name} ロード手順 (FM-7 F-BASIC) ===")
    lines.append("")
    lines.append(bar)
    lines.append(" 1. 準備")
    lines.append(bar)
    lines.append("")
    lines.append("  テープを先頭に戻した状態で入れ、F-BASIC を入力待ち")
    lines.append("  (Ready の表示) にしておきます。")
    if loader_lines:
        lines.append(f'  テープには BASIC のローダー "{LOADER_NAME}" と本体の機械語')
        lines.append(f'  ファイル "{loadm_name}" が、この順に入っています。')
    else:
        lines.append(f'  テープには本体の機械語ファイル "{loadm_name}" が入っています。')
    lines.append("")
    lines.append(bar)
    if loader_lines:
        lines.append(" 2. 入力 — 打つのはこの 1 行だけです")
    else:
        lines.append(" 2. 入力 — 次の 2 行を順に打ちます")
    lines.append(bar)
    lines.append("")
    if loader_lines:
        lines.append("  RUN \"CAS0:\"")
        lines.append("")
        lines.append("  ローダーが LOADING の案内を出し、続けて本体を読み込みます。")
    else:
        lines.append(f"  CLEAR ,&H{CLEAR_VALUE:04X}")
        lines.append(f"  LOADM \"CAS0:\",,R")
        lines.append("")
        lines.append("  CLEAR は、本体を読み込む場所を BASIC が使わないように")
        lines.append("  空けるためのものです。必ず先に打ってください。")
    lines.append("")
    lines.append(bar)
    lines.append(" 3. 待ち時間")
    lines.append(bar)
    lines.append("")
    lines.append(f"  本体の機械語ファイルの読み込みには約 {mins} 分 ({secs} 秒) 掛かります。")
    if total_secs:
        if loader_lines:
            lines.append("  WAV を先頭から再生すると、BASIC ローダーと前後の無音を含めて")
        else:
            lines.append("  WAV を先頭から再生すると、前後の無音を含めて")
        lines.append(f"  約 {total_secs:.0f} 秒です。")
    if loader_lines:
        lines.append("  ローダーから呼んだ読み込みは途中経過を表示しないので、")
        lines.append("  その間は画面の表示が変わりません。")
    else:
        lines.append(f"  Found: {loadm_name} が出た後は、読み終わるまで画面の表示が")
        lines.append("  変わりません。")
    lines.append("  読み終わるとそのままタイトル画面が出ます。上の目安の時間を")
    lines.append("  過ぎてもタイトル画面が出ない時は、4. の操作をしてください。")
    lines.append("")
    lines.append(bar)
    lines.append(" 4. 失敗した時の操作")
    lines.append(bar)
    lines.append("")
    if loader_lines:
        lines.append("  上の 1 行で読み込めない時は、テープを先頭に戻してから")
        lines.append("  次の 2 行を順に打ちます (手で打つ手順)。")
        lines.append("")
        lines.append(f"  CLEAR ,&H{CLEAR_VALUE:04X}")
        lines.append(f"  LOADM \"CAS0:{loadm_name}\",,R")
        lines.append("")
        lines.append("  CLEAR は、本体を読み込む場所を BASIC が使わないように")
        lines.append("  空けるためのものです。必ず先に打ってください。")
        lines.append("")
        lines.append("  ファイル名まで打ってください。テープの先頭には BASIC の")
        lines.append(f"  ローダー \"{LOADER_NAME}\" が入っているので、名前を省いた")
        lines.append("  LOADM \"CAS0:\" はそれに当たって Bad File Mode になります。")
        lines.append(f"  名前を付けると Skip: {LOADER_NAME} と出して読み飛ばし、")
        lines.append(f"  Found: {loadm_name} の後は 3. と同じ待ち時間です。")
        lines.append("")
    lines.append("  デバイス名は \"CAS0:\" と必ず書いてください (この手順では")
    lines.append("  省略しません)。綴りを間違えるとエラーで止まり、入力待ちに")
    lines.append("  戻ります。数字の 0、引用符、コロンを確かめ、テープを先頭に")
    lines.append("  戻してから打ち直してください。ファイルを探し続けて止まらない")
    lines.append("  時は、リセットするか電源を入れ直して F-BASIC を立ち上げ直し、")
    lines.append("  テープを先頭に戻してから打ち直してください。")
    lines.append("")
    lines.append("  綴りが合っているのに読み込めない時は、テープの先頭からの")
    lines.append("  読み込みをもう一度試してください。音声で読み込んでいる場合は、")
    lines.append("  音量と再生の開始位置も確かめてください。")
    lines.append("")
    return "\n".join(lines) + "\n"


# ===== Main =====

def parse_addr(s):
    s = s.strip()
    try:
        if s.lower().startswith('0x') or s.lower().startswith('&h'):
            v = int(s[2:], 16)
        elif s.startswith('$'):
            v = int(s[1:], 16)
        else:
            v = int(s, 0)
    except ValueError:
        print(f'error: アドレス/数値の書式が不正です: {s!r} '
              f'(例: 0x0200 / $0200 / &H0200 / 512)', file=sys.stderr)
        sys.exit(1)
    if not 0 <= v <= 0xFFFF:
        print(f'error: アドレス/数値が $0000-$FFFF の範囲外です: {s!r}',
              file=sys.stderr)
        sys.exit(1)
    return v


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src', help='入力の D77 ファイル')
    ap.add_argument('--addr', required=True, type=parse_addr,
                    help='本体のエントリアドレス (= 読み込み先の先頭。例 0x0400)')
    ap.add_argument('--skip', type=parse_addr, default=256,
                    help='連結したセクタの先頭から飛ばす byte 数 (既定 256 = '
                         '1 セクタ。C0 H0 R1 の IPL のセクタを飛ばす。'
                         '--skip 0 で飛ばさない)')
    ap.add_argument('--size', type=parse_addr, default=None,
                    help='本体として使う byte 数 (既定: IPL のセクタに記録された'
                         'セクタ数 x 256。--skip が 256 でない時は必須)')
    ap.add_argument('-o', '--out', default=None,
                    help='出力する T77 のパス (既定: <src>.t77)')
    ap.add_argument('-t', '--txt', default=None,
                    help='出力する手順のテキストのパス (既定: <src>.txt)')
    ap.add_argument('-w', '--wav', default=None,
                    help='出力する WAV のパス (既定: <src>.wav)')
    ap.add_argument('--no-wav', action='store_true',
                    help='WAV を出力しない')
    ap.add_argument('--silence', type=float, default=5.0,
                    help='WAV の先頭・ファイル間・末尾に入れる無音 (秒。既定 5.0)')
    ap.add_argument('--no-wav-silence-cue', action='store_true',
                    help='T77 のファイル間に無音の目印 0x0000 を入れない')
    ap.add_argument('--no-basic-loader', action='store_true',
                    help='テープの先頭に ASCII 保存の BASIC ローダーを置かない '
                         '(利用者が CLEAR と LOADM を手で打つ)')
    ap.add_argument('--tramp', required=True,
                    help='Makefile が scripts/trampoline.asm から組み立てた'
                         'ステージャのひな形')
    args = ap.parse_args()

    if not os.path.isfile(args.src):
        print(f'error: 入力の D77 が無い: {args.src}', file=sys.stderr)
        return 1
    if not os.path.isfile(args.tramp):
        print(f'error: ステージャのひな形が無い: {args.tramp}', file=sys.stderr)
        return 1
    if not (math.isfinite(args.silence) and args.silence >= 0):
        print(f'error: --silence は 0 以上の有限の秒数にする: {args.silence}',
              file=sys.stderr)
        return 1
    with open(args.tramp, 'rb') as f:
        template = f.read()

    base, _ = os.path.splitext(args.src)
    out_t77 = args.out or (base + '.t77')
    out_txt = args.txt or (base + '.txt')
    out_wav = args.wav or (base + '.wav')

    # 入力と出力が同じファイルを指していないこと (入力の D77 を上書きしたり、
    # 出力同士で別の形式のファイルを上書きしたりしない)。
    paths = [('入力の D77', args.src), ('ステージャのひな形', args.tramp),
             ('T77', out_t77), ('手順のテキスト', out_txt)]
    if not args.no_wav:
        paths.append(('WAV', out_wav))
    seen = {}
    for what, p in paths:
        key = os.path.realpath(p)
        if key in seen:
            print(f'error: {seen[key]} と {what} が同じファイル {p} を指している',
                  file=sys.stderr)
            return 1
        seen[key] = what

    with open(args.src, 'rb') as f:
        d77 = f.read()

    real_size = 0
    try:
        # 本体の長さは中身から推測しない。--size があればその長さ、無ければ
        # IPL のセクタに記録された本体のセクタ数 (ディスクの IPL が読む量) で
        # 決める。IPL のセクタを飛ばさない (--skip が 256 でない) 時は IPL の
        # 情報が使えないので --size が要る。
        sectors = extract_d77_sectors(d77)
        payload = b''.join(s[3] for s in sectors)
        print(f'[+] D77 payload          : {len(sectors)} セクタ、'
              f'{len(payload)} byte (全セクタ)')
        if args.size is not None:
            body = payload[args.skip:args.skip + args.size]
            if len(body) != args.size:
                raise RuntimeError(
                    f'--size {args.size} が D77 の中身 ({len(payload)} byte、'
                    f'skip {args.skip}) を超える')
            check_sector_order(sectors, args.skip + args.size)
            how = f'--size {args.size}'
        elif args.skip == SECTOR_SIZE:
            body_len = body_len_from_ipl(payload)
            check_sector_order(sectors, SECTOR_SIZE + body_len)
            body = payload[SECTOR_SIZE:SECTOR_SIZE + body_len]
            how = (f'IPL に記録された本体 {body_len // SECTOR_SIZE} セクタ '
                   f'x {SECTOR_SIZE} byte')
        else:
            raise RuntimeError(
                f'--skip が {SECTOR_SIZE} でない時は IPL のセクタ数が使えない。'
                f'--size で本体の長さを指定する')
        real_size = len(body)
        print(f'[+] working binary       : {real_size} byte '
              f'(skip={args.skip}、長さは {how})')
        if real_size == 0:
            raise RuntimeError('本体が空')

        # 収まりはアドレスを埋める前に検査する (check_capacity の docstring)。
        # 組み上がったブロックについては simulate_plan がもう一度検査する。
        check_capacity(real_size, len(template))
        stager = build_stager(template, real_size, args.addr)
        block = wrap_block(body, stager)
        # 生成物を書き出す **前に**、ステージャの命令を読み取り、計画を
        # 1 byte ずつ再生して突合せる。
        st = simulate_plan(args.addr, body, block)
    except CapacityError as e:
        limit = RAM_TOP - STAGER_LOAD_ADDR - len(template)
        if args.size is None:
            # IPL の経路では本体の長さがセクタ数 x 256 なので、上限もセクタの
            # 単位に切り下げて案内する。
            limit_sec = limit // SECTOR_SIZE
            advice = (f'本体を {limit_sec * SECTOR_SIZE} byte '
                      f'(= {limit_sec} セクタ) 以下に減らす')
        else:
            advice = f'本体を {limit} byte (${limit:04X}) 以下に減らす'
        print('error: CMT ロード不可 — LOADM の読み込み先のメモリ領域 '
              f'(${STAGER_LOAD_ADDR:04X}-${RAM_TOP - 1:04X}) に収まりません。',
              file=sys.stderr)
        print(f'       エントリ ${args.addr:04X} / 本体 {real_size} byte では '
              f'配置が成立しません。', file=sys.stderr)
        print(f'       詳細: {e}', file=sys.stderr)
        print(f'       対策: {advice}。', file=sys.stderr)
        return 1
    except RuntimeError as e:
        print(f'error: 変換を中止しました: {e}', file=sys.stderr)
        return 1

    exec_addr = STAGER_LOAD_ADDR + real_size
    loadm_name = LOADM_NAME
    loadm = make_loadm_payload(STAGER_LOAD_ADDR, block, exec_addr)
    print(f'[+] LOADM block          : ${STAGER_LOAD_ADDR:04X}-'
          f'${STAGER_LOAD_ADDR + len(block) - 1:04X} '
          f'(本体 {real_size} byte + ステージャ {len(stager)} byte)、'
          f'EXEC ${exec_addr:04X}')
    print(f'[+] stager decoded       : {len(st["ins"])} 命令。転送元 ${st["src"]:04X} → '
          f'転送先 ${st["dst"]:04X}、終端 ${st["end"]:04X}、'
          f'ジャンプ先 ${st["jump"]:04X} (命令から読み取った値)')
    print(f'[+] plan verified        : 64 KiB のメモリ像で命令を順に実行し、'
          f'${args.addr:04X} に本体 {real_size} byte が揃うことを確認')

    # テープに載せるファイルの並び。**BASIC ローダーを先頭に置く**ので、
    # 利用者が打つのは RUN "CAS0:" の 1 行だけで済む。
    load_secs = _file_seconds(loadm, loadm_name)
    loader_lines = (None if args.no_basic_loader
                    else basic_loader_lines(load_secs))
    tape_seq = [(loadm_name, 'loadm', loadm)]
    if loader_lines is not None:
        tape_seq.insert(0, (LOADER_NAME, 'basic', loader_lines))
        print(f'[+] BASIC loader         : "{LOADER_NAME}" '
              f'{len(loader_lines)} 行 (テープ先頭)')
        for l in loader_lines:
            print(f'        {l}')
    print(f'[+] load time            : 約 {load_secs:.0f} 秒 (本体の機械語ファイルの波形長。'
          f'ローダーと無音を含む全体は WAV の長さ)')

    t77 = build_t77(tape_seq,
                    mark_inter_file_silence=not args.no_wav_silence_cue)
    with open(out_t77, 'wb') as f:
        f.write(t77)
    print(f'\n[+] T77 written          -> {out_t77} ({len(t77)} bytes)')

    # WAV を出す時だけ、無音を含めた全体の長さを手順のテキストに書く。
    total_secs = None
    if not args.no_wav:
        total_secs = (sum(sum(h & ~POL for h in _build_file(e)) for e in tape_seq)
                      * TICK_SCALE_CY / T77_TICK_BASE_HZ
                      + args.silence * (len(tape_seq) + 1))
    txt = build_txt(loadm_name, load_secs, os.path.basename(out_t77),
                    loader_lines, total_secs)
    with open(out_txt, 'w', encoding='utf-8') as f:
        f.write(txt)
    print(f'[+] procedure written    -> {out_txt}')

    if not args.no_wav:
        wav = build_wav(tape_seq,
                        head_silence=args.silence,
                        gap_silence=args.silence,
                        tail_silence=args.silence)
        with open(out_wav, 'wb') as f:
            f.write(wav)
        dur = (len(wav) - 44) / WAV_SAMPLE_RATE / 2   # 16-bit mono
        print(f'[+] WAV written          -> {out_wav}  '
              f'({len(wav)} bytes, ~{dur:.1f} s, {WAV_SAMPLE_RATE} Hz 16-bit mono, '
              f'{args.silence:.1f}s silence head/gaps/tail)')

    print('\n--- procedure summary ---')
    if loader_lines is not None:
        print('  RUN "CAS0:"        ; 打つのはこの 1 行だけ (先頭の BASIC ローダー)')
        print('  --- 1 行の手順が通らない時に手で打つ手順 ---')
    dev = f'CAS0:{loadm_name}' if loader_lines is not None else 'CAS0:'
    print(f'  CLEAR ,&H{CLEAR_VALUE:04X}')
    print(f'  LOADM "{dev}",,R')

    return 0


if __name__ == '__main__':
    sys.exit(main())
