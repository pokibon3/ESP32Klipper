# ESP32Klipper

> **開発中です。** まだ SKR Pico の実機では一度も動かしておらず、仮想 MCU（Mac 上のプロセス）相手の試験しかしていません。設定項目・API・ファイル構成は予告なく変わります。実機のプロッタを動かす用途にはまだ使わないでください。

Klipper のホスト（本家では Raspberry Pi 上の Python）を ESP32 に移植した、ペンプロッタ用のコントローラです。CYD（ESP32-2432S028R）または M5Stack CoreS3 が printer.cfg と G-code を読み、Klipper 純正ファームの MCU（SKR Pico など）を UART で動かします。Raspberry Pi は要りません。

開発の経緯と試験結果は [docs/development.md](docs/development.md)、設計は「CYD Klipper互換ホスト 全体設計書」にあります。

## 機能

### CYD と CoreS3 の違い

| | CYD（ESP32-2432S028R） | M5Stack CoreS3 |
| --- | --- | --- |
| CPU・メモリ | ESP32、PSRAM なし | ESP32-S3、PSRAM 8MB |
| タッチ画面の UI | あり | あり（同じ UI） |
| WiFi・Mainsail・Moonraker 互換 API | **なし** | あり |
| 画面の System → WiFi | なし | あり（接続先の選択、アクセスポイントモード） |
| microSD（printer.cfg と G-code） | あり | あり |
| カードが無いとき | 内蔵ストレージの printer.cfg と G-code | 内蔵ストレージの printer.cfg と G-code（Mainsail から書き換え可） |
| G-code を入れる方法 | microSD カード | microSD カード、Mainsail からのアップロード |
| ステップの先送り（`buffer_time_high`） | 0.4 秒（メモリに合わせて短く） | 1.0 秒（本家と同じ） |
| MCU との接続 | UART2（TX=GPIO27、RX=GPIO22） | UART（ピンは printer.cfg の `[mcu] serial` で指定） |
| 実機での確認 | 画面・タッチ、仮想 MCU で印刷 | 画面・タッチ、WiFi、microSD、Mainsail、仮想 MCU で印刷 |

### 画面（両方）

320×240 のタッチ画面に 5 つのタブがあります。

- **Status**：状態、ファイル名、進み具合、残り時間。印刷中は速度の増減（M220）、一時停止・再開、中止。終わった後は Reprint で同じファイルをもう一度印刷
- **Files**：ファイルを選んで印刷
- **Move**：XY の移動（移動量を選ぶ）、原点復帰、ペンの上げ下げ、モータ OFF
- **Macros**：`[gcode_macro]` を実行（`_` で始まるものと PAUSE / RESUME / CANCEL_PRINT は出さない）
- **System**：状態、メモリ、IP アドレス、E-stop（M112）、Restart（MCU と本体を再起動）。CoreS3 は WiFi の設定も

### Mainsail（CoreS3）

`http://<IP>/` で Mainsail が開きます。Moonraker は使わず、本体が Moonraker 互換 API（WebSocket と HTTP）を出します。ファイルのアップロード・印刷・一時停止・中止、コンソール、printer.cfg の編集、ジョブ履歴、Mainsail の設定の保存が使えます。温度・Web カメラ・更新管理などペンプロッタに要らない API は空の結果を返します。

### 本家 Klipper との違い

ホスト側（klippy）を C++ で書き直しています。動きの計算（trapq、itersolve、stepcompress、Input Shaper）は本家の C コードを改造せずに使っているので、同じ printer.cfg から同じステップが出ます。MCU は Klipper 純正のファームのままです。

**printer.cfg のセクション**

| 区分 | セクション |
| --- | --- |
| 使える | `[mcu]`、`[printer]`（`corexy` と `cartesian` のみ）、`[stepper_x/y/z]`、`[tmc2209 …]`（UART、センサレス原点復帰の `diag_pin` 含む）、`[homing_override]`、`[input_shaper]`、`[pwm_tool …]`・`[output_pin …]`（ペン）、`[gcode_macro …]`、`[virtual_sdcard]`、`[pause_resume]`、`[print_stats]` |
| 読み飛ばす | 上記以外（`[servo]`、`[gcode_arcs]`、`[neopixel]`、`[fan]`、`[safe_z_home]`、`[extruder]`、`[heater_bed]`、`[bed_mesh]` など）。起動時に「not supported yet」と警告して動き続ける |
| 無いもの | 加速度センサー（`[adxl345]` など）と共振測定、`[include]`（printer.cfg は 1 ファイルにまとめる）、`[mcu host]`、複数の MCU、`SAVE_CONFIG` |

**G-code コマンド**

| 区分 | コマンド |
| --- | --- |
| 移動 | G0、G1、G4、G28、G90、G91、G92、G20/G21、M82/M83、M400、M204、M220、`SET_VELOCITY_LIMIT`、`SET_KINEMATIC_POSITION` |
| 状態 | M114、M119、`GET_POSITION`、`QUERY_ENDSTOPS` |
| モータ・ペン | M18、M84、`SET_PIN`、`SET_TMC_CURRENT`、`SET_TMC_FIELD`、`INIT_TMC`、`DUMP_TMC` |
| ファイル実行 | M20、M21、M23〜M27、`SDCARD_PRINT_FILE`、`SDCARD_RESET_FILE`、`PAUSE`、`RESUME`、`CANCEL_PRINT`、`CLEAR_PAUSE`、`SET_PRINT_STATS_INFO` |
| マクロ | `[gcode_macro]` の命令、`SET_GCODE_VARIABLE`、`SAVE_GCODE_STATE`、`RESTORE_GCODE_STATE`、`rename_existing` |
| 非常停止 | M112 |
| 何もしない | M104、M105、M106、M107、M140（温度・ファン。エラーにしない） |
| 無いもの | G2/G3（円弧）、M117/M118、`RESPOND`、`STATUS`、`HELP`、`RESTART`/`FIRMWARE_RESTART`（G-code としては無い。画面の Restart と Mainsail のボタンは使える）、`FORCE_MOVE`、`SET_STEPPER_ENABLE`、`SET_SERVO` |

**マクロ（Jinja2）**：本家は Python の Jinja2 を使いますが、ここでは必要な部分だけを C++ で実装しています。`{% if/elif/else %}`、`{% for %}`（`loop.*`）、`{% set %}`、コメント、空白制御、Python と同じ演算と文字列の書式、`printer.*`、`params`、よく使うフィルタ（`default`、`float`、`int`、`round`、`lower`、`upper`、`length`、`join`、`min`、`max` など）。マクロの定義、`{% macro %}`、`{% include %}` などは使えません。

**そのほかの違い**

- 本家は G-code ファイルを 8KB ずつ読んで行の一覧を作りますが、ここでは 1KB ずつ読んで 1 行ずつ処理します（メモリ節約）。
- CYD はメモリが少ないため、ステップの先送り・移動履歴・先読みを短くしています（計算結果は同じ。本家の値で計算したものと一致することを確認済み）。
- 本家は G-code の解析とマクロの展開に Python を使いますが、ここでは C++ です。CYD では 1 行あたり平均 7.5ms で処理します。

### 未対応

- Input Shaper の共振測定（`TEST_RESONANCES`、`SHAPER_CALIBRATE`）。`[input_shaper]` に値を書けば使える
- TMC の位相オフセットの追跡、StallGuard の連続取得
- 描画中のプレビュー、日本語表示

## 使い方

### 用意するもの

- ESP-IDF v5.4.2（`. ~/esp/esp-idf-v5.4.2/export.sh`）
- Klipper 本家のソース：`git clone --depth 1 https://github.com/Klipper3d/klipper.git third_party/klipper`
- CoreS3 のみ：Mainsail のリリース zip を `build/mainsail` に展開しておく

### ビルドと書き込み

最初に内蔵ストレージの中身（Mainsail、printer.cfg、G-code）を作ります。`PLOTTER_FLASH_FS=1` を付けて書き込むと、ファームと一緒に書き込みます（本体のファイルは置き換わります）。2 回目からは `app-flash` でファームだけ書き込みます。

```sh
. ~/esp/esp-idf-v5.4.2/export.sh

# CoreS3
python3 tools/make_plotter_fs.py --config <printer.cfg>
cd apps/plotter
idf.py -B ../../build/plotter_s3 set-target esp32s3
idf.py -B ../../build/plotter_s3 menuconfig       # Plotter → WiFi SSID / password（後から画面でも設定できる）
PLOTTER_FLASH_FS=1 idf.py -B ../../build/plotter_s3 -p /dev/cu.usbmodemXXXX flash

# CYD
python3 tools/make_plotter_fs.py --no-www --config <printer.cfg> --out build/plotter_fs_cyd
cd apps/plotter
idf.py -B ../../build/plotter_cyd set-target esp32
PLOTTER_FLASH_FS=1 idf.py -B ../../build/plotter_cyd -p /dev/cu.usbserial-XXXX flash
```

menuconfig の `Plotter` で主に設定するもの：WiFi（CoreS3）、MCU のポート（空なら printer.cfg の `[mcu] serial`）、起動時に自動で印刷するファイル。CYD の画面の種類（ILI9341 / ST7789）・向き・タッチの補正は `CYD board` で設定します。

### printer.cfg

`config/skr_pico_corexy_penplotter.cfg` が SKR Pico + CoreXY ペンプロッタの例です。`[mcu] serial` は `uart:<UART 番号>:<TX の GPIO>:<RX の GPIO>` で書きます（例：CYD は `uart:2:27:22`）。

### microSD カード

| 置き場所 | 内容 |
| --- | --- |
| カードの一番上の `printer.cfg` | 内蔵の printer.cfg の代わりに使う |
| `/gcodes`（無ければ一番上） | 印刷する G-code |

カードが無ければ内蔵ストレージを使います。CoreS3 では Mainsail からのアップロードと設定の編集もカードに入ります。

### WiFi の設定（CoreS3）

接続先は本体に保存されます。保存した接続先が無ければ menuconfig の値を使います。

| 方法 | 手順 |
| --- | --- |
| 画面 | System → WiFi → 一覧から選び、画面のキーボードでパスワードを入力 |
| 設定用アクセスポイント | WiFi につながらないと `ESP32Klipper-XXXX` が出る。スマホでつなぎ `http://192.168.4.1/` を開いて選ぶ |
| ブラウザ（接続中） | `http://<IP>/wifi` |

設定用アクセスポイントは、保存した WiFi が無いとき、起動時に 20 秒つながらないとき、動作中に 10 回続けて失敗したときに出ます。保存した WiFi につながると 1 分後に閉じます。

**アクセスポイントモード**：本体が自分の WiFi（`ESP32Klipper-XXXX`）になり、ルーターなしでスマホや PC をつないで `http://192.168.4.1/` で Mainsail を使えます。

- 切り替え：System → WiFi の **AP mode**（戻すときは **Use WiFi**）、または `/wifi` ページの「Use as access point」。再起動しても保たれる
- パスワード：本体ごとに 8 文字を自動で作り、WiFi 画面と System タブに表示（menuconfig の `Setup access point password` で固定もできる）
- 同時に 2 台までつなげる
- スマホが「インターネットなし」で離れる場合は「このまま接続」を選ぶ

### SKR Pico なしで試す（仮想 MCU）

Klipper 本家の MCU コードを Mac 上で動かす「仮想 MCU」を相手に、印刷まで試せます。MCU との通信は本体の USB を共用し、Mac の `tools/sim_bridge.py` が仮想 MCU に中継します。

1. 準備：`python3 tools/sim_mcu/build.py`（pyserial が要る。ESP-IDF の Python 環境には入っている）
2. 仮想 MCU 用にビルドする。`sdkconfig.sim` は MCU のポートを USB にし、起動後に `square.gcode` を自動で印刷し、原点復帰の DIAG を仮想 MCU に入れさせる
   ```sh
   cd apps/plotter
   idf.py -B ../../build/plotter_s3_sim -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.sim" set-target esp32s3
   idf.py -B ../../build/plotter_s3_sim menuconfig      # WiFi
   PLOTTER_FLASH_FS=1 idf.py -B ../../build/plotter_s3_sim -p /dev/cu.usbmodemXXXX flash
   cd ../..
   ```
   CYD は `build/plotter_cyd_sim`、`set-target esp32` で同じようにします。
3. 中継を起動する（本体をリセットしてからつなぐ）
   ```sh
   python3 tools/sim_bridge.py /dev/cu.usbmodemXXXX --log build/plotter_s3_result.txt --timeout 7200 --trace build/sim_trace.bin
   ```
   - `--timeout` 秒で終わる（既定 600 秒）。`--trace` は描いた内容の記録
   - 本体が再起動すると（画面の Restart も）中継は終わるので、起動し直す
   - 起動時のログは `--log` のファイルに入る。つながった後のログは `http://<IP>/server/files/klippy.log`（CoreS3）
4. 使う：Mainsail（`http://<IP>/`）や画面から印刷する。curl でも送れる
   ```sh
   curl -F "file=@examples/square.gcode" http://<IP>/server/files/upload
   curl -X POST "http://<IP>/printer/print/start?filename=square.gcode"
   ```

### 描いた内容を見る（`tools/plot_view.py`）

仮想 MCU の記録（`--trace`）から、ペンを下ろした線を復元します。printer.cfg からステップのピン、`rotation_distance`、キネマティクス、ペンのピンを読みます。Python の標準ライブラリだけで動きます。

```sh
# ブラウザでリアルタイムに見る → http://localhost:8770/
python3 tools/plot_view.py build/sim_trace.bin --cfg <printer.cfg> --serve 8770
# PNG と SVG にする
python3 tools/plot_view.py build/sim_trace.bin --cfg <printer.cfg> --png build/plot.png --svg build/plot.svg
```

- 用紙の範囲、ペンを下ろした線、ヘッドの位置（赤 = ペンが下）を 0.2 秒ごとに更新します
- Clear ボタンでそれまでの線を消します。中継を起動し直すと最初から描き直します
- 原点復帰のたびに、その軸を `position_endstop` に合わせます

## ソースの構成

| パス | 内容 |
| --- | --- |
| `apps/plotter/` | プロッタのファーム（CYD・CoreS3） |
| `components/klippy/` | printer.cfg、G-code、キネマティクス、TMC2209、原点復帰、マクロ、SD 実行 |
| `components/klippy_lite/` | `toolhead.py` と `motion_queuing.py` の C++ 移植 |
| `components/chelper/` | 本家の chelper（C）を ESP-IDF と Mac でビルドする層 |
| `components/mcu_link/` | MCU との通信（本家の `serialqueue.c` + C++ 移植） |
| `components/moonraker/` | Moonraker 互換 API |
| `components/ui/` | タッチ画面の UI（LVGL 8.4） |
| `components/cyd_board/`、`components/cores3_board/` | 各ボードの画面・タッチ・SD |
| `config/` | printer.cfg の例 |
| `examples/` | G-code の例 |
| `tools/` | 仮想 MCU、中継、描画ビューア、ストレージ作成 |
| `host/` | 同じコードの Mac 版（試験用） |

## ライセンス

GPLv3（Klipper のコードを含むため）。
