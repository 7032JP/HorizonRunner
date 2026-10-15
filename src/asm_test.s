* ============================================================
* asm_test.s — main から「TEST コマンド」 経由でサブ
*                  CPU にプログラム転送 + 実行する API
*
* TEST cmd は サブシステム ROM の 拡張コマンド領域の CMD $3F
* にある TEST/DEBUG 機能。 共有 RAM にコマンド列 (下のレイアウトの
* $FC80-$FC82 のヘッダ) とサブコマンド ($91 MOVE / $93 CALL / $90 END)
* を並べて発火すると、 サブ CPU 上で memory copy や任意アドレス JSR を
* 実行してくれる。
*
* 本作では:
*   - 自前の sub プログラム (= asm_subprog.s。絵と模様表を含む) を sub の
*     SUB_PROG_ADDR ($C100、config.mk) に転送
*   - sub_call(SUB_PROG_ADDR) で sub プログラムを 1 回起動 (= 共有 RAM の
*     コマンドキューを先頭から終端まで処理して RTS)
* という用途で使う。
*
* 共有 RAM レイアウト (main 側):
*   $FC80    : $00          ; 未使用
*   $FC81    : $00          ; 未使用
*   $FC82    : $3F          ; TEST コマンド
*   $FC83-$FC8A: 8 byte     ; 描画プログラムへの固定パラメータ (= サブ
*                           ; システム ROM はこの 8 byte を読まないので、
*                           ; 自前の用途に使う。並びは c_subprog.h /
*                           ; asm_subprog.s の表のとおり。_sub_param_copy
*                           ; が書き、.build_header は触らない)
*   $FC8B    : サブコマンド ; $91=MOVE / $93=CALL / $90=END
*   $FC8C+   : サブコマンド パラメータ
*     MOVE   : src(2) + dst(2) + len(2) + END
*     CALL   : addr(2) + END
*   $FC93-$FCFF: 描画コマンドキュー (108 byte + 終端 1 byte。c_subprog.h)
*
* HALT プロトコル:
*   1. HALT 要求 ($FD05=$80)
*   2. wait BUSY ($FD05 bit7=1) ← 省略不可 (省くと write 反映されない)
*   3. 共有 RAM に TEST cmd 列を書込
*   4. RELEASE ($FD05=$00) で sub 実行開始
*
* ============================================================

IO_SUBCTRL      equ     $FD05
SHARED_RAM      equ     $FC80
SHARED_SUB      equ     $D3A0           * sub 側から見た $FCA0 (= 共有 RAM 後半)

* ---- sub_wait_ready の空転上限 -------------------------------------
* BUSY ($FD05 bit7) が落ちるのを待つループの上限。 超えたら待つのをやめて
* 戻る (サブが応答しない場合に待ち続けないため。 c_main.c の
* PACE_SAFETY_CAP と同じ流儀)。
*
* 保証できるのはループの回数と cycle 数で、 時間への換算は次の前提に
* よる: メイン CPU 約 1.2MHz (1 cycle 約 0.833us)、 サブ CPU 約 2MHz、
* サブは VRAM ゲート ($D409) を開けている間 全命令一律 約 32% の速度。
* cycle 数は 6809 の命令表から数えた値。
*
* ループ 1 周 = lda ext(5) + bpl(3) + leax -1,x(5) + bne(3) = 16 cycle。
* 40,000 周 = 640,000 main cycle (上の前提で約 533ms)。
*   いちばん長い 1 命令は 1 プレーンぶんの CLS (16,000 byte。 asm_subprog.s
*   の cmd_cls は std ,x++ / leay -1,y / bne の 16 cycle x 8,000 周 =
*   128,000 sub cycle。 ゲート開の前提で約 200ms)。 上限はその約 2.7 倍。
*   上限を超えて main が先に戻ると、 次の sub_flush() が CLS の途中で
*   サブを HALT してコマンドキューを上書きし、 画面が壊れる。
*   CLS をプレーンごとに 1 命令へ割って 1 回の呼び出しを上限内に収めて
*   いるのはそのため (asm_subprog.s の cmd_cls のコメント参照)。
SUB_WAIT_CAP    equ     40000

* ---- sub_wait_busy の空転上限 (= BUSY 立ち上がり待ち) ---------------
* RELEASE してからサブシステム ROM が 「コマンドを受け取った」 印として
* BUSY を立てるまでには遅れがある (サブがコマンド待ちのループで $D382 を
* 見に来るまでの時間で、 数十から数百 main cycle)。 一方 main が RELEASE
* 後最初に $FD05 を読むのは約 41 cycle 後なので、 **「まだ BUSY が
* 立っていない」 状態を読んでしまう**ことがある。 そのまま
* sub_wait_ready に入ると bit7=0 が即座に成立し、 サブが 1 命令も
* 実行しないうちに 「完了した」 として戻ってしまう。
*
* 通常のバッチは、 命令の cycle 数を足すとおよそ 8,000〜15,000 sub cycle
* (情景の塗りが 1 行 106 cycle x 約 40 行、 自機の貼り付けが約 1,600 cycle、
* 絵の貼り付けと消し込みが 1 個あたり約 350〜1,000 cycle (6 行 x 2 byte
* から 14 行 x 4 byte まで)。 asm_subprog.s の fill_rows / do_sprite /
* blit_obj / erase_rect)。 これは待ちを含まない命令の cycle 数で、
* サブ CPU 2MHz で 4〜8ms、 VRAM ゲートを開けている間の 32% 速度で見れば
* 12〜23ms に当たる。 CLS (約 200ms) はこれより桁違いに長いので、 完了待ちの
* 上限 (SUB_WAIT_CAP) は CLS に合わせてある。
*
* そこで **完了待ちの前に立ち上がりを待つ**。 上限 400 周
* (= 6,400 cycle = 約 5.3ms) は上記の遅れに対して十分に長い。 上限に
* 達した時 (= サブがコマンドを拾っていない) は、 そのまま完了待ちへ進む。
SUB_BUSY_CAP    equ     400

* ---- 保険待ち (= BUSY 落ち直後に TEST を書かないための猶予) --------
* $FD05 bit7 が落ちてから サブシステム ROM がコマンドの後始末を終えて
* コマンド待ちループに戻るまでには、 サブ側で数十命令ぶんの隙がある。
* この隙に新しい TEST コマンド列を書いて RELEASE すると、 そのバッチが
* 丸ごと拾われずに捨てられる。
*
* ループ 1 周 = decb(2) + bne(3) = 5 cycle。 60 周 = 300 cycle
* = 300 x 0.833us = 250us = 0.25ms。 この間サブは VRAM ゲートを閉じて
* ROM へ戻っている (asm_subprog.s の qend) ので約 2MHz で約 500 sub
* cycle あり、 ROM ハンドラの後始末 (数十命令) を跨ぐのに十分。
SUB_POST_WAIT   equ     60

                section code

                export  _sub_wait_ready
                export  _sub_wait_busy
                export  _sub_halt
                export  _sub_release
                export  _sub_call
                export  _sub_call_async
                export  _sub_sync
                export  _sub_queue_copy
                export  _sub_param_copy
                export  _sub_takeover


* void sub_wait_ready(void) — $FD05 bit7=0 まで待つ (上限付き)
*
* $FD05 の bit7 = 1 がサブ CPU の HALT 中または BUSY 中。
*
* ★ BUSY はサブシステム ROM 自身が上げ下げしており、 **立ち下がりは
*   「コマンド処理完了」 を意味する**。 自前の subprog は ROM の TEST CALL
*   ハンドラの内側で走るので、 subprog の実行区間はまるごと BUSY=1 の
*   区間に含まれる。
*
* ★ ただし **単独で使ってはならない**。 RELEASE 直後は 「まだ立っていない
*   BUSY」 を読んで即抜けすることがある (SUB_BUSY_CAP のコメント参照)。
*   必ず _sub_wait_busy と対にすること (= _sub_call がそうしている)。
*
* X を破壊しない (= 呼び出し側の都合を変えないため pshs/puls で退避)。
_sub_wait_ready:
                pshs    x
                ldx     #SUB_WAIT_CAP
.wr:
                lda     IO_SUBCTRL
                bpl     .wr_done        * bit7=0 → ready
                leax    -1,x
                bne     .wr
                                        * 上限到達 = 待つのをやめて戻る
.wr_done:
                puls    x,pc


* void sub_wait_busy(void) — $FD05 bit7=1 (= サブがコマンドを拾った) まで待つ
*
* **RELEASE の直後にだけ使う。** 「サブがまだ動き出していないのに
* sub_wait_ready が即抜けする」 事故 (= SUB_BUSY_CAP のコメント参照) を
* 塞ぐためのもので、 これと sub_wait_ready を必ずこの順で対にする。
*
* 非同期パス (sub_sync) では使わない。 立ち上がりは sub_call_async が
* 待ち済みなので、 あちらは完了待ちだけでよい。
*
* X を破壊しない。
_sub_wait_busy:
                pshs    x
                ldx     #SUB_BUSY_CAP
.wb:
                lda     IO_SUBCTRL
                bmi     .wb_done        * bit7=1 → サブが受け取った
                leax    -1,x
                bne     .wb
                                        * 上限到達 = 受理を確認できないまま戻る
.wb_done:
                puls    x,pc


* void sub_sync(void) — 非同期で投げたバッチの完了待ち (+ 保険待ち)
*
* _sub_pending が 0 のとき (= 投げっぱなしのバッチが無いとき) は何も
* せずに戻る (= jsr の呼び出しを含めて約 23 cycle)。
*
* 非同期バッチが飛んでいるときだけ:
*   1. BUSY が落ちるまで待つ (上限付き)
*   2. SUB_POST_WAIT ぶん保険待ちする
* を行い、 pending を落とす。
_sub_sync:
                tst     _sub_pending
                beq     .sy_done
                clr     _sub_pending    * 先に落とす (= 再入しても二重待ちしない)
                lbsr    _sub_wait_ready
                pshs    b
                ldb     #SUB_POST_WAIT
.sy_w:
                decb
                bne     .sy_w
                puls    b
.sy_done:
                rts

* void sub_halt(void) — HALT 要求 → bit7=1 待ち (= HALT 完了確認)
*   先に BUSY が下りるのを待つことはしない。 HALT 要求はサブが
*   BUSY でも出してよく、 先に待つとサブが BUSY のままの状態 (warm start
*   など) で永久に抜けられない。
*   呼び手は sub_sync / sub_wait_ready を通した後 (= サブが BUSY で
*   ない状態) で呼ぶので、 bit7=1 は HALT の受理を意味する。 BUSY 中にも
*   呼べる subsys_halt (asm_subsys.s) とはそこが違う。
_sub_halt:
                lda     #$80
                sta     IO_SUBCTRL
.bw:            lda     IO_SUBCTRL
                bpl     .bw
                rts

* void sub_release(void) — HALT 解除
_sub_release:
                clr     IO_SUBCTRL
                rts


* ---- TEST cmd ヘッダを $FC80 に書き、 X = $FC8B (subcmd 先) を返す
*   $FC83-$FC8A (固定パラメータ 8 byte) には触らない。 そこは
*   _sub_param_copy が flush のたびに書く。
.build_header:
                ldx     #SHARED_RAM
                clr     ,x+             * $FC80
                clr     ,x+             * $FC81
                lda     #$3F
                sta     ,x              * $FC82 TEST
                leax    9,x             * $FC83-$FC8A を飛ばす
                rts                     * X = $FC8B


* ---- TEST CALL 列を組んで RELEASE するところまで (= 待たない)
*   entry: D = addr
*   先頭で _sub_sync を通すので、 完了待ちが上限内に終わる限り、 非同期
*   バッチが飛んでいる最中に HALT してしまう事故 (= サブが読んでいる
*   最中のキュー上書き) は起きない。
.issue_call:
                pshs    a,b             * addr 退避 (halt/build_header が D を壊す)
                lbsr    _sub_sync       * 前の非同期バッチの完了待ち (同期時 no-op)
* wait_ready は省略 (= sub_halt が内包する bit7 待ちで HALT 完了確認できる)。
* 呼び手 (c_subprog.c の sub_flush / subprog_init) は sub_halt の直後
* (HALT 中で bit7=1) に呼ぶので、 先に wait_ready すると待機の上限
* (SUB_WAIT_CAP) まで待ってしまう。
                lbsr    _sub_halt
                lbsr    .build_header   * X = $FC8B
                lda     #$93
                sta     ,x+             * CALL
                puls    a,b             * addr 復帰
                std     ,x++            * addr
                lda     #$90
                sta     ,x              * END
                lbsr    _sub_release
                rts


* void sub_call(unsigned addr)  — 同期 (完了を待つ)
*   addr に JSR 相当で飛ぶ (= sub の RTS まで待ってから戻る)
*   stack: [ret:2] [addr:2]
_sub_call:
                pshs    u
                ldd     4,s             * addr
                lbsr    .issue_call
                lbsr    _sub_wait_busy  * まずサブが拾うのを待つ (= 即抜け防止)
                lbsr    _sub_wait_ready * そのうえで sub の RTS 完了を待つ
                puls    u,pc


* void sub_call_async(unsigned addr)  — 非同期 (= 完了を待たずに戻る)
*   RELEASE してサブが受け取った (BUSY が立った) ことを確かめたら戻り、
*   メイン CPU はサブと並列に走る。 完了待ちは次に _sub_sync を通る所
*   (= sub_flush / sub_flush_async / sub_sync の直接呼び出し /
*   sub_takeover / subsys_halt / sub_cancel) が肩代わりする。
*   stack: [ret:2] [addr:2]
_sub_call_async:
                pshs    u
                ldd     4,s             * addr
                lbsr    .issue_call
* 立ち上がりだけは待つ。 ここを省くと 「サブがまだ拾っていない」 状態で
* pending を立てることになり、 次に sub_sync が見る BUSY が 「前の
* バッチのもの」 なのか 「まだ始まっていないだけ」 なのか区別できず、
* 完了待ちが無意味になる。
* 完了そのものは待たない (= 非同期の目的はここで戻ること)。
                lbsr    _sub_wait_busy
                lda     #1
                sta     _sub_pending    * 「投げっぱなしのバッチが有る」 印
                puls    u,pc


* void sub_queue_copy(const void *src, unsigned char *dst, unsigned n)
*   src から dst へ n byte コピーし、 dst[n] に $00 (= SUBCMD_END) を置く。
*   stack: [ret:2] [src:2] [dst:2] [n:2]
*
*   1 byte あたり 17 cycle (lda ,x+ 6 + sta ,y+ 6 + decb 2 + bne 3)。
*
*   ※ S をコピー用ポインタに転用してはならない。 メイン CPU のタイマ IRQ
*     (asm_timer.s、 約 2ms) が割り込むと S の指す先にマシン状態 12 byte
*     を積むためデータが壊れる。 X / Y / U のみを使うこと。
*   ※ n は 0..255 を前提 (= 実際は SUB_QUEUE_BYTES = 108 以下。 0 なら
*     終端だけを書く)。 上位 byte は見ない。
_sub_queue_copy:
                pshs    y
* stack: 0,s=saved Y(2) / 2,s=ret(2) / 4,s=src(2) / 6,s=dst(2) / 8,s=n(2)
                ldx     4,s             * src
                ldy     6,s             * dst
                ldb     9,s             * n の下位 byte
                beq     .qc_end
.qc:
                lda     ,x+
                sta     ,y+
                decb
                bne     .qc
.qc_end:
                clr     ,y              * キュー終端 (= SUBCMD_END $00)
                puls    y,pc


* void sub_param_copy(const unsigned char *src)
*   src の 8 byte を共有 RAM の固定パラメータ $FC83-$FC8A へ写す。
*   HALT 中にだけ呼ぶこと (= sub_flush / sub_flush_async の中)。
*   2 byte ずつ 4 回で写す (ldd ,x 5 + std ext 6、 2 組目からは ldd n,x 6
*   + std ext 6。 合計 47 cycle)。
*   stack: [ret:2] [src:2]
_sub_param_copy:
                ldx     2,s             * src
                ldd     ,x
                std     SHARED_RAM+3    * $FC83-$FC84
                ldd     2,x
                std     SHARED_RAM+5    * $FC85-$FC86
                ldd     4,x
                std     SHARED_RAM+7    * $FC87-$FC88
                ldd     6,x
                std     SHARED_RAM+9    * $FC89-$FC8A
                rts


* void sub_takeover(const void *code, unsigned len, unsigned dst, unsigned exec)
*   main RAM 上の code[] を sub の dst へ len byte 転送し、 exec が 0 で
*   なければ sub_call(exec) で実行開始する (0 なら転送のみ)。 共有 RAM
*   後半 $FCA0+ を chunk バッファ (= 最大 64 byte / chunk) として使う。
*   stack: [ret:2] [code:2] [len:2] [dst:2] [exec:2]
*
*   ★ 先頭で _sub_sync を通すこと。 転送は共有 RAM
*     $FCA0+ を chunk バッファに使うので、 非同期で投げたバッチをサブが
*     まだ読んでいる最中に HALT + 上書きすると、 そのバッチのコマンドと
*     パラメータが壊れる。
_sub_takeover:
                lbsr    _sub_sync       * 非同期バッチの完了待ち (同期時 no-op)
                pshs    u,y
                leas    -7,s
* 最終 stack (pshs u,y → leas -7,s 後):
*   0,s  = chunk (1B)
*   1,s  = w_dst (2B)
*   3,s  = w_remain (2B)
*   5,s  = ループ中の X (= 次の src) の退避 (2B)
*   7,s  = saved Y (2B)
*   9,s  = saved U (2B)
*  11,s  = ret addr (2B)
*  13,s  = arg code (2B)
*  15,s  = arg len  (2B)
*  17,s  = arg dst  (2B)
*  19,s  = arg exec (2B)
                ldd     15,s            * len
                std     3,s             * w_remain = len
                ldd     17,s            * dst
                std     1,s             * w_dst = dst
                ldx     13,s            * code → keep in X across loop

.loop:
                ldd     3,s             * remain
                beq     .done
                cmpd    #64
                bls     .szok
                ldd     #64
.szok:
                stb     ,s              * chunk (1B、 最大 64 なので B のみで OK)

                lbsr    _sub_halt
* 1) chunk byte を $FCA0+ にコピー
                ldy     #SHARED_RAM+$20 * = $FCA0
                ldb     ,s
.cp:
                lda     ,x+
                sta     ,y+
                decb
                bne     .cp
* 2) TEST MOVE cmd 構築 (X は保持したいので別レジスタで一旦受け)
                stx     5,s             * save X = next src ptr
                lbsr    .build_header   * X = $FC8B
                lda     #$91
                sta     ,x+             * MOVE
                ldd     #SHARED_SUB     * src (sub から見た $D3A0)
                std     ,x++
                ldd     1,s             * w_dst
                std     ,x++
                clra
                ldb     ,s              * chunk
                std     ,x++            * len
                lda     #$90
                sta     ,x              * END
                lbsr    _sub_release
                lbsr    _sub_wait_busy  * サブが MOVE を拾うのを待つ
                lbsr    _sub_wait_ready * MOVE 完了を待つ
                ldx     5,s             * restore code ptr

* 3) w_dst += chunk, w_remain -= chunk
*
*   w_dst += chunk: 下位 byte に chunk を足し、 その桁上がりを上位 byte
*   に足す。
                ldd     1,s             * D = w_dst (16-bit)
                addb    ,s              * B += chunk (lo)
                adca    #0              * A += carry (hi)
                std     1,s             * w_dst += chunk
*
*   w_remain -= chunk: chunk を 16-bit に符号拡張 (= sex で A = $FF
*   全 byte に伸ばす) して 16-bit 加算で減算する。 chunk は 1-64 の
*   範囲なので negb で 2's complement の負数にしてから sex で OK。
                ldb     ,s
                negb
                sex                     * D = -chunk (16-bit 符号拡張)
                addd    3,s
                std     3,s
                bra     .loop

.done:
                leas    7,s
                ldd     12,s            * exec (= 0 なら CALL skip = 転送のみ)
                beq     .skip
                pshs    b,a
                lbsr    _sub_call
                leas    2,s
.skip:
                puls    u,y,pc


* ---- 非同期バッチの在中フラグ --------------------------------------
* 「sub_call_async で投げたきり、 まだ完了待ちをしていないバッチが有る」
* を表す 1 byte。 _sub_call_async が立て、 _sub_sync が落とす。
*
* ★ bss ではなく code セクションに置いている。 asm_crt0.s の INILIB は
*   ダミー RTS で、 bss のゼロクリアを一切行わないため、 bss に置くと
*   起動時の値が不定になる (= 起動 1 発目のガードが誤発火し得る)。
*   code セクションは本体イメージごとディスクから RAM に展開されるので、
*   ここに置いた fcb 0 は確実に 0 で始まる。
_sub_pending    fcb     0

                end
