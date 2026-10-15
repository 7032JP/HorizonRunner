/* ============================================================
 * c_subprog.c — サブ CPU 側の描画プログラムをメイン CPU から動かす側
 *                (コマンドの積込みと呼び出し)
 *
 * 1 フレームの流れ:
 *   main 側で sub_page() / sub_scene() / sub_sprite() / sub_objs_*() を
 *   影バッファに積む
 *      → sub_flush_async() で [前のバッチの完了待ち → HALT → 共有 RAM
 *         (固定パラメータ $FC83-$FC8A とキュー $FC93-) へ一括コピー →
 *         sub_call_async(SUB_PROG_ADDR)] を行い、
 *         サブが描いている間に main は次のフレームの計算へ進む。
 *
 * 完了を待つ sub_flush() は、起動時の絵の転送と文字描画 (c_text.c) の
 * ように結果をすぐ使う場面で使う。
 * ============================================================ */

#include "c_subprog.h"

/* sub プログラム本体 (= bin2asm.py で asm_subprog.s から生成された rodata)。 */
extern const unsigned char subprog_bin[];
extern const unsigned int  subprog_len;

/* ----- 描画コマンドキュー (main RAM 上の影バッファ) ---------------- */

static unsigned char q_buf[SUB_QUEUE_BYTES];
static unsigned char q_len;
/* 固定パラメータの影 (並びは c_subprog.h の表)。flush のたびに共有 RAM へ
 * 写し、[0] (flags) を 0 に戻す。 */
static unsigned char q_prm[SUB_PARAM_BYTES];

/* n byte ぶんの領域を確保して先頭ポインタを返す。入らなければ先に
 * flush する (= あふれても捨てない)。n は必ず SUB_QUEUE_BYTES 以下である
 * こと (各 API の上限: 帯 2 x MAX_BANDS、OBJS 2 + 3 x MAX_OBJS、WRITE
 * 4 + 104。ここでは検査しない)。 */
static unsigned char *q_reserve(unsigned char n)
{
    unsigned char *p;
    if ((unsigned char)(q_len + n) > SUB_QUEUE_BYTES) {
        sub_flush();
    }
    p = &q_buf[q_len];
    q_len = (unsigned char)(q_len + n);
    return p;
}

/* sub_flush / sub_flush_async の共通部。キューと固定パラメータのフラグが
 * どちらも空なら何もせず 0 を返す (前の非同期バッチも待たない)。送るものが
 * あれば、前の非同期バッチの完了を sub_sync() で待ってから HALT し、共有 RAM
 * へ写して 1 を返す (呼び手がサブを呼ぶ)。 */
static unsigned char flush_prepare(void)
{
    if (q_len == 0 && q_prm[0] == 0) return 0;
    sub_sync();                 /* 前の非同期バッチ完了 (= 共有 RAM を書き換えてよい) */
    sub_halt();
    /* 共有 RAM へのコピーはアセンブラ (sub_param_copy / sub_queue_copy)。
     * キューの末尾に SUBCMD_END ($00) が自動で付く。 */
    sub_param_copy(q_prm);
    q_prm[0] = 0;
    sub_queue_copy(q_buf, (unsigned char *)SUB_QUEUE_BASE, q_len);
    q_len = 0;
    return 1;
}

/* 積んだコマンドをまとめてサブに実行させ、完了まで待つ。 */
void sub_flush(void)
{
    if (flush_prepare()) sub_call(SUB_PROG_ADDR);
}

/* 積んだコマンドをサブに投げ、完了を待たずに戻る (= 非同期)。描き上がりは
 * sub_sync() で待つ。 */
void sub_flush_async(void)
{
    if (flush_prepare()) sub_call_async(SUB_PROG_ADDR);
}

/* ----- 起動時の一度きり初期化 -------------------------------------- */

void subprog_init(void)
{
    /* ★ crt0 の INILIB はダミー RTS で bss をゼロクリアしないため、
     *   q_len と固定パラメータの flags はここで明示的に 0 にする
     *   (= 起動時の値は不定)。 */
    q_len = 0;
    q_prm[0] = 0;

    /* まず転送だけ行う (exec=0 で CALL を skip)。転送中は共有 RAM が
     * chunk バッファとして使われる。 */
    sub_takeover(subprog_bin, subprog_len, SUB_PROG_ADDR, 0);

    /* 空のキュー (END だけ) を 1 回実行させる。ここで END を書いておかないと、
     * chunk バッファの残骸がコマンドとして解釈される恐れがある。共有 RAM への
     * write は HALT 中のみ有効。サブ側の消し込み表の件数はここでは初期化
     * されず、sub_cls() が 0 にする (c_subprog.h の subprog_init の説明)。 */
    sub_halt();
    SUB_PARAM_BASE[0] = 0;          /* 固定パラメータのフラグも空にする */
    SUB_QUEUE_BASE[0] = SUBCMD_END;
    sub_call(SUB_PROG_ADDR);
}

/* ----- 描画コマンドの積込み ---------------------------------------- */

/* 3 プレーンを消す。プレーンごとに 1 命令へ割り、1 プレーンずつ送り出す
 * (理由は asm_subprog.s の cmd_cls)。 */
void sub_cls(void)
{
    unsigned char pl;
    for (pl = 0; pl < 3; pl++) {
        unsigned char *p = q_reserve(2);
        p[0] = SUBCMD_CLS;
        p[1] = pl;
        sub_flush();
    }
}

/* ヘッダは固定パラメータへ、帯はキューの先頭へ。サブは帯をキューの先頭
 * から読むので、キューに何か残っていれば先に送ってから積む。 */
void sub_scene(unsigned char clr_from, unsigned char clr_lines,
               unsigned char hz, const unsigned char *bands,
               unsigned char nbands)
{
    unsigned char i;
    unsigned char n2 = (unsigned char)(nbands * 2);
    unsigned char *p;
    if (q_len != 0) sub_flush();
    q_prm[0] |= SUB_PF_SCENE;
    q_prm[1] = clr_from;
    q_prm[2] = clr_lines;
    q_prm[3] = hz;
    q_prm[4] = nbands;
    p = q_reserve(n2);
    for (i = 0; i < n2; i++) {
        p[i] = bands[i];
    }
}

void sub_sprite(unsigned char x, unsigned char y, unsigned char ph)
{
    q_prm[0] |= SUB_PF_SPRITE;
    q_prm[5] = x;
    q_prm[6] = y;
    q_prm[7] = ph;
}

void sub_page(unsigned char page)
{
    q_prm[0] = (unsigned char)((q_prm[0] & (SUB_PF_SCENE | SUB_PF_SPRITE))
                               | SUB_PF_PAGE | page);
}

/* OBJS 命令を開く。個数ぶんの領域 (2 + 3*max byte) を先に確保し、呼び手が
 * 返されたポインタへ [x, y, k|ph<<6] を直接書く (= 二度写しを避ける)。
 * 閉じるまで他の積込みをしてはならない (閉じる時に未使用分を返すため)。 */
static unsigned char *objs_hdr;

unsigned char *sub_objs_open(unsigned char max)
{
    objs_hdr = q_reserve((unsigned char)(2 + max + max + max));
    objs_hdr[0] = SUBCMD_OBJS;
    return objs_hdr + 2;
}

void sub_objs_close(unsigned char n)
{
    unsigned char *end = objs_hdr + 2 + n + n + n;
    if (n == 0) {
        q_len = (unsigned char)(objs_hdr - q_buf);       /* 命令ごと取り消す */
    } else {
        objs_hdr[1] = n;
        q_len = (unsigned char)(end - q_buf);
    }
}

/* n = 0 は積まない (サブ側の WRITE は n を 1 以上として数えるため)。 */
void sub_write(unsigned int dst, const unsigned char *src, unsigned char n)
{
    unsigned char i;
    unsigned char *p;
    if (n == 0) return;
    p = q_reserve((unsigned char)(4 + n));
    p[0] = SUBCMD_WRITE;
    p[1] = (unsigned char)(dst >> 8);
    p[2] = (unsigned char)dst;
    p[3] = n;
    for (i = 0; i < n; i++) {
        p[4 + i] = src[i];
    }
}
