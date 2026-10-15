/* ============================================================
 * c_main.c — HorizonRunner 本体 (地平線 + 床スクロール + 自機 + 自弾 + 敵編隊
 *            + 地上物 (低木 / 木) + 空中の岩 + 敵弾 + 蛇型ボス + 当たり判定と
 *            爆発 + 得点と残機の表示 + タイトル / ゲームオーバーの画面)
 *
 * 画面の作り (= 描画フレーム 384x96 dot、描画フレームの左上は画面中央の 52 line / 16 byte):
 *
 *      +----------------------------------+  描画フレームの上端 (= 空。何も描かない)
 *      |                                  |
 *      |          (自機・自弾・敵)        |
 *      |==================================|  地平線 (黒で塗る。線は引かない)
 *      |                                  |  遠景 (帯が分解できないので黒)
 *      |----------------------------------|  床の帯 (緑。奥行きスクロール)
 *      |==================================|
 *      +----------------------------------+  描画フレームの下端
 *
 * ---- 三枚のプレーンの役割 ----
 *   B / R plane … ダブルバッファリングの表と裏。**自機・自弾・敵を白で描く**
 *   G plane     … **床のカラープレーン (緑)**。地平線の行は黒で塗る。ダブルバッファリングしない
 *
 * 白のページは毎フレーム裏で描き切ってからパレット差し替えで表に出す
 * (描いている途中のページは表に出さない)。床はカラープレーンに毎フレーム塗り直す。
 * ページに描いたキャラクタの消し込みは **サブ側が自分で憶えている**ので、
 * main から消し込みコマンドを送る必要は無い (docs/RENDER.md)。
 *
 * ---- 操作 ----
 * 初代 FM-7 は離鍵コードを返さないので方向はラッチ保持である。
 *   8/2/4/6 = 軸ごとに置換 / 7/9/1/3 = 両軸まとめて置換 (斜め) / 5 = 停止
 *   カーソルキー = 8/2/4/6 と同じ (c_input.h)
 *   BREAK   = 自弾の発射 (押しっぱなしでオート連射)
 *
 * 自機の上下に連動して地平線が逆へ動く (= カメラのピッチ)。前進は常時
 * 掛かっており、床の帯が手前へ流れ続ける。
 * ============================================================ */

#include "c_subsys.h"
#include "c_subprog.h"
#include "c_input.h"
#include "c_sound.h"
#include "c_text.h"
/* 床の遠近表・模様番号 (= scripts/make_floor.py がビルド時に焼く) と、
 * 敵の軌道表 (= scripts/make_enemy.py)。実行時に乗除算をしないための表で
 * あり、手で書き換えない。 */
#include "../build/floor_table.h"
#include "../build/enemy_path.h"
/* 地上物の滞在フレーム表 (= scripts/make_ground.py) と、OBJS コマンドで
 * 貼る絵の寸法表 (= scripts/make_sprite.py)。どちらも生成物。 */
#include "../build/ground_path.h"
#include "../build/sprite_geo.h"
/* 1 ステージのスクリプト (= scripts/make_stage.py)。秒で書いた出来事の一覧を
 * [符号, 次までのフレーム数] の表に落としたもの。 */
#include "../build/stage_script.h"
/* 追加絵 (爆発・敵の楕円弾・敵 B の近) の中身と置場 (= scripts/make_sprite.py)。
 * サブ側のコードの領域に入り切らないので、起動時に WRITE で VRAM 各プレーンの
 * 末尾へ送る。 */
#include "../build/expl_data.h"

/* フレームペーシング: 1 tick = 約 2ms、8 tick = 約 16.3ms = 約 61fps */
#define FRAME_TARGET        8
#define PACE_SAFETY_CAP     8000u

/* ---- 自機の可動範囲と速度 ----
 * 横は dot 単位 (= 8 位相の事前シフトがあるので 1 dot 刻みで置ける)。
 * 縦は line 単位。FM-7 の画素は縦長 (1 : 約 2.4) なので、横を縦より大きく
 * 刻まないと見かけの速度が釣り合わない。 */
#define HX_MIN      0
#define HX_MAX      (FRAME_W_DOTS - SPR_W_DOTS - 8)   /* 352 */
#define HY_MIN      0
#define HY_MAX      (FRAME_H_LINES - SPR_H_LINES - 1) /* 75 */
#define HSTEP       5                                  /* 横 5 dot / frame */
#define VSTEP       2                                  /* 縦 2 line / frame */

/* ---- 地平線 (= カメラのピッチ) ----
 * 自機が上へ行くほど地平線は下がる。必ず偶数行へ丸める (床の間引きの
 * 位相を揃えるため)。式 (HZ_MID + (HY_MID - hy) / 4) の値は 20〜39 で、
 * 偶数へ丸めて 20〜38。HZ_MIN / HZ_MAX はその範囲を保証する下限と上限。 */
#define HY_MID      ((HY_MIN + HY_MAX) / 2)   /* 37 */
#define HZ_MID      30
#define HZ_MIN      20
#define HZ_MAX      38

/* ---- 床 (擬似 3D、横一直線の縞) ----
 * 地平線から d line 下の床の「奥行き」は z = ZK / d である (= 透視投影)。
 * 帯の明暗は (z_lo[d] + zoff) の bit7 で決まり、zoff を毎フレーム進めると
 * 帯が手前へ流れる。帯 1 本の厚みは画面上で約 128 * d*d / ZK line (d の
 * 近くでの近似) なので、遠いほど薄く手前ほど厚い = 遠近が付く。
 *
 * 地平線に近い側 (d < FLR_D_ALIAS) は帯の厚みが 2 line を切って明滅に
 * なるため、**何も描かず黒のまま残す**。空と、地平線の下の暗がりだけが
 * 見える見通しの良い画になる。
 *
 * 床は 1 行おきに間引いて塗る (= 塗る line の数が約半分になる)。床の帯の
 * 説明で「行」と書いたら、この間引き後の論理行である (座標は line で書く)。 */
#define FLR_FAR_PAT FLR_SOLID_DARK  /* 遠景は黒 (= 何も描かない) */
#if FLR_LINES != FRAME_H_LINES
#error "FRAME_H_LINES と scripts/make_floor.py の LINES が食い違っている"
#endif

/* 1 フレームで進む奥行き (= z 単位)。地上物の滞在表もこの値で焼いてある。 */
#define ZSPEED      12
#if ZSPEED != GND_ZSPEED
#error "ZSPEED と scripts/make_ground.py の ZSPEED が食い違っている"
#endif

/* 帯の数の上限。hz 20〜38 と zoff 全てで実際の帯は最大 9 本で、余裕を
 * 持たせた数 (帯 1 本 2 byte)。 */
#define MAX_BANDS   24

/* ---- 自弾 ----
 * この手のゲームの自弾は「奥へ飛んでいく」。撃った位置から **画面中央**
 * へ向かって直線的に縮んでいけばそう見える。収束先を地平線にすると上に
 * 寄り過ぎるので、描画フレームのちょうど真ん中を狙う。
 *
 * 座標も進み幅も 8bit で持つ。16bit にすると毎フレームの加算と比較の
 * 負担が増えるので、8bit に収める。横位置は 2 dot 単位で持てば 0-191 に
 * 収まり、絵も 2 dot 刻みの位相で貼るので精度は足りる。 */
#define MAX_BUL     4
#define BUL_LIFE    16                      /* 寿命 (フレーム)。進み幅は差の 1/16 (切り捨て) なので、尽きる時は収束点の近く */
#define VANISH_HX   (FRAME_W_DOTS / 4)      /* 画面中央 x (2 dot 単位) = 96 */
#define VANISH_Y    (FRAME_H_LINES / 2)     /* 画面中央 y (line) = 48 */

static unsigned char bul_hx[MAX_BUL];   /* 描画フレーム内 x (2 dot 単位、0-191) */
static unsigned char bul_y[MAX_BUL];    /* 描画フレーム内 line (0-95) */
static unsigned char bul_dx[MAX_BUL];   /* 1 フレームの進み幅 (2 の補数) */
static unsigned char bul_dy[MAX_BUL];
static unsigned char bul_life[MAX_BUL]; /* 0 = 空きスロット */
/* 当たり判定用 (毎フレーム、進めた直後に 1 回だけ求めて憶える) */
static unsigned char bul_k[MAX_BUL];    /* 今フレームの絵の種類 (15-17) */
static unsigned char bul_x1[MAX_BUL];   /* 右端 (2 dot 単位、= hx + 幅) */
static unsigned char bul_y1[MAX_BUL];   /* 下端 (= y + 高さ) */
static unsigned char bul_st[MAX_BUL];   /* 奥行きの段階 (2 手前 / 1 中 / 0 奥) */
static unsigned char bul_pop[MAX_BUL];  /* 木に当たって爆ぜている残りフレーム (0 = 通常) */

/* ---- 当たり判定 (= 8bit の矩形比較だけ。乗除算は無い) ----
 * ここに書くのは敵・岩・地上物との判定。ボスは奥行きを比べず矩形だけ
 * (頭だけが自弾で傷付き、自機とは最も手前の段階で当たる)、敵弾は経過
 * フレーム数で当たる期間を決める (それぞれの箇所に書く)。
 * 奥行きは「段階」で比べる。敵・岩は軌道表の段階 (0 奥 / 1 中 / 2 手前)、
 * 地上物は d を GND_STAGE_* で刻んだ段階、自弾は残り寿命で刻んだ段階
 * (撃った直後 = 2、収束点の近く = 0)。**自弾の段階が対象と同じか 1 つ
 * 手前** なら奥行きが合っているとみなし、あとは画面上の矩形 (x は
 * 2 dot 単位、y は line) が重なれば命中。矩形の幅は sprite_geo.h の
 * obj_w (byte) x 4、高さは obj_h をそのまま使う。
 *
 * 自機は「対象が最も手前の段階 (2)」の時だけ当たる。自機の矩形は絵より
 * ひと回り小さく取る (= 理不尽な被弾を減らす)。 */
#define PL_INSET_X     4            /* 自機の当たり判定の矩形の内寄せ (2 dot 単位) */
#define PL_INSET_Y     6            /* 同 (line) */
#define PL_INV_FRAMES  240          /* 復帰後の無敵 (点滅) フレーム数 = 約 4 秒 */
#define PL_DIE_FRAMES  45           /* 被弾の演出。この間は敵ごと画面を止める */

/* ---- 爆発 ----
 * 撃たれた敵・岩・低木は、そのスロットのまま「爆発している状態」になり、
 * 爆発の絵を EXP_LEN フレーム見せて消える (= 表を増やさない)。敵・岩は
 * その場に留まり、低木は床と一緒に流れ続ける。
 * 絵は対象の段階と同じ大きさで、A (芯) → B (破片) → A の 3 段階。 */
#define EXP_LEN        10
#define EXP_KIND(t)    (((t) > 7 || (t) < 4) ? OBJ_K_EXPL_A : OBJ_K_EXPL_B)
#define POP_LEN        2            /* 木に当たった弾が爆ぜて見える長さ */

/* ---- 敵と岩 ----
 * 奥から迫って来る。軌道は build/enemy_path.h の表を引くだけで、実行時の
 * 乗除算は 1 回も無い。y は地平線からの相対 (2 の補数) なので、カメラの
 * ピッチにそのまま追従して地面に貼り付いて見える。空を飛ぶ物は y が
 * 負なだけで、同じ仕組みで動く。軌道ごとにステップ数とステップの間隔 (毎フレーム /
 * 2 フレームに 1 ステップ) と絵の種類が表に焼いてある。
 *
 * 自弾と自機の当たり判定も、軌道表の段階と 8bit の矩形だけで行う。 */
#define MAX_ENM     8

static unsigned char enm_step[MAX_ENM];  /* 0 = 空き。1..plen が生存 */
static unsigned char enm_path[MAX_ENM];
static unsigned char enm_ex[MAX_ENM];    /* 爆発の残りフレーム (0 = 生きている) */
/* 最後に描いた位置と絵の種類。爆発の間と被弾の演出中はここに描く
 * (hx = 255 は左端の外で描いていない印)。 */
static unsigned char enm_hx[MAX_ENM];
static unsigned char enm_hy[MAX_ENM];
static unsigned char enm_hk[MAX_ENM];

/* ---- 地上物 (低木 / 木) ----
 * 床と同じ速さで手前へ流れる景色であり、床に貼り付いて見えることが
 * 何より大事である。そこで床と同じ式 (z を毎フレーム ZSPEED 減らし、
 * d = ZK / z) を **「d に留まるフレーム数」の表** (gnd_dwell[]) に焼き、
 * 実行時は残りフレームを 1 減らして 0 なら d を 1 進めるだけにする。
 * 横位置はレーンごとの表 x[d] を引く (透視投影で d に比例するため)。
 * 絵の底が地面 (地平線から d line 下) に着くよう、上端は d - 高さ に置く。
 *
 * 種類ごとの性質は gnd_hard[] に持つ (低木は撃つと壊れる / 木は壊れない)。
 * 当たり判定ではここを見る。 */
#define MAX_GND     6
#define GND_KINDS   2

static unsigned char gnd_d[MAX_GND];      /* 0 = 空き。GND_D0..GND_D1 が生存 */
static unsigned char gnd_cnt[MAX_GND];    /* いまの d に留まる残りフレーム数 */
static unsigned char gnd_kind[MAX_GND];   /* 0 低木 / 1 木 */
static const unsigned char *gnd_xp[MAX_GND]; /* レーンの x[d] 表 (= 添字計算を避ける) */
static unsigned char gnd_ex[MAX_GND];     /* 爆発の残りフレーム (0 = 生きている) */

/* 種類 → 絵の起点 / 壊れないか (1 = 撃っても壊れない障害物) */
static const unsigned char gnd_kb[GND_KINDS]   = { OBJ_K_BUSH, OBJ_K_TREE };
static const unsigned char gnd_hard[GND_KINDS] = { 0, 1 };

/* 絵の段階を切り替える d。手前ほど大きい絵になる。 */
#define GND_STAGE_MID   16
#define GND_STAGE_NEAR  32

/* ---- 敵弾 ----
 * 0 自機狙い楕円弾 / 1 低空黒球弾 / 2 ボスの敵弾。
 * 発射時だけ自機までの差から進み幅を作り (48 フレームで自機の中心付近 (横は
 * 中央、縦は上から 8 line) へ届く速さ = 差 x 16/3 を 1/256 単位で持つ)、以後は
 * 8bit 加算だけで進める。低空黒球弾だけは縦の進み幅をこの計算で作らず、
 * 地面すれすれを手前へ流れる固定の速さ (EBUL_LOW_DYL) にする。
 *
 * 進み幅は **整数部 + 小数部 (1/256)** の 2 byte で持つ。整数だけだと差が
 * 小さい時に切り捨てで狙いがずれる。小数部の桁上がりは 8bit の比較 1 つで
 * 拾う (16bit 演算は使わない)。毎フレーム小数込みで進めるので、遅くても
 * 動きは滑らかである。
 *
 * 経過フレーム数 (ebul_age) は出す時に 1 で、動かすたびに 1 増える。出した
 * フレームにも動かして描くので、最初に描く時は 2 である。寿命と下の
 * しきい値はこの値で比べる。
 * 当たる経過フレーム数は種類で分ける。楕円弾と黒球は自機の奥行きを
 * 通り過ぎる間 (EBUL_HIT_AGE0..1) だけ当たり、奥で小さく描かれている間は
 * 当たらない。ボスの撃つ敵弾は手前の段階に入る前から当たる。 */
#define MAX_EBUL       6
#define EBUL_FRAMES   78             /* 寿命 (フレーム) */
#define EBUL_ST_MID   27             /* 絵が中の段階になる経過フレーム数 */
#define EBUL_ST_NEAR  51             /* 絵が近の段階になる経過フレーム数 */
#define EBUL_AIM       0
#define EBUL_LOW       1
#define EBUL_FIRE      2
#define EBUL_HIT_AGE0  42            /* 楕円弾・黒球が当たる経過フレーム数の範囲 */
#define EBUL_HIT_AGE1  60
#define EBUL_FIRE_HIT  18            /* ボスの敵弾が当たり始める経過フレーム数 */
#define EBUL_LOW_DYL  171            /* 黒球が地面すれすれを手前へ流れる速さ (171/256 line) */
static unsigned char ebul_hx[MAX_EBUL];
static unsigned char ebul_y[MAX_EBUL];
static unsigned char ebul_fx[MAX_EBUL];        /* 位置の小数部 (1/256) */
static unsigned char ebul_fy[MAX_EBUL];
static unsigned char ebul_dx[MAX_EBUL];        /* 進み幅の整数部 (2 の補数) */
static unsigned char ebul_dy[MAX_EBUL];
static unsigned char ebul_dxl[MAX_EBUL];       /* 進み幅の小数部 (1/256) */
static unsigned char ebul_dyl[MAX_EBUL];
static unsigned char ebul_age[MAX_EBUL];       /* 0 = 空き */
static unsigned char ebul_type[MAX_EBUL];
static unsigned char ebul_side;

/* ---- 蛇型ボス ----
 * 頭の位置を 64 フレームの輪に残し、胴体は一定間隔前の位置を読む。
 * 掛け算も三角関数も使わず、自然な蛇行になる。頭だけが弱点。
 *
 * 頭の位置は **整数部 + 小数部 (1/256)** の 16bit で持ち、毎フレーム
 * 小さく進める (= 1 フレームに 1 未満の速さでも飛ばずに滑らかに動く)。
 * 描画と当たり判定は整数部 (boss_hx / boss_y) だけを使う。 */
#define BOSS_SEGS      7
#define BOSS_HIST     64
#define BOSS_HP       12
/* 速さ (1/256 単位)。頭の動きの刻みはすべてここから組む。 */
#define BV_SLOW      171             /* 2/3 */
#define BV_CREEP      43             /* 1/6 */
#define BV_LOOP      512             /* 2 */
#define BV_DASH      683             /* 8/3 */
/* ボスの状態 (大域変数)。このファイルの外からは参照しないが、game_state と
 * 同じく build/horizonrunner.map でアドレスを引けるよう static にしない。 */
unsigned char boss_on;
unsigned char boss_phase;
unsigned char boss_hp;
unsigned char boss_die;
static unsigned char boss_t;
static unsigned char boss_hx, boss_y, boss_hi, boss_flash;
static unsigned int boss_px, boss_py;          /* 頭の位置 (整数部 + 小数部) */
static int boss_vx, boss_vy;                   /* 進み幅 (1/256 単位、2 の補数) */
static unsigned char boss_hist_x[BOSS_HIST];
static unsigned char boss_hist_y[BOSS_HIST];
/* 胴の各節が頭の何フレーム前の位置を読むか。節の間隔は頭の速さに比例
 * するので、頭の速さを変えたらここも釣り合う値にする (上限 BOSS_HIST - 1。
 * BOSS_HIST では輪を一周して頭と同じ位置に戻る)。 */
static const unsigned char boss_delay[BOSS_SEGS] = { 0,9,18,27,36,45,54 };

#define BOSS_POS(v)  ((unsigned int)(v) << 8)   /* 整数の位置 → 16bit */

/* 消し込み表は自機 1 + OBJS 25 件ぶん。通常時は
 * 自弾 4 + 敵 8 + 地上物 6 + 敵弾 6 + 自機爆発 1 = 25。ボス開始時は敵と地上物を
 * 消すので、ボス 7 体を加えても 自弾 4 + 敵弾 6 + ボス 7 + 自機爆発 1 = 18 で
 * この上限を下回る。 */
#define MAX_OBJS    25

/* ---- スクリプト (= ステージの構成) ----
 * build/stage_script.h の stage_script[] を先頭から読む。
 * [符号, 次までのフレーム数] の並びで、符号は次のとおり (make_stage.py)。
 *   0x00-0x3F : 敵・岩の軌道番号 (ENM_PATH_xxx)。同じ軌道へ間隔を空けて
 *               何体も流し込めば、横一列や蛇のような編隊になる。
 *   0x40-0x4F : 敵弾 (0 楕円 / 1 低空黒球 / 2 ボスの敵弾)
 *   0x50-0x5F : 蛇型ボスの局面 (0 接近 / 1 ループ / 2 退避 / 3 連射 / 4 至近)
 *   0x80-0xBF : 地上物。bit3-5 = 種類 (0 低木 / 1 木)、bit0-2 = レーン
 *   0xFE      : 何も出さずに待つ
 *   0xFF      : ステージの終わり (先頭へ戻る)
 * スクリプトの中身は秒で書いた一覧から生成する。時刻や体数を直す時は
 * scripts/make_stage.py を直す。 */

static unsigned char bands[MAX_BANDS * 2];

/* フレーム数と自機の状態 (大域変数。boss_on と同じ理由で static にしない)。 */
unsigned int frame_cnt;
unsigned char pl_inv;       /* 自機の無敵 (点滅) の残りフレーム */
/* 被弾の演出。0 でない間は **敵も床も自弾も止め、自機の爆発だけを見せる**。
 * 0 になったら無敵 (点滅) のまま再開する。動きの中で被弾が埋もれて
 * しまわないよう、当たった瞬間を止めて見せるための仕掛けである。 */
unsigned char pl_die;
static unsigned char pl_ex_hx;   /* 爆発の位置 (2 dot 単位 / line) */
static unsigned char pl_ex_y;

static void delay_loop(unsigned int units)
{
    volatile unsigned int i;
    volatile unsigned char j;
    for (i = 0; i < units; i++) {
        for (j = 0; j < 200; j++) { /* spin */ }
    }
}

/* ---- 床の帯を組み立てる ----
 * 地平線 hz (描画フレーム内 line) と奥行きスクロールの位相 zoff から、sub_scene() に
 * 渡す [行数, 模様] の並びを bands[] に作り、帯の数を nb へ入れる。ステージの中
 * (play) とタイトル画面の背景の両方がこれを使う。
 * 関数にせずマクロにしてあるのは、ステージの中で呼出しの手間を 1 つも足さない
 * ため。
 * ★ ステージの中では 1 フレームに 1 回まわり、中のループは地平線の高さ
 *   (hz は自機の上下で 20〜38) に応じて 17〜26 回まわる。
 *   **乗除算を絶対に置かない**こと (CMOC の乗除算は実行時ルーチンの
 *   呼出になり、ここに 1 つ置くだけでフレームレートを大きく落とす)。 */
#define FLOOR_BANDS(nb, hz, zoff)                                           \
    do {                                                                    \
        unsigned char rows_, dd_, run_, par_, npar_;                        \
        rows_ = (unsigned char)((FRAME_H_LINES - 1 - (hz)) >> 1);           \
        (nb) = 0;                                                           \
        if (rows_) {                                                        \
            unsigned char far_;                                             \
            far_ = (rows_ < (unsigned char)((FLR_D_ALIAS - 1) >> 1))        \
                       ? rows_ : (unsigned char)((FLR_D_ALIAS - 1) >> 1);   \
            bands[0] = far_;        /* 遠景は 1 本の黒い帯にまとめる */      \
            bands[1] = FLR_FAR_PAT;                                         \
            (nb) = 1;                                                       \
            if (rows_ > far_) {                                             \
                run_ = (unsigned char)(far_ + 1);                           \
                /* 遠近表 z_lo[] は物理 line 単位なので論理行を 2 倍して引く */ \
                par_ = (unsigned char)((z_lo[run_ << 1] + (zoff)) & 0x80);  \
                for (dd_ = (unsigned char)(run_ + 1); dd_ <= rows_; dd_++) { \
                    npar_ = (unsigned char)((z_lo[dd_ << 1] + (zoff)) & 0x80); \
                    if (npar_ != par_) {                                    \
                        if ((nb) < MAX_BANDS) {                             \
                            bands[(nb) * 2]     = (unsigned char)(dd_ - run_); \
                            bands[(nb) * 2 + 1] = par_ ? FLR_SOLID_LIGHT    \
                                                       : FLR_SOLID_DARK;    \
                            (nb)++;                                         \
                        }                                                   \
                        par_ = npar_;                                       \
                        run_ = dd_;                                         \
                    }                                                       \
                }                                                           \
                if ((nb) < MAX_BANDS) {                                     \
                    bands[(nb) * 2]     = (unsigned char)(rows_ - run_ + 1); \
                    bands[(nb) * 2 + 1] = par_ ? FLR_SOLID_LIGHT            \
                                               : FLR_SOLID_DARK;            \
                    (nb)++;                                                 \
                }                                                           \
            }                                                               \
        }                                                                   \
    } while (0)

/* 敵弾を 1 発出す。sx/sy は描画フレーム内座標 (x は 2 dot 単位)。 */
static void enemy_bullet(unsigned char type, unsigned char sx,
                         unsigned char sy, unsigned int hx, unsigned char hy)
{
    unsigned char i;
    int dx, dy;
    for (i = 0; i < MAX_EBUL; i++) if (ebul_age[i] == 0) break;
    if (i >= MAX_EBUL) return;

    /* 48 フレームで自機の中心付近 (横は中央、縦は上から 8 line) へ届く進み幅
     * = 差 x 16/3 (1/256 単位)。
     * 乗除算を置かず、差 x (4 + 1 + 1/4 + 1/16) = 差 x 5.3125 で近似する
     * (発射時のみの 16bit 演算。以後は 8bit 加算だけ)。 */
    dx = (int)((hx >> 1) + 6) - (int)sx;
    dy = (int)(hy + 8) - (int)sy;
    dx = (dx << 2) + dx + (dx >> 2) + (dx >> 4);
    dy = (dy << 2) + dy + (dy >> 2) + (dy >> 4);
    ebul_dx[i]  = (unsigned char)(dx >> 8);
    ebul_dxl[i] = (unsigned char)dx;
    ebul_dy[i]  = (unsigned char)(dy >> 8);
    ebul_dyl[i] = (unsigned char)dy;
    if (type == EBUL_LOW) {             /* 地面すれすれを手前へ流す */
        ebul_dy[i] = 0;
        ebul_dyl[i] = EBUL_LOW_DYL;
    }
    ebul_hx[i] = sx;
    ebul_y[i] = sy;
    ebul_fx[i] = 0;
    ebul_fy[i] = 0;
    ebul_type[i] = type;
    ebul_age[i] = 1;
}

static void boss_set_phase(unsigned char p, unsigned char hz)
{
    unsigned char i;
    if (!boss_on) {
        boss_on = 1;
        boss_hp = BOSS_HP;
        boss_die = 0;
        boss_flash = 0;
        boss_hx = 84;
        boss_y = 4;
        boss_px = BOSS_POS(boss_hx);
        boss_py = BOSS_POS(boss_y);
        boss_hi = 0;
        for (i = 0; i < BOSS_HIST; i++) {
            boss_hist_x[i] = boss_hx;
            boss_hist_y[i] = boss_y;
        }
        /* ボス戦へ通常物を持ち込まず、消し込み表の上限も守る。 */
        for (i = 0; i < MAX_ENM; i++) enm_step[i] = 0;
        for (i = 0; i < MAX_GND; i++) gnd_d[i] = 0;
        for (i = 0; i < MAX_EBUL; i++) ebul_age[i] = 0;
    }
    boss_phase = p;
    boss_t = 0;
    if (p == 1) { boss_vx = BV_LOOP; boss_vy = BV_SLOW; }
    /* 局面の切り替えは stage_script が決める。5 連射 (局面 3) は左下から
     * 斜めに再接近しながら撃つ。頭が動き続けるので胴が斜めに連なり、敵弾が
     * 胴に沿って数珠つなぎに見える。 */
    if (p == 3) {
        boss_hx = 28;
        boss_y = (hz > 6) ? (unsigned char)(hz - 6) : 0;
        boss_px = BOSS_POS(boss_hx);
        boss_py = BOSS_POS(boss_y);
    }
    if (p == 4) { boss_vx = -BV_DASH; boss_vy = BV_SLOW; }
}

static void boss_update(unsigned int hx, unsigned char hy, unsigned char hz)
{
    unsigned char target;
    if (!boss_on) return;
    if (boss_die) {
        boss_die--;
        if (!boss_die) boss_on = 0;
        return;
    }
    boss_t++;
    switch (boss_phase) {
    case 0:                             /* 奥から接近しながら敵弾を撃つ */
        target = (hz > 12) ? (unsigned char)(hz - 12) : 4;
        if (boss_y < target) boss_py += BV_CREEP;
        if (boss_t & 16) boss_px += BV_SLOW; else boss_px -= BV_SLOW;
        if (boss_px < BOSS_POS(56))  boss_px = BOSS_POS(56);
        if (boss_px > BOSS_POS(112)) boss_px = BOSS_POS(112);
        if ((boss_t & 63) == 1)
            enemy_bullet(EBUL_FIRE, boss_hx, boss_y, hx, hy);
        break;
    case 1:                             /* 輪を描く蛇行 (弾は撃たない) */
        boss_px = (unsigned int)(boss_px + (unsigned int)boss_vx);
        boss_py = (unsigned int)(boss_py + (unsigned int)boss_vy);
        if (boss_hx < 20) boss_vx = BV_LOOP; else if (boss_hx > 156) boss_vx = -BV_LOOP;
        if (boss_y < 5) boss_vy = BV_SLOW; else if (boss_y > 42) boss_vy = -BV_SLOW;
        break;
    case 2:                             /* 画面奥へ退避しつつ敵弾 1 発 */
        if (boss_y > 7) boss_py -= BV_SLOW;
        if (boss_hx < 84) boss_px += BV_SLOW; else if (boss_hx > 84) boss_px -= BV_SLOW;
        if (boss_t == 40)
            enemy_bullet(EBUL_FIRE, boss_hx, boss_y, hx, hy);
        break;
    case 3:                             /* 左下から斜めに寄りつつ 5 連射 */
        if (boss_hx < 150) boss_px += BV_SLOW;
        if (boss_y > 4) boss_py -= BV_CREEP;
        if (boss_t == 1 || boss_t == 25 || boss_t == 49 ||
            boss_t == 73 || boss_t == 97)
            enemy_bullet(EBUL_FIRE, boss_hx, boss_y, hx, hy);
        break;
    default:                            /* 至近戦。自機の高さを左右へ大きく蛇行 */
        /* 頭は自機の正面 (y 20-44) を往復する。範囲は「外れたら
         * 戻る向きにする」形で書き、範囲外から入った時に符号が毎フレーム
         * 反転して立ち往生しないようにする。 */
        boss_px = (unsigned int)(boss_px + (unsigned int)boss_vx);
        boss_py = (unsigned int)(boss_py + (unsigned int)boss_vy);
        if (boss_hx < 8) boss_vx = BV_DASH; else if (boss_hx > 164) boss_vx = -BV_DASH;
        if (boss_y < 20) boss_vy = BV_SLOW; else if (boss_y > 44) boss_vy = -BV_SLOW;
        if ((boss_t & 31) == 1)
            enemy_bullet(EBUL_FIRE, boss_hx, boss_y, hx, hy);
        break;
    }
    boss_hx = (unsigned char)(boss_px >> 8);
    boss_y  = (unsigned char)(boss_py >> 8);
    boss_hi = (unsigned char)((boss_hi + 1) & (BOSS_HIST - 1));
    boss_hist_x[boss_hi] = boss_hx;
    boss_hist_y[boss_hi] = boss_y;
}

/* スクリプトの 1 項目を実体にする。空きが無ければその 1 体は諦める。 */
static void spawn(unsigned char code, unsigned int hx, unsigned char hy,
                  unsigned char hz)
{
    unsigned char bi;
    if (code == SC_WAIT) return;
    if (code & SC_GND) {                    /* 地上物 (bit7) */
        for (bi = 0; bi < MAX_GND; bi++) {
            if (gnd_d[bi] == 0) break;
        }
        if (bi < MAX_GND) {
            gnd_d[bi]    = GND_D0;
            gnd_ex[bi]   = 0;
            gnd_cnt[bi]  = gnd_dwell[GND_D0];
            gnd_kind[bi] = (unsigned char)((code >> 3) & 7);
            gnd_xp[bi]   = &gnd_x[gnd_xbase[code & 0x07]];
        }
    } else if (code < SC_SHOT) {            /* 敵・岩 (軌道番号) */
        for (bi = 0; bi < MAX_ENM; bi++) {
            if (enm_step[bi] == 0) break;
        }
        if (bi < MAX_ENM) {
            enm_step[bi] = 1;
            enm_ex[bi]   = 0;
            enm_path[bi] = code;
        }
    } else if (code < SC_BOSS) {            /* 敵弾 */
        unsigned char type = (unsigned char)(code & 0x0F);
        unsigned char sx;
        ebul_side ^= 1;
        sx = ebul_side ? 42 : 142;
        if (type > EBUL_LOW) type = EBUL_FIRE;
        enemy_bullet(type, sx,
                     (type == EBUL_LOW) ? (unsigned char)(hz + 2)
                                        : (unsigned char)(hz - 8),
                     hx, hy);
    } else if (code < SC_BOSS + 0x10) {     /* ボスの局面 */
        boss_set_phase((unsigned char)(code & 0x0F), hz);
    }
}

/* ============================================================
 * 遊びの外側 — タイトル / 残機 / 得点 / ゲームオーバー / ステージクリア
 *
 * 文字は c_text.c が WRITE 命令で VRAM へ直に書く (サブ側は増やさない)。
 * 得点と残機は **描画フレーム (384x96) の外**、カラープレーン (緑) の上に置く。情景
 * コマンドは描画フレームの中しか触らないので、一度書けば消えない。値が変わった
 * 時に書き直しを予約し、**次のフレームの頭で**書き直す (毎フレームの仕事を
 * 増やさないため)。
 * ============================================================ */

/* 表示の位置 (画面全体の byte / line。描画フレームは 16-63 byte / 52-147 line) */
#define HUD_Y          40           /* 描画フレームの上、8 line ぶんの行 */
#define HUD_SCORE_X    16
#define HUD_VAL_X      22           /* 得点の数字 6 桁 */
#define HUD_HI_X       36           /* ハイスコアの見出し "HI" */
#define HUD_HIVAL_X    39           /* ハイスコアの数字 6 桁 */
#define HUD_REST_X     56
#define HUD_LIFE_X     61           /* 残機の数字 1 桁 */
/* ステージクリアの表示 (STAGE CLEAR の文字) は描画フレームの中の空へ出す。情景コマンドが消しに来るのは地平線
 * (描画フレーム内 20 line 以降) から下だけなので、ここは塗り潰されない。 */
#define BANNER_X       29
#define BANNER_Y       (FRAME_Y_LINE + 6)
#define BANNER_W       22           /* 横 2 倍の 11 字 = 22 byte */

#define LIVES_START    5
#define OVER_DELAY     (PL_DIE_FRAMES + 20)  /* 最後の 1 機が爆ぜてからゲームオーバーの画面に切り替わるまで */
#define CLEAR_SHOW     150          /* ステージクリアの表示を出しておく長さ */
#define OVER_SHOW      600          /* ゲームオーバーの画面を出しておく長さ (約 10 秒) */
#define OVER_ESC_FROM  120          /* ESC で飛ばせるようになるまで (約 2 秒) */
/* ESC のキーコードがこれだけのフレーム続けて来なかった後の ESC を、新しい押下
 * とみなす。押したままだとキーリピートのコードが来続けるので、この数は
 * リピートが始まるまでの間隔より長く取る (約 1 秒)。 */
#define OVER_ESC_QUIET 60

/* 書き直しの予約 (= 次のフレームの頭でまとめて送る)。得点と残機を分けて
 * あるのは、撃破のたびに残機まで書き直さない (= 文字の WRITE を増やさない)
 * ためである。 */
#define HUD_D_SCORE    0x01         /* 得点の数字 6 桁 */
#define HUD_D_BANNER   0x02         /* ステージクリアの表示を出す */
#define HUD_D_UNBANNER 0x04         /* ステージクリアの表示を消す */
#define HUD_D_LIFE     0x08         /* 残機の数字 1 桁 */
#define HUD_D_VAL      (HUD_D_SCORE | HUD_D_LIFE)

/* いまの場面 (大域変数)。 */
#define GS_BOOT    0        /* 起動中 (main() の先頭で代入する) */
#define GS_TITLE   1
#define GS_CLEAR   2
#define GS_OVER    3
#define GS_PLAY    4
unsigned char game_state;
unsigned char lives;                /* 残機 (0 = 後が無い) */
static unsigned char hud_dirty;
static unsigned char over_t;        /* 残機切れ後の秒読み (0 = 継続) */
static unsigned char clear_t;       /* ステージクリアの表示の残りフレーム */
/* 得点は 10 進の桁で持つ (掛け算も割り算もしないため)。[0] が 10 万の位。 */
static unsigned char sc_dig[6];
static char sc_str[7];
/* ハイスコア。電源を切るまで (= プログラムが動いている間) 持つ。crt0 は
 * bss を消さないので main() の頭で 0 にする。hi_live = 1 は「この回の得点が
 * もうハイスコアを超えている」(= 以後は比べずに写すだけ)。 */
static unsigned char hi_dig[6];
static unsigned char hi_live;

/* 桁 place (0 = 最上位) に amount (0-9) を足し、繰り上がりを上へ伝える。
 * 実行時の乗除算は無い。 */
static void score_add(unsigned char place, unsigned char amount)
{
    signed char i = (signed char)place;
    unsigned char v = amount;
    while (v && i >= 0) {
        unsigned char d = (unsigned char)(sc_dig[i] + v);
        if (d >= 10) { sc_dig[i] = (unsigned char)(d - 10); v = 1; }
        else         { sc_dig[i] = d;                      v = 0; }
        i--;
    }
    if (v) {    /* 最上位の桁から繰り上がった = 999999 を越えた。999999 で頭打ちにする */
        for (i = 0; i < 6; i++) sc_dig[i] = 9;
    }
    hud_dirty |= HUD_D_SCORE;
    /* ハイスコアも一緒に更新する (6 byte の比較と写しだけ。描画はしない) */
    if (!hi_live) {
        for (i = 0; i < 6; i++) {
            if (sc_dig[i] != hi_dig[i]) {
                if (sc_dig[i] > hi_dig[i]) hi_live = 1;
                break;
            }
        }
    }
    if (hi_live) {
        for (i = 0; i < 6; i++) hi_dig[i] = sc_dig[i];
    }
}

/* 6 桁の数字を "000000" の並びにする (タイトル・ゲームオーバーの画面・HUD で使い回す) */
static const char *digits_text(const unsigned char *dig)
{
    unsigned char i;
    for (i = 0; i < 6; i++) sc_str[i] = (char)('0' + dig[i]);
    sc_str[6] = '\0';
    return sc_str;
}
#define score_text()  digits_text(sc_dig)
#define hi_text()     digits_text(hi_dig)

/* ハイスコアを HUD と同じ行に書く (タイトルとステージの頭で 1 回だけ。ステージの中では
 * 書き直さない = プレイ中のハイスコアの描画処理は増やさない)。 */
static void hi_put(void)
{
    text_put(TEXT_PLANE_G, HUD_HI_X, HUD_Y, "HI");
    text_put(TEXT_PLANE_G, HUD_HIVAL_X, HUD_Y, hi_text());
}

/* 被弾。**画面を止めて自機の爆発を見せ、無敵で再開する**までを 1 か所で
 * 決める。残機を 1 つ減らし、尽きたらゲームオーバーの画面へ切り替えるまでの秒読みを始める。
 *
 * 無敵の残りフレーム (pl_inv) はここで一度に積む。演出の間は自機を描かない
 * = 減らさないので、**止まっている間は目減りせず、再開してから 4 秒**残る。 */
static void player_hit(unsigned int hx, unsigned char hy)
{
    pl_inv   = PL_INV_FRAMES;
    pl_die   = PL_DIE_FRAMES;
    pl_ex_hx = (unsigned char)(hx >> 1);
    pl_ex_y  = (unsigned char)(hy + 3);
    input_dir_clear();          /* 方向のラッチを解く (= 復帰後に流されない) */
    sfx_destroy();
    if (lives) {
        lives--;
        hud_dirty |= HUD_D_LIFE;
    }
    if (lives == 0 && over_t == 0) over_t = OVER_DELAY;
}

/* 1 フレーム待つ (= タイトルとゲームオーバーの画面でだけ使う。ステージの中は
 * play() の中に直に書いてある同じ待ちを使い、呼出しの手間を省く)。 */
static void wait_frame(void)
{
    unsigned int guard = 0;
    while (timer_get() < FRAME_TARGET && guard < PACE_SAFETY_CAP) guard++;
    timer_consume(FRAME_TARGET);
}

/* 予約された書き直しを送る。**非同期の描画とかち合わないよう**、中で
 * sub_sync() を通してから送る。 */
static void hud_service(void)
{
    if (!hud_dirty) return;
    sub_sync();
    if (hud_dirty & HUD_D_SCORE) {
        text_put(TEXT_PLANE_G, HUD_VAL_X, HUD_Y, score_text());
    }
    if (hud_dirty & HUD_D_LIFE) {
        char rest[2];
        rest[0] = (char)('0' + lives);
        rest[1] = '\0';
        text_put(TEXT_PLANE_G, HUD_LIFE_X, HUD_Y, rest);
    }
    if (hud_dirty & HUD_D_BANNER) {
        text_put_wide(TEXT_PLANE_G, BANNER_X, BANNER_Y, "STAGE CLEAR");
    }
    if (hud_dirty & HUD_D_UNBANNER) {
        text_erase(TEXT_PLANE_G, BANNER_X, BANNER_Y, BANNER_W, 8);
    }
    hud_dirty = 0;
    sub_flush();
}

/* 何か押されたか (BREAK か RETURN)。 */
static unsigned char start_pressed(void)
{
    if (break_shots()) return 1;
    if (input_edges() & IN_START) return 1;
    return 0;
}

/* ---- タイトル画面 ----
 * 背景ではステージの中と同じ床 (FLOOR_BANDS() + sub_scene()) を流し、自機・敵・
 * 弾は出さない。床はカラープレーン (緑) の描画フレームの中だけを塗るので、字と
 * 図は **描画フレームの外** (= ステージの中で得点・残機が載っている所と同じ扱い) に置き、
 * 毎フレームの床の塗りで消えもちらつきもしないようにする。
 *
 *   line  12      タイトルの文字 (白、横 2 倍)
 *   line  28      PUSH BREAK KEY TO START (点滅)
 *   line  40      HI 000000 (hi_put()。ステージの中の得点と同じ行)
 *   line  52-147  描画フレーム = 床が流れる
 *   line 150-197  テンキーの図 (横 2 倍) と操作の説明
 *
 * 白のページは 0 番だけを表に出したまま切り替えない (絵を貼らないので、
 * サブの消し込みも起きない)。 */
#define TITLE_Y        12           /* タイトルの文字 */
#define TITLE_PUSH_X   28
#define TITLE_PUSH_Y   28           /* 点滅する案内 */
/* 開始を受け付けるのは、BREAK が押されておらず RETURN のキーコードも来ない
 * 状態がこれだけのフレーム続いた後の押下だけ。それまでの押下は捨てる (ステージの中の
 * BREAK 連射を押したままゲームオーバーの画面を抜けて来ても、そのまま
 * 始まらない)。押したままだと RETURN はキーリピートのコードが来続け、
 * BREAK は押下中のまま読めるので、この数はリピートが始まるまでの間隔より
 * 長く取る (約 1 秒)。 */
#define TITLE_QUIET    60
#define PAD_X          24           /* テンキーの図の左端 (byte) */
#define PAD_Y          150          /* テンキーの図の上端 (line) */
#define PAD_DY         10           /* 図の行の間隔 (line。字の 8 line + 隙間 2) */
#define PAD_LEGEND_X   40           /* 説明の左端 (byte) */

/* テンキーの図。横 2 倍で貼るので 1 字 = 2 byte。数字を外側に、5 を中心に
 * 置き、5 から各数字へ向かう矢印を間に挟む。
 *   7   8   9
 *     ↖ ↑ ↗
 *   4 ← 5 → 6
 *     ↙ ↓ ↘
 *   1   2   3
 * 矢印は小文字の a-d、f-i で書く (a = 1 の向き … i = 9 の向き。中央の 5 は
 * 数字をそのまま出す。scripts/make_font.py の ARROWS)。CMOC は 8 進のエスケープを
 * 解さず、C の \x は続く 16 進の数字を全部取り込むので、文字列の中では
 * 普通の文字に割り当てる。
 * ★ 文字列へのポインタの表 (const char *[]) にしてはならない。CMOC は
 *   その初期値を起動時のコード (INITGL) で書き込むが、本作の crt0 はそれを
 *   呼ばない。 */
static void title_pad(void)
{
    text_put_wide(TEXT_PLANE_G, PAD_X, PAD_Y,              "7 8 9");
    text_put_wide(TEXT_PLANE_G, PAD_X, PAD_Y + PAD_DY,     " ghi ");
    text_put_wide(TEXT_PLANE_G, PAD_X, PAD_Y + PAD_DY * 2, "4d5f6");
    text_put_wide(TEXT_PLANE_G, PAD_X, PAD_Y + PAD_DY * 3, " abc ");
    text_put_wide(TEXT_PLANE_G, PAD_X, PAD_Y + PAD_DY * 4, "1 2 3");
    text_put(TEXT_PLANE_G, PAD_LEGEND_X, PAD_Y,              "MOVE   TENKEY");
    text_put(TEXT_PLANE_G, PAD_LEGEND_X, PAD_Y + PAD_DY * 2, "5    = STOP");
    text_put(TEXT_PLANE_G, PAD_LEGEND_X, PAD_Y + PAD_DY * 4, "SHOT   BREAK KEY");
}

static void title_screen(void)
{
    unsigned char blink = 0;
    unsigned char shown = 0;
    unsigned char zoff = 0;     /* 床の奥行きスクロールの位相 */
    unsigned char nb;
    unsigned char quiet = 0;    /* 押下の来ないフレームの連続数 */

    game_state = GS_TITLE;
    sub_sync();
    sub_cls();                      /* 3 プレーンとも消す */
    sub_page(0);
    sub_flush();
    palette_page(0);

    text_put_wide(TEXT_PLANE_0, 26, TITLE_Y, "HORIZON RUNNER");
    hi_put();                       /* ハイスコア (ステージの中の得点の行) */
    title_pad();
    sub_flush();

    input_edges();                  /* 溜まっている押下を捨てる */
    break_shots();

    for (;;) {
        wait_frame();
        if (quiet < TITLE_QUIET) {
            /* キーコードがまだ途切れていない。押下は捨て、来なければ数える */
            if (start_pressed() || break_held()) quiet = 0;
            else quiet++;
        } else if (start_pressed()) {
            break;
        }
        /* 背景の床を 1 フレームぶん流す (ステージの中と同じ速さ・同じ地平線) */
        zoff = (unsigned char)(zoff + ZSPEED);
        FLOOR_BANDS(nb, HZ_MID, zoff);
        sub_scene(0, 0, HZ_MID, bands, nb);
        /* 「押してください」だけ点滅させる (約 0.5 秒ごと) */
        if ((blink & 31) == 0) {
            shown ^= 1;
            if (shown) {
                text_put(TEXT_PLANE_G, TITLE_PUSH_X, TITLE_PUSH_Y,
                         "PUSH BREAK KEY TO START");
            } else {
                text_erase(TEXT_PLANE_G, TITLE_PUSH_X, TITLE_PUSH_Y, 23, 8);
            }
        }
        sub_flush();
        blink++;
    }
}

/* ---- ゲームオーバーの画面 ----
 * 約 10 秒出してからタイトルへ戻る。ESC で先に戻れるが、受け付けるのは
 * 約 2 秒経ってから、かつ ESC のキーコードが OVER_ESC_QUIET フレーム途切れた
 * 後の ESC だけ (= 押したままのリピートでは飛ばない)。BREAK では飛ばない
 * (連射の押しっぱなしのまま死んでも、得点とハイスコアが読めるように)。 */
static void gameover_screen(void)
{
    unsigned int t;
    unsigned char quiet = 0;    /* ESC のキーコードが来ていないフレームの連続数 */

    game_state = GS_OVER;
    sound_stop();               /* BGM はステージの中だけ。ここからタイトルまで無音 */
    sub_sync();
    sub_cls();
    sub_page(0);
    sub_flush();
    palette_page(0);

    text_put_wide(TEXT_PLANE_0, 31, 80, "GAME OVER");
    text_put(TEXT_PLANE_G, 34, 108, "SCORE ");
    text_put(TEXT_PLANE_G, 40, 108, score_text());
    text_put(TEXT_PLANE_G, 34, 120, "HI");
    text_put(TEXT_PLANE_G, 40, 120, hi_text());
    text_put(TEXT_PLANE_G, 31, 140, "PUSH ESC TO TITLE");
    sub_flush();

    for (t = 0; t < OVER_SHOW; t++) {
        wait_frame();
        input_edges();          /* この画面の間の押下は捨てる (タイトルへ持ち越さない) */
        break_shots();
        if (esc_edges()) {
            if (t >= OVER_ESC_FROM && quiet >= OVER_ESC_QUIET) break;
            quiet = 0;
        } else if (quiet < 255) {
            quiet++;
        }
    }
}

/* ---- ステージを 1 回遊ぶ (= 残機が尽きるまで戻らない) ---- */
static void play(void)
{
    unsigned int hx;            /* 自機の横位置 (描画フレーム内 dot) */
    unsigned char hy;           /* 自機の縦位置 (描画フレーム内 line) */
    unsigned char hz;           /* 地平線の描画フレーム内 line */
    unsigned char hz_prev;      /* カラープレーンにいま描いてある地平線 */
    unsigned char zoff;         /* 奥行きスクロールの位相 */
    unsigned char sx, sy, ph;   /* 自機の VRAM 位置と横位相 */
    unsigned char page;         /* これから描くページ (0 = B plane / 1 = R plane) */
    unsigned char bi;           /* 自弾 / 敵のスロット番号 */
    unsigned char sc_t;         /* スクリプトの次の項目までのフレーム数 */
    unsigned int  sc_i;         /* スクリプトのどこを読んでいるか (項目 x 2) */
    unsigned char *op;          /* OBJS 命令の書込先 */
    unsigned char nobj;         /* このページに描く絵の個数 */
    unsigned char nbul;         /* 今フレーム生きている自弾の数 (0 なら判定を飛ばす) */
    unsigned char plx0, plx1, ply0, ply1;   /* 自機の当たり判定の矩形 (2 dot 単位 / line) */

    hx = 176;                   /* 描画フレームの横中央あたり */
    hy = HY_MID;
    hz = HZ_MID;
    hz_prev = 0;                /* カラープレーンは直後の sub_cls() で消す */
    zoff = 0;
    page = 0;
    sc_t = 0;
    sc_i = 0;
    for (bi = 0; bi < MAX_BUL; bi++) { bul_life[bi] = 0; bul_pop[bi] = 0; }
    for (bi = 0; bi < MAX_ENM; bi++) { enm_step[bi] = 0; enm_ex[bi] = 0; }
    for (bi = 0; bi < MAX_GND; bi++) { gnd_d[bi] = 0; gnd_ex[bi] = 0; }
    for (bi = 0; bi < MAX_EBUL; bi++) ebul_age[bi] = 0;
    ebul_side = 0;
    boss_on = 0;
    boss_phase = 0;
    boss_hp = 0;
    boss_die = 0;
    frame_cnt = 0;
    pl_inv = 0;
    pl_die = 0;
    lives   = LIVES_START;
    over_t  = 0;
    clear_t = 0;
    hud_dirty = 0;
    for (bi = 0; bi < 6; bi++) sc_dig[bi] = 0;
    hi_live = 0;

    sub_cls();                  /* 3 プレーンとも消す */
    sub_page(0);
    sub_flush();
    palette_page(0);

    /* 得点と残機の見出し (= 変わらない字) はステージの頭で 1 回だけ書く。
     * 情景コマンドは描画フレームの中しか触らないので、以後は消えない。 */
    text_put(TEXT_PLANE_G, HUD_SCORE_X, HUD_Y, "SCORE");
    text_put(TEXT_PLANE_G, HUD_REST_X,  HUD_Y, "REST");
    hi_put();                   /* ステージの頭のハイスコア (ステージの中では書き直さない) */
    sub_flush();
    hud_dirty = HUD_D_VAL;
    hud_service();
    game_state = GS_PLAY;
    sound_stop();               /* 曲を先頭へ戻す (= 最初の sound_tick() で 1 拍目から鳴る) */

    for (;;) {
        unsigned char d, nb, clr_from, clr_lines;
        unsigned char frz;      /* 1 = 被弾の演出中 (敵も床も自弾も止める) */

        /* ---- フレームペーシング (deadline、持ち越し式) ---- */
        {
            unsigned int guard = 0;
            while (timer_get() < FRAME_TARGET && guard < PACE_SAFETY_CAP) {
                guard++;
            }
            timer_consume(FRAME_TARGET);
        }
        frame_cnt++;
        sound_tick();

        /* 予約された書き直し (得点・残機・ステージクリアの表示) を送る。毎フレーム
         * 1 回呼び、前のフレームで予約された分だけ文字の WRITE をサブへ送る (送信の
         * 回数はキューの容量 108 byte と同じフレームに積んだ量で決まり、ステージ
         * クリアの文字 208 byte は複数回に分かれる)。 */
        hud_service();
        if (over_t) {           /* 残機切れ。爆発を見せ切ってからゲームオーバーの画面へ */
            over_t--;
            if (over_t == 0) return;
        }
        /* 被弾の演出 (player_hit() 参照)。演出が終わっても残機切れ (over_t) なら
         * ゲームオーバーの画面に切り替わるまで止めたままにする (自機を戻さない)。 */
        frz = 0;
        if (pl_die) {
            pl_die--;
            frz = 1;
        } else if (over_t) {
            frz = 1;
        }
        if (clear_t) {
            clear_t--;
            if (clear_t == 0) {
                hud_dirty |= HUD_D_UNBANNER;
                game_state = GS_PLAY;
            }
        }

        /* ---- 入力 (方向はラッチ保持。演出中は動かさない) ---- */
        d = frz ? 0 : input_dir();
        if (d & IN_UP)    { hy = (hy > HY_MIN + VSTEP) ? (unsigned char)(hy - VSTEP) : HY_MIN; }
        if (d & IN_DOWN)  { hy = (hy < HY_MAX - VSTEP) ? (unsigned char)(hy + VSTEP) : HY_MAX; }
        if (d & IN_LEFT)  { hx = (hx > HX_MIN + HSTEP) ? (hx - HSTEP) : HX_MIN; }
        if (d & IN_RIGHT) { hx = (hx < HX_MAX - HSTEP) ? (hx + HSTEP) : HX_MAX; }

        /* ---- カメラのピッチ (= 地平線の上下)。必ず偶数行へ丸める ---- */
        {
            signed char t = (signed char)((signed char)HY_MID - (signed char)hy);
            signed char h = (signed char)(HZ_MID + (t >> 2));
            if (h < HZ_MIN) h = HZ_MIN;
            if (h > HZ_MAX) h = HZ_MAX;
            hz = (unsigned char)((unsigned char)h & 0xFE);
        }

        /* ---- 前進 (演出中は床も止める) ---- */
        if (!frz) zoff = (unsigned char)(zoff + ZSPEED);

        /* ---- 床の帯を組み立てる (タイトル画面の背景と共用) ---- */
        FLOOR_BANDS(nb, hz, zoff);

        /* ---- 空へ戻す行 (= 地平線が下がった分) ----
         * 床はカラープレーン 1 枚なので履歴は 1 つで足りる。 */
        if (hz > hz_prev) {
            clr_from  = hz_prev;
            clr_lines = (unsigned char)((hz - hz_prev) >> 1);
        } else {
            clr_from  = 0;
            clr_lines = 0;
        }
        hz_prev = hz;

        /* ---- 発射 (BREAK。押しっぱなしでオート連射) ---- */
        {
            unsigned char n = break_shots();
            if (frz) n = 0;         /* 演出中は撃たない (押下は捨てる) */
            while (n) {
                n--;
                for (bi = 0; bi < MAX_BUL; bi++) {
                    if (bul_life[bi] == 0) break;
                }
                if (bi >= MAX_BUL) break;       /* 空きが無い */
                {
                    /* 発射時だけ 16 分割の割り算をする (= 4 bit 右シフト)。
                     * 以後は毎フレーム足すだけで収束点へ向かう。
                     * ★ 差を 8bit で取ってはならない。砲口の x は最大 189 で
                     *   signed char に収まらず、符号が反転して弾が逆向きに
                     *   飛ぶ。差の計算だけは 16bit で行う (発射時のみ)。 */
                    unsigned char gx = (unsigned char)((hx + 26) >> 1);
                    unsigned char gy = (unsigned char)(hy + 6);
                    bul_hx[bi] = gx;
                    bul_y[bi]  = gy;
                    bul_dx[bi] = (unsigned char)(((int)VANISH_HX - (int)gx) >> 4);
                    bul_dy[bi] = (unsigned char)(((int)VANISH_Y - (int)gy) >> 4);
                    bul_life[bi] = BUL_LIFE;
                    bul_pop[bi]  = 0;
                    sfx_shot();
                }
            }
        }

        /* ---- 湧き (= スクリプトどおりに 1 体ずつ流す) ----
         * 間隔 0 の項目が続けば同じフレームに複数を出す (最大 4 つまで
         * 見て、残りは次のフレームへ持ち越す = 1 フレームの仕事を抑える)。
         * 項目に書かれた間隔 g は次の項目までのフレーム数で、次の項目は
         * ちょうど g フレーム後に出る (= 先に減らしてから 0 を見る)。
         * 被弾の演出中 (frz) はスクリプトごと止める。
         * ステージクリアの文字を出している間 (clear_t) もスクリプトを止める
         * (= 文字の行 (描画フレームの上端) に敵を重ねない)。 */
        if (!frz && !clear_t && sc_t) sc_t--;
        if (!frz && !clear_t && sc_t == 0) {
            unsigned char guard = 4;
            for (;;) {
                unsigned char code = stage_script[sc_i];
                if (code == SC_END) {       /* ステージの終わり → 先頭へ戻る */
                    /* ボスが生きている間は終端に留まり、通常編隊を重ねない
                     * (255 フレーム後にもう一度終端を読む)。 */
                    if (boss_on) { sc_t = 255; break; }
                    sc_i = 0;
                    code = stage_script[0];
                }
                spawn(code, hx, hy, hz);
                sc_t = stage_script[sc_i + 1];
                sc_i += 2;
                if (sc_t || --guard == 0) break;
            }
        }

        if (!frz) {   /* ボスを撃破し切った瞬間がステージクリア (= 爆発が終わった時) */
            unsigned char was = boss_on;
            boss_update(hx, hy, hz);
            if (was && !boss_on) {
                score_add(2, 5);            /* 5000 点 */
                clear_t = CLEAR_SHOW;
                game_state = GS_CLEAR;
                hud_dirty |= HUD_D_BANNER;
                /* スクリプトの残り (まだ出していないボスの局面) を飛ばして
                 * 終端まで進める (= 倒した後にボスの局面を出さない)。文字が
                 * 消えると SC_END → 先頭に戻り、最初の編隊から始まる。
                 * 残っている敵・地上物・敵弾もここで片付ける。 */
                while (stage_script[sc_i] != SC_END) sc_i += 2;
                sc_t = 0;
                for (bi = 0; bi < MAX_ENM; bi++) { enm_step[bi] = 0; enm_ex[bi] = 0; }
                for (bi = 0; bi < MAX_GND; bi++) { gnd_d[bi] = 0; gnd_ex[bi] = 0; }
                for (bi = 0; bi < MAX_EBUL; bi++) ebul_age[bi] = 0;
            }
        }

        /* ---- 自機の VRAM 位置 ---- */
        sx = (unsigned char)(FRAME_X_BYTE + (unsigned char)(hx >> 3));
        ph = (unsigned char)(hx & 7);
        sy = (unsigned char)(FRAME_Y_LINE + hy);

        /* ---- 裏のページへ描き切る ----
         * ページの選択 (= サブが前回の絵を消す) → 情景 → 自機 → 絵の一覧
         * (地上物 → 自弾 → 敵・岩 → ボス → 敵弾 → 自機の爆発)。
         * ★ 絵は全部 1 つの OBJS 命令に詰める。キューは 108 byte しか無く、
         *   あふれるとサブを 2 回呼ぶことになり、往復の分だけ遅くなる。
         *   1 個 3 byte なので、最大の 25 個 (MAX_OBJS) でも 2 + 3 x 25 = 77 byte。 */
        sub_page(page);
        sub_scene(clr_from, clr_lines, hz, bands, nb);
        /* 演出中は自機を描かない (爆発だけを見せる)。復帰後の無敵中は
         * 2 フレームおきに描かない (= 点滅)。無敵の残り (pl_inv) は当たり
         * 判定を全部通した後に減らす (= 復帰後 PL_INV_FRAMES フレームの間は
         * 当たらない)。 */
        if (frz) {
            /* 自機は消えている (下で爆発を描く) */
        } else if (pl_inv) {
            if (!(pl_inv & 2)) sub_sprite(sx, sy, ph);
        } else {
            sub_sprite(sx, sy, ph);
        }
        op = sub_objs_open(MAX_OBJS);
        nobj = 0;

        /* ---- 自機の当たり判定の矩形 (2 dot 単位 / line。絵よりひと回り小さい) ---- */
        plx0 = (unsigned char)((hx >> 1) + PL_INSET_X);
        plx1 = (unsigned char)((hx >> 1) + (SPR_W_DOTS / 2) - PL_INSET_X);
        ply0 = (unsigned char)(hy + PL_INSET_Y);
        ply1 = (unsigned char)(hy + SPR_H_LINES - PL_INSET_Y);

        /* ---- 自弾を進める (描くのは地上物の後。判定に使う矩形と段階を
         *      ここで 1 回だけ求めて憶える) ---- */
        nbul = 0;
        for (bi = 0; frz == 0 && bi < MAX_BUL; bi++) {
            unsigned char k, lf;
            lf = bul_life[bi];
            if (lf == 0) continue;
            if (bul_pop[bi]) {              /* 木に当たって爆ぜている弾は動かない */
                bul_pop[bi]--;
                if (bul_pop[bi] == 0) bul_life[bi] = 0;
                continue;
            }
            bul_hx[bi] = (unsigned char)(bul_hx[bi] + bul_dx[bi]);
            bul_y[bi]  = (unsigned char)(bul_y[bi] + bul_dy[bi]);
            lf--;
            bul_life[bi] = lf;
            if (lf == 0) continue;
            /* 描画フレームの外へ出た弾は消す。8bit で回っているので、負へ回り込んだ弾は
             * 大きな値になり、この検査にそのまま引っ掛かる。 */
            if (bul_y[bi] > (unsigned char)(FRAME_H_LINES - 4) ||
                bul_hx[bi] > (unsigned char)(FRAME_W_DOTS / 2 - 8)) {
                bul_life[bi] = 0;
                continue;
            }
            /* 遠ざかるほど小さく。残り寿命で絵を 3 段階 (15-17) に切り替える。 */
            if (lf >= 12)      { k = OBJ_K_BULLET;     bul_st[bi] = 2; }
            else if (lf >= 7)  { k = OBJ_K_BULLET + 1; bul_st[bi] = 1; }
            else               { k = OBJ_K_BULLET + 2; bul_st[bi] = 0; }
            bul_k[bi]  = k;
            bul_x1[bi] = (unsigned char)(bul_hx[bi] + (obj_w[k] << 2));
            bul_y1[bi] = (unsigned char)(bul_y[bi] + obj_h[k]);
            nbul++;
        }

        /* ---- 地上物を進めて描く ----
         * 残りフレームを 1 減らし、0 になったら d を 1 進めて次の滞在数を
         * 表から取る。滞在 0 の d (= 手前で 1 フレームに 2 line 以上進む
         * 区間) は続けて飛ばす。絵の一覧の先頭に積む (重なった所の見え方は
         * サブ側の貼り方で決まる。asm_subprog.s の blit_obj)。
         * 爆発中 (gnd_ex) も床と一緒に流し、絵だけ爆発に差し替える。 */
        for (bi = 0; bi < MAX_GND; bi++) {
            unsigned char gd, gc, k, gy, gx, hw, lim, st, ex;
            gd = gnd_d[bi];
            if (gd == 0) continue;
            ex = gnd_ex[bi];
            if (!frz) {             /* 演出中はステップも爆発も止める */
                if (ex) {
                    ex--;
                    if (ex == 0) { gnd_d[bi] = 0; continue; }
                    gnd_ex[bi] = ex;
                }
                gc = (unsigned char)(gnd_cnt[bi] - 1);
                while (gc == 0) {
                    gd++;
                    if (gd > GND_D1) break;
                    gc = gnd_dwell[gd];
                }
                if (gd > GND_D1) { gnd_d[bi] = 0; continue; }
                gnd_d[bi]   = gd;
                gnd_cnt[bi] = gc;
            }
            /* 床は hz から 1 行おきの偶数位相へ描くので、接地点も床と同じ
             * 位相へ丸める (= 絵の底を間引いた黒い line に置かない)。 */
            gy = (unsigned char)(hz + (gd & 0xFE));      /* 絵の底 = 描いた床 */
            if (gy >= FRAME_H_LINES) { gnd_d[bi] = 0; continue; }
            st = (gd < GND_STAGE_MID) ? 0 : ((gd < GND_STAGE_NEAR) ? 1 : 2);
            k = (unsigned char)(st + (ex ? EXP_KIND(ex) : gnd_kb[gnd_kind[bi]]));
            gy = (unsigned char)(gy - obj_h[k]);        /* 上端 */
            gx = gnd_xp[bi][gd];                        /* 絵の中心 */
            hw = obj_hw[k];
            gx = (gx > hw) ? (unsigned char)(gx - hw) : 0;
            lim = (unsigned char)(FRAME_W_DOTS / 2 - (obj_w[k] << 2));
            if (gx > lim) gx = lim;
            if (ex == 0 && !frz) {
                unsigned char x1 = (unsigned char)(gx + (obj_w[k] << 2));
                unsigned char y1 = (unsigned char)(gy + obj_h[k]);
                unsigned char hard = gnd_hard[gnd_kind[bi]];
                unsigned char bj;
                /* 自弾との判定 (段階が同じか 1 つ手前 → 矩形の重なり) */
                if (nbul) {
                    for (bj = 0; bj < MAX_BUL; bj++) {
                        if (bul_life[bj] == 0 || bul_pop[bj]) continue;
                        if (bul_st[bj] != st && bul_st[bj] != (unsigned char)(st + 1)) continue;
                        if (bul_hx[bj] >= x1 || gx >= bul_x1[bj]) continue;
                        if (bul_y[bj]  >= y1 || gy >= bul_y1[bj]) continue;
                        if (hard) {                 /* 木: 弾だけ爆ぜて消える */
                            bul_pop[bj] = POP_LEN;
                        } else {                    /* 低木: 壊れて爆発 */
                            bul_life[bj] = 0;
                            ex = EXP_LEN;
                            score_add(4, 5);        /* 50 点 */
                            sfx_destroy();
                        }
                        break;
                    }
                }
                /* 自機との接触 (最も手前の段階だけ) */
                if (st == 2 && pl_inv == 0 &&
                    plx0 + 2 < x1 && gx + 2 < plx1 && ply0 + 2 < y1 && gy + 2 < ply1) {
                    player_hit(hx, hy);
                    if (!hard) ex = EXP_LEN;
                }
                if (ex) {                           /* 今フレームから爆発の絵へ */
                    gnd_ex[bi] = ex;
                    k = (unsigned char)(st + OBJ_K_EXPL_A);
                    gy = (unsigned char)(hz + (gd & 0xFE) - obj_h[k]);
                }
            }
            op[0] = (unsigned char)(FRAME_X_BYTE + (gx >> 2));
            op[1] = (unsigned char)(FRAME_Y_LINE + gy);
            op[2] = (unsigned char)(k | ((gx & 3) << 6));
            op += 3;
            nobj++;
        }

        /* ---- 自弾を描く (進めるのは上で済ませた) ---- */
        for (bi = 0; bi < MAX_BUL; bi++) {
            unsigned char k;
            if (bul_life[bi] == 0) continue;
            /* 木に当たった弾は、その場で小さく爆ぜる絵 (遠の爆発 A) */
            k = bul_pop[bi] ? OBJ_K_EXPL_A : bul_k[bi];
            op[0] = (unsigned char)(FRAME_X_BYTE + (bul_hx[bi] >> 2));
            op[1] = (unsigned char)(FRAME_Y_LINE + bul_y[bi]);
            op[2] = (unsigned char)(k | ((bul_hx[bi] & 3) << 6));
            op += 3;
            nobj++;
        }

        /* ---- 敵と岩を進めて描く ----
         * ステップの間隔は軌道ごと (毎フレーム / 2 フレームに 1 ステップ)。描画は
         * 毎フレーム行い、2 フレームに 1 ステップの軌道はステップが進む側のフレームで
         * 次のステップとの中間を描く (= 毎フレーム半ステップずつ動き、飛ばない)。
         * 爆発中 (enm_ex) はステップを止め、その場に爆発の絵を出して消える。 */
        for (bi = 0; bi < MAX_ENM; bi++) {
            unsigned char st, idx, k, ey, exh, pth, sg, ex;
            unsigned int  base;
            st = enm_step[bi];
            if (st == 0) continue;
            ex = enm_ex[bi];
            if (ex && !frz) {       /* 演出中は爆発のこまも止める */
                ex--;
                if (ex == 0) { enm_step[bi] = 0; continue; }
                enm_ex[bi] = ex;
            }
            pth  = enm_path[bi];
            idx  = (unsigned char)(st - 1);
            base = enm_pbase[pth] + idx;
            /* 絵の段階 (遠/中/近) も表に焼いてある。種類の起点は軌道ごと
             * (敵 A / 敵 B / 敵 C / 岩)。 */
            sg = enm_pk[base];
            k  = (unsigned char)(sg + (ex ? EXP_KIND(ex) : enm_kbase[pth]));
            if (ex || frz) {
                /* 爆発中と被弾の演出中は最後に描いた位置を使い (= 中間の位置に
                 * 描いた直後でもずれない)、被弾の演出中は絵の種類も保持する。
                 * 画面外の判定は絵の高さぶんだけ行う。 */
                exh = enm_hx[bi];
                if (exh == 255) continue;       /* 左端の外で描いていない */
                ey  = enm_hy[bi];
                if (ex == 0) k = enm_hk[bi];
                if ((unsigned char)(ey + obj_h[k]) >= FRAME_H_LINES) {
                    enm_step[bi] = 0;
                    continue;
                }
            } else {
                ey = (unsigned char)(hz + enm_pd[base]);   /* 上は負 (2 の補数) */
                /* 描画フレームの外 (下端を割った / 上へ抜けた) なら通り過ぎたとして消す。
                 * 8bit で回っているので、上へ抜けた負の値も大きな値になって
                 * この 1 つの検査に引っ掛かる。 */
                if ((unsigned char)(ey + obj_h[k]) >= FRAME_H_LINES || ey >= FRAME_H_LINES) {
                    enm_step[bi] = 0;
                    continue;
                }
                exh = enm_px[base];
                /* 255 = 左端の外 (軌道表が焼いた印)。描かず・当てず、ステップだけ
                 * 進める。 */
                if (exh == 255) {
                    enm_hx[bi] = 255;
                    if ((frame_cnt & enm_ptick[pth]) == 0) {
                        st++;
                        enm_step[bi] = (st > enm_plen[pth]) ? 0 : st;
                    }
                    continue;
                }
                /* 2 フレームに 1 ステップの軌道の中間のステップ。16 bit に広げた足し算と
                 * 1 bit の右シフトだけで求める (奥行き d は 2 の補数なので符号付きで足す)。
                 * 次のステップが左端の外 (255) なら中間は作らず今のステップのまま。 */
                if (enm_ptick[pth] && !(frame_cnt & 1) &&
                    st < enm_plen[pth] && enm_px[base + 1] != 255) {
                    unsigned char ey2 = (unsigned char)(hz + (unsigned char)
                            (((int)(signed char)enm_pd[base]
                              + (int)(signed char)enm_pd[base + 1]) >> 1));
                    /* 中間の縦位置は描画フレームの外へ出ることがある (地平線 20、
                     * 奥行き -20 と -21 の中間は 255 になり、描画フレームの 1 行上を
                     * 指す)。外なら中間は作らず、範囲内と分かっている今のステップの
                     * 位置のまま描く (描かないフレームを作らない)。 */
                    if (ey2 < FRAME_H_LINES &&
                        (unsigned char)(ey2 + obj_h[k]) < FRAME_H_LINES) {
                        exh = (unsigned char)(((unsigned int)exh + enm_px[base + 1]) >> 1);
                        ey  = ey2;
                    }
                }
            }
            {   /* 描画フレームの右端からはみ出さないよう寄せる */
                unsigned char lim =
                    (unsigned char)(FRAME_W_DOTS / 2 - (obj_w[k] << 2));
                if (exh > lim) exh = lim;
            }
            if (ex == 0 && !frz) {
                unsigned char x1 = (unsigned char)(exh + (obj_w[k] << 2));
                unsigned char y1 = (unsigned char)(ey + obj_h[k]);
                unsigned char bj;
                enm_hx[bi] = exh;               /* 今フレームに描く位置と絵を憶える */
                enm_hy[bi] = ey;
                enm_hk[bi] = k;
                /* 自弾との判定 (段階が同じか 1 つ手前 → 矩形の重なり) */
                if (nbul) {
                    for (bj = 0; bj < MAX_BUL; bj++) {
                        if (bul_life[bj] == 0 || bul_pop[bj]) continue;
                        if (bul_st[bj] != sg && bul_st[bj] != (unsigned char)(sg + 1)) continue;
                        if (bul_hx[bj] >= x1 || exh >= bul_x1[bj]) continue;
                        if (bul_y[bj]  >= y1 || ey  >= bul_y1[bj]) continue;
                        bul_life[bj] = 0;
                        ex = EXP_LEN;
                        score_add(3, 2);        /* 200 点 */
                        sfx_destroy();
                        break;
                    }
                }
                /* 自機との接触 (最も手前の段階だけ) */
                if (sg == 2 && pl_inv == 0 &&
                    plx0 + 2 < x1 && exh + 2 < plx1 && ply0 + 2 < y1 && ey + 2 < ply1) {
                    player_hit(hx, hy);
                    ex = EXP_LEN;
                }
                if (ex) {                           /* 今フレームから爆発の絵へ */
                    enm_ex[bi] = ex;
                    k = (unsigned char)(sg + OBJ_K_EXPL_A);
                }
            }
            op[0] = (unsigned char)(FRAME_X_BYTE + (exh >> 2));
            op[1] = (unsigned char)(FRAME_Y_LINE + ey);
            op[2] = (unsigned char)(k | ((exh & 3) << 6));
            op += 3;
            nobj++;
            if (ex == 0 && !frz && (frame_cnt & enm_ptick[pth]) == 0) {
                st++;
                enm_step[bi] = (st > enm_plen[pth]) ? 0 : st;
            }
        }

        /* ---- 蛇型ボス ----
         * 履歴を尾から頭の順に貼る。頭だけが自弾で傷付き、胴体への弾は
         * 通り抜ける。撃破時は全節を同時に爆発へ差し替える。 */
        if (boss_on) {
            unsigned char si, bst;
            bst = (boss_phase == 0)
                    ? ((boss_t < 120) ? 0 : ((boss_t < 240) ? 1 : 2))
                    : ((boss_phase == 2) ? 1 : 2);
            if (boss_flash && !frz) boss_flash--;
            si = BOSS_SEGS;
            while (si) {
                unsigned char idx, bx, by, k, x1, y1;
                si--;
                idx = (unsigned char)((boss_hi - boss_delay[si]) & (BOSS_HIST - 1));
                bx = boss_hist_x[idx];
                by = boss_hist_y[idx];
                if (boss_die) {     /* 撃破: 全節が芯と破片を 4 フレームおきに交互 */
                    k = (unsigned char)(((boss_die & 4) ? OBJ_K_EXPL_A
                                                        : OBJ_K_EXPL_B) + bst);
                } else if (si == 0) {
                    k = (unsigned char)((boss_flash ? OBJ_K_EXPL_A : OBJ_K_FLYER) + bst);
                } else {
                    k = (unsigned char)(OBJ_K_ORB + bst);
                }
                x1 = (unsigned char)(bx + (obj_w[k] << 2));
                y1 = (unsigned char)(by + obj_h[k]);

                if (!boss_die && si == 0 && nbul) {
                    unsigned char bj;
                    for (bj = 0; bj < MAX_BUL; bj++) {
                        if (bul_life[bj] == 0 || bul_pop[bj]) continue;
                        if (bul_hx[bj] >= x1 || bx >= bul_x1[bj]) continue;
                        if (bul_y[bj] >= y1 || by >= bul_y1[bj]) continue;
                        bul_life[bj] = 0;
                        boss_flash = 3;
                        sfx_destroy();
                        if (boss_hp) boss_hp--;
                        if (!boss_hp) {
                            unsigned char ei;
                            boss_die = 48;
                            for (ei = 0; ei < MAX_EBUL; ei++) ebul_age[ei] = 0;
                        }
                        break;
                    }
                }
                if (!boss_die && bst == 2 && pl_inv == 0 &&
                    plx0 < x1 && bx < plx1 && ply0 < y1 && by < ply1) {
                    player_hit(hx, hy);
                }
                op[0] = (unsigned char)(FRAME_X_BYTE + (bx >> 2));
                op[1] = (unsigned char)(FRAME_Y_LINE + by);
                op[2] = (unsigned char)(k | ((bx & 3) << 6));
                op += 3;
                nobj++;
            }
        }

        /* ---- 敵弾 3 種 ---- */
        for (bi = 0; bi < MAX_EBUL; bi++) {
            unsigned char age, st, k, x1, y1, lim;
            age = ebul_age[bi];
            if (!age) continue;
            if (!frz) {             /* 演出中はその場に止める */
                {   /* 小数部を足し、桁上がりを整数部へ (8bit の比較 1 つ) */
                    unsigned char f = (unsigned char)(ebul_fx[bi] + ebul_dxl[bi]);
                    if (f < ebul_fx[bi]) ebul_hx[bi]++;
                    ebul_fx[bi] = f;
                    f = (unsigned char)(ebul_fy[bi] + ebul_dyl[bi]);
                    if (f < ebul_fy[bi]) ebul_y[bi]++;
                    ebul_fy[bi] = f;
                }
                ebul_hx[bi] = (unsigned char)(ebul_hx[bi] + ebul_dx[bi]);
                ebul_y[bi] = (unsigned char)(ebul_y[bi] + ebul_dy[bi]);
                age++;
                if (age > EBUL_FRAMES || ebul_hx[bi] > 184) {
                    ebul_age[bi] = 0;
                    continue;
                }
                ebul_age[bi] = age;
            }
            st = (age < EBUL_ST_MID) ? 0 : ((age < EBUL_ST_NEAR) ? 1 : 2);
            if (ebul_type[bi] == EBUL_AIM) {
                k = (unsigned char)(OBJ_K_ESHOT + st);
            } else if (ebul_type[bi] == EBUL_LOW) {
                k = (unsigned char)(OBJ_K_ORB + st);
            } else {
                k = (unsigned char)(((age & 2) ? OBJ_K_EXPL_A : OBJ_K_EXPL_B) + st);
            }
            /* 描画フレームの外 (下端を割った / 上へ抜けた) なら消す。下端は絵の
             * 高さぶん見る (敵と同じ。サブ側は縦に切り詰めない)。8bit で回って
             * いるので、上へ抜けた負の値も大きな値になって引っ掛かる。演出中は
             * 動かないので、ここで消えるのは動かしたフレームだけ。 */
            if (ebul_y[bi] >= FRAME_H_LINES ||
                (unsigned char)(ebul_y[bi] + obj_h[k]) >= FRAME_H_LINES) {
                ebul_age[bi] = 0;
                continue;
            }
            lim = (unsigned char)(FRAME_W_DOTS / 2 - (obj_w[k] << 2));
            if (ebul_hx[bi] > lim) ebul_hx[bi] = lim;
            x1 = (unsigned char)(ebul_hx[bi] + (obj_w[k] << 2));
            y1 = (unsigned char)(ebul_y[bi] + obj_h[k]);
            if (((ebul_type[bi] == EBUL_FIRE) ? (age >= EBUL_FIRE_HIT)
                     : (age >= EBUL_HIT_AGE0 && age <= EBUL_HIT_AGE1)) && pl_inv == 0 &&
                plx0 < x1 && ebul_hx[bi] < plx1 &&
                ply0 < y1 && ebul_y[bi] < ply1) {
                player_hit(hx, hy);
                ebul_age[bi] = 0;
                continue;
            }
            op[0] = (unsigned char)(FRAME_X_BYTE + (ebul_hx[bi] >> 2));
            op[1] = (unsigned char)(FRAME_Y_LINE + ebul_y[bi]);
            op[2] = (unsigned char)(k | ((ebul_hx[bi] & 3) << 6));
            op += 3;
            nobj++;
        }

        /* ---- 無敵の残りを減らす (当たり判定を全部通した後。演出中と、被弾した
         *      フレーム (= pl_die を積んだばかり) は減らさない) ---- */
        if (!frz && !pl_die && pl_inv) pl_inv--;

        /* ---- 自機の被弾爆発 (止まった画面の中でこれだけが動く) ----
         * 芯 (A) と破片 (B) を 4 フレームおきに入れ替えて、いちばん手前の
         * 大きさで見せる (ボスの全身爆発と同じ作り)。 */
        if (frz) {
            unsigned char k = (unsigned char)(((pl_die & 4) ? OBJ_K_EXPL_A
                                                            : OBJ_K_EXPL_B) + 2);
            op[0] = (unsigned char)(FRAME_X_BYTE + (pl_ex_hx >> 2));
            op[1] = (unsigned char)(FRAME_Y_LINE + pl_ex_y);
            op[2] = (unsigned char)(k | ((pl_ex_hx & 3) << 6));
            op += 3;
            nobj++;
        }
        sub_objs_close(nobj);

        /* ---- 前のフレームの描き上がりを待って表に出し、今のフレームを
         *      投げる ----
         * main の計算 (上のぜんぶ) はサブが前のフレームを描いている間に
         * 済ませてある (hud_service() が文字を送ったフレームは、その中で
         * 同期するので重ならない)。ここで前のページを表に出し (= パレット
         * 差し替えだけ)、今組んだキューを投げてすぐ次の計算へ向かう。表示は
         * 1 フレーム遅れるが、main と sub の時間が重なるぶんフレームレートが
         * 上がる。 */
        sub_sync();
        palette_page((unsigned char)(1 - page));
        sub_flush_async();
        page = (unsigned char)(1 - page);
    }
}

int main(void)
{
    /* ★ crt0 の INILIB は bss をゼロクリアしないので、起動時の状態を明示する。 */
    game_state = GS_BOOT;
    {
        unsigned char i;
        for (i = 0; i < 6; i++) hi_dig[i] = 0;
    }

    /* ---- 起動シーケンス (順序に意味がある) ----
     * sub_cancel() でサブをサブシステム ROM のコマンド待ちへ戻してから、
     * ROM のコマンド (CURSOR) を送る。少し待ってサブがそのコマンドを終える
     * のを待ってから subprog_init() で描画プログラムを転送する (描画 API は
     * この後でしか使えない)。input_init() は kb_init() の後、timer_init() で
     * IRQ を許可する前に呼ぶ (c_input.h)。 */
    sub_cancel();
    {
        unsigned char p = 0x00;
        subsys_call(SCMD_CURSOR, &p, 1);    /* カーソル blink を止める */
    }
    delay_loop(500);                        /* サブが CURSOR を終えるのを待つ */
    subprog_init();
    palette_init();
    kb_init();
    input_init();
    timer_init();
    timer_start();
    sound_init();

    /* 追加絵 (爆発・敵の楕円弾・敵 B の近) をサブ側 VRAM の末尾 (表示されない
     * 領域) へ送る。1 回に 100 byte ずつ切って WRITE 命令で運ぶ (キューは 108 byte)。 */
    {
        unsigned char si;
        const unsigned char *src = expl_data;
        for (si = 0; si < EXPL_SEGS; si++) {
            unsigned int dst = ((unsigned int)expl_seg[si * 4] << 8)
                             | expl_seg[si * 4 + 1];
            unsigned int n   = ((unsigned int)expl_seg[si * 4 + 2] << 8)
                             | expl_seg[si * 4 + 3];
            while (n) {
                unsigned char c = (n > 100) ? 100 : (unsigned char)n;
                sub_write(dst, src, c);
                sub_flush();
                dst += c;
                src += c;
                n   -= c;
            }
        }
    }

    /* タイトル → ステージ → ゲームオーバーの画面、の一巡を延々と繰り返す。 */
    for (;;) {
        title_screen();
        play();
        gameover_screen();
    }

    return 0;   /* 到達しない */
}
