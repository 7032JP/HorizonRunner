* ============================================================
* asm_kbd.s — キーボード IRQ の許可とパレット
*
* FM-7 のキーボードはサブ CPU が走査し、 キーコードを $FD01 に流す方式。
* キーボード IRQ ($FD02 bit0) を許可すると、 キーを押すたび (とキーリピート
* のたび) に IRQ が入り、 そのフラグは $FD01 を読むまでクリアされない。
* 受信は asm_timer.s の IRQ ハンドラ _irq_isr が行い、 キーコードを
* asm_input.s の _input_feed_key へ渡す。 方向の保持と受信の集計は
* asm_input.s が行う。
*
* テンキー → キーコード:
*   '8' = $38 (上)、 '2' = $32 (下)、 '4' = $34 (左)、 '6' = $36 (右)
*
* C API (c_subprog.h):
*   void kb_init(void)                    キーボード IRQ を許可する (起動時 1 回)
*   void palette_init(void)               起動直後のパレット (白黒)
*   void palette_page(unsigned char page) 表示するページ (B / R プレーン) を選ぶ
* ============================================================

IO_IRQMASK      equ     $FD02           * W: IRQ マスク (= bit0 でキーボード IRQ enable)
IO_PALETTE      equ     $FD38           * W: パレット ($FD38-$FD3F = 色番号 0-7)
                                        *    値 = G*4 + R*2 + B (= デジタル GRB)

                section code

                export  _kb_init
                export  _palette_init
                export  _palette_page


* void kb_init(void) — キーボード IRQ を許可する (起動時 1 回)
*   起動処理では続けて asm_timer.s の timer_init() が $FD02 をキーボード
*   bit0 + タイマ bit2 = $05 に書き直し、 CPU の IRQ も許可する。 キーボードの
*   許可はここで、 タイマの許可は timer_init() で行う (timer_init は両方の
*   ビットを書く)。
_kb_init:
                lda     #$01
                sta     IO_IRQMASK
                rts


* void palette_init(void) — 論理色番号 → 物理色 を割り当てる
*
* 起動直後の暫定パレット。 色番号の bit0 (= B plane) だけで白黒を
* 決める並びにしておき、 本体が最初に palette_page(0) を呼んだ時点で
* 下の pal_b (= B plane を表示、 G plane を緑) に差し替わる。
*   偶数色番号 (= bit0 が 0、 B plane が 0) → 黒
*   奇数色番号 (= bit0 が 1、 B plane が 1) → 白
*
*   色番号: 0  1  2  3  4  5  6  7
*   物理色: 黒 白 黒 白 黒 白 黒 白
*   GRB値 : 0  7  0  7  0  7  0  7   (= G*4 + R*2 + B)
*   ※ パレットは書込専用なので read-modify-write (clr 等) は使わず、
*     必ず sta で書く。
_palette_init:
                ldx     #IO_PALETTE
                ldb     #4              * 黒/白 のペアを 4 組
.pal_loop:
                clra
                sta     ,x+             * 偶数色番号 = 黒
                lda     #$07
                sta     ,x+             * 奇数色番号 = 白
                decb
                bne     .pal_loop
                rts


* void palette_page(unsigned char page) — 表示するプレーンを選ぶ (ダブルバッファリング)
*
* 本作は **B plane と R plane を 2 枚の画面として使い分ける**。 描いている
* 最中の絵を見せないためのダブルバッファリングであり、 パレットを差し替えるだけで
* 表示ページが入れ替わる (= VRAM の中身は 1 byte も動かさない)。
*
*   page 0 → B plane が選択プレーン (pal_b)
*   page 1 → R plane が選択プレーン (pal_r)
*   色は 「選択プレーンのビットが 1 なら白、 0 なら G plane が 1 で緑、
*   0 で黒」 (下の表)。
*
* CMOC の引数規約: char 引数は 16-bit に昇格してスタックに積まれるので、
* 値 (下位) は 3,s にある (0-1,s は戻り先)。
* パレットは書込専用なので必ず sta で書く (read-modify-write をしない)。
_palette_page:
                ldb     3,s             * page (昇格ワードの下位)
                bne     .pp_r
                ldx     #pal_b
                bra     .pp_go
.pp_r:
                ldx     #pal_r
.pp_go:
                ldy     #IO_PALETTE
                ldb     #8
.pp_loop:
                lda     ,x+
                sta     ,y+
                decb
                bne     .pp_loop
                rts

* ------------------------------------------------------------
* パレット表 (3 枚のプレーンの役割は config.mk)
* ------------------------------------------------------------
*   B plane / R plane … ダブルバッファリングの表と裏。 自機 / 自弾 / 敵を白で描く
*   G plane           … 緑のカラープレーン。 地平線と床、 得点などの文字。
*                       ダブルバッファリングしない
*
* 色番号は bit0 = B / bit1 = R / bit2 = G。 設定値は G*4 + R*2 + B なので
* $07 = 白 / $04 = 緑 / $00 = 黒。
*
*   表示中のページのビットが 1     → 白 (前景が最優先)
*   表示中のページが 0 で G が 1   → 緑 (地平線と床が透ける)
*   どちらも 0                     → 黒
*
* 裏のページのビットは色に一切効かない。 これが 「描いている最中の絵が見えない」
* ダブルバッファリングの実体である。
* 色番号        0   1   2   3   4   5   6   7
*   (G,R,B)   000 001 010 011 100 101 110 111
pal_b           fcb     $00,$07,$00,$07,$04,$07,$04,$07   * B plane を表示
pal_r           fcb     $00,$00,$07,$07,$04,$04,$07,$07   * R plane を表示

                end
