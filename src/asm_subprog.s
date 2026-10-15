* ============================================================
* asm_subprog.s — サブ CPU 上で動く描画プログラム (HorizonRunner)
*
* main から TEST MOVE で sub の SUB_PROG_ADDR ($C100) へ転送され、
* sub_call($C100) で実行される。1 回の呼び出しで共有 RAM に積まれた
* コマンド列を先頭から終端まで全部処理し、RTS で呼び手 (サブシステム ROM)
* に戻る。main 側は BUSY が下りるのを待って完了を知る (asm_test.s)。
*
* ------------------------------------------------------------
* 三枚のプレーンの役割 (config.mk を正とする)
* ------------------------------------------------------------
*   B plane ($0000-) / R plane ($4000-) … ダブルバッファリングのページ 0 / 1。
*                                         自機 / 自弾 / 敵を白で描く
*   G plane ($8000-)                    … 緑のカラープレーン。地平線と床 (と main が
*                                         送る文字)。ダブルバッファリングしない
*
* パレットは「表示中のプレーン (B か R) の該当ビットが 1 なら白、0 でも G が 1 なら緑、どちらも 0 なら
* 黒」に潰してある (src/asm_kbd.s)。よってキャラクタを描くページの背景は常に
* 真っ黒であり、自機と幅 2/3 byte の絵は OR で貼るだけでよい (マスクが
* 要らない)。幅 4 byte の絵だけは上書きで貼る (blit_obj の説明)。
*
* ------------------------------------------------------------
* 描画フレームは 384x96 dot (= 横 48 byte x 縦 96 line)
* ------------------------------------------------------------
* 寸法の理由は c_subprog.h。描画フレームは画面中央に置き、左上 = 52 line 目 /
* 16 byte 目 = 52*80+16 = $1050。行送りは +80。
*
* ------------------------------------------------------------
* 床の塗りは PSHU で 1 行 48 byte を 8 命令で
* ------------------------------------------------------------
* PSHU は U を減らしながら複数レジスタを書き出す命令で、5 + n cycle で
* n byte 書ける。D/X/Y の 3 本 = 6 byte を 11 cycle、つまり 1 byte 約 1.83
* cycle であり、STD の 4 cycle/byte の半分以下。描画フレームの幅 48 byte は 6 の倍数な
* ので端数無しにちょうど 8 回で埋まる。さらに床は **1 行おきに間引いて**
* 塗るので、実質 48 line ぶんの塗りで 96 line の描画フレームが埋まる。
*
*   PSHU が書く順序 (低位→高位) は A,B,Xhi,Xlo,Yhi,Ylo。D/X/Y に「左から
*   右へ 6 byte ぶんの模様」を入れておけばよい。U は行の**右端の次**を
*   指した状態で始め、8 回押すと行頭に戻る。
*
*   ★ fill_rows は A も模様の一部として使う。D に模様を入れた後で行数を
*     lda で読むと A が壊れ、6 byte の先頭 1 byte だけが化ける。行数の設定
*     (sta w_cnt) は D を作る前に済ませる。
*
* ------------------------------------------------------------
* 消し込みはサブ側が自分で憶える
* ------------------------------------------------------------
* ダブルバッファリングの裏のページには 2 フレーム前の絵が残っている。床は G plane にあって
* 毎フレーム塗り直すので勝手に消えるが、**ページに描いたキャラクタは誰も
* 消さない**。そこでキャラクタを描くたびにサブ側が「ページごとの消し込み表」
* へ矩形を積み、次にそのページが描画先に選ばれた時 (固定パラメータの PAGE)
* にまとめて黒で潰す。
* main から消し込みコマンドを送らずに済むので、共有 RAM のキュー
* (実効 108 byte) を情景と描画だけに使える。
*
* 通信プロトコル = 固定パラメータ (8 byte) + コマンドキュー
*   並びとコマンドの一覧は c_subprog.h を正とする。要点だけ書く。
*
* (1) 固定パラメータ (sub 側 $D383-$D38A = main 側 $FC83-$FC8A)
*   毎フレームの描画で使うパラメータ (描画先のページ / 情景のヘッダ / 自機)
*   を決まった並びで置く (下の p_* の equ)。入口でフラグを見て、立っている
*   ものだけをキューより先に処理する。処理の順は PAGE → SCENE → SPRITE →
*   キュー。
*
* (2) コマンドキュー (sub 側 $D393 から。main 側 $FC93 から 108 byte + 終端)
*   SCENE が有るときは先頭に帯 [cnt,pat] x n が生で並び、その後ろから
*   コマンドが始まる。
*   [cmd][params...] ... [$00]
*   $00 END     —                     キュー終端
*   $01 CLS     p                     1 プレーンを全消去 (p = 0 B / 1 R / 2 G)
*   $09 OBJS    n,[x,y,k|ph<<6]xn      絵を n 個まとめて貼る (1 個 3 byte)
*   $0A WRITE   ah,al,n,[byte]xn      サブ側のアドレスへ n byte 書く
* ============================================================

* ---- アドレス定数 (config.mk と要一致。check_layout.py が検査する) ----
SUB_WORK_ADDR   equ     $C000
SUB_TABLE_ADDR  equ     $C020
SUB_PROG_ADDR   equ     $C100
SUB_CODE_END    equ     $CF80
SUB_STACK_ADDR  equ     $CF80           * 自前のスタック (下端)
SUB_STACK_TOP   equ     $CFC0           * 同 (上端。entry で S に入れる値)

VRAM_GATE       equ     $D409
CMD_QUEUE       equ     $D393           * = main 側 $FC93
PARAM_BASE      equ     $D383           * 固定パラメータ 8 byte (= main 側 $FC83)
p_flags         equ     PARAM_BASE+0    * bit7 PAGE / bit6 SCENE / bit5 SPRITE / bit0 ページ
p_ct            equ     PARAM_BASE+1    * 空へ戻す開始 line
p_cn            equ     PARAM_BASE+2    * その行数
p_hz            equ     PARAM_BASE+3    * 地平線 line
p_nband         equ     PARAM_BASE+4    * 床の帯の数
p_sx            equ     PARAM_BASE+5    * 自機 x (byte)
p_sy            equ     PARAM_BASE+6    * 自機 y (line)
p_sph           equ     PARAM_BASE+7    * 自機の横位相 (0-7)
LINE_BYTES      equ     80
VRAM_DISP_END   equ     16000           * 200 line x 80 byte
GPLANE          equ     $8000           * 緑のカラープレーン (床と地平線)

* ---- 描画フレーム (= 384x96 dot) ----
FRAME_TL        equ     52*LINE_BYTES+16        * $1050
FRAME_W_BYTES   equ     48
FRAME_H_LINES   equ     96
ROW_ADV         equ     LINE_BYTES*2+FRAME_W_BYTES  * 208 (= 2 行先の右端の次)

* ---- ページごとの消し込み表 (SUB_TABLE_ADDR に置く) ----
* 自機 1 + OBJS 25 件が上限なので 26 件持つ。
LIST_MAX        equ     26              * 1 ページあたりの矩形の数
LIST0           equ     SUB_TABLE_ADDR              * $C020-$C087
LIST1           equ     SUB_TABLE_ADDR+LIST_MAX*4   * $C088-$C0EF

* ---- 作業変数 (= 直接ページは使わず拡張アドレスで触る) ----
w_cnt           equ     SUB_WORK_ADDR+0   * 塗る行数 / 汎用カウンタ
w_vram          equ     SUB_WORK_ADDR+1   * 次に塗る行の「右端の次」 (2)
w_rows          equ     SUB_WORK_ADDR+3   * 描画フレーム内の残り行数
*               SUB_WORK_ADDR+4   (空き)
w_qsv           equ     SUB_WORK_ADDR+5   * キューポインタの退避 (2)
w_ct            equ     SUB_WORK_ADDR+7   * 空へ戻す開始 line
w_cn            equ     SUB_WORK_ADDR+8   * その行数
w_hz            equ     SUB_WORK_ADDR+9   * 地平線 line
w_nband         equ     SUB_WORK_ADDR+10  * 残りの帯の数
w_base          equ     SUB_WORK_ADDR+11  * 描画先のページの先頭 (2)
w_tmp           equ     SUB_WORK_ADDR+13  * 模様番号などの一時置場
w_page          equ     SUB_WORK_ADDR+14  * いま選ばれているページ (0/1)
w_ln0           equ     SUB_WORK_ADDR+15  * ページ 0 の消し込み表の件数
w_ln1           equ     SUB_WORK_ADDR+16  * ページ 1 の消し込み表の件数
w_rx            equ     SUB_WORK_ADDR+17  * erase_rect / reg_rect の x (byte)
w_ry            equ     SUB_WORK_ADDR+18  * 同 y (line)
w_rw            equ     SUB_WORK_ADDR+19  * 同 w (byte)
w_rh            equ     SUB_WORK_ADDR+20  * 同 h (line)
w_ew            equ     SUB_WORK_ADDR+21  * 敵の幅 (byte)
*               SUB_WORK_ADDR+22  (空き)
*               SUB_WORK_ADDR+23  (空き)
w_nobj          equ     SUB_WORK_ADDR+24  * OBJS の残り個数
w_ssave         equ     SUB_WORK_ADDR+25  * 呼び手のスタックポインタの退避 (2)

                org     SUB_PROG_ADDR

* ---- エントリ (= sub_call が飛んでくる先) --------------------------
* 呼び手 (サブシステム ROM) のスタックは使わない。入口で S を退避して
* 自前の領域 (SUB_STACK_ADDR-SUB_STACK_TOP、config.mk) に張り替え、戻る前に
* 元へ戻す。この間の pshs / lbsr / NMI のフレームは全部自前の領域に積まれる。
* 戻り先は呼び手のスタックに積まれたままなので、S を戻してから rts する。
entry:
                sts     w_ssave         * 呼び手の S を退避
                lds     #SUB_STACK_TOP  * 自前のスタックへ
                lda     VRAM_GATE       * VRAM gate OPEN (read で開く)
                ldu     #CMD_QUEUE      * ← gate が開くまでの間の 1 命令
* 固定パラメータのフラグを見て、立っているものをキューより先に処理する。
* do_scene だけは帯をキューの先頭から読むので U が進む。
                lda     p_flags
                bpl     .en_scene       * bit7 = 0 → PAGE 無し
                lbsr    do_page
.en_scene:
                lda     p_flags
                bita    #$40
                beq     .en_sprite
                lbsr    do_scene
.en_sprite:
                lda     p_flags
                bita    #$20
                beq     qloop
                lbsr    do_sprite
qloop:
                lda     ,u+
                beq     qend            * $00 END → 終わり
                cmpa    #$09
                lbeq    cmd_objs
                cmpa    #$0A
                lbeq    cmd_write
                cmpa    #$01
                lbeq    cmd_cls
                                        * 未知コマンド → 即終了 (暴走させない)
qend:
                sta     VRAM_GATE       * VRAM gate CLOSE (write で閉じる)
                lds     w_ssave         * 呼び手のスタックへ戻す
                rts

* ---- $01 CLS p: 1 プレーンを全消去 (p = 0 B / 1 R / 2 G) ------------------
* 起動時は G plane も消す (BASIC が残した内容を消すため)。
* 消したのがページ (B / R) なら、そのページの消し込み表も空にする
* (G の消去では表を触らない)。
*
* ★ 3 プレーンまとめて消してはならない。消去のループは std ,x++ (8) +
*   leay -1,y (5) + bne (3) = 16 cycle で 2 byte、16,000 byte で 128,000
*   cycle。サブ CPU は約 2 MHz だが VRAM ゲートを開けている間は約 32% の
*   速さになるので、1 プレーンで約 200ms 掛かる。3 プレーン続けると main 側の
*   完了待ち上限 (asm_test.s の SUB_WAIT_CAP) を超え、main は「終わった」
*   と思って次のバッチを書き込み、**消去中のサブがそれを踏み潰す**。
*   プレーンごとに 1 命令へ割り、1 回の呼び出しを上限内に収める。
cmd_cls:
                ldb     ,u+             * 消すプレーン (0 = B / 1 = R / 2 = G)
                ldx     #0
                cmpb    #1
                blo     .cl_go          * 0 = B プレーン
                beq     .cl_r           * ★ ldx はフラグを壊す。分岐を先に
                ldx     #GPLANE
                bra     .cl_go
.cl_r:
                ldx     #$4000
.cl_go:
                pshs    b               * プレーン番号を控える
                ldy     #VRAM_DISP_END/2
                ldd     #0
.c1:
                std     ,x++
                leay    -1,y
                bne     .c1
                puls    b
                cmpb    #1
                bhi     .cl_done        * G → 消し込み表は触らない
                beq     .cl_r1
                clr     w_ln0           * B → ページ 0 の表を空にする
                lbra    qloop
.cl_r1:
                clr     w_ln1           * R → ページ 1 の表を空にする
.cl_done:
                lbra    qloop

* ============================================================
* fill_rows — D/X/Y の 6 byte 模様で、描画フレームの幅 48 byte の行を w_cnt 行塗る
*   entry: U = 塗る先頭行の「右端の次」/ D,X,Y = 6 byte の模様
*          w_cnt = 行数 (1 以上)
*   exit : U = 塗り終えた次の行の「右端の次」/ D,X,Y は保存される
*   ※ 1 行塗るごとに 2 line 進む (= 間引き)
* ============================================================
fill_rows:
                pshu    d,x,y           * 48 byte = 6 byte x 8 回
                pshu    d,x,y
                pshu    d,x,y
                pshu    d,x,y
                pshu    d,x,y
                pshu    d,x,y
                pshu    d,x,y
                pshu    d,x,y
                leau    ROW_ADV,u
                dec     w_cnt
                bne     fill_rows
                rts

* ============================================================
* row_addr — 描画フレーム内 line 番号 (B) から G plane 上の「その行の右端の次」
*   entry: B = 描画フレーム内 line (0-95)
*   exit : X = GPLANE + FRAME_TL + B*80 + 48
* ============================================================
row_addr:
                lda     #LINE_BYTES
                mul                     * D = line * 80
                addd    #GPLANE+FRAME_TL+FRAME_W_BYTES
                tfr     d,x
                rts

* ---- SCENE (固定パラメータ ct,cn,hz,n + キュー先頭の帯 [cnt,pat]xn) ----
* 地平線と床を G plane (緑) に描く。空 (地平線より上) には何も描かない。
*   entry: U = キューの先頭 (= 帯の並び)
*   exit : U = 帯の並びの次 (= コマンドの先頭)
do_scene:
                ldb     p_ct            * ct (空へ戻す開始 line)
                stb     w_ct
                ldb     p_cn            * cn (その行数)
                stb     w_cn
                ldb     p_hz            * hz (地平線 line)
                stb     w_hz
                ldb     p_nband         * n  (帯の数)
                stb     w_nband
                stu     w_qsv           * 以後 U は PSHU 用に使う

* (1) 空へ戻す (= 地平線が下がって、床だった行が空になった分)
                ldb     w_cn
                beq     .sc_hz
                stb     w_cnt
                ldb     w_ct
                lbsr    row_addr
                tfr     x,u
                ldd     #0
                tfr     d,x
                tfr     d,y
                lbsr    fill_rows

* (2) 地平線の行 (1 行)。黒で塗って位置を合わせるだけで、線は描かない。
*     遠景の帯も黒 (main 側の FLOOR_BANDS) なので、床の縞は最初の明るい帯
*     から見える。塗り終えると U は床の先頭行を指す。
.sc_hz:
                lda     #1
                sta     w_cnt           * ★ 行数は D を作る前に決める
                ldb     w_hz
                lbsr    row_addr
                tfr     x,u
                ldd     #$0000
                tfr     d,x
                tfr     d,y
                lbsr    fill_rows
                stu     w_vram
                lda     #FRAME_H_LINES-1
                suba    w_hz
                lsra                    * 間引きぶん (= 論理行数) へ
                sta     w_rows

* (3) 床の帯を順に塗る
.sc_band:
                lda     w_nband
                lbeq    .sc_end
                deca
                sta     w_nband
                ldu     w_qsv
                ldb     ,u+             * cnt (この帯の行数)
                lda     ,u+             * pat (模様の番号)
                stu     w_qsv
                sta     w_tmp
                cmpb    w_rows          * 描画フレームからはみ出さないようクリップ
                bls     .sc_ok
                ldb     w_rows
.sc_ok:
                tstb
                beq     .sc_band
                stb     w_cnt
                ldb     w_rows
                subb    w_cnt
                stb     w_rows
* 模様表から 6 byte を D/X/Y に載せる (= 帯 1 本につき 1 回だけ)
                ldb     w_tmp
                lda     #6
                mul                     * D = 番号 * 6
                addd    #flr_pat
                tfr     d,y
                ldd     0,y
                ldx     2,y
                ldy     4,y             * ★ Y は最後 (表の先頭を指すため)
                ldu     w_vram
                lbsr    fill_rows
                stu     w_vram
                bra     .sc_band
.sc_end:
                ldu     w_qsv
                rts

* ============================================================
* erase_rect — w_rx/w_ry/w_rw/w_rh の矩形を黒で潰す (消し込み専用)
*   絵の幅は 2 / 3 / 4 byte しか無い (scripts/make_sprite.py がそれ以外の
*   幅を受け付けない。自機は 4 byte) ので、幅ごとに展開して 1 行を
*   std 1〜2 発で潰す。1 行 20 / 25 / 26 cycle (幅 2 / 3 / 4 byte。std ,x 5
*   + sta 2,x 5 または std 2,x 6 + leax 5 + dec 7 + bne 3) で済む。
* ============================================================
erase_rect:
                ldb     w_ry
                lda     #LINE_BYTES
                mul                     * D = y*80
                addb    w_rx
                adca    #0
                addd    w_base
                tfr     d,x             * X = VRAM
                lda     w_rh
                sta     w_cnt
                ldb     w_rw
                cmpb    #3
                beq     .er3
                bhi     .er4            * 4 byte
.er2:
                ldd     #0
.er2l:
                std     ,x
                leax    LINE_BYTES,x
                dec     w_cnt
                bne     .er2l
                rts
.er3:
                ldd     #0
.er3l:
                std     ,x
                sta     2,x
                leax    LINE_BYTES,x
                dec     w_cnt
                bne     .er3l
                rts
.er4:
                ldd     #0
.er4l:
                std     ,x
                std     2,x
                leax    LINE_BYTES,x
                dec     w_cnt
                bne     .er4l
                rts

* ============================================================
* reg_rect — w_rx/w_ry/w_rw/w_rh を、いまのページの消し込み表へ積む
*   表に入らない矩形はそのページでは消されない (次の CLS か上書きまで残る)。
*   自機 1 + OBJS 25 件 (main 側の MAX_OBJS) で LIST_MAX = 26 なので、表は
*   尽きない。
* ============================================================
reg_rect:
                lda     w_page
                bne     .rg1
                ldb     w_ln0
                cmpb    #LIST_MAX
                bhs     .rg_done
                incb
                stb     w_ln0
                decb
                lda     #4
                mul
                addd    #LIST0
                bra     .rg_put
.rg1:
                ldb     w_ln1
                cmpb    #LIST_MAX
                bhs     .rg_done
                incb
                stb     w_ln1
                decb
                lda     #4
                mul
                addd    #LIST1
.rg_put:
                tfr     d,x
                lda     w_rx
                sta     ,x+
                lda     w_ry
                sta     ,x+
                lda     w_rw
                sta     ,x+
                lda     w_rh
                sta     ,x+
.rg_done:
                rts

* ---- PAGE (固定パラメータ flags bit0): 描画先のページを選び、そのページを消し込む
*   U は使わない (= 呼び手のキューポインタをそのまま保つ)。
do_page:
                ldb     p_flags
                andb    #1
                stb     w_page
                ldx     #0
                tstb
                beq     .pg_set
                ldx     #$4000
.pg_set:
                stx     w_base
* このページに前回 (= 2 フレーム前) 描いた矩形を全部黒へ戻す
                ldb     w_page
                bne     .pg_l1
                ldb     w_ln0
                ldx     #LIST0
                clr     w_ln0
                bra     .pg_loop
.pg_l1:
                ldb     w_ln1
                ldx     #LIST1
                clr     w_ln1
.pg_loop:
                tstb
                beq     .pg_end
                stb     w_tmp
                lda     ,x+
                sta     w_rx
                lda     ,x+
                sta     w_ry
                lda     ,x+
                sta     w_rw
                lda     ,x+
                sta     w_rh
                pshs    x
                lbsr    erase_rect
                puls    x
                ldb     w_tmp
                decb
                bra     .pg_loop
.pg_end:
                rts

* ---- SPRITE (固定パラメータ x,y,ph): 自機を貼る ------------------------
* 背景は常に黒なので OR で貼るだけ (= マスク不要)。U は使わない。
do_sprite:
                ldb     p_sx            * x (byte 単位 0-79)
                stb     w_rx
                ldb     p_sy            * y (line 0-199)
                stb     w_ry
                lda     #LINE_BYTES
                mul                     * D = y*80
                addb    w_rx
                adca    #0
                addd    w_base
                pshs    d               * VRAM アドレスを退避
                ldb     p_sph           * ph (位相 0-7)
                lda     #SPR_PHASE_BYTES
                mul                     * D = ph*80
                addd    #spr_data
                tfr     d,y             * Y = 絵の先頭
                puls    d
                tfr     d,x             * X = VRAM
                lda     #SPR_H_LINES
                sta     w_cnt
.sp_line:
                lda     ,x
                ora     ,y+
                sta     ,x+
                lda     ,x
                ora     ,y+
                sta     ,x+
                lda     ,x
                ora     ,y+
                sta     ,x+
                lda     ,x
                ora     ,y+
                sta     ,x+
                leax    LINE_BYTES-SPR_W_BYTES,x
                dec     w_cnt
                bne     .sp_line
                lda     #SPR_W_BYTES
                sta     w_rw
                lda     #SPR_H_LINES
                sta     w_rh
                lbsr    reg_rect
                rts

* ---- $0A WRITE ah,al,n,[byte]xn: サブ側のアドレスへ n byte 書く --------
* 起動時の絵の転送 (爆発・敵弾・敵 B の近の追加絵を VRAM 各プレーンの末尾へ)
* と、文字描画 (main 側のフォントを行ごとに運ぶ) の転送口。書くのはこの
* サブ CPU 自身である (VRAM へ書ける経路はこれだけ。c_subprog.h の
* sub_takeover の注記)。VRAM ゲートは entry で開けてあるのでそのまま書ける。
cmd_write:
                ldx     ,u++            * X = 書込先
                ldb     ,u+             * n (1-104。0 は main 側の sub_write が積まない)
.pk_loop:
                lda     ,u+
                sta     ,x+
                decb
                bne     .pk_loop
                lbra    qloop

* ---- $09 OBJS n,[x,y,k|ph<<6]xn: まとめて貼る --------------------------
* k は絵の種類 (0-2 飛行体 / 3-5 球 / 6-8 低木 / 9-11 木 / 12-14 岩 /
* 15-17 自弾 / 18-20 爆発 A / 21-23 爆発 B / 24-26 敵楕円弾 / 27-29 敵 B。
* それぞれ 遠/中/近。自弾 15-17 だけは 近/中/遠 = 大/中/小 の順)。
* 寸法と置場は enm_tab から引く。追加絵は VRAM 末尾にあるが、読み方は同じ。
cmd_objs:
                ldb     ,u+             * n
                stb     w_nobj
.ob_loop:
                tst     w_nobj
                lbeq    qloop
                dec     w_nobj
                ldb     ,u+             * x (byte)
                stb     w_rx
                ldb     ,u+             * y (line)
                stb     w_ry
                ldb     ,u+             * k | ph<<6
                tfr     b,a
                andb    #$3F            * B = k (0-63)
                lsra
                lsra
                lsra
                lsra
                lsra
                lsra                    * A = ph (0-3)
                lbsr    blit_obj
                bra     .ob_loop

* ============================================================
* blit_obj — 絵 k を位相 A で (w_rx, w_ry) に貼り、消し込み表へ積む
*   entry: B = k / A = ph / w_rx, w_ry / U は保存される
* ============================================================
blit_obj:
                pshs    a               * ph を退避
                lda     #6
                mul                     * D = k*6
                addd    #enm_tab
                tfr     d,x             * X = 寸法表の該当行
                puls    a               * A = ph
                ldb     3,x             * 位相ごとの byte 数 (下位。256 未満)
                mul                     * D = ph * 位相ごとの byte 数
                addd    4,x             * + 絵の先頭
                tfr     d,y             * Y = この位相の絵
                lda     ,x              * w
                sta     w_rw
                sta     w_ew
                lda     1,x             * h
                sta     w_rh
                sta     w_cnt
* VRAM = w_base + y*80 + x
                ldb     w_ry
                lda     #LINE_BYTES
                mul
                addb    w_rx
                adca    #0
                addd    w_base
                tfr     d,x             * X = VRAM
* 幅ごとに展開した貼り付けを選ぶ。2 byte ずつ ldd/std で運ぶ。
*
* 幅 2/3 byte の絵は **OR で貼る**。上書きで貼ると read-modify-write が
* 要らず 2 byte あたり 9 cycle 速いが、絵の入れ物 (2/3/4 byte) には位相ずらし
* ぶんの黒い余白があり、横一列の編隊のように絵が重なると**後から貼った
* 絵の黒い余白が前の絵を欠けさせる**。キャラクタのページは背景が黒で
* 床は別プレーンなので、OR なら重なりは白の和になり欠けない。OR の増分は
* 1 行あたり 2 byte で約 9 cycle (ldd ,x / ora ,y+ / orb ,y+ / std ,x =
* 22 cycle。上書きは 13 cycle)。
* ただし **幅 4 byte (近) だけは上書きのまま**にする。近の絵を OR に
* すると 14 行 x 2 組で 1 個あたり約 270 cycle 増えるので、処理時間を
* 抑えるため上書きし、重なった場合は先に描いた絵が欠ける。
                ldb     w_ew
                cmpb    #2
                beq     .en2
                cmpb    #3
                beq     .en3
.en4:
                ldd     ,y++
                std     ,x
                ldd     ,y++
                std     2,x
                leax    LINE_BYTES,x
                dec     w_cnt
                bne     .en4
                bra     .en_done
.en3:
                ldd     ,x
                ora     ,y+
                orb     ,y+
                std     ,x
                lda     2,x
                ora     ,y+
                sta     2,x
                leax    LINE_BYTES,x
                dec     w_cnt
                bne     .en3
                bra     .en_done
.en2:
                ldd     ,x
                ora     ,y+
                orb     ,y+
                std     ,x
                leax    LINE_BYTES,x
                dec     w_cnt
                bne     .en2
.en_done:
                lbra    reg_rect        * 消し込み表へ積んで戻る

* ---- 床の模様表 (= scripts/make_floor.py が生成) --------------------
                include "../build/floor_data.s"

* ---- 自機と敵の絵 (= scripts/make_sprite.py が生成) -----------------
                include "../build/sprite_data.s"

                end
