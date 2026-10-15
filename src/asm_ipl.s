* ============================================================
* asm_ipl.s — FM-7 IPL (ブートセクタ。 読み込まれた位置から $FB00 へ
*                       relocate して実行)
*
* ブート ROM が track0/side0/sector1 (256 byte) をメモリに読み込み、 その
* 先頭へ JMP する。 読み込み先のアドレスはブート ROM が決めるので、 IPL は
* それに頼らない: PCR で自分の現在位置を取得し、 自分 256 byte を高位
* RAM $FB00 へコピー (= トランポリン) して位置非依存で実行する。 移す
* までの区間ではスタックも DP も使わない (clr $FD0F は拡張アドレス指定)。
* 本体は config.mk の ORG (= 既定 $0400) へ展開する。 本体が IPL の
* 読み込み先と重なっても、 IPL は既に $FB00 へ移っているので差し支えない。
*
* 実行フロー:
*   1. boot ROM が IPL (本ファイル) をメモリに読み込み、 先頭へ JMP
*   2. IPL 先頭の bootstrap が自分 256 byte を $FB00 へコピー (PCR)
*   3. JMP $FB?? でコピー後の IPL 本体へ
*   4. FDC を操作して sector 2 以降を ORG ($0400) へ展開 (= 複数 track/side
*      にまたがる場合は自動的に SEEK しながらシーケンシャル read)
*   5. JMP ORG ($0400) で本体の入口 (asm_crt0.s の _start) へ。 そこで
*      スタックを張ってから main() へ進む
*
* 本体配置 (= bin2d77.py と一致):
*   body_idx 0   : track 0 / side 0 / sec 2    (IPL の次から)
*   body_idx 14  : track 0 / side 0 / sec 16
*   body_idx 15  : track 0 / side 1 / sec 1    (= side wrap)
*   body_idx 30  : track 0 / side 1 / sec 16
*   body_idx 31  : track 1 / side 0 / sec 1    (= track + side wrap)
*   ...
*   一般式: flat = body_idx + 1
*           track = flat / 32
*           side  = (flat / 16) % 2
*           sec   = (flat % 16) + 1
*
*   body_sectors は 1 byte。 本体の上限は bin2d77.py が ORG と
*   RELOC_FLOOR ($FA00。 IPL の写し $FB00 の手前) から
*   (RELOC_FLOOR - ORG) / 256 sector と決める (ORG = $0400 なら
*   246 sector = 62,976 byte。 track 7 / side 1 / sec 7 まで)。
*
* メタデータ:
*   bin2d77.py が IPL バイナリ +2 オフセット の body_sectors を
*   書き換える。
* ============================================================

                ifndef BODY_LOAD
BODY_LOAD       equ     $0400           * fallback (config.mk ORG と要同期)
                endc
RELOC_LEN       equ     $0100           * コピーサイズ = 1 sector = 256 byte
SEC_PER_TRACK   equ     16

* ── MB8877 FDC レジスタ ──────────────────────────
FDC_CMD         equ     $FD18           * W: Command / R: Status
FDC_TRK         equ     $FD19           * Track
FDC_SEC         equ     $FD1A           * Sector
FDC_DATA        equ     $FD1B           * Data
FDC_HEAD        equ     $FD1C           * Head/Side select + density (bit0=side)
FDC_DRVSEL      equ     $FD1D           * Drive (bit0-1) / Motor (bit7)

ST_BUSY         equ     $01
ST_DRQ          equ     $02
CMD_READ        equ     $80             * Read Sector (single)
CMD_SEEK        equ     $1B             * Seek + head load + 30ms step (verify なし)

                org     $FB00           * relocate 先 = 実行アドレス

start:
                bra     code            * meta を跨ぐ (PCR なのでロード位置不問)
meta:
body_sectors    fcb     1               * +2: body sector 数 (bin2d77.py が書き換え)
                fcb     0,0,0,0,0,0,0,0,0,0,0,0,0
                                        * 先頭の bra 2 byte を含め $FB00-$FB0F
                                        * の 16 byte (meta 自体は $FB02 から 14 byte)

code:
* ---- bootstrap (この区間は boot ROM のロード位置で実行される) ----
*    使えるのは: 即値 / 単純 indexed / PCR / 絶対JMP
*    使ってはいけない: 自分自身 ($FBxx) への絶対 LDA/STA など
*    (= $FB00 にはまだ何もコピーされていない)
*
*    PCR で「自分の今いる位置」 を取得して、 そこから $FB00 へ
*    relocate する。 これで読み込み先が $FB00-$FBFF と重ならない限り、
*    どこにロードされても自分自身を $FB00 へコピーできる。
*
*    先に F-BASIC ROM overlay を OFF にする (= $FD0F へ write)。
*    $FB00 付近が ROM のままだと、 写した自分を実行できない。
                clr     $FD0F           * disable F-BASIC ROM overlay

                leax    start,pcr       * X = 現在の `start` 位置 (= ロード base)
                ldy     #start          * Y = $FB00 (= relocate 先)
copy_loop:
                lda     ,x+
                sta     ,y+
                cmpy    #start+RELOC_LEN
                bne     copy_loop
                jmp     ipl_main        * 絶対 JMP $FB?? → relocate 後の本体へ

* ---- ipl_main (ここから先は $FBxx で実行される) ----------------
ipl_main:
                orcc    #$50            * IRQ/FIRQ マスク
                                        * (以後もスタックと DP は使わない。
                                        *  本体が crt0 で自分のスタックを張る)

                lda     #$80            * motor on / drive 0
                sta     FDC_DRVSEL
                clr     FDC_HEAD        * side 0
                clr     FDC_TRK         * track 0 (= boot ROM 状態を踏襲)

                lda     #BODY_LOAD/256  * X = BODY_LOAD (= ORG) を D 経由で組む
                clrb                    * (ORG はページ境界前提で下位=0)
                tfr     d,x

                ldb     body_sectors    * B = 残セクタ数 ($FB02 を読む)
                clr     cur_track
                clr     cur_side
                lda     #2              * 開始セクタ (= IPL の次)
                sta     cur_sec

* ---- read 1 sector ----
read_one:
                lda     cur_sec
                sta     FDC_SEC
                lda     #CMD_READ
                sta     FDC_CMD

* FDC はコマンドを書いてから BUSY=1 になるまでに数十 cycle の遅れがある。
* 即 status を読むと BUSY=0/DRQ=0 で「完了」 と誤判定する。
* まず BUSY=1 を確認してから DRQ ループへ。
read_wait_busy:
                lda     FDC_CMD
                bita    #ST_BUSY
                beq     read_wait_busy

wait_drq:
                lda     FDC_CMD
                bita    #ST_DRQ
                bne     got_byte
                bita    #ST_BUSY
                bne     wait_drq
                bra     sec_done
got_byte:
                lda     FDC_DATA
                sta     ,x+
                bra     wait_drq

* ---- セクタ完了 → 次の (track, side, sec) を計算 ----
sec_done:
                decb                    * 残セクタ -= 1
                beq     all_done

                inc     cur_sec
                lda     cur_sec
                cmpa    #SEC_PER_TRACK+1
                blo     read_one        * まだ同じ side 内、 続き read

* sec が 17 に達した → side をひっくり返す
                lda     #1
                sta     cur_sec
                lda     cur_side
                eora    #$01            * 0↔1 反転
                sta     cur_side
                sta     FDC_HEAD        * side select 更新
                bne     read_one        * side 0→1 切替なら SEEK 不要

* side 1→0 wrap → 次のトラックへ SEEK
                inc     cur_track
                lda     cur_track
                sta     FDC_DATA        * SEEK 先 track 番号を data reg へ
                lda     #CMD_SEEK
                sta     FDC_CMD
seek_wait_busy:
                lda     FDC_CMD
                bita    #ST_BUSY
                beq     seek_wait_busy
seek_wait_done:
                lda     FDC_CMD
                bita    #ST_BUSY
                bne     seek_wait_done
                bra     read_one

all_done:
                jmp     BODY_LOAD       * JMP ORG ($0400) → 本体の入口 (asm_crt0.s の _start)

cur_sec         fcb     0
cur_side        fcb     0
cur_track       fcb     0

                end     start
