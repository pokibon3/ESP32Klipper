# ESP32Klipper

> **開発中です。** まだ SKR Pico の実機では一度も動かしておらず、仮想 MCU（Mac 上のプロセス）相手の試験しかしていません。設定項目・API・ファイル構成は予告なく変わります。実機のプロッタを動かす用途にはまだ使わないでください。

CYD（ESP32-2432S028R）または M5Stack CoreS3 を Klipper のホストにして、Klipper 純正ファームの SKR Pico を UART で動かすペンプロッタ用コントローラ。設計は「CYD Klipper互換ホスト 全体設計書」を参照。

現在の段階：**M1（性能測定）・M2（MCU 接続、1 軸駆動）・M3（CoreXY、printer.cfg、TMC2209、Input Shaper）が完了。M4 は原点復帰（センサレス含む）・Jinja2 マクロ・SD 実行（一時停止・再開・中止）・ローカル UI まで完了**（UI の実機での見え方・タッチは未確認）。残りは CYD での実 SD カードの読み込み。**M5（Moonraker 互換 API + Mainsail）は CoreS3 上で仮想 MCU 相手に動作確認済み**（2026-11-13）。CoreS3 は WiFi の設定（画面・設定用アクセスポイント）と microSD にも対応。

SKR Pico 実機はまだ使っていない。MCU 側は、Klipper 本家の MCU コードを Mac 上のプロセスとして動かす「仮想 MCU」（`tools/sim_mcu/`）で検証している。

## 構成

| パス | 内容 |
| --- | --- |
| `third_party/klipper/` | Klipper 本家（`git clone --depth 1 https://github.com/Klipper3d/klipper.git third_party/klipper` で取得）。改造せずに使う |
| `components/chelper/` | chelper（trapq、itersolve、stepcompress、steppersync など）を ESP-IDF と Mac でビルドする移植層 |
| `components/klippy_lite/` | `toolhead.py` と `motion_queuing.py` の C++ 移植 |
| `components/mcu_link/` | MCU 通信層。`serialqueue.c` は本家のまま使い、`msgproto.py`・`serialhdl.py`・`clocksync.py`・`mcu.py`（接続と設定送信）を C++ に移植 |
| `components/klippy/` | printer.cfg の読み込み、G-code、CoreXY/Cartesian、ステッパー有効化、pwm_tool、Input Shaper、TMC2209 |
| `components/link_tests/` | M2/M3 の試験本体（Mac と ESP32 で共通） |
| `components/m1_bench/` | M1 の性能測定 |
| `apps/link_test/` | M2/M3 試験の ESP-IDF プロジェクト |
| `bench/` | M1 性能測定の ESP-IDF プロジェクト |
| `host/` | 同じコードの Mac 版（試験と結果の照合用） |
| `tools/sim_mcu/` | 仮想 MCU（Klipper の MCU コード + Mac 用の時刻・シリアル・GPIO・TMC2209 の模擬） |
| `tools/sim_bridge.py` | CYD の USB シリアルと仮想 MCU を中継する |
| `config/` | printer.cfg のサンプル（SKR Pico、CoreXY ペンプロッタ） |

## M2/M3：MCU 接続と CoreXY

### 仮想 MCU

`tools/sim_mcu/` は Klipper の MCU コード（`src/` の sched、command、stepper、endstop、trsync など）を改造せずに Mac 上でビルドする。差し替えるのは時刻、シリアル、GPIO だけ。

- ピン名（gpio0〜29）とクロック（12MHz）は RP2040 と同じにしてある。SKR Pico 用の printer.cfg がそのまま使える
- GPIO の変化回数を数えられる（`sim_get_pin`）。ステップのパルス数を確認できる
- tmcuart のコマンドには TMC2209 の模擬（アドレス 0〜3）が応答する（`tmcuart_sim.c`）。書き込んだレジスタは `sim_tmc_get` で読み返せる

```sh
python3 tools/sim_mcu/build.py      # build/sim_mcu/sim_mcu を作る
```

Mac 版の試験は、仮想 MCU を子プロセスとして起動して socketpair で接続する（ポート名 `sim:<パス>`）。

### Mac での試験

```sh
cmake -S host -B build/host -G Ninja && ninja -C build/host
./build/host/m2_axis_test                                  # M2：1 軸の往復
./build/host/m3_square_test config/skr_pico_corexy_penplotter.cfg   # M3
```

M3 は printer.cfg から本体を組み立て、300mm/s（F18000）で 100mm の正方形を 5 周描く。ペンは各周で上げ下げする。次を確認する。

- CoreXY の両モータの MCU 上の位置が原点 0 に戻る
- ペン出力（gpio20）の切り替え回数が指令どおり
- TMC2209 のレジスタ値が本家 Klipper の計算と一致（GCONF=0x1C0、CHOPCONF=0x34030053、IHOLD_IRUN=0x00080F0F、PWMCONF=0xC80D0E24）

### CYD での試験

CYD の USB シリアル（UART0）で Klipper の通信を行い、`tools/sim_bridge.py` が Mac 上の仮想 MCU に中継する。試験中はログ出力を止め、結果は試験の後にまとめて表示する。

```sh
cd apps/link_test
. ~/esp/esp-idf-v5.4.2/export.sh
idf.py -B ../../build/link_esp32 set-target esp32
idf.py -B ../../build/link_esp32 -p /dev/cu.usbserial-XXXX flash
python ../../tools/sim_bridge.py /dev/cu.usbserial-XXXX     # 結果は build/link_test_result.txt
```

試験の種類（M2/M3）と通信ポートは menuconfig の「Link test」で選ぶ。SKR Pico 実機では `uart:2:27:22`（CYD の UART2、TX=GPIO27、RX=GPIO22）にする。

### 結果（CYD、2026-11-12）

| 試験 | 結果 |
| --- | --- |
| M2：1 軸 +100mm → 50mm | 合格（MCU 位置 3368 ステップ、パルス数一致） |
| M3：CoreXY 300mm/s 正方形 5 周 + TMC2209 | 合格（両モータ原点 0、TMC レジスタ一致、通信の再送・不正データ 0） |
| ステップ生成の負荷（M3） | 1 回の生成の最悪値 73% |
| 空きヒープ（M3） | 接続後 186KB、描画後 149KB、最低 142KB |

ESP32 向けに次を変えている。いずれも動作は本家と同じ。

| 項目 | 本家 | ESP32 版 |
| --- | --- | --- |
| serialqueue のデバッグ用履歴 | 送受信 100 件ずつ | 8 件ずつ（`port/serialqueue_wrap.c` で確保後に削る） |
| ステップ生成スレッドのスタック | Linux の既定 | 3KB（実測 約 1.2KB） |
| UART の送受信バッファ | — | 各 1KB |
| データ辞書の展開 | zlib | ESP32 の ROM の miniz（作業領域はヒープに確保） |

ESP-IDF の UART は既定でテキスト用の改行変換（受信 CR→LF、送信 LF→CRLF）を行う。Klipper のバイナリ通信が壊れるため、接続中は変換を止めている。

### M4：原点復帰・マクロ・SD 実行

```sh
./build/host/m4_homing_test config/skr_pico_corexy_penplotter.cfg
./build/host/m4_sdcard_test printer.cfg file.gcode   # 一時停止・再開・中止
```

- 原点復帰：`G28`、`[homing_override]`、TMC2209 の仮想エンドストップ（DIAG）によるセンサレス。仮想 MCU ではモータが一定ステップ進んだら DIAG を立てて再現する（`sim_trigger_on_steps`）。
- マクロ：Jinja2 のサブセット（`{% if/for/set %}`、フィルタ、`printer.*`、`params`、`rename_existing`、`SET_GCODE_VARIABLE`）。
- SD 実行：`virtual_sdcard`・`print_stats`・`pause_resume` 相当（`SDCARD_PRINT_FILE`、`M23/M24/M25/M26/M27`、`PAUSE/RESUME/CANCEL_PRINT`、`SAVE/RESTORE_GCODE_STATE`）。本家は 8KB ずつ読んで行の一覧を作るが、ESP32 では 1KB ずつ読んで 1 行ずつ取り出す（本家の方式では一時的に約 36KB 使い、空きヒープが 31KB まで下がった）。

結果（CYD、2026-11-12、ペンプロッタ用の実 printer.cfg と約 180KB の G-code）

| 試験 | 結果 |
| --- | --- |
| 原点復帰（センサレス、CoreXY） | 合格 |
| マクロ経由で G-code 全行 | 合格（1 行 平均 7.5ms、目安 14ms 以内。先読み切れ 0） |
| SD 実行：30% で一時停止 → 再開 → 完了 | 合格（停止中に動きなし、印刷時間から停止時間を除外、ペン切り替え 1752 回で一致、終了位置一致） |
| SD 実行：2 回目を 10% で中止 | 合格（状態 cancelled、動き停止、プリンタは ready のまま） |
| 空きヒープの最低値 | マクロ試験 63〜68KB、SD 試験 67KB |

### ローカル UI（CYD の画面とタッチ）

LVGL 8.4（`third_party/lvgl`、無改変）で 320×240 の画面を作る。タブは Status（状態、ファイル、進み具合、一時停止・再開・中止）、Files（ファイルを選んで印刷）、Move（XY の移動、原点復帰、ペンの上げ下げ、モータ OFF）の 3 つ。UI は別タスク（コア 0）で動き、klippy からは 0.2 秒ごとに作る状態のコピー（`Printer::snapshot()`）を読み、操作は G-code として `submit_script()` に渡す。

```sh
./build/host/ui_screens build/ui_screens   # 各画面を PNG に出力（実機なしで配置を確認）
```

- 表示：ILI9341（SPI2、40MHz、描画バッファ 8 行）。タッチ：XPT2046（ビットバング。SPI3 は SD カード用に空けておく）。向き・色反転・タッチの補正値は menuconfig の「CYD board」。
- CYD での負荷（SD 印刷の試験、UI 表示あり）：合格、先読み切れ 0、klippy の処理時間 +6%（UI なし 73.3 秒 → 78.0 秒）。UI の更新は 0.5 秒ごとで、変わった部分だけ描き直す。両コアがフラッシュのキャッシュを共有するため、描画が多いと klippy も遅くなる（5 回/秒・毎回描き直しでは +15%、先読み切れ 1 回）。
- メモリ：UI 全体で約 30KB（描画バッファ 5KB、LVGL のオブジェクト 10KB、タスクのスタック 5KB など）。SD 印刷中の空きヒープの最低値は 62KB → 38KB。
- 画面の向き・色・タッチ位置は実機で未確認（menuconfig の「Link test」で UI demo を選ぶと、仮想 MCU 相手に画面から操作できる）。

### M5：CoreS3 で Moonraker 互換 API と Mainsail

`apps/plotter` を CoreS3 向け（`PLOTTER_NETWORK`）にビルドすると、WiFi に接続し、`http://<IP>/` で Mainsail、`/websocket` と HTTP で Moonraker 互換 API を提供する。画面には CYD と同じ LVGL の UI を出す。

```sh
python3 tools/make_plotter_fs.py                 # Mainsail・printer.cfg・G-code を build/plotter_fs に用意
cd apps/plotter
PLOTTER_FLASH_FS=1 idf.py -B ../../build/plotter_s3_sim -DSDKCONFIG=../../build/plotter_s3_sim/sdkconfig build
# 書き込み後、仮想 MCU と中継（MCU の通信は USB の usbjtag を共用）
python ../../tools/sim_bridge.py /dev/cu.usbmodemXXXX --log ../../build/plotter_s3_result.txt --timeout 1800
python3 ../../tools/moonraker_check.py <IP>:80
```

- MCU が USB を共用している間はコンソールの出力を RAM（PSRAM 128KB）に溜め、`/server/files/klippy.log` で読める。MCU が停止（shutdown）したときは、本家と同じく直近 100 件の送受信（`Dumping send queue` など）も書き出す。
- 結果（2026-11-13、CoreS3 + 仮想 MCU）：再起動 6 回すべてで、起動時の原点復帰と自動印刷が完了し、`moonraker_check.py` は失敗 0。続けて 1 分間に `square.gcode` を 11 回印刷しても停止なし。

CoreS3 で見つけて直した問題

| 症状 | 原因と対処 |
| --- | --- |
| 起動しない（再起動の繰り返し） | CoreS3 の PSRAM は Octal ではなく Quad。`sdkconfig.defaults.esp32s3` を `SPIRAM_MODE_QUAD` に |
| Mainsail が接続すると落ちる（httpd 内で LoadProhibited） | HTTP サーバーのスタック 8KB があふれていた。16KB に |
| 状態の問い合わせ中に落ちる（コア 1） | `run_on_klippy()` が待ちきれずに戻った後で、klippy が呼び出し元の消えたローカル変数を使っていた。待ちきれなかった処理は実行しないように |
| 印刷中に `MCU shutdown: Timer too close` | CYD 用に縮めた先読み（ステップを実行の約 50ms 前に送る）では、WiFi や USB の揺らぎで間に合わない。PSRAM のある基板では Klipper 本来の値（`buffer_time_high` 1.0、ステップ生成 0.45〜0.70 秒）に |
| 起動時の原点復帰で `Timer too close` | drip 移動の前の待ち（`await_flush_time`）で使う `Printer::pause()` が、待っている間にステップを送っていなかった。本家の `reactor.pause()` と同じく、待つ間も `service()` を回すように |

#### WiFi の設定

接続先は NVS に保存し、無ければ menuconfig（`Plotter` → `WiFi SSID` / `WiFi password`）の値を使う。

| 方法 | 手順 |
| --- | --- |
| 画面 | System タブ → WiFi → 一覧から選び、画面のキーボードでパスワードを入力 |
| 設定用アクセスポイント | 保存した WiFi が無いか、20 秒つながらないと `ESP32Klipper-XXXX` を出す。スマホでつなぎ `http://192.168.4.1/` を開いて選ぶ。保存した WiFi へは 30 秒ごとに再接続を試し、つながった 1 分後に閉じる |
| ブラウザ（接続中） | `http://<IP>/wifi` |

- 設定用アクセスポイントのパスワードは menuconfig の `Setup access point password`。空（既定）だと誰でもつなげ、開いている間は Mainsail も操作できる。
- アクセスポイントでの設定は未確認（画面と通常の接続は確認済み）。

#### microSD カード（CoreS3）

`PLOTTER_CORES3_SD`（既定で有効）。CYD と同じく、カードの一番上の `printer.cfg` が内蔵のものに代わり、G-code はカードの `/gcodes`（無ければ一番上）から読む。Mainsail からのアップロードと設定の編集もカードに入る。カードが無ければ内蔵ストレージ（`/data`）を使う。

- カードは画面と同じ SPI2（CS 4）につながり、カードの MISO と画面の DC が同じ GPIO35。バスをセマフォで取り合い、カードの通信中だけ GPIO35 を入力に切り替える。画面を表示したまま読める。
- 確認：32GB のカード、既存機の printer.cfg（マクロ多数）で起動し、仮想 MCU とのリンクまで動作。
- 大きな printer.cfg を読むと内蔵 RAM を約 70KB 使い、MCU 通信のスレッドが作れずに落ちた。設定の読み込み中だけ PSRAM を優先させ（`heap_caps_malloc_extmem_enable(0)`）、起動後の内蔵 RAM の空きは 48KB → 105KB。

起動時の内蔵 RAM の使い道（CoreS3、既存機の printer.cfg）：WiFi 78KB、画面 37KB、HTTP サーバー 29KB、printer.cfg 12KB（PSRAM 優先にする前は 68KB）、SD 3KB。

### 未対応

- Input Shaper の共振測定（`TEST_RESONANCES`、`SHAPER_CALIBRATE`）
- TMC の位相オフセットの追跡、StallGuard の連続取得
- SKR Pico 用の本物の MCU ファームのビルド（Homebrew の arm-none-eabi-gcc に newlib がないため。`brew install --cask gcc-arm-embedded` が必要）


## M1 性能測定

ESP32 には倍精度の浮動小数点演算器がない。Klipper のホスト側の移動計画とステップ生成（倍精度で書かれている）が、実際の描画速度に間に合うかを、MCU をつながずに測る。

`bench/data/card_opt.gcode` を次の流れで処理し、所要時間を測る。

```
G-code 解析（Fraxinus0pen の printer.cfg のマクロの動作を再現）
 → toolhead（先読み・台形加速、toolhead.py を C++ に移植）
 → trapq → itersolve（CoreXY）→ Input Shaper（zv）→ stepcompress
 → steppersync（MCU に送るメッセージを作り、送らずに数える）
```

trapq 以降は Klipper の C コード（`klippy/chelper`）を改造せずに使っている。MCU に送るメッセージは `components/m1_bench/src/serialqueue_stub.c` が受け取って数える。

### Mac でのビルドと実行

```sh
cmake -S host -B build/host -G Ninja
ninja -C build/host
./build/host/bench_host bench/data/card_opt.gcode
```

### ESP32 でのビルドと実行

```sh
. ~/esp/esp-idf-v5.4.2/export.sh
cd bench

# CYD（ESP32）
idf.py -B ../build/esp32 set-target esp32
idf.py -B ../build/esp32 -p /dev/cu.usbserial-XXXX flash monitor

# CoreS3（ESP32-S3）
idf.py -B ../build/esp32s3 set-target esp32s3
idf.py -B ../build/esp32s3 -p /dev/cu.usbmodemXXXX flash monitor
```

起動すると次の 2 モードで 4 条件ずつ測り、結果をシリアルに出力する。

- **モード A**：ステップ生成のスレッドがどちらのコアでも動ける
- **モード B**：ステップ生成をコア 1 に固定する（設計どおり、コア 0 を WiFi と UI に空ける）

4 条件は、送り速度 1 倍の Input Shaper なし／あり、2 倍・5 倍の Input Shaper あり。

### 出力の見方

| 項目 | 意味 |
| --- | --- |
| `motion time` | G-code が生む動作時間（機械が実際に動く時間） |
| `cpu time` | それを計算するのにかかった時間（`plan` は G-code 解析と移動計画、`step gen` はステップ生成） |
| `realtime ratio` | motion time ÷ cpu time。1 を超えれば機械より速く計算できている |
| `worst load` | ステップ生成 1 回あたりの、計算時間 ÷ その回が受け持つ動作時間の最大値 |
| `mcu messages` | MCU に送るメッセージの量と、250000 baud の UART に占める割合 |
| `output hash` | 生成したメッセージ列のハッシュ。Mac 版と一致すれば計算結果が同じ |
| `heap` | 測定中のヒープ使用量（G-code 1 行ごとに採取した値） |
| `heap true min` | 行の途中の一時的なピークも含めた最低空きヒープ（ESP32 のみ） |

### 合格の目安

- モード B の「x1 feed, zv shaper」で `realtime ratio` が **3 以上**。マクロ展開、Moonraker 互換 API、画面描画の分の余裕を残すため。
- 同じ条件で `worst load` が **50% 未満**。
- `output hash` が Mac 版と一致する。

### バッファ設定

Klipper の既定値のままでは CYD のヒープ（約 290KB）に収まらないため、ESP32 向けに縮めている（`bench.hpp` の `esp32_toolhead()` / `esp32_queuing()`）。Mac 版で `BENCH_KLIPPER=1` を付けると Klipper の既定値で測る。

| 項目 | Klipper | ESP32 版 |
| --- | --- | --- |
| MCU に対する先行時間（buffer_time_high） | 1.0 s | 0.5 s |
| ステップ生成の先行（BGFLUSH_SG_LOW/HIGH） | 0.45〜0.7 s | 0.20〜0.35 s |
| 動作履歴の保持（MOVE_HISTORY_EXPIRE） | 30 s | 0.25 s |
| 先読みキューの上限（ESP 版で追加） | なし | 64 移動 |

先読みの上限は、上限に達したら Klipper の遅延フラッシュ（lazy flush）を早めに試すだけで、計画のアルゴリズムは変えない。Mac 版では上限 64 でも全条件のハッシュが上限なしと一致した。`BENCH_MAXMOVES=<数>` で変えられる（0 = 上限なし）。

履歴は実行済みの動作の記録（原点復帰時の位置の逆算などに使う）で、G-code のバッファではない。Klipper の既定値では card_opt.gcode で最大約 1.7MB を使う。

### 測定結果（CYD = ESP32-2432S028R、2026-11-12、先読み上限 64）

出力ハッシュは 8 条件すべてで Mac 版と一致した。

| 条件 | A：realtime / worst load | B：realtime / worst load | 真のピーク使用量（上限なし → 64） |
| --- | --- | --- | --- |
| x1 shaper なし | 10.95x / 12.5% | 6.96x / 20.7% | 105KB → 105KB |
| x1 ZV | 4.59x / 34.3% | 2.77x / 57.7% | 109KB → 109KB |
| x2 ZV | 2.81x / 63.9% | 1.69x / 91.2% | 129KB → 114KB |
| x5 ZV | 2.40x / 111.6% | 1.44x / 168.8% | 217KB → 127KB |

Mac 版のハッシュ：x1 shaper なし `46da49023c004e7e`、x1 ZV `8b34bf312c01c66f`、x2 ZV `b69f13825f9f202e`、x5 ZV `e3c162ef08684e41`。

### この測定に含まれないもの

- `G01` などのマクロ展開（Jinja2）。M1 では `G01` を `G1` として直接処理している。Jinja2 を実装した後に追加で測る。
- 原点復帰（センサレス）の動作。`G28` は位置の設定で置き換えている。
- 実際の UART 送信、クロック同期、SD カードからの読み込み。
- 本物の Klipper の出力との突き合わせ。Mac 版と ESP32 版が一致することは確かめられるが、Klipper 本体（Python）と同じ結果になるかは、Klipper のバッチモードとの比較を別途行う。

## ライセンス

GPLv3（Klipper のコードを含むため）。`bench/data/` の G-code と printer.cfg（既存機 Fraxinus0pen のもの）は測定用のデータで、公開対象ではない（`.gitignore` 済み）。
