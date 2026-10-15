* ============================================================
* asm_bootrom.s — 自前 512 byte ブート ROM ($FE00-$FFFF)
*
* 「ディスクからの IPL 起動」だけを行う、 独自に書いた最小のブート ROM。
* やることは次のとおり:
*   1. DP / SP セットアップ
*   2. FDC を操作して track 0 / side 0 / sector 1 (= IPL) を IPL_LOAD
*      ($0100。 この ROM の設計として決めた置き場) に読み込む
*   3. DP=$00 にして JMP IPL_LOAD で IPL の先頭に制御を渡す
* 本リポジトリの IPL (asm_ipl.s) は読み込まれた位置から $FB00 へ自分を
* 写してから本体を展開するので、IPL_LOAD は 256 byte を読み書き・実行
* できる RAM で、その $FB00 に重ならなければどこでもよい (本体 (ORG) と
* 重なっても差し支えない)。
*
* 用途:
*   内蔵のブート ROM が使えない環境 ($FE00-$FFFF に置ける環境) で代わりに
*   使う。 機械の内蔵ブート ROM でディスクから起動できるなら不要。
*
* メモリ配置:
*   $FE00 : 本コード
*   $FFFE : reset vector → $FE00
* ============================================================

* ── MB8877 FDC レジスタ (DP=$FD 経由で <$18 形式。 asm_ipl.s と同じ) ──
FDC_CMD         equ     $18             * W: Command / R: Status
FDC_TRK         equ     $19             * Track
FDC_SEC         equ     $1A             * Sector
FDC_DATA        equ     $1B             * Data
FDC_HEAD        equ     $1C             * Head/Side select + density (bit0=side)
FDC_DRVSEL      equ     $1D             * Drive (bit0-1) / Motor (bit7)

ST_BUSY         equ     $01
ST_DRQ          equ     $02
CMD_READ        equ     $80             * Read Sector (single)

IPL_LOAD        equ     $0100           * IPL の読み込み先 (この ROM の設計値)

                org     $FE00

start:
                orcc    #$50            * IRQ/FIRQ マスク
                lda     #$FD
                tfr     a,dp            * DP=$FD (I/O 短縮アドレス用)
                lds     #$FC7F          * SP は共有 RAM 直下

                lda     #$80            * motor on / drive 0 (asm_ipl.s と同じ)
                sta     <FDC_DRVSEL
                clr     <FDC_HEAD       * side 0
                clr     <FDC_TRK        * トラックレジスタを 0 にする (ヘッドは動かさない)

                ldx     #IPL_LOAD       * 転送先
                lda     #$01
                sta     <FDC_SEC        * sector 1
                lda     #CMD_READ
                sta     <FDC_CMD

* FDC はコマンドを書いてから BUSY=1 になるまでに数十 cycle の遅れがある。
* 即 status を読むと BUSY=0/DRQ=0 で「完了」と誤判定して 1 byte も読まずに
* IPL へ飛んでしまうので、 asm_ipl.s と同じく先に BUSY=1 を確認する。
wait_busy:
                lda     <FDC_CMD
                bita    #ST_BUSY
                beq     wait_busy

wait_drq:
                lda     <FDC_CMD        * status
                bita    #ST_DRQ
                bne     got_byte
                bita    #ST_BUSY
                bne     wait_drq
                bra     done            * idle → 終了

got_byte:
                lda     <FDC_DATA
                sta     ,x+
                bra     wait_drq

done:
                clra
                tfr     a,dp            * DP=$00 に戻してから IPL へ
                jmp     IPL_LOAD

* 512 byte への padding と $FFFE 配置の reset vector は
* scripts/pad_bootrom.py が後段で行う (lwasm --raw は `org $FFFE`
* で自動 pad しないため)

                end     start
