* ============================================================
* asm_timer.s — メイン CPU の周期タイマ IRQ (約2ms) を数える
*               「経過 tick カウンタ」フレームペーシング (deadline 方式)
*
* 【設計】 deadline 方式:
*   - FM-7 メイン 6809 には約 2ms 周期 (約 491Hz) のタイマ IRQ がある
*     (要因フラグ $FD03 bit2、 許可は $FD02 bit2)。 これを IRQ ハンドラ
*     _irq_isr で数え、 16-bit カウンタ frame_tick を ++ する (= 自走する
*     経過 tick カウンタ。 1 tick ≈ 2ms)。
*   - メインループは timer_get() が FRAME_TARGET に達するまで待ち、
*     timer_consume(FRAME_TARGET) で frame_tick からそのぶんを差し引く
*     (超過分は次のフレームへ持ち越す。 持ち越しは 1 フレームぶんまで)。
*     処理が目標時間内に収まる場合は、 1 フレームの実時間が FRAME_TARGET
*     × 2ms に揃う (IRQ は処理中も数え続ける)。 timer_start() は起動時に
*     1 回、 カウンタを 0 にするのに使う。
*
* FIRQ はマスクのままにする (BREAK キーが FIRQ 要因でもあるため。 理由は
* asm_input.s の冒頭)。
*
* C API (c_subprog.h):
*   void     timer_init(void)   起動時 1 回。 IRQ ベクタ設置 + タイマ IRQ 許可。
*   void     timer_start(void)  経過カウンタ frame_tick を 0 に戻す (起動時)。
*   unsigned timer_get(void)    経過 tick (16-bit、 1≈2ms)。
*   void     timer_consume(unsigned n)  frame_tick から n を差し引く。
*   unsigned char irq_save(void) / void irq_restore(unsigned char cc)
*                               短い I/O の列を IRQ から守る。
* ペーシングはメインループ先頭で
*     guard = 0;
*     while (timer_get() < FRAME_TARGET && guard < PACE_SAFETY_CAP) guard++;
*     timer_consume(FRAME_TARGET);
* と書く (= c_main.c。 guard はハング防止の上限)。
*
* ※ 前提にしている動き:
*   ・IRQ ベクタ $FFF8 は IPL が RAM モード ($FD0F write) にして以降 RAM。
*   ・タイマ IRQ フラグ $FD03 bit2 は active-low (0=発生)。 $FD03 read で
*     ack される前提 (= クリアされないと IRQ が再発火し続ける)。
*   ・キーボード IRQ ($FD02 bit0) も許可し、 IRQ ハンドラがキーコードを
*     受け取る (= キー入力は IRQ 駆動)。
*
* 【ゲーム用入力モジュールとの連携 (asm_input.s)】
*   IRQ ハンドラは要因ごとに次を呼ぶ。
*     ・タイマ要因のとき  _input_tick     — BREAK のサンプリング (エッジ検出 +
*       チャタリング除去 + オート連射)。
*     ・キー要因のとき    _input_feed_key — キーコード (B) を方向保持状態と
*       受信ビットへ反映する。
* ============================================================

IO_IRQFLAG      equ     $FD03           * R: IRQ 要因 (active-low。 bit2=タイマ, bit0=キーボード)
IO_IRQMASK      equ     $FD02           * W: IRQ 許可 (bit2=タイマ, bit0=キーボード)
IO_KEYDATA      equ     $FD01           * R: キーコード (= read でキーボード IRQ を ack)
TIMER_BIT       equ     $04             * $FD03/$FD02 bit2 = タイマ
KBD_BIT         equ     $01             * $FD03/$FD02 bit0 = キーボード
IRQ_VECTOR      equ     $FFF8           * 6809 IRQ ベクタ (RAM モードで RAM)

                section code

                export  _timer_init
                export  _timer_start
                export  _timer_get
                export  _timer_consume
                export  _irq_save
                export  _irq_restore
                import  _input_tick     * asm_input.s: 2ms ごとの BREAK サンプリング等
                import  _input_feed_key * asm_input.s: キーコード (B) を入力状態へ反映


* void timer_init(void) — IRQ ベクタを設置し、 タイマ IRQ を許可する (起動時 1 回)。
*   キーボード IRQ (bit0) も合わせて許可する (= キー入力を IRQ 駆動にするため)。
_timer_init:
                ldd     #_irq_isr
                std     IRQ_VECTOR      * $FFF8/$FFF9 に handler のアドレス
                lda     #TIMER_BIT+KBD_BIT  * bit2=タイマ + bit0=キーボード IRQ 許可
                sta     IO_IRQMASK
                andcc   #$EF            * I フラグ解除 = IRQ 許可 (F は立てたまま)
                rts


* void timer_start(void) — 経過 tick カウンタを 0 に戻す。
*   2 byte を std 1 命令で書くので、 IRQ ハンドラの ++ と割り込み合わない。
_timer_start:
                ldd     #0
                std     frame_tick
                rts


* unsigned timer_get(void) — 経過 tick (16-bit) を D で返す。
*   ldd は 1 命令なので IRQ に割られない (マスク不要)。
_timer_get:
                ldd     frame_tick
                rts


* void timer_consume(unsigned n) — 経過 tick カウンタから n を差し引く。
*
*   フレームペーシングの締めで呼ぶ。 差し引きにすると、 締切を過ぎたフレーム
*   の超過が翌フレームへ持ち越され、 締切より早く終わったフレームがその借り
*   を返す (持ち越しの上限 n に当たらない範囲で超過を吸収する)。
*
*   - 差し引いた残りが n を超えるときは n に丸める (= 借りは 1 フレーム
*     ぶんまで。 ステージの切替の CLS など数フレーム級の停止の後で、 早回しの
*     追い掛けが何フレームも続くのを防ぐ)。
*   - n 未満しか経っていないとき (= ペーシングの guard で抜けた場合)
*     は 0 にする (= timer_start と同じ)。
*   - 読んで引いて書き戻す間に IRQ ハンドラが ++ すると 1 tick 失うので、
*     その間だけ IRQ をマスクする。 入口の CC を控えて出口で戻す
*     (irq_restore と同じく、 無条件に許可へ倒さない)。
*   - 比較は符号なし (bcc / bls) で行う。
_timer_consume:
                ldd     2,s             * n
                pshs    cc              * 入口の IRQ マスク状態を控える
                orcc    #$10            * IRQ を一時停止
                pshs    b,a             * n を作業用に退避
                ldd     frame_tick
                subd    ,s              * 経過 - n
                bcc     .tc_pos         * 借りなし (経過 >= n)
                ldd     #0              * n 未満なら 0 (= timer_start と同じ)
                bra     .tc_store
.tc_pos:
                cmpd    ,s
                bls     .tc_store       * 余り <= n はそのまま持ち越す
                ldd     ,s              * 借りは 1 フレームぶんに丸める
.tc_store:
                std     frame_tick
                puls    a,b
                puls    cc              * 入口の IRQ マスク状態へ戻す
                rts


* unsigned char irq_save(void) — マスク前の CC を返して IRQ をマスクする。
*
*   irq_restore と組で使い、 途中で割込まれたくない短い I/O の列を囲む。
*   使いどころは c_sound.c の psg_write で、 PSG のレジスタ選択と書き込みの
*   列の途中で IRQ の処理 (タイマとキー入力) に割り込まれないようにする。
*   戻り値: B = マスク前の CC (= そのまま irq_restore へ渡す)。
_irq_save:
                tfr     cc,b            * マスク前の CC を控える
                orcc    #$10            * IRQ マスク
                clra
                rts


* void irq_restore(unsigned char cc) — irq_save の控えどおりに IRQ を戻す。
*
*   ★ 無条件に許可へ倒してはいけない (呼び出し元が元々 IRQ マスク中かも
*     しれない)。 控えの I ビットが 0 だった時だけ許可へ戻す。 他のビットは
*     触らない。
*   CMOC は char 引数を 16-bit へ昇格してスタックに積むので、 値 (下位) は
*   3,s に居る。
_irq_restore:
                ldb     3,s             * 控えた CC (昇格ワードの下位)
                andb    #$10
                bne     .ir_done        * 元からマスク中 → 触らない
                andcc   #$EF            * 元は許可 → 許可へ戻す
.ir_done:
                rts


* --- メイン IRQ ハンドラ (タイマ + キーボード) ---
*   IRQ は全レジスタを自動 push する (E=1) ので、 ここと jsr 先で A/B/X/Y/CC
*   を自由に使って RTI で復帰できる。 $FD03 を 1 回読み (= タイマ ack)、
*   タイマ(bit2)なら frame_tick++、 キーボード(bit0)なら $FD01 を読んで
*   (= キーボード ack) _input_feed_key へ渡す。
_irq_isr:
                lda     IO_IRQFLAG      * $FD03 read (= 要因判別 + タイマ ack)
                pshs    a               * 要因を退避 (以下の jsr が A を壊すため)
                bita    #TIMER_BIT
                bne     .chk_kbd        * bit2=1 → タイマでない
                inc     frame_tick+1    * 16-bit ++ (下位)
                bne     .tick_in
                inc     frame_tick      * 桁上げ (上位)
.tick_in:
                jsr     _input_tick     * BREAK サンプリング + オート連射
.chk_kbd:
                lda     ,s              * 退避した $FD03 の値を読み直す
                bita    #KBD_BIT
                bne     .ii_done        * bit0=1 → キーボードでない
                ldb     IO_KEYDATA      * $FD01 read (= キーボード ack、 B=キーコード)
                cmpb    #$FF
                beq     .ii_done        * バス浮き ($FF) は無視
                tstb
                beq     .ii_done        * $00 も無視 (= 「キー無し」 と同義)
                jsr     _input_feed_key * B = キーコード → 方向保持/受信ビットへ反映
.ii_done:
                puls    a               * 退避した要因を捨てる
                rti


                section bss
frame_tick      rmb     2               * 16-bit 経過 tick (IRQ が ++、 1≈2ms)

                end
