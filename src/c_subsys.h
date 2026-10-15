/* ============================================================
 * c_subsys.h — メインCPU から FM-7 サブシステム (= 標準の
 *              サブシステム ROM が走っている前提) を呼ぶ API
 *
 * 共有 RAM ($FC80-$FCFF) の サブシステム ROM 規約上のレイアウト:
 *   main side  sub side  役割
 *   $FC80      $D380     ATN フラグ (sub→main 応答 bit7)
 *   $FC81      $D381     sub→main 応答コード
 *   $FC82      $D382     ★ main→sub コマンドコード (= 0 で「コマンドなし」)
 *   $FC83-$FC8E $D383-$D38E コマンドパラメータ
 *   ※ 描画プログラム (c_subprog.h) が TEST コマンド ($3F) で使う間は、
 *     $FC83-$FC8A の 8 byte を描画プログラムへの固定パラメータに使う
 *     (TEST はこの 8 byte を読まない)。$FC8B 以降が TEST のサブコマンド。
 *
 * sub はメインループで $D382 を polling し、非0 を見たら
 * コマンド表でハンドラを実行する。 subsys_call() は params 配置 +
 * cmd 書き込み + 完了待ち を 1 発で行う。
 *
 * 完了検出: sub はコマンドを受け取ると BUSY ($FD05 bit7) を立て、 処理を
 *   終えると下ろす。 subsys_call() は BUSY の立ち上がりと立ち下がりを
 *   この順に (上限付きで) 待つ。 詳細は c_subsys.c 参照。
 *
 * 本作が使うサブシステム ROM のコマンドは SCMD_CURSOR ($0C) だけ。
 * ============================================================ */

#ifndef C_SUBSYS_H
#define C_SUBSYS_H

/* サブシステム ROM のコマンドコード (本作で使うコマンドの定数) */
#define SCMD_CURSOR       0x0C    /* params[0]=テキストモードフラグ
                                   *   bit0: カーソルの点滅 ON/OFF */

/* params (= $D383+) に乗せられる最大 byte 数 = 12 ($D383-$D38E) */
#define SUBSYS_MAX_PARAMS  12

/* 低レベル (asm_subsys.s) — main から sub を HALT/RELEASE する。
 * subsys_call() が内部で呼ぶ。 ゲーム本体から直接呼ぶ必要はない。 */
void subsys_halt(void);
void subsys_release(void);

/* サブCPU に CANCEL を発行し、 実行中の処理を中断してサブシステム ROM の
 * コマンド待ちループへ戻す。 テープ起動 (= warm start) で本体へ突入した
 * 際、 サブを takeover 可能なクリーン状態に揃えるために起動時 1 回呼ぶ。
 * ディスク起動 (cold start) でも同じ初期化手順として呼ぶ。 */
void sub_cancel(void);

/* ------------------------------------------------------------
 * subsys_call(cmd, params, param_len)
 *   サブシステム ROM の標準コマンドを 1 発で発行する。 詳細は c_subsys.c。
 *
 *   引数:
 *     cmd       : サブシステム ROM コマンドコード (SCMD_* 定数)
 *     params    : $FC83 (サブ側 $D383) から書き込むパラメータ。
 *                 param_len が 0 なら書き込まない (その時だけ NULL でよい)
 *     param_len : params の長さ (0 〜 SUBSYS_MAX_PARAMS)
 *
 *   戻り値: sub からの応答コード ($FC81、 サブ側 $D381 の値)。
 *           param_len が SUBSYS_MAX_PARAMS を超える時は何も送らず 0xFF
 * ------------------------------------------------------------ */
unsigned char subsys_call(unsigned char cmd,
                          const unsigned char *params,
                          unsigned char param_len);

#endif
