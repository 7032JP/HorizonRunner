#!/usr/bin/env python3
"""敵・岩が空中を動く軌道を、ビルド時に表として焼く。

なぜ表なのか:
  透視投影の軌道は本来「奥行き z の逆数」で決まり、実行時に計算すると
  割り算と掛け算が要る。CMOC の乗除算は実行時ルーチン呼出で重いので、
  軌道は **ビルド時に焼いて実行時は表を引くだけ**にする。

軌道の書き方 (= このファイルだけを直せば動きが変わる):
  各軌道は **キーフレームの並び** で書く。1 つのキーフレームは
      (秒, x, y, 段階)
  で、x / y は 0..255 x 0..95 の相対座標 (地平線は y = HORIZON)、段階は
  絵の大きさ (0 遠 / 1 中 / 2 近)。キーフレームの間は直線で補間し、
  ステップ (= 各軌道の tick (1 か 2) フレームごとに 1 つ) 単位に刻んで
  表にする。動く区間の秒は **TIME_SCALE 倍に伸ばして**刻む (= 同じ道のりを
  TIME_SCALE 倍の時間で動く)。軌道の形を変えずに速さだけを変える時は
  この値を直す。移動の速さが毎秒 HOVER_SPEED 未満の区間 (滞空) は
  秒を伸ばさない (= 滞空の長さは変えない)。
  tick 2 の軌道は c_main.c がステップの中間を補って毎フレーム動かす。
  画面座標への換算:
      絵の中心の x (2 dot 単位)    = x * 3/4
      地平線からの line 数 d      = (y - HORIZON) * (y >= HORIZON なら 1.7 /
                                                    それ以外は 0.53)
  enm_px に焼くのは、絵の中心から HALF_W (絵の幅の半分) を引いた左端。
  入力の y は 0..95 の絶対値で、出力の d は **地平線からの相対** で持つ。
  こうしておくと、自機の上下でカメラがピッチして地平線が動いても、敵は
  地面に貼り付いたまま一緒に動く。空を飛ぶ物 (d < 0) は 2 の補数で持ち、
  実行時は 8bit の足し算で済ませる。

出力 (build/enemy_path.h):
  ENM_PATHS             … 軌道の本数 (軌道ごとの表の長さに使う)
  ENM_PATH_xxx          … 軌道番号の名前 (読み手向けの対応表。実行コードは
                           scripts/make_stage.py が焼いた番号を使う)
  enm_px[base+s]        … 描画フレーム内の x (2 dot 単位、0-191)。絵の左端。255 = 左端の外 (描かない)
  enm_pd[base+s]        … 地平線から下へ何 line (2 の補数、上は負)
  enm_pk[base+s]        … 絵の段階 (0/1/2)
  enm_pbase[path]       … その軌道の表の起点 (掛け算を出さないための表)
  enm_plen[path]        … ステップ数
  enm_ptick[path]       … ステップを進めるフレーム間隔のマスク (0=毎フレーム/1=2 フレームに 1 ステップ)
  enm_kbase[path]       … その軌道が使う絵の種類の起点 (0 敵 A / 3 敵 C / 12 岩 / 27 敵 B)
"""

import sys

HORIZON = 57            # 相対座標での地平線 (0..95 のうち)
SKY_SCALE = 0.53        # 空側の y → line 換算 (描画フレームの空 30 line / 相対 57)
GND_SCALE = 1.7         # 地面側の y → line 換算 (描画フレームの地面 65 line / 相対 38)
X_SCALE = 0.75          # 相対 x 0..255 → 2 dot 単位 0..191
FPS = 60
TIME_SCALE = 1.5        # 動く区間の秒をこの倍率で伸ばす (= 速さは 1/1.5)
HOVER_SPEED = 20        # これ未満の速さ (相対座標 / 秒) の区間は滞空とみなす

# 絵の種類の起点 (scripts/make_sprite.py の並びと一致必須)
K_FLYER, K_ORB, K_ROCK, K_FLYER_B = 0, 3, 12, 27
# 絵の幅の半分 (2 dot 単位)。x を「絵の中心」で書けるようにするための値
HALF_W = {K_FLYER: (2, 4, 6), K_ORB: (2, 4, 6), K_ROCK: (2, 4, 6),
          K_FLYER_B: (2, 3, 4)}


def to_screen(x, y):
    """相対座標 → (描画フレーム内 x 2 dot 単位, 地平線からの line 数)"""
    sx = x * X_SCALE
    dy = y - HORIZON
    d = dy * (GND_SCALE if dy >= 0 else SKY_SCALE)
    return sx, d


def stretch(keys):
    """キーフレームの秒を区間ごとに伸ばす。動く区間は TIME_SCALE 倍、
    滞空の区間はそのまま。"""
    out = [keys[0]]
    t = keys[0][0]
    for (t0, x0, y0, _), (t1, x1, y1, k1) in zip(keys, keys[1:]):
        dt = t1 - t0
        v = ((x1 - x0) ** 2 + (y1 - y0) ** 2) ** 0.5 / dt if dt > 0 else 0.0
        t += dt * (TIME_SCALE if v >= HOVER_SPEED else 1.0)
        out.append((t, x1, y1, k1))
    return out


def bake(keys, kbase, tick=1):
    """キーフレーム → (px, pd, pk) のステップの列。tick はフレーム間隔 (1 or 2)。"""
    # c_main.c は tick - 1 をフレーム番号のマスクにして進めるので、tick は
    # 1 か 2 しか正しく動かない。
    if tick not in (1, 2):
        raise SystemExit(f"tick={tick} は 1 か 2 でなければならない")
    px, pd, pk = [], [], []
    keys = stretch(keys)
    total = keys[-1][0]
    steps = int(round(total * FPS / tick))
    for s in range(steps + 1):
        t = s * tick / FPS
        # 区間を探して補間
        for i in range(len(keys) - 1):
            t0, x0, y0, k0 = keys[i]
            t1, x1, y1, k1 = keys[i + 1]
            if t <= t1 or i == len(keys) - 2:
                f = 0.0 if t1 == t0 else max(0.0, min(1.0, (t - t0) / (t1 - t0)))
                x = x0 + (x1 - x0) * f
                y = y0 + (y1 - y0) * f
                # 段階は区間の前半は k0、後半は k1 (= 切り替わりを中間に)
                k = k0 if f < 0.5 else k1
                break
        sx, d = to_screen(x, y)
        sx -= HALF_W[kbase][k]          # 中心 → 左端
        di = int(round(d))
        if di < -128 or di > 127:
            raise SystemExit(f"d={di} が 8bit を超える")
        # 生成時の打ち切り: 左端 (2 dot 単位) が -12..200 を外れるか d が 66 を
        # 超えたら、軌道はそこで終わり。実行時の画面外の判定 (絵の高さと
        # 地平線の位置を含む) は c_main.c が別に行う。
        if sx < -12 or sx > 200 or di > 66:
            break
        # 左端より外は **描かない印 (255)** を焼く。サブ側で部分的に切り詰める
        # より、main が 8bit 比較 1 つで飛ばす方が速い。ステップは進めるので、
        # 左端から入って来る軌道は sx が戻ったステップから現れる。はみ出しが
        # 2 (= 4 dot) 以内なら 0 へ寄せる。
        px.append(255 if sx < -2 else max(0, min(191, int(round(sx)))))
        pd.append(di & 0xFF)
        pk.append(k)
    if len(px) > 255:
        raise SystemExit(f"ステップ数 {len(px)} が 255 を超える (tick を 2 にする)")
    return px, pd, pk


# ---------------------------------------------------------------
# 軌道の定義  path(名前, 絵の起点, tick, キーフレーム)
# ---------------------------------------------------------------
PATHS = []


def path(name, kbase, tick, keys):
    PATHS.append((name, kbase, tick, keys))


# 敵 A: 横断。奥の左端に小さく現れ、右端まで横切ってから折り返し、
#       中央寄りから手前へ突進して自機の左を抜ける。弾なし。
path("CROSS_A", K_FLYER, 2, [
    (0.0,  10, 32, 0), (2.5, 250, 38, 0), (2.9, 120, 40, 0),
    (3.3, 100, 45, 1), (3.75, 60, 60, 2), (4.0, 20, 78, 2)])

# 敵 A: 突進 (右)。右端に至近の大きさで現れ、左へ水平に動きつつ手前へ抜ける。
path("RUSH_AR", K_FLYER, 1, [
    (0.0, 240, 58, 2), (0.5, 120, 62, 2), (0.85, 30, 82, 2)])

# 敵 A: 突進 (左)。左端から入って左中で 1 秒滞空し、手前へ抜ける。
path("RUSH_AL", K_FLYER, 1, [
    (0.0, 0, 58, 2), (0.3, 25, 50, 2), (1.3, 25, 50, 2),
    (1.6, 60, 60, 2), (2.0, 120, 82, 2)])

# 敵 A: 突進 (左) の遠方組。遠方中央に小さく現れ、滞空しつつ徐々に近付き、
#       最後は右下へ抜ける。
path("RUSH_AL_FAR", K_FLYER, 2, [
    (0.0, 110, 40, 0), (1.5, 118, 42, 0), (2.5, 125, 44, 1),
    (3.0, 130, 50, 2), (3.4, 160, 72, 2)])

# 敵 A: 横薙ぎ。至近距離で右端から自機の高さを高速で横切り、
#       左を抜けたあと左奥へ小さくなって去る。
path("SWEEP_A", K_FLYER, 1, [
    (0.0, 255, 42, 2), (0.25, 5, 44, 2), (0.6, 20, 52, 1),
    (1.0, 50, 55, 0), (1.3, 60, 56, 0)])

# 敵 B: 滞空。右下の地平線に現れ、右端付近で上りながら遠方に滞空し、
#       そのあと自機へ急接近して通過。絵は敵 B 専用 (小ぶりの飛行体)。
path("HOVER_B", K_FLYER_B, 2, [
    (0.0, 210, 62, 0), (2.5, 220, 40, 0), (2.7, 200, 42, 1),
    (2.9, 150, 45, 2), (3.2, 90, 66, 2)])

# 敵 C: 滞空群。地平線右上に小さく現れ、4 通りの持ち場へ散って滞空し、
#       全体がゆっくり手前へ。近付いた順に自機の周りを通過。
path("HOVER_C_TOP", K_ORB, 2, [
    (0.0, 190, 40, 0), (1.0, 128, 25, 0), (2.5, 128, 25, 1),
    (3.5, 128, 25, 2), (4.5, 60, 8, 2)])
path("HOVER_C_LOW_L", K_ORB, 2, [
    (0.0, 190, 40, 0), (1.0, 75, 57, 0), (3.0, 75, 58, 1),
    (4.0, 75, 60, 2), (4.6, 30, 82, 2)])
path("HOVER_C_LOW_R", K_ORB, 2, [
    (0.0, 190, 40, 0), (1.0, 165, 57, 0), (3.0, 165, 58, 1),
    (4.0, 165, 60, 2), (4.6, 200, 82, 2)])
path("HOVER_C_RIGHT", K_ORB, 2, [
    (0.0, 190, 40, 0), (1.0, 200, 50, 0), (3.0, 200, 50, 1),
    (4.0, 205, 52, 2), (4.6, 245, 62, 2)])

# 浮遊岩: 地平線付近に小さく現れ、近付くにつれ左右と上下へ広がって通過。
#         群は scripts/make_stage.py がこれらを組み合わせて作る。
ROCKS = [
    ("ROCK_1", 100, 48,  60, 30),
    ("ROCK_2", 130, 48, 200, 40),
    ("ROCK_3", 160, 48, 210, 25),
    ("ROCK_4", 200, 50, 250, 15),
    ("ROCK_5", 220, 50,  90, 45),
    ("ROCK_6",  30, 45,   0, 20),
    ("ROCK_7",  60, 45,  30, 62),
    ("ROCK_8", 150, 45, 140, 20),
]
for name, x0, y0, x1, y1 in ROCKS:
    path(name, K_ROCK, 2, [
        (0.0, x0, y0, 0), (0.6, (x0 + x1) / 2, (y0 + y1) / 2, 1),
        (1.25, x1, y1, 2), (1.5, x1 + (x1 - x0) * 0.3, y1 + (y1 - y0) * 0.3, 2)])

# 敵弾とボスはこの表には載せず、c_main.c で動かす。敵弾は発射時に自機との
# 差から進み幅 (整数部 + 小数部) を作り、以後は整数部と小数部を別々に 8bit で
# 加算して進める。ボスは局面ごとの動きで、位置を 16bit で加算する (自機を
# 狙うのはボスが撃つ敵弾の方)。


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_enemy.py <out.h>")
    out_path = sys.argv[1]

    px, pd, pk = [], [], []
    base, plen, ptick, kbase, names = [], [], [], [], []
    for name, kb, tick, keys in PATHS:
        x, d, k = bake(keys, kb, tick)
        base.append(len(px))
        plen.append(len(x))
        ptick.append(tick - 1)
        kbase.append(kb)
        names.append(name)
        px += x
        pd += d
        pk += k

    H = []
    H.append("/* ============================================================")
    H.append(" * enemy_path.h — scripts/make_enemy.py が生成 (手で直さない)")
    H.append(" *")
    H.append(" *   敵・岩の軌道の表。実行時はステップ数を 1 つ進めて表を引くだけで、")
    H.append(" *   乗除算は 1 回も起きない。y は地平線からの相対 (2 の補数) なので、")
    H.append(" *   カメラのピッチに素直に追従する。")
    H.append(" * ============================================================ */")
    H.append("")
    H.append("#ifndef ENEMY_PATH_H")
    H.append("#define ENEMY_PATH_H")
    H.append("")
    H.append(f"#define ENM_PATHS   {len(PATHS)}")
    H.append("/* 軌道番号の名前 (読み手向けの対応表。スクリプトの表には番号で入る) */")
    for i, n in enumerate(names):
        H.append(f"#define ENM_PATH_{n:<14} {i}")
    H.append("")

    def arr(name, values, size=None, per_line=16):
        H.append(f"static const unsigned char {name}[{size or len(values)}] = {{")
        for i in range(0, len(values), per_line):
            H.append("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
        H.append("};")
        H.append("")

    arr("enm_px", px)
    arr("enm_pd", pd)
    arr("enm_pk", pk)
    arr("enm_plen", plen, "ENM_PATHS")
    arr("enm_ptick", ptick, "ENM_PATHS")
    arr("enm_kbase", kbase, "ENM_PATHS")
    # 表の起点は 255 を超えるので 16bit で持つ (掛け算を出さないための表)
    H.append("static const unsigned int enm_pbase[ENM_PATHS] = {"
             + ", ".join(str(b) for b in base) + "};")
    H.append("")
    H.append("#endif")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(H) + "\n")
    per_path = len(PATHS) * 3 + len(PATHS) * 2     # plen/ptick/kbase + pbase
    print(f"make_enemy.py: {out_path} (paths={len(PATHS)}, steps={len(px)}, "
          f"座標・段階の表 {len(px)*3} byte + 軌道ごとの表 {per_path} byte)")


if __name__ == "__main__":
    main()
