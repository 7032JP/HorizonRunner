/* ============================================================
 * c_input.h — ゲーム用 入力モジュール の C インタフェース
 *
 * 実体は src/asm_input.s (IRQ 駆動)。
 *
 * 【操作仕様】
 *   テンキー 8 / 2 / 4 / 6 … 上 / 下 / 左 / 右 へ移動
 *   テンキー 5             … 停止
 *   テンキー 7 / 9 / 1 / 3 … 斜め (左上 / 右上 / 左下 / 右下) の直接指定
 *   カーソルキー           … 8/2/4/6 の代替
 *   BREAK キー             … ショット (押しっぱなしで自動連射)
 *   RETURN                 … 開始 / 決定
 *   ESC                    … ゲームオーバーの画面からタイトルへ戻る
 *
 * 【移動は状態保持型】
 *   この機種のキーボードは、 通常のキーについて 「キーを離した」 通知を
 *   出さない。 BREAK キーだけは専用のステータスで押下中かを直接読める。
 *   そのため方向は 「押した瞬間に確定し、 離しても保持される」。 止めるには
 *   テンキー 5 を押す (= input_dir_clear() と同じ)。
 *   斜めは縦横それぞれの軸を独立に持つので、 8 の後に 6 を押せば右上になる。
 *
 * 【使い方】
 *   起動時 (kb_init(); の後、timer_init(); で IRQ を許可する前に 1 回):
 *       input_init();
 *
 *   毎フレーム:
 *       unsigned char d = input_dir();
 *       if (d & IN_UP)    py--;
 *       if (d & IN_DOWN)  py++;
 *       if (d & IN_LEFT)  px--;
 *       if (d & IN_RIGHT) px++;
 *
 *       n = break_shots();            // 前フレーム以降のショット数 (c_main.c は
 *                                     // この値のぶん自弾を出す)
 *
 *       e = input_edges();            // 受信したキーに対応する IN_* のビット和
 *       // RETURN の押下は c_main.c の title_screen() が IN_START で、 ESC の
 *       // 押下は gameover_screen() が esc_edges() で見る。 どちらも、 キーコードが
 *       // 一定時間途切れた後の受信を押下とみなす
 * ============================================================ */

#ifndef C_INPUT_H
#define C_INPUT_H

/* --- 入力ビット定義 (asm_input.s の IN_* と必ず一致させること) --- */
#define IN_UP       0x01    /* 上    (テンキー 8 / カーソル上) */
#define IN_DOWN     0x02    /* 下    (テンキー 2 / カーソル下) */
#define IN_LEFT     0x04    /* 左    (テンキー 4 / カーソル左) */
#define IN_RIGHT    0x08    /* 右    (テンキー 6 / カーソル右) */
#define IN_STOP     0x10    /* 停止  (テンキー 5)。 input_edges() にだけ現れる */
#define IN_START    0x80    /* 開始/決定 (RETURN)              */

/* 起動時に 1 回。 内部状態を 0 にし、 BREAK の初期レベルを今の実状態に合わせる。 */
void          input_init(void);

/* 保持中の移動方向ビット (IN_UP / IN_DOWN / IN_LEFT / IN_RIGHT の OR)。
 * 斜めのときは 2 ビット立つ。 停止中は 0。 */
unsigned char input_dir(void);

/* 移動方向を停止状態に戻す (テンキー 5 と同じ効果)。 自機ミス時などに使う。 */
void          input_dir_clear(void);

/* 前回呼出以降に受信したキーに対応する IN_* のビット和 (キーリピートで
 * 届いた分も含む。 回数と順序は分からない)。 読むとクリアされる。 押し直しを
 * 見分けたい時は、 キーコードが一定時間途切れた後の受信を押下とみなす
 * (c_main.c のキー待ちがそうしている)。 */
unsigned char input_edges(void);

/* 前回呼出以降に発生したショット数 (人間の連打 + オート連射の合成)。
 * 読むとクリアされる。 255 で飽和する。 */
unsigned char break_shots(void);

/* BREAK 押下中なら 1、 でなければ 0 (タイマ IRQ でチャタリング除去済の確定状態)。 */
unsigned char break_held(void);

/* 前回呼出以降に ESC のキーコードが来た回数。 読むとクリアされる。 255 で
 * 飽和する。 キーリピートの分も数える。 この機種はキーを離したことを通知
 * しないので、 呼び手はキーコードが一定時間途切れた後の受信を新しい押下と
 * みなす。 */
unsigned char esc_edges(void);

#endif /* C_INPUT_H */
