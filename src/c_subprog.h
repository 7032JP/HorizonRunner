/* ============================================================
 * c_subprog.h — サブ CPU 側の描画プログラムをメイン CPU から呼ぶ C API
 *
 * 構成要素:
 *   - 低レベル:   sub_halt / sub_call / sub_call_async / sub_sync /
 *                 sub_takeover / sub_queue_copy / sub_param_copy
 *                 (= asm_test.s 実装、sub への code/data 転送と JSR)
 *   - 高レベル:   subprog_init / sub_cls / sub_scene / sub_sprite / sub_page /
 *                 sub_objs_open / sub_objs_close / sub_write /
 *                 sub_flush / sub_flush_async
 *                 (= c_subprog.c 実装、描画コマンドキュー)
 *   - キーとパレット: kb_init / palette_init / palette_page (= asm_kbd.s 実装)
 *   - タイマ:     timer_init / timer_start / timer_get / timer_consume /
 *                 irq_save / irq_restore (= asm_timer.s 実装)
 *   ゲーム用の入力 API は c_input.h。
 *
 * 描画は B / R プレーンをダブルバッファリングの 2 ページとして白で描き、
 * G プレーンを床のカラープレーン (緑) に使う。
 * ============================================================ */

#ifndef C_SUBPROG_H
#define C_SUBPROG_H

/* ---- sub 上のロード位置 ----
 *
 * ★ 配置アドレスは config.mk / asm_subprog.s の equ / ここ の 3 箇所に
 *   書かれている。片方だけ直すと転送先と参照先が静かにずれ、エラーも
 *   例外も出ずに絵が化ける。build/subprog.bin を作り直すたびに
 *   scripts/check_layout.py が 3 者を突き合わせて検査する。
 *   **正は config.mk である。** コード中にアドレスをハードコードしないこと。 */

/* サブ側 作業変数 (32 byte の領域) */
#define SUB_WORK_ADDR     0xC000
/* ページごとの消し込み表 (2 ページ x 26 件 x 4 byte = 208 byte) */
#define SUB_TABLE_ADDR    0xC020
/* subprog コード本体の入口。$C100-$CF7F (= 3,712 byte まで)。
 * 絵 (自機 / 敵 / 地上物 / 岩) と床の模様表もここに続けて置く。
 * build/subprog.bin を作り直すたびに check_layout.py がサイズを検査し、
 * SUB_PROG_ADDR + size が SUB_CODE_END を超えたらビルドを止める
 * (= 超えるとサブ CPU のスタックを踏んで暴走する)。 */
#define SUB_PROG_ADDR     0xC100
#define SUB_CODE_END      0xCF80

/* ---- 共有 RAM の使い方 (main $FC80-$FCFF / sub $D380-$D3FF の 128 byte) ----
 *
 *   $FC80-$FC82  TEST コマンドのヘッダ (asm_test.s の .build_header が書く)
 *   $FC83-$FC8A  固定パラメータ 8 byte (下の表。sub_param_copy が書く)
 *   $FC8B-$FC92  TEST のサブコマンド (CALL / MOVE) とそのパラメータ
 *   $FC93-$FCFF  描画コマンドキュー 108 byte + 終端 1 byte
 *
 * 固定パラメータは、毎フレームの描画で使うパラメータ (描画先のページ / 情景の
 * ヘッダ / 自機。自機は被弾の演出中や無敵の点滅で描かないフレームもある) を、
 * コマンドにせず決まった並びで置く場所である。
 * サブ側は入口でフラグを見て、立っているものをキューより先に処理する
 * (順は PAGE → SCENE → SPRITE → キュー)。
 *
 *   +0 flags  bit7: PAGE 有り / bit6: SCENE 有り / bit5: SPRITE 有り /
 *             bit0: 描画先のページ (0 = B / 1 = R)
 *   +1 ct     空へ戻すフレーム内 line (偶数)
 *   +2 cn     その行数 (間引き後。0 なら消さない)
 *   +3 hz     地平線のフレーム内 line (偶数)
 *   +4 n      床の帯の数 (帯 [cnt,pat] x n はキューの先頭に置く)
 *   +5 x      自機の x (byte 単位 0-79)
 *   +6 y      自機の y (line 0-199)
 *   +7 ph     自機の横位相 (0-7)
 *
 * 積込みは main RAM 上の影 (固定パラメータ 8 byte とキュー) に対して行い、
 * sub_flush() の中で「HALT → 共有 RAM へ一括コピー → sub_call」とする。
 * 送った後は flags を 0 に戻すので、次の flush で何も立っていなければ
 * サブはキューだけを処理する。 */
#define SUB_PARAM_BASE    ((volatile unsigned char *)0xFC83)
#define SUB_PARAM_BYTES   8
#define SUB_PF_PAGE       0x80   /* flags: 描画先のページを bit0 で選ぶ (1 = R) */
#define SUB_PF_SCENE      0x40   /* flags: ct,cn,hz,n と先頭の帯で情景を描く */
#define SUB_PF_SPRITE     0x20   /* flags: x,y,ph に自機を貼る */
#define SUB_QUEUE_BASE    ((volatile unsigned char *)0xFC93)
#define SUB_QUEUE_BYTES   108

/* sub 描画コマンドコード (= asm_subprog.s のディスパッチと要一致) */
#define SUBCMD_END        0x00   /* (なし)          キュー終端             */
#define SUBCMD_CLS        0x01   /* p               1 プレーンを全消去 (0 B/1 R/2 G) */
#define SUBCMD_OBJS       0x09   /* n,[x,y,k|ph<<6] 敵/地上物/岩/自弾/爆発を n 個 */
#define SUBCMD_WRITE      0x0A   /* ah,al,n,[byte]  サブ側のアドレスへ n byte 書く */

/* 画面寸法。SCREEN_H_LINES は C 側では参照せず、asm_subprog.s の値 (200) と
 * 一致させるために置く。 */
#define SCREEN_W_BYTES    80     /* 横 80 byte (= 640 px) */
#define SCREEN_H_LINES    200    /* 縦 200 line */

/* ---- 描画フレーム (= 384x96 dot) ----
 *
 * FM-7 の 640x200 は画素が正方形ではなく縦長で、4:3 の画面に映すと
 * 1 画素の縦横比はおよそ 1 : 2.4 になる。よって見かけの縦横比は
 * 「横ドット数 : 縦ドット数 x 2.4」で考えねばならない。384x96 は見かけ
 * 384 : 230 (約 1.67 : 1) の横長になる。
 * 描画フレームは画面中央に置く。左上の B プレーンアドレス = 52*80 + 16 = $1050。
 * 行送りは +80。描画フレームの幅 48 byte は 6 の倍数なので、サブ側の PSHU 塗り
 * (D/X/Y の 6 byte を 1 命令で書く) がちょうど 8 回で 1 行を埋める。
 * FRAME_W_BYTES は C 側では参照せず、asm_subprog.s の値と一致させるために置く。 */
#define FRAME_X_BYTE      16     /* 描画フレームの左上の x (byte 単位) */
#define FRAME_Y_LINE      52     /* 描画フレームの左上の y (line 単位) */
#define FRAME_W_BYTES     48     /* 横 48 byte = 384 dot */
#define FRAME_H_LINES     96     /* 縦 96 line */
#define FRAME_W_DOTS      384

/* ---- 自機 (= scripts/make_sprite.py が焼く)。SPR_W_BYTES は C 側では参照せず、
 * 生成される値と一致させるために置く。敵などの寸法は build/sprite_geo.h の
 * obj_w[] / obj_h[] を使う。 ---- */
#define SPR_W_BYTES       4      /* 貼り付け幅 (byte)。24 dot + 位相ずらし */
#define SPR_H_LINES       20
#define SPR_W_DOTS        24

/* ----- 低レベル (asm_test.s 実装) ----- */
void          sub_wait_ready(void);
void          sub_wait_busy(void);
void          sub_halt(void);
/* TEST CALL を発行してサブの完了を待ってから戻る (= 同期) */
void          sub_call(unsigned int addr);
/* TEST CALL を発行して待たずに戻る (= 非同期)。sub_flush_async() が毎
 * フレームの描画に使い、完了は sub_sync() で待つ。 */
void          sub_call_async(unsigned int addr);
/* 非同期で投げたバッチの完了待ち (未完了バッチが無ければ no-op) */
void          sub_sync(void);
/* src から dst へ n byte コピーし、dst[n] に $00 (= SUBCMD_END) を置く。
 * n は下位 1 byte だけを使う (キューは SUB_QUEUE_BYTES 以下)。 */
void          sub_queue_copy(const void *src, unsigned char *dst,
                             unsigned int n);
/* src の 8 byte を固定パラメータ $FC83-$FC8A へ写す (HALT 中に呼ぶ) */
void          sub_param_copy(const unsigned char *src);
/* ROM の TEST MOVE 経由の転送 + (exec != 0 なら) 実行。
 * ★ 転送先に VRAM (= B/R/G プレーン) は指定できない。通常 RAM 専用。
 *   VRAM へ書くには描画プログラムの WRITE 命令 (sub_write) を使う。 */
void          sub_takeover(const void *code, unsigned int len,
                           unsigned int dst, unsigned int exec);

/* ----- 起動時の一度きり初期化 (c_subprog.c 実装) ----- */

/* sub の SUB_PROG_ADDR に subprog 本体を転送し、空キューで 1 回動かす。
 * 描画 API を使う前に必ず 1 回呼ぶこと。サブ側の消し込み表の件数はここでは
 * 初期化されないので、sub_page() より前に sub_cls() を 1 回呼ぶ (CLS が
 * 件数を 0 にする)。 */
void          subprog_init(void);

/* ----- 描画コマンドの積込み (c_subprog.c 実装) ----- */

void          sub_cls(void);   /* 3 プレーンとも全消去 (プレーンごとに 1 回ずつ送る) */

/* 空の消去 + 地平線 + 床の帯を積む。ヘッダ (ct,cn,hz,n) は固定パラメータ
 * へ、帯はキューの先頭へ置く (キューに何か残っていれば先に送る)。
 *   clr_from  : 空に戻す範囲の先頭のフレーム内 line (偶数)
 *   clr_lines : 空に戻す行数 (1 行おきに消すので、line 数の半分。0 で消さない)
 *   hz        : 地平線のフレーム内 line
 *   bands     : [行数, 模様の番号] の並び。地平線の 2 line 下から
 *               1 行おき (2 line に 1 行) に順に塗る。フレームから
 *               はみ出す分はサブ側で切り捨てる。模様の番号は
 *               build/floor_data.s (scripts/make_floor.py) の 0 = 黒 /
 *               1 = $AA の網
 *   nbands    : 帯の数 (2 x nbands がキューに入ること) */
void          sub_scene(unsigned char clr_from, unsigned char clr_lines,
                        unsigned char hz, const unsigned char *bands,
                        unsigned char nbands);
/* 自機を貼る。x は byte 単位 (画面全体で 0-79)、y は line (0-199)、
 * ph は 0-7 dot の横位相。固定パラメータに置く (キューは使わない)。
 * 絵 (SPR_W_BYTES x SPR_H_LINES) はクリップしないので、x + 4 <= 80 かつ
 * y + 20 <= 200 の範囲で呼ぶこと。 */
void          sub_sprite(unsigned char x, unsigned char y, unsigned char ph);
/* 次に送るバッチ全体の描画先のページを選ぶ (= ダブルバッファリング)。
 * 0 = B plane / 1 = R plane。固定パラメータに置くのでキューより先に処理され、
 * 1 つのバッチの中でページを切り替えることはできない。表示ページの切替は
 * palette_page() が行う。 */
void          sub_page(unsigned char page);
/* 敵 / 地上物 / 岩 / 自弾 / 爆発を n 個まとめて 1 命令で積む。x は byte 単位
 * (画面全体で 0-79)、y は line (0-199)、k は絵の種類 (0 以上 OBJ_KINDS 未満。
 * build/sprite_geo.h)、ph は 0-3 (2 dot 単位の横位相)。絵はクリップしないので
 * x + obj_w[k] <= 80 かつ y + obj_h[k] <= 200 の範囲で呼ぶこと。**キューは
 * 108 byte しか無く、あふれるとサブを 2 回呼ぶことになり、呼び出しの往復の
 * ぶんだけ時間を失う** ので、毎フレームの絵 (自弾・敵・地上物・岩) は全部これに詰めて
 * 1 命令で送る。1 個 3 byte。max はサブ側の消し込み表 (自機 1 + 25 件) に
 * 合わせて 25 以下、n は max 以下。
 *   p = sub_objs_open(max);   最大 max 個ぶんの領域を確保し、書込先を返す
 *   p[0]=x; p[1]=y; p[2]=k|(ph<<6); p+=3;  … を n 回
 *   sub_objs_close(n);        個数を確定し、余った領域を返す
 * open から close の間に他の sub_* を呼んではならない。 */
unsigned char *sub_objs_open(unsigned char max);
void          sub_objs_close(unsigned char n);
/* サブ側のアドレス dst へ src から n byte 書く (n は 1〜104 = キュー 108 byte
 * からヘッダ 4 byte を引いた数。0 は積まない)。書くのはサブ CPU である
 * (メイン CPU から VRAM は見えない)。起動時の絵の転送と文字描画 (c_text.c)
 * が使う。毎フレームは使わないこと。 */
void          sub_write(unsigned int dst, const unsigned char *src,
                       unsigned char n);
/* 積んだコマンドをまとめてサブに実行させる (完了まで待つ)。キューと固定
 * パラメータがどちらも空なら何もしない (前の非同期バッチの完了は sub_sync()
 * で待つ)。 */
void          sub_flush(void);
/* 同上だが完了を待たない。完了は sub_sync() で待つ。毎フレームの描画は
 * こちらを使い、main の計算とサブの描画を重ねる。 */
void          sub_flush_async(void);

/* ----- キーボード IRQ の許可とパレット (asm_kbd.s 実装) ----- */
void          kb_init(void);
void          palette_init(void);
/* 表示するページを選ぶ (= ダブルバッファリングの表裏切替)。0 = B plane / 1 = R plane。
 * パレットを差し替えるだけなので VRAM は 1 byte も動かない。 */
void          palette_page(unsigned char page);

/* ----- 周期タイマ (asm_timer.s 実装。1 tick = 約 2ms) ----- */
void          timer_init(void);
void          timer_start(void);
unsigned int  timer_get(void);
void          timer_consume(unsigned int n);
/* 短い I/O の列を囲み、途中で IRQ の処理 (タイマとキー入力) に割り込まれない
 * ようにする。irq_restore は irq_save の前の IRQ マスク状態 (CC の I ビット)
 * へ戻す。 */
unsigned char irq_save(void);
void          irq_restore(unsigned char cc);

#endif
