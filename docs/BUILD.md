# HorizonRunner — ビルドの手順

この文書は、ソースから配布物 (`disks/` の 4 ファイル) を作るための手順を説明します。
必要なツール、ビルドの実行、`make` のターゲット、生成物の置き場所、`config.mk` で変えられる値、D77 から T77 / WAV / TXT (ロード手順) への変換を扱います。
コマンドはすべてリポジトリの最上位で実行します。
アドレスは `$FE00` のように `$` を付けて 16 進で書きます。
できた配布物の使い方は [PLAY.md](PLAY.md) を、ビルドの中で何が起きているかは [ARCHITECTURE.md](ARCHITECTURE.md) の「ビルドの流れ」を参照してください。

## 1. 必要なツール

| ツール | 用途 | 確認したバージョン | 入手先 |
|---|---|---|---|
| cmoc | FM-7 の CPU (6809) 向けの C コンパイラ | 0.1.97 | <http://sarrazip.com/dev/cmoc.html> |
| lwasm / lwlink | 6809 向けのアセンブラとリンカ (LWTOOLS に含まれます) | 4.23 | <http://www.lwtools.ca/> |
| python3 | データの生成とイメージの変換 | 3.12.3 | <https://www.python.org/> |
| make | ビルドの実行 | GNU Make 4.3 | <https://www.gnu.org/software/make/> |

`scripts/` の Python スクリプトは標準ライブラリだけを使うので、追加のパッケージは要りません。
`Makefile` はグループ化ターゲット (`&:`) を使っているので、GNU Make 4.3 以降が必要です。

## 2. ビルドする

```sh
make
```

本体、D77、ブート ROM、T77、WAV、テープのロード手順テキストを `build/` に作ります。
このうち D77、T77、WAV、ロード手順テキストの 4 ファイルを `disks/` へコピーします。
同じソース・設定・ツールのバージョンなら、`build/` を消してから `make` を実行し直しても `disks/` の 4 ファイルは同じ中身になります (上の表のバージョンで確認しています)。

## 3. make のターゲット

表に出てくる語は次のとおりです。
HFE は、フロッピーディスクの代わりにイメージファイルを読ませる機器のためのイメージで、2D は 40 トラック、2DD は 80 トラックのフォーマットです。
ブート ROM は、ディスクの先頭セクタを読み込んで実行するだけのプログラムです。

| コマンド | 作るもの |
|---|---|
| `make` | `build/` に D77・ブート ROM・T77・WAV・ロード手順テキストを作り、このうち D77・T77・WAV・ロード手順テキストの 4 ファイルを `disks/` へコピーします |
| `make hfe` | `build/horizonrunner.hfe` (2D、40 トラック) と `build/horizonrunner_2dd.hfe` (2DD、80 トラック) |
| `make t77` | `build/horizonrunner.t77`、`build/horizonrunner.wav`、`build/horizonrunner.cmt.txt` (`disks/` へはコピーしません) |
| `make wav` | `make t77` と同じです (T77・WAV・ロード手順テキストは 1 回の変換でまとめてできます) |
| `make disk` | D77、HFE 2 種、ブート ROM |
| `make dist` | `disks/` の 4 ファイル (必要なら先に作ります) |
| `make release` | `make` の全部に HFE 2 種を足したもの |
| `make bin` | 本体 `build/horizonrunner.bin` だけ |
| `make bootrom` | ブート ROM `build/bootrom.bin` だけ |
| `make clean` | `build/` を消します (`disks/` は消しません) |
| `make distclean` | `make clean` と同じです |
| `make help` | ターゲットの一覧を表示します |

2DD の HFE は 80 トラックのドライブで読めるよう、2D の各トラックを隣り合う 2 本のトラックへ同じ内容で書いています。

ブート ROM (`build/bootrom.bin`) は 512 byte で、`$FE00-$FFFF` に置いて使います。
ブート ROM を自前のものに置き換えられる環境のためのもので、内蔵のブート ROM でディスクから起動する時は使いません。

## 4. 生成物の置き場所

### build/

中間ファイルと生成物の置き場所です。
git の管理対象ではありません。
主なファイルは次のとおりです (サイズは現在のソースでビルドした時の値です)。

| ファイル | 中身 | サイズ (byte) |
|---|---|---|
| `horizonrunner.bin` | 本体 | 24,779 |
| `subprog.bin` | サブ CPU 側の描画プログラム (本体に埋め込まれます) | 3,596 |
| `ipl.bin` | ディスクの先頭セクタに置く IPL (本体を読み込むプログラム) | 176 |
| `horizonrunner.d77` | ディスクイメージ (IPL + 本体) | 348,848 |
| `bootrom.bin` | ブート ROM | 512 |
| `trampoline.bin` | テープのステージャ (本体を最終の位置へ運ぶプログラム) | 30 |
| `horizonrunner.t77` | テープイメージ | 1,353,944 |
| `horizonrunner.wav` | テープの音声 | 22,025,944 |
| `horizonrunner.cmt.txt` | テープからのロード手順 | 3,695 |
| `horizonrunner.map` | リンクの結果 (各セクションとシンボルの配置) | — |

### disks/

配布物の置き場所です。
git の管理対象で、`make clean` では消えません。
中身は `build/` の D77、T77、WAV、ロード手順テキストのコピーです。

## 5. config.mk で変えられる値

設定は `config.mk` にまとめてあります。

| 名前 | 既定値 | 意味 |
|---|---|---|
| `NAME` | `horizonrunner` | 生成物のファイル名と D77 のディスク名 |
| `ORG` | `0x0400` | 本体の開始アドレス |
| `SUB_*` | — | サブ CPU 側のメモリの割り当て |

`ORG` は本体のリンク先、IPL が本体を読み込む先、D77 を作る時の本体の上限の計算 (`scripts/bin2d77.py` の `--org`)、テープ変換の `--addr` に渡ります。
ページ境界 (下位 8 bit が 0 のアドレス。256 byte 単位) に揃えてください。
テープも作る場合は、本体の仮置き先 `$1400` より小さいアドレスである必要があります (変換がそれ以上のアドレスを受け付けません)。
既定値以外にする時は、本体の末尾が `$FA00` を超えないことと、サブ側との共有 RAM やスタックの配置 ([ARCHITECTURE.md](ARCHITECTURE.md) の「メモリマップ」) に掛からないことも確かめてください。

`SUB_*` のうち `SUB_WORK_ADDR` / `SUB_TABLE_ADDR` / `SUB_PROG_ADDR` / `SUB_CODE_END` の 4 つは `src/asm_subprog.s` と `src/c_subprog.h` にも、`SUB_STACK_ADDR` / `SUB_STACK_TOP` の 2 つは `src/asm_subprog.s` にも同じ値が書いてあります。
値を変える時は、これらの写しも同じ値に直してください。
サブ CPU 側のプログラム (`build/subprog.bin`) を作る時に `scripts/check_layout.py` が写しを突き合わせ、食い違っていればビルドを止めます。
`scripts/check_layout.py` は、各領域が作業変数・消去の表・コード・スタックの順にサブ側のコンソール RAM (`$C000-$CFFF`) の中に並ぶこと、作業変数 32 byte が消去の表に、消去の表 208 byte がコードの先頭に掛からないこと、スタックが 32 byte 以上あることも調べます。
`SUB_TAIL*` は `scripts/make_sprite.py` が `config.mk` から直接読むので、`config.mk` だけを直します。
`scripts/check_layout.py` は、`build/subprog.bin` が `SUB_CODE_END` を超えていないかも調べます。

テープ変換で使うメモリの割り当て (本体の仮置き先 `$1400`、RAM の上限 `$8000`、ステージャのスタック `$FBFF`、ローダーの `CLEAR ,&H13FF`) は `config.mk` にはなく、`scripts/d77_to_t77_chunks.py` の先頭の定数 (BASIC ローダーの生成部分も同じ定数を使います) と `scripts/trampoline.asm` の定数で決まります。

## 6. D77 から T77 / WAV / TXT への変換

`make` は D77 を作った後、`scripts/d77_to_t77_chunks.py` で T77、WAV、ロード手順のテキストを作ります。
手で変換する時は、`build/horizonrunner.d77` ができている状態で次のコマンドを実行します。

```sh
mkdir -p build
lwasm --raw -o build/trampoline.bin scripts/trampoline.asm
python3 scripts/d77_to_t77_chunks.py build/horizonrunner.d77 --addr 0x0400 \
    --tramp build/trampoline.bin \
    -o build/horizonrunner.t77 -w build/horizonrunner.wav -t build/horizonrunner.cmt.txt
```

`mkdir` は出力先の用意です。
`lwasm` は、読み込みの後に本体を最終の位置へ運ぶプログラム (ステージャ、30 byte) をアセンブルします。
`d77_to_t77_chunks.py` が変換本体です。
`--addr` には `config.mk` の `ORG` と同じ値を渡します。
`make` が作る `disks/` の T77、WAV、テキストは、これらのコマンドで作ったものと同じ中身です。

変換は次の順に進みます。

1. D77 のセクタをつなげます。
2. 先頭の 256 byte (IPL のセクタ) を飛ばし、IPL に記録された本体のセクタ数 (ディスクの IPL が読む量) だけを本体とします。本体の長さを中身から推測しないので、本体の末尾がゼロで初期化されたデータでも欠けません (`--size` で長さを直接指定できます。`--skip` で飛ばす量を変えた時は `--size` が必要です)。本体はセクタ単位 (256 byte) なので、テープに載る本体は `horizonrunner.bin` の 24,779 byte を 256 byte に切り上げた 24,832 byte になります (末尾のセクタの残りは 0 です)。
3. 本体の直後にステージャを付けて、`$1400` から置く 1 本の機械語ファイルにします。ステージャには、仮置きした本体の末尾の次のアドレス (`$1400` + 本体の長さ) と、`--addr` の値を書き込みます。本体とステージャが `$1400-$7FFF` に収まらない時は失敗します。
4. 変換前に配置と転送の結果を検証し、問題があれば何も出力せずに終了します (仕組みは [ARCHITECTURE.md](ARCHITECTURE.md) の「ビルドの流れ」)。
5. 先頭に BASIC のローダー (`LOADER`)、続けて本体の機械語ファイル (`C01`) を置いた T77、同じ内容の WAV、ロード手順のテキストを書き出します。

ローダーは `CLEAR ,&H13FF` で `$1400` から上を空け、読み込みに掛かる時間 (本体の機械語ファイルの部分。現在のソースで約 227 秒) を表示してから、機械語ファイルを `LOADM "CAS0:",,R` で読み込んで実行します。
読み込んだ後の動き (ステージャが本体を `$0400` へ写して起動する流れ) は [ARCHITECTURE.md](ARCHITECTURE.md) の「起動の流れ」を参照してください。

WAV は 44100 Hz、16 bit、モノラルで、先頭・ファイルの間・末尾に 5 秒の無音を入れます (`--silence` で秒数を変えられます)。ローダーと無音を含む全体で約 250 秒です。
`--no-wav` を付けると WAV を作りません。
`--no-basic-loader` を付けると先頭のローダーを置かず、ロードの手順は手で打つものだけになります。
