/* ============================================================
 * c_text.h — 文字を VRAM へ直に書く (タイトル / テンキーの図 / 得点 /
 *            ハイスコア / 残機 / ステージクリアとゲームオーバーの表示)
 *
 * 字形はメイン側 (build/font.h) に持ち、WRITE 命令に載せてサブ CPU に
 * VRAM へ書かせる。サブ側の描画プログラムに文字の処理は無い。
 *
 * 使える字: 空白、A-Z、0-9、- . ! =。小文字 a-d / f-i はテンキーの図の
 * 矢印 (a = 1 の向き … i = 9。e = 5 は字形が無い)。それ以外の字は空白になる。
 *
 * 使いどころ:
 *   - 毎フレームは使わない。text_put() / text_put_wide() は 1 回の呼出しで
 *     8 個の WRITE 命令 (1 line に 1 個) を積み、text_erase() は h 行 x
 *     (wb を 64 byte ごとに切った個数) の WRITE 命令を積む。
 *   - キューの残量と字数、横倍率によっては、積んでいる途中で q_reserve() が
 *     sub_flush() を呼んで送る。残りは呼んだ後に sub_flush() で送り出す。
 *   - 値が変わったフレームだけ書き直すこと (得点・残機)。
 *   - 積む先はメイン側の影のキューで、送る時に前の非同期の描画の完了を待つ。
 *
 * 置き場所は「プレーンの先頭 + line * 80 + byte」。line と byte は画面全体
 * (0-199 / 0-79) で数える。範囲検査はしないので、通常幅は xb + n <= 80、
 * 横 2 倍は xb + 2n <= 80、字は y + 8 <= 200、消去は xb + wb <= 80 と
 * y + h <= 200 を呼び手が守ること。
 * ============================================================ */

#ifndef C_TEXT_H
#define C_TEXT_H

/* 書き込むプレーン (= config.mk / asm_subprog.s の三枚と同じ) */
#define TEXT_PLANE_0   0x0000   /* ダブルバッファリングのページ 0 (白) */
#define TEXT_PLANE_1   0x4000   /* ダブルバッファリングのページ 1 (白) */
#define TEXT_PLANE_G   0x8000   /* カラープレーン (緑) */

/* 1 回に書ける字数 */
#define TEXT_MAX       32

/* 8x8 のまま書く。x は byte (= 8 dot) 単位なので字は byte 境界に乗る。 */
void text_put(unsigned int plane, unsigned char xb, unsigned char y,
              const char *s);

/* 横だけ 2 倍に伸ばして書く (1 字 16 dot)。FM-7 の画素は縦長なので、
 * これでようやく字がほぼ正方に見える。タイトルの文字、テンキーの図、
 * GAME OVER / STAGE CLEAR の文字はこちら。 */
void text_put_wide(unsigned int plane, unsigned char xb, unsigned char y,
                   const char *s);

/* 指定プレーンの矩形を 0 で消す (幅は byte 単位)。書いた字を消すのに使う。 */
void text_erase(unsigned int plane, unsigned char xb, unsigned char y,
                unsigned char wb, unsigned char h);

#endif
