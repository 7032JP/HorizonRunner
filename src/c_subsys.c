/* ============================================================
 * c_subsys.c — FM-7 サブシステム呼び出しの実装
 *
 * サブシステム ROM (= サブ CPU 側で動いている標準のサブシステム
 * プログラム) のコマンドの受け渡し:
 *
 *   $D380 (= main $FC80) : ATN bit7 (sub↔main の attention)
 *   $D381 (= main $FC81) : sub→main 応答コード
 *   $D382 (= main $FC82) : ★ main→sub コマンドコード (0=idle)
 *   $D383-$D38E          : コマンドパラメータ (12 byte)
 *
 * 共有 RAM アクセスは「サブ HALT」 が必須:
 *   メイン CPU の共有 RAM ($FC80-$FCFF) アクセスはサブ CPU を HALT して
 *   いる間に行う。 従って毎回 [HALT → R/W → RELEASE] を行う。
 *
 * 完了の見分け方:
 *   サブはコマンドを受け取ると BUSY ($FD05 bit7) を立て、 処理を
 *   終えると下ろす。 そこで RELEASE の後、 まず BUSY が立つのを待ち
 *   (= 受け取る前に「もう終わった」と読まないため)、 次に BUSY が
 *   下りるのを待つ。 どちらの BUSY 待ちも上限付き (asm_test.s の
 *   sub_wait_busy / sub_wait_ready) で、 上限に達すると待たずに進む。
 * ============================================================ */

#include "c_subsys.h"
#include "c_subprog.h"      /* sub_wait_busy / sub_wait_ready (asm_test.s) */

#define SHARED_RAM      ((volatile unsigned char *)0xFC80)

#define SH_RESPONSE      1
#define SH_COMMAND       2
#define SH_PARAM_BASE    3      /* $FC83 (サブ側 $D383) = params[0] */


/* halt → read offset → release → return byte */
static unsigned char shared_peek(unsigned char off)
{
    unsigned char v;
    subsys_halt();
    v = SHARED_RAM[off];
    subsys_release();
    return v;
}


unsigned char subsys_call(unsigned char cmd,
                          const unsigned char *params,
                          unsigned char param_len)
{
    unsigned char i;

    if (param_len > SUBSYS_MAX_PARAMS) {
        return 0xFF;
    }

    /* 1. sub HALT → params + cmd 書込 → sub RELEASE で起動 */
    subsys_halt();
    for (i = 0; i < param_len; i++) {
        SHARED_RAM[SH_PARAM_BASE + i] = params[i];
    }
    SHARED_RAM[SH_COMMAND] = cmd;
    subsys_release();

    /* 2. サブが受け取る (BUSY が立つ) のを待ち、 続けて処理を終える
     *    (BUSY が下りる) のを待つ。 */
    sub_wait_busy();
    sub_wait_ready();

    /* 3. BUSY が下りてからサブがコマンド待ちに戻るまでの猶予。 目的は
     *    asm_test.s の SUB_POST_WAIT (250us) と同じだが、 こちらは
     *    サブシステム ROM の標準コマンドの後始末を跨ぐため長めに取る
     *    (200 周 x 約 28 cycle = 約 5,600 cycle、 約 4.7ms)。 */
    {
        volatile unsigned int spin;
        for (spin = 0; spin < 200; spin++) { }
    }

    /* 4. 応答コード ($FC81、 サブ側 $D381) を読んで返す。 */
    return shared_peek(SH_RESPONSE);
}
