* ============================================================
* asm_input.s — ゲーム用 入力モジュール (IRQ 駆動)
*
* 【この機種のキーボードの制約 — 設計の前提】
*   本機のキーボードは、 押されたキーのコードを $FD01 に流す方式である。
*   通常のキーには 「離した」 事を知らせる通知が無く、 押された時 (とキー
*   リピート) のコードしか届かないので、 「今押されているか」 を知る手段が
*   無い。 BREAK キーだけは別で、 専用ステータス $FD04 bit1 に押下中ずっと
*   出続ける (active-low) ので、 押されているかを直接読める。
*
*   そのため本モジュールは次の 2 本立てにする。
*
*   (A) 方向は 「状態保持型」 にする
*       テンキー 8/2/4/6 を押すと方向が確定し、 キーを離してもその方向を
*       保持し続ける。 テンキー 5 で停止する。 離鍵の通知が要らない。
*
*   (B) ショットは BREAK に割り当て、 周期タイマ IRQ でサンプリングして
*       押下エッジを数え、 押下状態が続く間はオート連射を生成する。
*
*   キーコードの受信は input_edges() (受信したキーの種類のビット和) と
*   esc_edges() (ESC の受信回数) で C 側へ渡す。 どちらもキーリピートの分を
*   含む。 押し直しを見分けたい時は、 呼ぶ側がキーコードの一定フレームの
*   途切れを見てから、 次の受信を新しい押下として扱う。
*
* 【なぜ BREAK を FIRQ にしないか】
*   $FD04 bit1 はレベル出力で FIRQ の要因でもある。 FIRQ を許可すると押下中
*   は FIRQ が発火し続けてメインループが進まなくなる。 よって FIRQ はマスク
*   のまま、 タイマ IRQ (約 2ms) の中でポーリングする。
*
* C API は c_input.h を参照。 _input_tick と _input_feed_key は IRQ ハンドラ
* (asm_timer.s) が呼ぶ内部の入口である。
* ============================================================

IO_BREAK        equ     $FD04           * R: bit1 = BREAK (0 = 押下、 active-low)

* --- 入力ビット定義 (c_input.h の IN_* と必ず一致させること) ---
IN_UP           equ     $01
IN_DOWN         equ     $02
IN_LEFT         equ     $04
IN_RIGHT        equ     $08
IN_STOP         equ     $10
IN_START        equ     $80

IN_VMASK        equ     $03             * 縦軸 (UP|DOWN)
IN_HMASK        equ     $0C             * 横軸 (LEFT|RIGHT)
IN_DIRMASK      equ     $0F             * 方向 4 ビット
* 上の各マスクのビット反転 (= 「この軸だけ落とす」 and マスク)。
* 単項演算子を使わず、 $FF からの引き算で作る。
IN_NVMASK       equ     $FF-IN_VMASK    * 縦軸だけ落とす
IN_NHMASK       equ     $FF-IN_HMASK    * 横軸だけ落とす
IN_NDMASK       equ     $FF-IN_DIRMASK  * 方向 4 ビットを全部落とす

* --- 調整用定数 ---
* BRK_DEBOUNCE: BREAK の状態変化を確定させるのに必要な連続一致 tick 数。
*   1 tick ≒ 2ms (2.035ms) なので 3 tick ≒ 6ms。 同じレベルが 3 tick 続いた
*   時だけ状態を確定し、 接点のチャタリングを吸収する。
BRK_DEBOUNCE    equ     3

* AUTO_PERIOD: オート連射の周期 (tick 数)。
*   30 tick ≒ 61ms ≒ 16.4 発/秒。 押下状態が続く間、 この周期で 1 発ずつ積む。
*   人間の連打がこれを上回った場合は連打が優先される (下の合成規則を参照)。
AUTO_PERIOD     equ     30
KEY_ESC         equ     $1B             * ESC のキーコード (esc_edges で数える)

                section code

                export  _input_init
                export  _input_dir
                export  _input_dir_clear
                export  _input_edges
                export  _break_shots
                export  _break_held
                export  _esc_edges
                export  _input_tick         * IRQ ハンドラ (asm_timer.s) が呼ぶ
                export  _input_feed_key     * IRQ ハンドラ (asm_timer.s) が呼ぶ


* ------------------------------------------------------------
* void input_init(void) — 内部状態を初期化する (起動時 1 回)
*   BREAK の初期レベルを今の実状態に合わせ、 起動直後に偽のショットが
*   1 発入るのを防ぐ。 初期化の間は IRQ をマスクする。
* ------------------------------------------------------------
_input_init:
                pshs    cc
                orcc    #$10            * IRQ マスク (初期化中に tick が入らないように)
                clr     dir_state
                clr     edge_state
                clr     brk_shots
                clr     esc_cnt
                clr     brk_dbnc
                lda     #AUTO_PERIOD
                sta     brk_auto
                lda     IO_BREAK        * 今の BREAK レベルを初期状態にする
                anda    #$02
                beq     .ipressed       * 0 = 押されている (active-low)
                clr     brk_state
                bra     .idone
.ipressed       lda     #1
                sta     brk_state
.idone          puls    cc
                rts


* ------------------------------------------------------------
* unsigned char input_dir(void) — 移動方向ビットを返す
*   bit0=上 bit1=下 bit2=左 bit3=右。 斜めは 2 ビット同時に立つ。
*   1 バイト読みは 6809 の単一命令なので IRQ に割られない (マスク不要)。
* ------------------------------------------------------------
_input_dir:
                ldb     dir_state       * キーの保持方向
                clra
                rts


* ------------------------------------------------------------
* void input_dir_clear(void) — 方向を停止状態に戻す (テンキー 5 と同じ効果)
* ------------------------------------------------------------
_input_dir_clear:
                clr     dir_state
                rts


* ------------------------------------------------------------
* unsigned char input_edges(void) — 前回呼出以降に受信したキーの種類のビット和
*   同じキーを何度受信しても 1 ビットなので、 回数と順序は分からない
*   (キーリピートの分も含む)。 読み出しと同時にクリアする。
*   read + clear を atomic にするため IRQ を一瞬マスクする。
* ------------------------------------------------------------
_input_edges:
                pshs    cc
                orcc    #$10
                ldb     edge_state
                clr     edge_state
                puls    cc
                clra
                rts


* ------------------------------------------------------------
* unsigned char break_shots(void) — 前回呼出以降に発生したショット数
*   (人間の押下エッジ + オート連射の合計)。 読み出すとクリアする。
*   255 で飽和する (= wrap して 0 に戻らない)。
* ------------------------------------------------------------
_break_shots:
                pshs    cc
                orcc    #$10
                ldb     brk_shots
                clr     brk_shots
                puls    cc
                clra
                rts


* ------------------------------------------------------------
* unsigned char esc_edges(void) — 前回呼出以降に ESC のキーコードが来た回数
*   ESC は回数を保持するためのカウンタ (esc_cnt) で数える
*   (input_feed_key の頭で拾う)。 キーリピートの分も
*   1 回ずつ数える。 呼ぶ側はキーコードが一定フレーム途切れた後の受信を
*   新しい押下として扱う。 255 で飽和する。
*   read + clear は break_shots と同じく IRQ を一瞬マスクして行う。
* ------------------------------------------------------------
_esc_edges:
                pshs    cc
                orcc    #$10
                ldb     esc_cnt
                clr     esc_cnt
                puls    cc
                clra
                rts


* ------------------------------------------------------------
* unsigned char break_held(void) — BREAK 押下中なら 1 (チャタリング除去済)
* ------------------------------------------------------------
_break_held:
                ldb     brk_state
                clra
                rts


* ------------------------------------------------------------
* void input_tick(void) — 周期タイマ IRQ (約 2ms) から呼ばれる
*
*   BREAK をサンプリングし、 チャタリング除去 → 押下エッジを数える
*   → 押下状態が続く間はオート連射を積む。
*
*   【人間の連打とオート連射の合成】
*     押下エッジが立った瞬間に 「1 発足す」 と同時に brk_auto を
*     AUTO_PERIOD へ 「巻き戻す」。 よって
*       ・連打が AUTO_PERIOD より速い → オートが満了する前に毎回巻き戻る
*         ので、 オートは 1 発も撃たず 「連打の回数そのもの」 が出る (連打優先)。
*       ・押しっぱなし → オートが満了して AUTO_PERIOD ごとに 1 発ずつ
*         積まれる (押下状態が続いている間だけ。 離した確定で止まる)。
*     二重計上は起きない。
* ------------------------------------------------------------
_input_tick:
                * --- 発射のレベルを読む (A = 0:離 / 1:押) ---
                lda     IO_BREAK
                anda    #$02
                beq     .braw1          * 0 = BREAK 押下 (active-low)
                clra
                bra     .bcmp
.braw1          lda     #1
.bcmp
                cmpa    brk_state
                bne     .bchg
                clr     brk_dbnc        * 現状と一致 → チャタリング計数をリセット
                bra     .bauto
.bchg
                inc     brk_dbnc        * 現状と違う → 連続一致数を数える
                ldb     brk_dbnc
                cmpb    #BRK_DEBOUNCE
                blo     .bauto          * まだ確定しない
                clr     brk_dbnc
                sta     brk_state       * 新しい状態を確定
                tsta
                beq     .tdone          * 離した確定 → 何も撃たない
                bsr     .addshot        * 押した確定 = 立ち上がりエッジ → 1 発
                lda     #AUTO_PERIOD
                sta     brk_auto        * オート連射の位相を巻き戻す
                bra     .tdone
.bauto
                lda     brk_state
                beq     .tdone          * 離しているならオート連射しない
                dec     brk_auto
                bne     .tdone
                bsr     .addshot        * オート連射 1 発
                lda     #AUTO_PERIOD
                sta     brk_auto
.tdone
                rts

* ショット数を 1 増やす (255 で飽和)。 _input_tick からのみ呼ぶ。
.addshot
                lda     brk_shots
                cmpa    #$FF
                beq     .as_done
                inca
                sta     brk_shots
.as_done
                rts


* ------------------------------------------------------------
* void input_feed_key(void) — B = 受信したキーコード。 IRQ ハンドラが呼ぶ
*
*   keytab を引いて、 方向の保持状態 (dir_state) と 受信ビット (edge_state)
*   を更新する。
*
*   方向の更新は 「軸ごとの置き換え」 で行う:
*     dir_state = (dir_state & andmask) | ormask
*   縦の指示 (8/2) は縦軸だけを、 横の指示 (4/6) は横軸だけを書き換えるので、
*   8 を押した後に 6 を押せば自然に 「右上」 (2 ビット同時) になる。
*   反対方向 (8 の後に 2 など) は同じ軸を書き換えるので 「後から入った方が
*   勝つ」 (相殺して停止にはしない)。
* ------------------------------------------------------------
_input_feed_key:
                cmpb    #KEY_ESC        * ESC は keytab を引かずに数えるだけ
                bne     .fnesc
                lda     esc_cnt
                cmpa    #$FF            * 255 で飽和
                beq     .fdone
                inca
                sta     esc_cnt
                rts
.fnesc
                stb     feed_code
                ldx     #keytab
.fl             lda     ,x
                beq     .fdone          * テーブル終端 (code = 0)
                cmpa    feed_code
                beq     .fhit
                leax    4,x
                bra     .fl
.fhit
                lda     dir_state
                anda    1,x             * 対象の軸をクリア
                ora     2,x             * 新しい方向を立てる
                sta     dir_state
                lda     3,x             * 受信ビット
                ora     edge_state
                sta     edge_state
.fdone
                rts


* ------------------------------------------------------------
* キーコード変換テーブル
*   1 エントリ 4 byte: [キーコード, dir の and マスク, dir の or マスク,
*                      input_edges() に立てるビット]
*   キーコード 0 で終端。
*
*   この機種のキーボードは押されたキーの文字コードを返すので、
*   テンキーの数字は普通の数字と同じコードで来る ('8' = $38 など)。
*   カーソルキーは制御コード $1C-$1F に割り当てられている。
* ------------------------------------------------------------
keytab
                * --- テンキー 8/2/4/6 = 上/下/左/右 (主操作) ---
                fcb     $38,IN_NVMASK,IN_UP,IN_UP               * '8' 上
                fcb     $32,IN_NVMASK,IN_DOWN,IN_DOWN           * '2' 下
                fcb     $34,IN_NHMASK,IN_LEFT,IN_LEFT           * '4' 左
                fcb     $36,IN_NHMASK,IN_RIGHT,IN_RIGHT         * '6' 右
                * --- テンキー 5 = 停止 (方向 4 ビットを全部落とす) ---
                fcb     $35,IN_NDMASK,$00,IN_STOP               * '5' 停止
                * --- テンキー 7/9/1/3 = 斜めの直接指定 (両軸を同時に置換) ---
                fcb     $37,IN_NDMASK,IN_UP+IN_LEFT,IN_UP+IN_LEFT       * '7' 左上
                fcb     $39,IN_NDMASK,IN_UP+IN_RIGHT,IN_UP+IN_RIGHT     * '9' 右上
                fcb     $31,IN_NDMASK,IN_DOWN+IN_LEFT,IN_DOWN+IN_LEFT   * '1' 左下
                fcb     $33,IN_NDMASK,IN_DOWN+IN_RIGHT,IN_DOWN+IN_RIGHT * '3' 右下
                * --- カーソルキー (代替操作) ---
                fcb     $1E,IN_NVMASK,IN_UP,IN_UP               * カーソル 上
                fcb     $1F,IN_NVMASK,IN_DOWN,IN_DOWN           * カーソル 下
                fcb     $1D,IN_NHMASK,IN_LEFT,IN_LEFT           * カーソル 左
                fcb     $1C,IN_NHMASK,IN_RIGHT,IN_RIGHT         * カーソル 右
                * --- 方向以外 (dir は変えない = and $FF / or $00) ---
                fcb     $0D,$FF,$00,IN_START                    * RETURN 開始/決定
                fcb     $00,$00,$00,$00                         * 終端

                section bss
dir_state       rmb     1               * 保持中の移動方向 (IN_UP/DOWN/LEFT/RIGHT)
edge_state      rmb     1               * 前回 input_edges() 以降の受信ビット和
feed_code       rmb     1               * _input_feed_key の作業用 (受信コード)
brk_state       rmb     1               * BREAK の確定状態 (0 = 離 / 1 = 押)
brk_dbnc        rmb     1               * BREAK チャタリング除去の連続一致数
brk_auto        rmb     1               * オート連射までの残り tick
brk_shots       rmb     1               * 未回収のショット数 (255 で飽和)
esc_cnt         rmb     1               * 未回収の ESC の回数 (255 で飽和)

                end
