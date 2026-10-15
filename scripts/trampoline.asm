;
; ============================================================================
; FM-7 LOADM 用ステージャ (6809、lwasm の文法)
;
; d77_to_t77_chunks.py と対になるソース。テープには本体の全部を機械語
; ファイル 1 本として載せ、並びは
;
;     [ 本体 (ORG を基準にした像、N byte) ][ このステージャ ]
;
; で、STAGER_LOAD ($1400) から連続して読み込まれる。BASIC ローダーが実行
; する LOADM "CAS0:",,R が、本体の直後にあるステージャへ飛ぶ。ステージャは
;
;   1. IRQ/FIRQ をマスクし (BASIC の IRQ ハンドラは、これから外す ROM の
;      オーバレイの中にある)、
;   2. ROM のオーバレイを外し ($FD0F へ書く。$8000-$FBFF がただの RAM に
;      なる。本作はこの状態で動く)、
;   3. 本体を $1400 からエントリアドレスへ前方コピーし、
;   4. スタックを張ってエントリへ飛ぶ。
;
; 本体とステージャは $1400-$7FFF に収まっていなければならない (Python 側が
; 検査する)。
;
; ステージャは $1400 + N の場所でそのまま動く。コピー先はコピー元より低い
; アドレスなので、コピー先の範囲はステージャより下で終わり、動いている
; コードが書き潰されることは無い。自分自身への絶対参照は持たず (相対分岐
; だけ)、固定の origin は要らない。下の `org 0` は形だけ。
;
; Python 側が埋めるオペランド (ビッグエンディアン。出現回数は次のとおり):
;     $DEAD  (1 回)  転送元の終端 = STAGER_LOAD + N
;     $BEEF  (2 回)  エントリアドレス (LDY #entry / JMP entry)
;
; D77TOT77WAV (MIT) のトランポリンの方式を元にした。
; D77TOT77WAV.LICENSE.txt を参照。
; ============================================================================

STAGER_LOAD     equ     $1400           ; LOADM がブロックを置く場所
ENTRY_STACK     equ     $FBFF           ; 本体へ飛ぶ時のスタック
ROM_PORT        equ     $FD0F

PLACEHOLDER_END   equ   $DEAD           ; <- 埋める: 転送元の終端
PLACEHOLDER_ENTRY equ   $BEEF           ; <- 埋める: エントリアドレス

                org     0
stager          orcc    #$50                    ; FIRQ/IRQ をマスク
                lda     #$00
                sta     ROM_PORT                ; 書くとオーバレイが外れる
                ldx     #STAGER_LOAD            ; 転送元 = 読み込まれた本体
                ldy     #PLACEHOLDER_ENTRY      ; 転送先 = エントリ
.l              lda     ,x+
                sta     ,y+
                cmpx    #PLACEHOLDER_END        ; <- Python 側が埋める
                bne     .l
                lds     #ENTRY_STACK
                jmp     PLACEHOLDER_ENTRY       ; -> 本体へ。ROM は外れたまま

                end
