# CYD Klipper互換ホスト 全体設計書

2026-11-12 作成、2026-11-13 更新。開発の経緯と試験結果は [development.md](development.md)、使い方は [README](../README.md)。

**開発中**：SKR Pico の実機ではまだ動かしておらず、仮想 MCU 相手の試験のみ。設計・設定項目・API は変わる。ソースは [github.com/pokibon3/ESP32Klipper](https://github.com/pokibon3/ESP32Klipper)（GPLv3）。

## 1. 概要

CYD（ESP32-2432S028R）または M5Stack CoreS3 を Raspberry Pi＋Klippy の代わりのホストにし、純正 Klipper ファームを載せた SKR Pico（RP2040）を UART で駆動するペンプロッタ用コントローラを作る。タイミングに厳しい処理はすべて MCU 側が担うため、ESP32 は時刻付きのステップ予約を数百 ms 先行して送るだけでよい。

**目標**

- SKR Pico 上の純正 Klipper MCU ファームを無改造で使う
- Klipper の printer.cfg をそのまま読める（対応セクションの範囲で）
- Moonraker 互換 API を提供し、Mainsail からの操作・ファイル管理・設定編集ができる
- 本体単体でも SD カードの G-code を選んで描画でき、プレビューと進捗を表示する

**非目標**

- G-code の最適化（経路並べ替え、ペンアップ移動の削減など）
- ヒーター、押出機、ベッドメッシュなど 3D プリンタ固有の機能
- pressure advance、複数 MCU、共振の測定（Input Shaper は測定済みの値の適用のみ対応）
- Klippy（Python）の移植。必要な部分を C/C++ で再実装する

**前提**

- 対象ボードは CYD（ESP32、PSRAM なし、Flash 4MB）と M5Stack CoreS3（ESP32-S3、PSRAM 8MB、Flash 16MB）の 2 種。同じソースから両方をビルドし、機能とメモリ設計は制約の厳しい CYD に合わせる
- 開発環境は ESP-IDF（FreeRTOS）。描画は LVGL を想定
- Klipper の `klippy/chelper` の C コードを流用するため、本プロジェクトは GPLv3 の OSS として公開する

**決定事項（2026-11-12）**

| 項目 | 決定 |
| --- | --- |
| 機構 | CoreXY を先行。Cartesian は後から対応 |
| 原点復帰 | TMC2209 のセンサレスホーミングを必須とする。物理エンドストップは前提にしない |
| ペン昇降 | SKR Pico のレーザー出力からトランジスタバッファを介してソレノイドを駆動する。マクロから制御し、サーボも選べるようにしておく |
| 基準バージョン | Klipper、Mainsail とも最新版に追従する |
| 公開 | OSS（GPLv3） |
| SKR Pico との接続 | 両ボードとも UART。USB Host は使わない |

## 2. システム構成

CYD と SKR Pico は 3 本（TX・RX・GND）だけで接続する。両方 3.3V ロジックなのでレベル変換は不要。

**ハードウェア**

| 部品 | 役割 | 主な仕様 |
| --- | --- | --- |
| CYD（ESP32-2432S028R） | ホスト、UI、Web サーバ | ESP32-WROOM-32、2.8」 ILI9341 320×240、XPT2046 抵抗膜タッチ、microSD |
| SKR Pico v1.0 | Klipper MCU | RP2040、TMC2209 ×4（UART 制御）、エンドストップ、サーボ端子 |
| ペンプロッタ機構 | 駆動対象 | XY ステッパ 2 個（CoreXY を先行）、ペン昇降用ソレノイド（SKR Pico のレーザー出力からトランジスタバッファ経由）。エンドストップなし（センサレス） |

**配線**

| CYD | 方向 | SKR Pico | 備考 |
| --- | --- | --- | --- |
| GPIO27（CN1、UART2 TX） | → | GPIO1（UART0 RX、Pi ヘッダ） | 250000 baud |
| GPIO22（CN1、UART2 RX） | ← | GPIO0（UART0 TX、Pi ヘッダ） |  |
| GND | — | GND | 必須 |
| 5V（任意） | ← | 5V（Pi ヘッダ） | CYD を SKR Pico から給電する場合。電流容量は要確認 |

**SKR Pico のピン割当（確認済み）**

BTT 公式のピン配置図（[BTT SKR Pico V1.0-PIN.pdf](https://github.com/bigtreetech/SKR-Pico/tree/master/Hardware)）と Klipper 同梱の [generic-bigtreetech-skr-pico-v1.0.cfg](https://github.com/Klipper3d/klipper/blob/master/config/generic-bigtreetech-skr-pico-v1.0.cfg) で照合した。

| 用途 | ピン | 端子 | 備考 |
| --- | --- | --- | --- |
| X ステッパ | STEP gpio11、DIR gpio10、EN gpio12 | X | TMC アドレス 0 |
| Y ステッパ | STEP gpio6、DIR gpio5、EN gpio7 | Y | TMC アドレス 2 |
| Z ステッパ | STEP gpio19、DIR gpio28、EN gpio2 | Z1（Z2 は並列） | TMC アドレス 1。Klipper の仕様上定義する |
| E0 ステッパ | STEP gpio14、DIR gpio13、EN gpio15 | E | TMC アドレス 3。未使用 |
| TMC2209 UART | RX gpio9、TX gpio8 | — | 4 ドライバで共有 |
| センサレス（DIAG） | X gpio4、Y gpio3、Z gpio25、E0 gpio16 | X/Y/Z/E0-STOP | DIAG ジャンパを挿すとその軸の外部エンドストップは使えない |
| ホストとの UART | TX gpio0、RX gpio1 | Pi 用 5 ピン（IO0、IO1、GND、5V、5V） | 5V も出ている |
| ペン用ソレノイド | gpio20 | Laser（12/24V、GND、IO20） | FAN3 と共有。FAN3 は使わない |
| サーボ（代替） | gpio29 | SERVOS（IO29、5V、GND） |  |
| 未使用 | FAN1 gpio17、FAN2 gpio18、HE gpio23、HB gpio21、TH0 gpio27、THB gpio26、PROBE gpio22、RGB gpio24 |  |  |

Laser 端子の IO20 は 3.3V ロジック信号と推定している（ボードのロジック電圧は 3.3V）。RP2040 に直結かバッファ経由かは、ピン配置図だけでは分からない。

**ペン用ソレノイドの駆動回路**

SKR Pico のレーザー出力は信号レベルなので、ソレノイドは外付けのトランジスタバッファで駆動する。

```
SKR Pico レーザー出力 ── R(ゲート) ── N-ch MOSFET(ローサイド)
                              │            │
                        R(プルダウン)   ソレノイド ── +V(12/24V)
                              │            │   └ 逆起電力吸収ダイオード
                             GND          GND
```

- トランジスタはロジックレベル駆動の N-ch MOSFET（または NPN）をローサイドに置く
- ソレノイドと並列に逆起電力吸収ダイオードを必ず入れる
- ゲートにプルダウン抵抗を入れ、MCU のリセット中や起動直後にソレノイドが動かないようにする
- 電源は SKR Pico の入力電源（12/24V）から取る。ソレノイドの電圧と電流でトランジスタを選ぶ
- 連続通電で発熱する場合は、吸引後に PWM で保持電流へ下げる（マクロで実現、第 6 章）
- レーザー出力は Laser 端子の IO20。同じ端子の 12/24V と GND をソレノイドの電源に使える

**CYD の GPIO 割当**

| 用途 | GPIO | 備考 |
| --- | --- | --- |
| TFT（HSPI） | 12, 13, 14, 15, 2 | バックライト 21 |
| タッチ（XPT2046） | 25, 32, 39, 33, 36 | ソフト SPI |
| SD（VSPI） | 18, 19, 23, 5 |  |
| RGB LED | 4, 16, 17 | 状態表示に使う |
| UART2 → SKR Pico | 27 (TX), 22 (RX) |  |
| 予備 | 35 | 入力専用。非常停止ボタン候補 |

CYD はロットにより配線が違う（バックライトや USB-C 版の表示 IC など）。上表は一般的な ESP32-2432S028R の割当で、実機で要確認。SKR Pico 側は上の表で確認済み。

**対象ボードの比較**

| 項目 | CYD（ESP32-2432S028R） | M5Stack CoreS3 |
| --- | --- | --- |
| SoC | ESP32（Xtensa LX6 ×2、240MHz） | ESP32-S3（Xtensa LX7 ×2、240MHz） |
| メモリ | SRAM 520KB、PSRAM なし、Flash 4MB | SRAM 512KB、PSRAM 8MB、Flash 16MB |
| 表示 | 2.8」 ILI9341 320×240 | 2.0」 IPS ILI9342C 320×240 |
| タッチ | XPT2046 抵抗膜（SPI） | FT6336U 静電容量（I2C） |
| SD | 専用 SPI（VSPI） | LCD と SPI バス共有 |
| 電源・IO | 直結 | AXP2101（PMIC）と AW9523B（IO エキスパンダ）の初期化が必要 |
| SKR Pico との接続 | UART2（CN1、GPIO27/22） | Port C（UART、GPIO17/18） |
| その他 | RGB LED、LDR | IMU、カメラ、マイク、スピーカー（本設計では不使用） |

**CoreS3 の配線**

| CoreS3 | 方向 | SKR Pico | 備考 |
| --- | --- | --- | --- |
| Port C TX（GPIO17） | → | GPIO1（UART0 RX） | Grove ケーブルを加工 |
| Port C RX（GPIO18） | ← | GPIO0（UART0 TX） |  |
| GND | — | GND |  |

両ボードとも SKR Pico とは UART で接続し、USB Host 接続は採用しない。

CoreS3 の Port C のピン番号と TX/RX の向きは M5Stack の公式資料で要確認。

## 3. ソフトウェアアーキテクチャ

Klippy の構造（リアクタ＋設定セクションごとのモジュール＋シリアルスレッド）を C++ と FreeRTOS で再現する。ボード依存部分は最下層の BSP に閉じ込め、CYD と CoreS3 はその層だけが異なる。

*（図「全体構成：ESP32 の 4 タスクと SKR Pico」は省略）*

Mainsail もローカル画面も、同じ klippy タスクに操作を渡し、同じ状態を読む。SKR Pico へは時刻付きの予約だけが流れる。

**レイヤ構成**

| 層 | 主な内容 | ボード依存 |
| --- | --- | --- |
| アプリ | ローカル UI（LVGL）、Moonraker 互換 API | なし（画面はどちらも 320×240） |
| Klippy コア | リアクタ、設定パーサ、G-code ディスパッチ、各モジュール（toolhead、stepper、servo…） | なし |
| chelper 移植 | serialqueue、trapq、itersolve、stepcompress、kin\_\* | なし |
| プラットフォーム | UART、ファイル（SD）、NVS、WiFi、HTTP/WebSocket、時刻 | なし（ESP-IDF 共通） |
| BSP | 表示、タッチ、SD のピン、UART のポート、電源、LED、メモリ上限値 | **あり** |

**BSP の実装**

- ボードはビルド時に選ぶ。`idf.py set-target esp32` と `sdkconfig.defaults.cyd`、`idf.py set-target esp32s3` と `sdkconfig.defaults.cores3` の組み合わせとする
- CoreS3 は Espressif の esp-bsp（`m5stack_core_s3`）を使い、AXP2101 と AW9523B の初期化も任せる
- CYD は esp\_lcd（ILI9341）と XPT2046 ドライバで自前の BSP を書く
- どちらも esp\_lvgl\_port 経由で LVGL に接続し、上位層には `bsp_display_*`、`bsp_touch_*`、`bsp_sd_mount`、`bsp_mcu_uart_open`、`bsp_led_set` だけを見せる
- メモリ上限（描画バッファ、WebSocket 接続数、G-code 先読みバッファなど）は BSP が定数として持ち、CoreS3 では PSRAM を使って大きくする

**FreeRTOS タスク**

| タスク | コア | 優先度 | 役割 |
| --- | --- | --- | --- |
| serialqueue | 1 | 最高 | UART 送受信、フレーム化、ack・再送、時刻指定コマンドの送出タイミング制御 |
| klippy（リアクタ） | 1 | 高 | G-code 実行、移動計画、ステップ生成、クロック同期、各モジュールのタイマ |
| web | 0 | 中 | HTTP、WebSocket、JSON-RPC（Moonraker 互換） |
| ui | 0 | 低 | LVGL の描画とタッチ処理 |
| WiFi / lwIP | 0 | （IDF 既定） |  |

コア 1 にリアルタイム寄りの処理、コア 0 に通信と UI を分け、WiFi の負荷がステップ生成を妨げないようにする。

**タスク間のやりとり**

- web と ui から klippy への操作（G-code 実行、印刷開始など）はコマンドキューで渡す
- klippy は各モジュールの `get_status()` を定期的に集めてスナップショットを作り、web と ui はそれを読むだけにする。変更差分が Moonraker の `notify_status_update` になる
- klippy の待ち処理（G4、M400、G28 など）は、Klippy の greenlet の代わりに C++20 コルーチンで書く。リアクタはタイマとコルーチンの再開を一つのループで処理する

**モジュール方式**

printer.cfg のセクション名ごとに C++ のモジュールを登録する（例：`[servo pen]` → `ServoModule`）。各モジュールは Klippy の Python モジュールと同じ役割を持つ。

- コンストラクタで設定値を読む（既定値と範囲チェックは Klipper と同じ）
- G-code コマンドを登録する
- MCU 接続時に `config_*` コマンドを組み立てる（build\_config）
- `klippy:connect`、`klippy:ready`、`klippy:shutdown` などのイベントを受ける
- `get_status()` で Moonraker 向けの状態オブジェクトを返す

**ディレクトリ構成（案）**

```
main/                 起動、タスク生成
components/
  bsp_cyd/            CYD 用 BSP
  bsp_cores3/         CoreS3 用 BSP（esp-bsp のラッパ）
  chelper/            Klipper chelper の移植（GPLv3）
  klippy/             リアクタ、設定、G-code、MCU、クロック同期
  klippy_modules/     toolhead, stepper, tmc2209, servo, gcode_macro …
  jinja_lite/         Jinja2 サブセット
  moonraker/          HTTP / WebSocket / JSON-RPC
  ui/                 LVGL 画面
```

## 4. MCU 通信層

Klipper のホストと MCU の間のプロトコルをそのまま実装する。MCU 側は純正ファームなので、仕様の正解は Klipper の `docs/Protocol.md` と `klippy/chelper/serialqueue.c`、`klippy/mcu.py`、`klippy/clocksync.py` とする。

**フレーム層**

- フレーム = 長さ（1 バイト）＋シーケンス（1 バイト）＋ペイロード＋CRC16（2 バイト）＋同期バイト 0x7E、最大 64 バイト
- ペイロードはコマンド ID と引数を VLQ で符号化して並べる
- シーケンス番号で ack を受け、未応答なら再送する。MCU の受信ウィンドウ（データ辞書の `RECEIVE_WINDOW`）を超えて送らない

**serialqueue の移植**

- 時刻指定コマンド（`queue_step` など）は `min_clock` と `req_clock` を持ち、MCU のバッファに入る時刻になるまで待って送る。このロジックは `serialqueue.c` をそのまま使う
- pthread と pipe/poll による待ちを、FreeRTOS のミューテックスと UART ドライバのイベントキュー、タスク通知に置き換える
- ホスト時刻は `esp_timer_get_time()`（μs、単調増加）を秒に換算して使う
- ボーレートは既定 250000。MCU 側ビルドと合わせれば上げられるが、ペンプロッタでは不要

**実装結果（M2）**

- serialqueue.c と pollreactor.c は本家のまま使う。ESP-IDF には pipe() がないので、背景スレッドを起こすパイプを eventfd に置き換えた（強制インクルードのヘッダで pipe・read・write・close をすり替え、UART の fd はそのまま通す）。UART は VFS 経由で開くので poll() で待てる
- ESP-IDF の UART は既定でテキスト用の改行変換（受信 CR→LF、送信 LF→CRLF）を行い、バイナリのフレームが壊れる。接続中は変換を止める
- serialqueue はデバッグ用に送受信 100 件ずつのメッセージを保持し、約 22KB を使う。本家の serialqueue.c を別ファイルから #include し、確保直後に 8 件ずつに削る
- データ辞書の展開は ESP32 の ROM の miniz を使う。作業領域（約 11KB）はスタックではなくヒープに確保する
- コマンドの書式はデータ辞書と完全一致で照合する（本家の lookup\_command と同じ）

**接続手順**

1. `identify` でデータ辞書を少しずつ読み出し、zlib を展開する（ESP32 ROM の miniz を使用）
2. JSON を解析し、コマンド・応答の書式、ピン名の列挙、`CLOCK_FREQ` などの定数を取り出す。解析後は JSON を破棄し、必要な表だけ残す
3. クロック同期を開始する
4. `get_config` で状態を確認する。未設定なら各モジュールの `config_*` を送り、`finalize_config` に設定内容の CRC を渡す
5. 設定済みで CRC が一致しなければ MCU をリセット（`restart_method: command`）して 1 からやり直す
6. 全モジュールに `klippy:ready` を通知し、G-code を受け付ける

**クロック同期**

- 約 1 秒ごとに `get_clock` を送り、送信時刻と応答のクロック値から、ホスト時刻と MCU クロックの関係（オフセットと周波数）を減衰付きの線形回帰で推定する（`clocksync.py` の移植）
- MCU の 32 ビットクロックを 64 ビットに拡張して扱う。RP2040 は 12MHz なので 32 ビットは約 6 分で一周する
- 移動計画は「print\_time」（MCU クロック基準の秒）で行い、送信直前にクロック値に変換する

**異常検知**

- 一定時間 MCU から応答がなければ「通信喪失」としてシャットダウンする
- MCU からの `shutdown` や `is_shutdown` 応答（例：Timer too close）は理由を保持し、UI と API に表示する。復帰は `FIRMWARE_RESTART`

**MCU ファームウェア（SKR Pico）**

SKR Pico には Klipper 本家の MCU ファームを無改造で書き込む。Raspberry Pi との接続と違うのは、通信インターフェースの選択だけ。

| 項目（make menuconfig） | 設定値 |
| --- | --- |
| Micro-controller Architecture | Raspberry Pi RP2040 |
| Bootloader offset | No bootloader（Katapult を使う場合は 16KiB） |
| Communication interface | UART0 の GPIO1/GPIO0 |
| Baud rate | 250000（ホストの printer.cfg の `baud` と一致させる） |

- **ビルド**：arm-none-eabi-gcc が入った Linux、macOS、WSL または Docker で `make` する。Raspberry Pi は不要
- **初回の書き込み**：SKR Pico の BOOT ボタンを押しながらリセットし、PC に現れる RPI-RP2 ドライブへ `klipper.uf2` をコピーする
- **対応バージョン**：MCU コマンドの引数は Klipper の版で変わることがある（特に原点復帰まわり）。Klipper の最新版に追従する。接続時にデータ辞書のコマンド書式を検証し、未対応の変更があれば内容を示して停止する。Klipper の更新ごとに最新の MCU ファームで接続テストを行う
- **将来案：本体からの更新**：RP2040 の ROM ブートローダは USB しか使えない。Katapult（旧 CanBoot）を UART 設定で入れておけば、SD 上の `klipper.bin` を CYD・CoreS3 から UART 経由で書き込める

## 5. 運動制御

移動計画からステップ生成までは Klipper と同じパイプラインをそのまま再現する。G-code の最適化はしないが、先読みによる角の速度計算は加減速の一部なので実装する。

**パイプライン**

1. **gcode\_move**：G0/G1/G2/G3、G90/G91、G92、`SET_GCODE_OFFSET` を処理し、ツール座標の移動に変換する
2. **toolhead**：移動ごとに頂点速度（`square_corner_velocity` から求めるジャンクション偏差）を計算し、先読みキューで台形加速を確定する（`toolhead.py` の移植）
3. **trapq**：確定した移動を「開始時刻・加速・定速・減速」の区間として積む
4. **itersolve＋kin\_cartesian / kin\_corexy**：各ステッパの位置を時間の関数として解き、ステップ時刻を生成する
5. **stepcompress**：ステップ時刻列を `queue_step`（interval、count、add）に圧縮し、serialqueue に渡す

3〜5 は chelper の C コードをほぼそのまま使う。1・2 は Python から C++ に書き直す。

**バッファリング**

- Klipper のバッファは「時間」で量が決まるため、使うメモリは 1 秒あたりの移動数に比例する。本家の既定値のままでは CYD に収まらない（M1 で実測。card\_opt.gcode で最大約 1.7MB、大半は 30 秒分の動作履歴）。ESP 版は次の値に縮める

  | 項目 | Klipper | CYD（PSRAM なし） | CoreS3（PSRAM あり） |
  | --- | --- | --- | --- |
  | MCU に対する先行時間（buffer\_time\_high） | 1.0 s | 0.4 s | 1.0 s |
  | ステップ生成の先行（BGFLUSH\_SG\_LOW/HIGH） | 0.45〜0.7 s | 0.10〜0.20 s | 0.45〜0.70 s |
  | 動作履歴（MOVE\_HISTORY\_EXPIRE） | 30 s | 0.25 s | 2.0 s |
  | 先読みキューの上限 | なし | 32 移動 | 32 移動 |
- 動作履歴は G-code のバッファではなく、実行済みの動作の記録（原点復帰時の位置の逆算、motion\_report に使う）。0.25 s でセンサレスホーミングの位置逆算に足りるかは M4 で確認する
- 先読みキューの上限は、上限に達したら Klipper の遅延フラッシュ（lazy flush）を早めに試すだけで、計画のアルゴリズムは変えない。M1 では上限 64 でも出力ハッシュが上限なしと一致した（動作は完全に同じ）
- これらの値で card\_opt.gcode を 1〜5 倍速で流し、先読み切れ（runout）は 0 回、動作時間は本家の値と同じだった
- G-code ファイルの読み込みが遅れたら、先読みを打ち切って減速停止する（Klipper の「バッファ枯渇」と同じ動作）
- CoreS3 で CYD の値を使うと、ステップが実行の約 50ms 前にしか MCU に届かず、WiFi や USB の揺らぎで `Timer too close` になった（M5）。PSRAM のある基板は Klipper 本来の値に戻した

**負荷の目安**

既存機 Fraxinus0pen の設定（`rotation_distance: 47.5`、`microsteps: 16`）では約 67 ステップ/mm になる。最高速度 500mm/s では 1 モータあたり約 33,700 ステップ/秒、`TEST_PRINT` の F12000（200mm/s）では約 13,500 ステップ/秒。CoreXY では XY 移動で 2 モータが同時に回る。

ただし実際の描画（card\_opt.gcode）は描画 50mm/s、空送り 100mm/s なので、必要なのは 1 モータあたり約 3,400〜6,700 ステップ/秒と、上限の 1/5 程度。M1 の性能測定はこの実負荷を必須の合格条件とし、500mm/s は参考値として測る。

**M1 の実測結果（CYD、2026-11-12）**

card\_opt.gcode（動作時間 130 秒）を ESP 版のバッファ設定で流した結果。出力ハッシュは全条件で Mac 版と一致し、計算結果は同じ。A はステップ生成スレッドを両コアで動かした場合、B はコア 1 に固定した場合（コア 0 を WiFi と UI に空ける設計どおり）。

| 条件 | A：実時間比 / 最悪負荷 | B：実時間比 / 最悪負荷 | UART 使用率 |
| --- | --- | --- | --- |
| 等速・Input Shaper なし | 10.95x / 12.5% | 6.96x / 20.7% | 12.8% |
| 等速・ZV（既存機の設定） | 4.59x / 34.3% | 2.77x / 57.7% | 16.0% |
| 2 倍速・ZV | 2.81x / 63.9% | 1.69x / 91.2% | 21.3% |
| 5 倍速・ZV | 2.40x / 111.6% | 1.44x / 168.8% | 23.0% |

実時間比は動作時間 ÷ 計算時間、最悪負荷はステップ生成 1 回の計算時間 ÷ その回が受け持つ動作時間の最大値。既存機の設定（等速・ZV）は B でも平均 2.8 倍速く、実用上は間に合う。事前に決めた基準（B で 3 倍以上・負荷 50% 未満）にはわずかに届かないので、Jinja2 マクロと WiFi を載せた M3 で再測定して判断する。2 倍速までは動作するが、5 倍速は 1 回の負荷が 100% を超え、先行時間を食い潰す恐れがあるため対象外とする。

**最大の懸念は倍精度演算**。chelper は時刻と位置を double で計算するが、ESP32 と ESP32-S3 の FPU は単精度のみで、double はソフトウェア演算になる。そこで M1 の段階で、MCU をつながずに ESP32 上で itersolve・stepcompress だけを走らせ、ステップ/秒の上限を実測する。足りない場合の対策は次の順。

1. 移動内の計算を「移動開始からの相対時刻」にして単精度化する（絶対時刻と累積位置は double のまま）
2. マイクロステップを下げる、または `max_velocity` を下げる（printer.cfg 側の対応）

M1 の結果、倍精度のままで必要な速度が出たため、どちらの対策も不要になった（上の実測結果）。chelper は無改造で使う。

**Input Shaper**

- `[input_shaper]`（`#*#` ブロックの値を含む）と `SET_INPUT_SHAPER` に対応する。Klipper の `kin_shaper.c` を移植し、各ステッパの位置計算に畳み込む
- 計算量は zv で約 2 倍、ei 系で 3〜5 倍になる。上記の性能測定では Input Shaper ありの条件も測る
- 共振の測定（`TEST_RESONANCES`、`SHAPER_CALIBRATE`）は対応しない。Raspberry Pi 上の Klipper で測った値を使う（既存機の X zv 83.2Hz、Y zv 58.6Hz はそのまま使える）

**ペンの同期**

- `SET_SERVO` や `SET_PIN` は toolhead の先読みコールバックで、直前の移動が終わる print\_time に合わせて予約する（Klipper の `servo.py` と同じ）。これでペンの昇降と XY 移動がずれない
- サーボが動き終わるまでの待ちはマクロ内の `G4` で入れる

ペン昇降の駆動は 2 方式に対応し、どちらも上記の仕組みで XY 移動と同期する。

| 方式 | printer.cfg | 制御コマンド | 待ち時間 |
| --- | --- | --- | --- |
| ソレノイド（標準） | `[output_pin]`（レーザー出力のピン） | `SET_PIN` | 吸引時間。実測してマクロ変数で調整 |
| サーボ（代替） | `[servo]` | `SET_SERVO` | 100〜200ms 程度 |

ソレノイドは ON/OFF のみ。発熱対策で保持電流を下げる場合も、`SET_PIN` の値をマクロ内で 2 段階に切り替えるだけで、ホストに専用機能は作らない。

**原点復帰（G28）**

1. `config_endstop` と `config_trsync` で、TMC2209 の DIAG につながるピンをエンドストップとして設定しておく
2. 原点復帰の直前に TMC2209 のレジスタを書き換え、StallGuard を有効にする（`TCOOLTHRS` を最大にし、stealthChop で動かす）
3. `trsync_start` と `endstop_home` で検知を開始し、機械端へ少しずつ移動を送る（drip 移動）
4. 脱調で DIAG が立ち、MCU がステッパを止めたら、`stepper_get_position` で位置を読み、座標を確定する
5. レジスタを通常動作の値に戻す

**センサレスホーミング（必須）**

- Klipper の `tmc.py` にある仮想エンドストップ（`tmc2209_stepper_x:virtual_endstop`）を移植し、上記 2 と 5 のレジスタ切り替えを原点復帰の前後に自動で行う
- SKR Pico はジャンパで各ドライバの DIAG をエンドストップ入力につなぐ。`diag_pin` はその入力ピンを指定する
- CoreXY では X と Y のモータが両方動くので、X と Y を 1 軸ずつ原点復帰し、その間に少し待つ。電流を下げて衝撃を抑える処理も含め、Klipper と同じく `[homing_override]` マクロで書く（第 6 章）
- 二度当てはしない（`homing_retract_dist: 0`）
- 感度（`driver_SGTHRS`）は機体ごとに調整が必要。`SET_TMC_FIELD` で変えながら試し、決まった値を printer.cfg に書く
- センサレスには printer.cfg の Jinja マクロと TMC のレジスタ操作が必要なので、M4 で実装する。それまでは `SET_KINEMATIC_POSITION` で手動原点を使う

**ステッパの励磁**

- `enable_pin` は移動開始時に自動で ON、`M84` と `idle_timeout` で OFF
- TMC2209 は MCU の `tmcuart_send` でレジスタを書き、`run_current`、`hold_current`、マイクロステップ、stealthChop を設定する（詳細は第 6 章）

## 6. printer.cfg 互換

Klipper の printer.cfg をそのまま読めるようにする。対応したセクションは Klipper と同じオプション名・既定値・検証で動かし、未対応のものは理由を示してエラーにする。

**互換レベル**

| レベル | 内容 | 初版 |
| --- | --- | --- |
| L1 書式 | INI 形式、複数行の値、`#` と `;` のコメント、`[include *.cfg]`、末尾の `#*#` SAVE\_CONFIG ブロック | 対応 |
| L2 意味 | 対応セクションのオプション名・既定値・範囲チェック。未使用オプションはエラー | 対応 |
| L3 マクロ | `[gcode_macro]` と `[delayed_gcode]` の Jinja2 | サブセット（第 7 章） |
| L4 運用 | Mainsail での編集、`SAVE_CONFIG`、`RESTART`、`FIRMWARE_RESTART` での再読み込み | 対応 |

**対応セクション**

| 優先度 | セクション | 状況（2026-11-13） |
| --- | --- | --- |
| 必須 | `[mcu]`、`[printer]`（corexy 先行、cartesian は後）、`[stepper_x]`・`[stepper_y]`・`[stepper_z]`、`[tmc2209 stepper_*]`（`diag_pin`、`driver_SGTHRS` を含む）、`[homing_override]`、`[virtual_sdcard]`、`[input_shaper]` | 実装済み（corexy、cartesian とも） |
| ペン用 | `[pwm_tool]`（ソレノイド、既存機で使用）、`[output_pin]`、`[servo]`（代替）、`[gcode_macro]` | `[servo]` 以外は実装済み。`[servo]` は未実装 |
| Mainsail 連携 | `[pause_resume]`、`[display_status]`、`[respond]`、`[idle_timeout]` | `[pause_resume]` は実装済み。`display_status`・`idle_timeout` は状態オブジェクトだけ返す。`[respond]` は未実装 |
| 既存機で使用 | `[gcode_arcs]`、`[neopixel]` | 未実装（警告して読み飛ばす） |
| あると便利 | `[fan]`、`[force_move]`、`[board_pins]`、`[static_digital_output]`、`[delayed_gcode]`、`[save_variables]` | 未実装（警告して読み飛ばす） |
| 未対応 | `[extruder]`、`[heater_bed]`、`[bed_mesh]`、加速度センサー（`[adxl345]`、`[lis2dw]`）、`[mcu host]`、`[display]`、`[menu]` など | 未対応。実装では、知らないセクションはエラーにせず「not supported yet」と警告して読み飛ばす。`[include]` も未対応 |

**パーサ仕様**

- Python の configparser（Klipper の使い方）に合わせる：オプション名は小文字化、`:` と `=` の両方、インデントされた行は前の値の続き、値の後ろのコメントは削除
- 同じセクションが複数あれば後の値で上書きし、`#*#` ブロックの内容が最後に優先される
- エラーは「ファイル名・セクション・オプション」を示して、UI と Mainsail に出す
- ファイルは SD の `/sd/config/printer.cfg`。Mainsail の設定エディタからは `config` ルートとして見せる

**ホスト固有の解釈**

- `[mcu] serial:` に `/dev/...` を書いた場合は BSP の既定 UART（CYD は UART2、CoreS3 は Port C）と解釈し、`baud` はそのまま使う。明示したい場合は `serial: uart2` も受け付ける
- ピン名（`!gpio10`、`^gpio4`、`mcu:gpio4`、`tmc2209_stepper_x:virtual_endstop`）は Klipper と同じ書式で解釈し、MCU のデータ辞書のピン一覧で存在を確認する
- cartesian / corexy は Klipper と同じく Z 軸を必須とする（SKR Pico の Z ドライバを割り当てる）

**設定例（抜粋）**

```ini
[mcu]
serial: /dev/ttyAMA0
baud: 250000
restart_method: command

[printer]
kinematics: corexy
max_velocity: 300
max_accel: 3000
square_corner_velocity: 5.0

# --- XY：センサレスホーミング ---
[stepper_x]
step_pin: gpio11
dir_pin: !gpio10
enable_pin: !gpio12
microsteps: 16
rotation_distance: 40
endstop_pin: tmc2209_stepper_x:virtual_endstop
position_endstop: 0
position_max: 300
homing_speed: 20
homing_retract_dist: 0

[tmc2209 stepper_x]
uart_pin: gpio9
tx_pin: gpio8
uart_address: 0
run_current: 0.6
diag_pin: ^gpio4          # DIAG ジャンパで X-STOP に接続
driver_SGTHRS: 100        # 機体ごとに調整

# stepper_y：step gpio6、dir !gpio5、enable !gpio7、uart_address 2、diag_pin ^gpio3
# stepper_z は Klipper の仕様上必要。未使用なら空きドライバに割り当てる

[homing_override]
axes: xy
set_position_z: 0
gcode:
  {% set all = 'X' not in params and 'Y' not in params %}
  {% if all or 'X' in params %}
    _HOME_AXIS AXIS=X
  {% endif %}
  {% if all or 'Y' in params %}
    _HOME_AXIS AXIS=Y
  {% endif %}

[gcode_macro _HOME_AXIS]
gcode:
  {% set a = params.AXIS %}
  {% set s = "stepper_" ~ a|lower %}
  {% set cur = printer.configfile.settings["tmc2209 " ~ s].run_current %}
  SET_TMC_CURRENT STEPPER={s} CURRENT={cur * 0.7}
  G4 P200
  G28 {a}
  G91
  G1 {a}10 F1200
  G90
  SET_TMC_CURRENT STEPPER={s} CURRENT={cur}
  G4 P500

# --- ペン昇降：レーザー出力 → トランジスタバッファ → ソレノイド ---
[output_pin pen]
pin: gpio20               # Laser 端子（FAN3 と共有）
pwm: True                 # 保持電流を下げないなら False でもよい
cycle_time: 0.0002
value: 0
shutdown_value: 0

#[servo pen_servo]       # サーボを使う場合
#pin: gpio29

[gcode_macro _PEN]
variable_mode: "solenoid" # "solenoid" または "servo"
variable_pull_ms: 30      # 吸引にかかる時間
variable_release_ms: 30   # 解放にかかる時間
variable_hold: 1.0        # 保持時の出力（1.0 = 下げない）
variable_up_angle: 90
variable_down_angle: 30
gcode:

[gcode_macro PEN_DOWN]
gcode:
  {% set c = printer["gcode_macro _PEN"] %}
  {% if c.mode == "solenoid" %}
    SET_PIN PIN=pen VALUE=1
    G4 P{c.pull_ms}
    {% if c.hold < 1.0 %}
      SET_PIN PIN=pen VALUE={c.hold}
    {% endif %}
  {% else %}
    SET_SERVO SERVO=pen_servo ANGLE={c.down_angle}
    G4 P150
  {% endif %}

[gcode_macro PEN_UP]
gcode:
  {% set c = printer["gcode_macro _PEN"] %}
  {% if c.mode == "solenoid" %}
    SET_PIN PIN=pen VALUE=0
    G4 P{c.release_ms}
  {% else %}
    SET_SERVO SERVO=pen_servo ANGLE={c.up_angle}
    G4 P150
  {% endif %}

# レーザー向けツールの G-code もそのまま描けるようにする
[gcode_macro M3]
gcode: PEN_DOWN

[gcode_macro M4]
gcode: PEN_DOWN

[gcode_macro M5]
gcode: PEN_UP
```

ソレノイドが通電でペンを上げる機構の場合は、`PEN_UP` と `PEN_DOWN` の中身を入れ替える。

**合格ライン：既存機 Fraxinus0pen の printer.cfg**

SKR Pico と Raspberry Pi で動いている既存機の printer.cfg を、無改造で読み込んで同じように動くことを合格ラインとする。中で使われている機能と扱いは次のとおり。

| 使われている機能 | 扱い |
| --- | --- |
| `[pwm_tool LASER]`（gpio20）と `SET_PIN PIN=LASER` | 対応。ペン用ソレノイドの駆動 |
| `M3`、`M5`、`G00`〜`G04`、`M03`、`M05` のマクロ | 対応。`G00` などゼロ付きの名前もマクロ名として登録する |
| Jinja2 の `params` の `for` 反復、`'%s%s' % (p, params[p])`、`range()` | 対応（第 7 章） |
| `rename_existing`（`PAUSE`、`RESUME`、`CANCEL_PRINT`） | 対応 |
| `[homing_override]` が 2 回定義され、`set_position_x/y/z` と `axes`、`gcode` が分かれている | Klipper と同じく同名セクションを結合し、後の値で上書きする。`[display_status]` の重複も同じ |
| `[stepper_z]`（物理エンドストップ `^!gpio25`） | 対応。物理エンドストップの原点復帰も実装する |
| `[neopixel]`（gpio24、12 個） | 対応（MCU の neopixel コマンドを使う） |
| `#*#` ブロックの `[input_shaper]` | 対応（第 5 章） |
| `[mcu] serial: /dev/serial0` | BSP の既定 UART と解釈する |
| `virtual_sdcard` の `path: /home/<ユーザー>/...` | SD の `gcodes` に置き換え、警告を出す |

同じ考え方で、Micro4 で動いている Pochitron の printer.cfg も互換性の確認に使う。こちらはボードが違うので、読み込みと解釈の試験だけに使う（`[mcu host]`、`[display]`、`[menu]` は警告して無視）。

SKR Pico のピン番号は Klipper 同梱の `config/generic-bigtreetech-skr-pico-v1.0.cfg` と照合すること。

## 7. G-code とマクロ

G-code の構文と拡張コマンド（`SET_SERVO SERVO=pen ANGLE=30` 形式）は Klipper と同じにする。コマンドは各モジュールが登録し、未知のコマンドは Klipper と同じ「Unknown command」を返す。

**対応コマンド（初版）**

| 分類 | コマンド | 状況（2026-11-13） |
| --- | --- | --- |
| 移動 | G0、G1、G2、G3（`[gcode_arcs]`）、G4、G28、G90、G91、G92、M400 | G2・G3 は未実装。ほかは実装済み（G20/G21、M82/M83 も） |
| 状態 | M114、M115、`GET_POSITION`、`STATUS`、`HELP` | M114・`GET_POSITION` は実装済み（M119・`QUERY_ENDSTOPS` も）。M115・`STATUS`・`HELP` は未実装 |
| 速度 | M204、M220、`SET_VELOCITY_LIMIT` | 実装済み |
| ステッパ | M17、M18、M84、`SET_KINEMATIC_POSITION`、`FORCE_MOVE`、`SET_STEPPER_ENABLE`、`SET_TMC_CURRENT`、`SET_TMC_FIELD`、`DUMP_TMC` | M18・M84・`SET_KINEMATIC_POSITION`・`SET_TMC_CURRENT`・`SET_TMC_FIELD`・`DUMP_TMC` は実装済み（`INIT_TMC` も）。M17・`FORCE_MOVE`・`SET_STEPPER_ENABLE` は未実装 |
| ペン・出力 | `SET_SERVO`、`SET_PIN` | `SET_PIN` は実装済み。`SET_SERVO` は未実装 |
| ファイル実行 | M20、M23、M24、M25、M27、`SDCARD_PRINT_FILE`、`SDCARD_RESET_FILE`、`PAUSE`、`RESUME`、`CANCEL_PRINT`（マクロで上書き可） | 実装済み（M21・M26・`CLEAR_PAUSE`・`SET_PRINT_STATS_INFO`・`SAVE_GCODE_STATE`・`RESTORE_GCODE_STATE` も） |
| システム | M112、`RESTART`、`FIRMWARE_RESTART`、`SAVE_CONFIG`、`RESPOND`、`SET_GCODE_VARIABLE`、`SAVE_VARIABLE` | M112・`SET_GCODE_VARIABLE` は実装済み。再起動は Moonraker API（`printer.restart`、`printer.firmware_restart`）と画面の Restart からで、G-code の `RESTART`・`FIRMWARE_RESTART` は未実装。`SAVE_CONFIG`・`RESPOND`・`SAVE_VARIABLE` は未実装 |

ペン昇降用の組み込みコマンドは作らず、すべてマクロで制御する。同梱のサンプル設定で `PEN_UP` と `PEN_DOWN` を定義し、レーザー向けツールが出す `M3`、`M4`、`M5` もそれにつなぐ（第 6 章の設定例）。ソレノイドとサーボの切り替えはマクロ変数 1 つで行う。

**Jinja2 サブセット（jinja\_lite）**

Jinja2 全体は重すぎるので、マクロで実際に使われる範囲を C++ で実装する。テンプレートは設定読み込み時に構文木へ変換し、実行時はそれを評価する。

| 対応 | 内容 |
| --- | --- |
| 構文 | `{{ }}`、`{% if/elif/else %}`、`{% for %}`、`{% set %}`、`{# #}` |
| 式 | 数値、文字列、リスト、辞書、四則演算、`//`、`%`、比較、`and`/`or`/`not`、`in`、`~`、三項演算 `x if c else y` |
| フィルタ | `int`、`float`、`string`、`default`、`round`、`abs`、`lower`、`upper`、`length`、`join` |
| 参照 | `params.X`、`rawparams`、`printer.<オブジェクト>.<フィールド>`、`printer["gcode_macro FOO"].var`、`variable_*` |
| 組み込み | `action_respond_info`、`action_raise_error`、`action_emergency_stop` |
| 未対応 | `{% macro %}`、`{% include %}`、継承、任意の Python メソッド呼び出し（よく使う `.get()`、`.lower()`、`.split()` などは個別対応） |

- `printer` 参照は各モジュールの `get_status()` をそのまま使うので、Moonraker の状態オブジェクトと中身が揃う
- `variable_*` は Klipper と同じく Python リテラルとして解釈する（数値、文字列、リスト、辞書、True/False/None）
- 合格ライン：Mainsail が提供する `mainsail.cfg`（PAUSE / RESUME / CANCEL\_PRINT）を無改造で読み込んで動くこと。動かない構文があれば個別に対応を足す

既存機の printer.cfg で使われている次の構文も必須とする。

- `{% for p in params %}`（辞書のキーでの反復）と `params[p]`
- `'%s%s' % (p, params[p])`（Python の `%` による文字列書式、タプル）
- `range(98)` とループ内の算術（`X{i * 0.5}`）
- `'X' not in params`

**入力 G-code の例：Inkscape Lasertools の出力**

既存機で描いている G-code（`03.ｽﾀｯｸﾁｬﾝ.gcode`、1,421 行、35KB）を解析した。補助の試験に使う（本命は次の card\_opt.gcode）。

| 項目 | 内容 | 設計への影響 |
| --- | --- | --- |
| コマンド | `G01` 1,362 行、`G00` 15、`M3` 14、`M5` 17、`M03 S1` 1、`G28`、`G90`、`G21` | ほぼ全行がゼロ付きの `G01`/`G00` で、printer.cfg のマクロを通って `G1` に変換される。マクロ展開の速さが描画速度に直結する |
| ペンの上下 | `M3` で下げ、`M5` で上げる。15 ストローク | プレビューの判定は `M3`/`M03`/`M4` と `M5`/`M05` を既定にする |
| 線分の長さ | 中央値 1.0mm、下位 10% は 0.19mm 以下、最大 5.0mm | 短い線分が連続するので、先読みと角の速度計算が描画時間を決める |
| 速度 | 全行 F9000（150mm/s） | 1mm の線分を 150mm/s で流すと、毎秒最大 150 行のマクロ展開と移動計画が必要 |
| 描画範囲 | X 14.4〜86.6mm、Y 22.9〜92.9mm。描画 1,237mm、空送り 283mm | Fraxinus0pen の可動範囲（110×160mm）に収まる |
| 書式 | タブ区切りの行末コメント（`G90\t;Absolute programming`）、空行、`F` だけの `G01` | G-code パーサはタブと `;` コメントを Klipper と同じに扱う |
| ファイル名 | 半角カナを含む | SD と UI で日本語ファイル名を扱う（第 9・10 章） |

マクロは設定読み込み時に構文木へ変換済みなので、実行時は評価と結果の G-code の再解析だけで済む。それでも 1 行ごとのコストは通常の `G1` より大きいので、M1 の性能測定に「`G01` マクロを毎秒 150 行以上処理できること」を加える。

**本命の G-code：card\_opt.gcode**

実際に描くカードの G-code（9,317 行、181KB）。経路の最適化と 97.5% の縮小は作成側で済んでいる。**M4 の受け入れ試験はこのファイルを Fraxinus0pen の printer.cfg で描き切ること**とする。

| 項目 | 内容 | 設計への影響 |
| --- | --- | --- |
| ペンの上下 | 875 ストローク。`M3` 874 回、`M5` 876 回。ストローク長は中央値 2.05mm | ソレノイドの切り替えが 1 枚で約 1,750 回。`[pwm_tool]` を Klipper どおりに実装し、切り替えを移動と同じ時刻軸で MCU に予約することが必須。`M3` と `M5` のマクロ内の `G4`（計 12ms）だけで 1 枚あたり約 10 秒 |
| 描画の線分 | `G01` 6,682 行。中央値 0.41mm、下位 10% は 0.10mm 以下 | 非常に短い線分が続く。F3000 でも毎秒約 120 行のマクロ展開と移動計画が必要 |
| 空送り | `G00` 874 行。中央値 0.69mm、最大 50mm | ほとんどが短い移動で、加減速で時間が決まる |
| 速度 | 描画 F2000、F3000（50mm/s）、空送り F6000（100mm/s） | 実際のステップレートは 1 モータあたり描画で約 3,400、空送りで約 6,700 ステップ/秒（第 5 章） |
| 描画範囲 | X 0.7〜78.7mm、Y 3.0〜53.4mm。描画 3,690mm、空送り 1,238mm | 移動の純粋な所要時間は概算で 2.5 分前後。先読みの出来で実時間が変わる |
| 終了処理 | 末尾の `M02` | printer.cfg の `M02` マクロ（Y150 へ移動してペンを上下し、`M18`）をそのまま実行する |

試験では、既存機（Raspberry Pi 上の Klipper）で描いたカードと並べて比較し、所要時間も記録する。

## 8. Moonraker 互換 API と Mainsail 連携

Mainsail は Moonraker の HTTP と WebSocket（JSON-RPC 2.0）だけを使うので、そのサブセットを ESP32 上に実装する。Moonraker と Klippy の間のソケットは作らず、API 層が klippy のスナップショットとコマンドキューを直接使う。

**Mainsail の配信**

| ボード | 配信元 | 備考 |
| --- | --- | --- |
| CYD | SD の `/sd/www/`（gzip 済みファイルをそのまま送る） | Flash 4MB には入れない |
| CoreS3 | Flash の専用パーティション（LittleFS）または SD | Flash 16MB に余裕がある |

PC 側で開いた Mainsail から接続する使い方も許す（CORS を許可）。

**実装する API**

| 分類 | メソッド | 実装 |
| --- | --- | --- |
| 接続 | `server.info`、`server.connection.identify`、`printer.info`、`server.config` | 実装 |
| 状態 | `printer.objects.list`、`printer.objects.query`、`printer.objects.subscribe`、`notify_status_update` | 実装 |
| 操作 | `printer.gcode.script`、`printer.gcode.help`、`printer.print.start/pause/resume/cancel`、`printer.emergency_stop`、`printer.restart`、`printer.firmware_restart`、`notify_gcode_response` | 実装 |
| ファイル | `server.files.list`、`get_directory`、`metadata`、`roots`、`post_directory`、`delete_directory`、`move`、`copy`、`delete_file`、HTTP のアップロードとダウンロード、`notify_filelist_changed` | 実装（`gcodes` と `config` ルート） |
| データベース | `server.database.list/get_item/post_item/delete_item` | SD 上の JSON ファイルで実装。Mainsail の UI 設定が入る |
| 履歴 | `server.history.list/totals` | 簡易実装（直近 N 件） |
| 空応答 | `server.temperature_store`、`server.gcode_store`、`server.webcams.list`、`machine.update.status`、`server.announcements.list`、`server.job_queue.status` | 空の結果を返す |
| システム | `machine.system_info`、`machine.proc_stats` | ESP32 の情報（ヒープ、タスク、WiFi）を詰める |

**状態オブジェクト**

`webhooks`、`configfile`、`toolhead`、`gcode_move`、`print_stats`、`virtual_sdcard`、`pause_resume`、`display_status`、`idle_timeout`、`motion_report`、`system_stats`、`stepper_enable`、`servo <name>`、`output_pin <name>`、`gcode_macro <name>`、`tmc2209 <name>` を提供する。中身のフィールド名は Klipper の Status\_Reference に合わせる。

**制約と対策**

- 同時接続数は CYD で 2、CoreS3 で 4 を上限とする（BSP 定数）
- JSON は書き出しをストリーム化し、大きな応答（`configfile`、ファイル一覧）も全体をメモリに置かない
- `notify_status_update` は 250ms 間隔で変更差分のみ送る（Moonraker と同じ考え方）
- Mainsail が要求する Moonraker の最低バージョンを `server.info` で満たすように応答し、Mainsail の最新版に追従し、リリースごとに必要な API の差分を確認する
- 認証は初版ではなし（ローカルネットワーク前提）。後で API キー方式を足す
- G-code ビューアは押出量（E）で描画を判定するため、E のないペンプロッタの G-code の表示は実機で確認する

**実装結果（M5、CoreS3）**

CoreS3 上で Mainsail の配信と Moonraker 互換 API が動いた（仮想 MCU 相手、2026-11-13）。再起動 6 回すべてで、起動時の原点復帰と自動印刷が完了し、`moonraker_check.py` の失敗 0、その後 1 分間に square.gcode を 11 回続けて印刷しても停止しなかった。途中で見つけて直した問題は次の 5 つ。

| 症状 | 原因 | 対処 |
| --- | --- | --- |
| 起動を繰り返し画面が出ない | PSRAM の設定が Octal（CoreS3 は Quad） | Quad に変更 |
| Mainsail の接続で落ちる | HTTP サーバーのスタック 8KB 不足 | 16KB に変更 |
| 状態の問い合わせ中に落ちる | Moonraker が klippy への依頼を待ちきれず戻った後、klippy が消えた変数を使った | 時間内に始まらなかった依頼は実行しない |
| 印刷中に `Timer too close` | CYD 向けに縮めた先行時間（ステップ先行約 50ms） | PSRAM のある基板は Klipper 本来の値（第 5 章） |
| 起動時の原点復帰で `Timer too close` | 待ち時間（pause）の間にステップを送っていなかった | 本家と同じく待つ間もフラッシュする |

- MCU が USB を使う間はコンソールを止めるため、ログを PSRAM のリング（128KB）に溜め、`/server/files/klippy.log` で読めるようにした
- MCU 停止時は本家と同じく直近 100 件の送受信を記録する（CYD は 8 件）

**WiFi の設定（CoreS3）**

接続先は NVS に保存し、無ければビルド時の値（menuconfig）を使う。次の 3 通りで変えられる（2026-11-13）。

| 方法 | 手順 | 確認 |
| --- | --- | --- |
| 画面 | System タブ → WiFi → 一覧から選び、画面のキーボードでパスワードを入力 | 済み |
| 設定用アクセスポイント | 保存した WiFi が無いか 20 秒つながらないと `ESP32Klipper-XXXX` を出す。スマホで `http://192.168.4.1/` を開いて選ぶ。保存した WiFi へは 30 秒ごとに再接続を試し、つながった 1 分後に閉じる | 未確認 |
| ブラウザ（接続中） | `http://<IP>/wifi` | 未確認 |
| アクセスポイントモード | 画面の System → WiFi → AP mode（または /wifi ページ）。本体が自分の WiFi になり、http://192.168.4.1/ で Mainsail。他のネットワークは探さない。モードは NVS に保存。戻すときは Use WiFi | 未確認 |

- アクセスポイントのパスワードは本体ごとに 8 文字を自動で作り（0/O/1/l/I は使わない）、NVS に保存して画面に表示する。menuconfig で設定すればそちらを使う。設定用とアクセスポイントモードで共通
- 同時接続は 2 台まで
- 接続中に 10 回続けて失敗すると、設定用アクセスポイントを出す（パスワード違いで 1 分弱）

## 9. ローカル UI

画面は両ボードとも 320×240 なので、LVGL の画面を共通にする。左の 240×240 を描画エリア、右の 80px をボタン列にし、ペンプロッタの「何を描くか、どこまで描いたか」を一目で分かるようにする。

**画面一覧**

| 画面 | 内容 |
| --- | --- |
| ホーム | 状態（待機・描画中・一時停止・エラー）、XY 座標、ペンの上下、MCU 接続、IP アドレス |
| ファイル | SD の `gcodes` 一覧（名前、サイズ、更新日時） |
| プレビュー | 描画経路の 2D 表示（ペン下線は実線、移動は省略または薄色）、外形寸法、推定時間、開始ボタン |
| 描画中 | プレビューの上に描いた部分を強調色で重ねる、進捗率、経過時間、一時停止・再開・中止 |
| ジョグ | XY 移動（0.1 / 1 / 10 / 50mm）、原点復帰、「ここを原点」、ペン上下、モータ OFF |
| 設定 | WiFi 設定、接続用 URL の QR コード、タッチ校正（CYD のみ）、再起動 |
| エラー | シャットダウン理由、設定エラーの箇所、`FIRMWARE_RESTART` ボタン |

**描画中画面のレイアウト**

```
+------------------------+--------+
|                        | 一時   |
|   描画エリア 240x240     | 停止   |
|   灰: 未描画の経路     +--------+
|   青: 描画済み         | 中止   |
|   ●: 現在位置         +--------+
|                        | ペン↑  |
+------------------------+--------+
| 42%  12:34 / 残り 17:10  X120.5 Y88.0 |
+---------------------------------+
```

**プレビューの生成**

- G-code を先頭から読み、ペンの上下を判定しながら線分を描く。判定ルールは設定で選ぶ：マクロ名（`PEN_DOWN` など）、`M3`/`M5`、`SET_SERVO`、Z がしきい値以下
- 結果は SD にキャッシュし（`.thumbs/<ファイル名>.bin`）、同じファイルは再解析しない。同じ解析でメタデータ（外形、線の総長、推定時間）も作り、Moonraker の `server.files.metadata` にも使う
- キャンバスは CYD では 1〜2 ビット/画素（240×240 で 7〜15KB）、CoreS3 では RGB565 を PSRAM に置く
- 描画中は `motion_report` の現在位置とペン状態から、描いた線分を逐次重ね書きする

**日本語の表示**

- ファイル名に半角カナや漢字が使われる（例：`03.ｽﾀｯｸﾁｬﾝ.gcode`）ので、LVGL に日本語フォントを入れる
- 全漢字は Flash を圧迫するので、ASCII、半角カナ、ひらがな、カタカナ、JIS 第 1 水準漢字に絞った 1 サイズのサブセットを使う。フォントにない文字は「□」で表示し、ファイル操作には影響させない
- CoreS3 は Flash に余裕があるので、第 2 水準まで入れてもよい

**操作系の方針**

- UI からの操作はすべて G-code 文字列として klippy に渡す（例：ペン↑ボタン → `PEN_UP`）。Mainsail からの操作と挙動が揃う
- ボタンに割り当てるマクロ名は printer.cfg の独自セクション `[esp_ui]` で変えられるようにする（Klipper 本家では未知セクションになるので、別ファイルに分けるのが安全）
- 初回起動時は WiFi の AP モードで立ち上げ、画面の QR コードからスマホで SSID を設定する

**実装結果（M4、CYD）**

最初の版は Status・Files・Move の 3 タブで、描画中のプレビューと日本語表示はまだ入れていない。画面の向き・色・タッチ位置は実機で未確認。

Status タブの左下のボタンは、印刷中は Pause、一時停止中は Resume、印刷が終わったあと（完了・中止・エラー）は Reprint になる（2026-11-13）。Reprint は `print_stats` に残ったファイル名を使い、Files から印刷するときと同じ確認を出してから `SDCARD_PRINT_FILE` を送る。ほかのコマンドが動いている間は押せない。再起動でファイル名が消えると Reprint は出ない。

- LVGL 8.4 を無改変で同梱（`third_party/lvgl`）。表示は ILI9341（SPI2、40MHz、描画バッファ 8 行 = 5KB）、タッチは XPT2046 をビットバングで読む（SPI3 は SD カード用に空ける）
- UI タスクはコア 0、優先度 2。klippy は 0.2 秒ごとに状態のコピー（`Printer::snapshot()`）を作り、UI はそれを読むだけ。操作は G-code として `submit_script()` に渡す
- 描画は klippy を遅くする。両コアがフラッシュのキャッシュを共有するためで、5 回/秒・毎回描き直しでは klippy の処理時間が +15%、先読み切れ 1 回。更新を 0.5 秒ごと、変わった部分だけにして +6%、先読み切れ 0
- 画面転送の待ちで CPU を回さないよう、転送完了の割り込みまで UI タスクを休ませる
- 実機なしで画面の配置を確かめるため、Mac で各画面を PNG に出すツール（`host/ui_screens`）を作った

## 10. ストレージ・メモリ予算

CYD の RAM はほぼ余裕がなく、本設計で最大の制約になる。下表は設計時の見積もりで、マイルストーンごとに実測（`heap_caps_get_free_size`、タスクのスタック残量）で置き換える。

**RAM 予算（見積もり）**

| 用途 | CYD（内蔵 RAM） | CoreS3 |
| --- | --- | --- |
| WiFi、lwIP | 60KB | 60KB（内蔵） |
| LVGL（部分描画バッファ 2 面＋ヒープ） | 60KB | 全画面バッファを PSRAM へ |
| プレビューキャンバス | 15KB（2 ビット/画素） | 115KB（RGB565、PSRAM） |
| serialqueue、コマンドキュー | 16KB | 16KB（内蔵） |
| 運動系（先読み、trapq、ステップ生成、MCU メッセージ） | 110KB（M1 実測ピーク、等速・ZV） | 110KB（内蔵） |
| 先読みキュー | 64 移動（約 16KB、上の行に含む） | 512 移動（PSRAM） |
| 設定、モジュール、マクロ構文木 | 30KB | PSRAM |
| HTTP、WebSocket | 2 接続（約 25KB） | 4 接続 |
| タスクのスタック | 40KB | 40KB（内蔵） |
| SD（FAT） | 10KB | 10KB |
| データ辞書の展開（接続時のみ） | 一時的に 40KB | PSRAM |

M1 で運動系を実測した結果、当初の見積もり（33KB）の約 3 倍の 110KB だった（先読み上限 64 で 2 倍速 114KB、5 倍速 127KB。上限なしでは 5 倍速で 217KB）。この値で CYD の合計は常駐で約 365KB となり、ESP32 の使えるヒープ（WiFi を起動する前で約 290KB）を超える。運動系の内訳は、先読みの移動（1 件 256 バイト）と 96 バイトの小さな割り当て（trapq の移動と MCU メッセージ）が大半。削減案は、メッセージと trapq 移動の固定プール化、ステップ生成の先行をさらに短くする、LVGL の描画バッファを小さくする、の 3 つ。これとは別に、対策として Bluetooth を無効化し、データ辞書の展開は WiFi や LVGL より前に済ませて解放する。それでも足りない場合は「CYD では Mainsail を使う間は LVGL を簡易表示に落とす」などの機能制限を入れる。

**実測（M3、CYD、WiFi・UI なし）**

起動直後の空きヒープ約 280KB からの減り方。

| 段階 | 減少 |
| --- | --- |
| printer.cfg 読み込み、各オブジェクトとステップ生成スレッド（3 本）の生成 | 35KB |
| UART ドライバ | 4KB |
| serialqueue、通信スレッド 2 本、データ辞書（メッセージ表） | 46KB |
| 設定送信、移動キューの準備 | 8KB |
| 300mm/s 描画中の運動系 | 37KB |

最低空きヒープは 142KB。スタックは klippy 16KB（残り 10.6KB）、ステップ生成 3KB×3（各残り約 2KB）、serialq 6.5KB、serialhdl 3KB に合わせた。ここから WiFi（約 60KB）、HTTP/WebSocket、LVGL を載せる。

**実測（M4、CYD、SD 印刷の試験）**

既存機の printer.cfg と card\_opt.gcode（約 180KB）で、印刷中の最低空きヒープは UI なし 62KB、UI あり 38KB。WiFi（約 60KB）はこのままでは載らない。

| 項目 | 使用量 |
| --- | --- |
| マクロ（設定読み込み時に展開） | 約 37KB |
| 密な描画中の運動系（一時的） | 約 60KB |
| SD の読み込み | 1KB（本家どおり 8KB ずつ行の一覧にすると約 36KB で、最低空きが 31KB まで下がった） |
| ローカル UI 全体 | 約 30KB（描画バッファ 5KB、画面のオブジェクト 10KB、タスクのスタック 5KB など） |

スタックはマクロの入れ子に合わせて klippy を 32KB（実使用約 9KB）に増やした。

**Flash パーティション**

| 領域 | CYD（4MB） | CoreS3（16MB） |
| --- | --- | --- |
| nvs | 24KB | 24KB |
| otadata | 8KB | 8KB |
| ota\_0 / ota\_1 | 各 1.9MB | 各 3MB |
| www（LittleFS、Mainsail） | なし（SD を使う） | 9MB |

**SD カードの配置**

| パス | 内容 |
| --- | --- |
| `/sd/config/` | printer.cfg、include ファイル、`mainsail.cfg` |
| `/sd/gcodes/` | G-code（Moonraker の `gcodes` ルート） |
| `/sd/www/` | Mainsail の配信ファイル（CYD） |
| `/sd/db/` | Moonraker 互換データベース、履歴、`save_variables` |
| `/sd/.thumbs/` | プレビューとメタデータのキャッシュ |
| `/sd/logs/` | `klippy.log` 相当のログ（ローテーションあり、Mainsail の `logs` ルートで閲覧） |

NVS には WiFi の認証情報とタッチ校正値だけを置き、その他の設定はすべて SD のテキストファイルにする。

SD カードの読み出しは klippy タスクを止めないよう、G-code の読み込みを 4〜8KB 単位の先読みバッファにする。CoreS3 は SD と LCD が SPI バスを共有するので、描画とファイル読み出しが競合しないようバスの排他制御を BSP で行う。

SD のファイルシステム（FatFs）は、日本語ファイル名のために長いファイル名（LFN）を有効にし、API の文字コードを UTF-8、短いファイル名のコードページを 932（日本語）に設定する。Moonraker 互換 API でもファイル名は UTF-8 のまま扱う。

**CoreS3 の microSD（実装済み、2026-11-13）**

CYD と同じ配置（一番上の `printer.cfg`、`/gcodes` か一番上の G-code）で動いた。32GB のカードと既存機の printer.cfg で、仮想 MCU とのリンクまで確認。

- カードは画面と同じ SPI2（CS 4）。カードの MISO と画面の DC が同じ GPIO35 なので、バスをセマフォで取り合い、カードの通信中だけ GPIO35 を入力に切り替える
- 既存機の printer.cfg は読み込みに内蔵 RAM を約 70KB 使い、MCU 通信のスレッドが作れずに落ちた。読み込み中だけ PSRAM を優先させて 12KB に減らし、起動後の内蔵 RAM の空きは 48KB → 105KB
- 起動時の内蔵 RAM の使い道：WiFi 78KB、画面 37KB、HTTP サーバー 29KB、printer.cfg 12KB、SD 3KB

## 11. エラー処理・安全

エラーの分類と復帰方法は Klipper に合わせる。「設定エラー」「コマンドエラー」「シャットダウン」の 3 種があり、シャットダウン中は全ステッパを止めて `FIRMWARE_RESTART` を待つ。

| 事象 | 検知 | 動作 | 復帰 |
| --- | --- | --- | --- |
| 設定エラー | 起動時、`RESTART` 時の読み込み | MCU には接続しない。箇所を UI と Mainsail に表示 | 設定を直して `RESTART` |
| コマンドエラー | 範囲外移動、未知コマンド、未原点復帰での移動 | そのコマンドだけ拒否。ファイル実行中なら中断 | なし（そのまま続行可） |
| MCU シャットダウン | MCU からの `shutdown` 応答 | ホストもシャットダウン状態へ | `FIRMWARE_RESTART` |
| 通信喪失 | MCU の応答が一定時間ない | シャットダウン | 配線を確認して `FIRMWARE_RESTART` |
| ステップ供給の遅れ | MCU が「Timer too close」で停止 | シャットダウン。直前のタスク負荷をログに残す | `FIRMWARE_RESTART` |
| 非常停止 | `M112`、UI の停止ボタン長押し、Mainsail の非常停止 | MCU に `emergency_stop` を即時送信 | `FIRMWARE_RESTART` |
| ESP32 のハングアップ | タスクウォッチドッグ | ESP32 再起動 | 自動。MCU は通信が途絶えればステップ供給切れで停止する |

**安全に関する方針**

- 非常停止は klippy タスクのキューを経由せず、serialqueue に最優先で入れる。klippy が詰まっていても止められるようにする
- 未原点復帰の軸は Klipper と同じく移動を拒否する。手動原点は `SET_KINEMATIC_POSITION` を明示的に使う
- 移動範囲は `position_min`、`position_max` で制限し、範囲外はコマンドエラーにする
- 予備の GPIO35（CYD）に物理の非常停止ボタンをつなげるようにしておく。CoreS3 は Port B を使う
- ペンプロッタは加熱がないので、熱暴走のような危険はない。主なリスクはベルトや指の挟まれと軸の衝突

**センサレスホーミングとソレノイドの注意**

- センサレスは誤検出すると原点がずれ、未検出だと機械端に当たり続ける。原点復帰中は電流を下げ、未検出でも機構を傷めにくくする
- 原点復帰が終わっていない軸があれば、描画ファイルの実行を拒否する
- ソレノイドは `shutdown_value: 0` とゲートのプルダウンで、シャットダウン・リセット・起動時に必ず非通電にする。非通電でペンが上がる機構にしておくと、異常時に紙を汚さない

## 12. 開発計画・マイルストーン

単体のペンプロッタとして使えるようになるのは M4、Mainsail 対応は M5。各段階は完了条件を満たしてから次へ進む。日程は未定。

**進捗（2026-11-13）**

M1〜M3 を完了し、M4 も実 SD カードの読み込みと画面の実機確認を残して完了した。M5 は CoreS3 で Mainsail から印刷できるところまで動いた。M1 は性能の見通しを先に立てるため、ステップ計算の性能測定を先に行い、MCU 接続（フレーム層、identify、クロック同期）は M2 にまとめて実装した。

| 段階 | 状態 | CYD での結果 |
| --- | --- | --- |
| M1 性能測定 | 完了 | 既存機の設定（ZV）で実時間の 2.8〜4.6 倍速。運動系のメモリは ESP 向けバッファで約 110KB |
| M2 MCU 接続・1 軸 | 完了 | identify、クロック同期、設定送信が通り、1 軸往復の MCU 位置とパルス数が一致 |
| M3 CoreXY | 完了 | printer.cfg から構築し 300mm/s の正方形 5 周で両モータが原点に戻る。TMC2209 のレジスタが本家の計算と一致。ステップ生成の最悪負荷 73%、最低空きヒープ 142KB（WiFi・UI なし） |
| M4 ペンプロッタとして完成 | ほぼ完了 | 仮想 MCU 相手に、既存機の printer.cfg で原点復帰（センサレス）、Jinja2 マクロ、card\_opt.gcode 全行（1 行平均 7.5ms、先読み切れ 0）、SD 実行（30% で一時停止 → 再開 → 完了、2 回目を中止）が合格。ローカル UI を表示したままでも合格（先読み切れ 0、klippy の処理時間 +6%）。残り：実 SD カード（microSD 未入手）、画面の向き・色・タッチの実機確認 |
| M5 Moonraker / Mainsail | CoreS3 で動作 | CoreS3（仮想 MCU）で Mainsail 配信、WebSocket API、印刷の開始が動作。再起動 6 回、API 確認の失敗 0、1 分に 11 回の連続印刷で停止なし（第 8 章）。CYD は WiFi なし（画面と microSD のみ）と決定。WiFi の設定（画面・設定用アクセスポイント）と microSD にも対応。残り：ブラウザでの画面操作の確認、設定用アクセスポイントの確認、待ち時間処理の修正後の CYD 実機での再試験 |

*（図「開発マイルストーン：M0〜M6、各段階の完了条件」は省略）*

M0 以降は毎回 CYD と CoreS3 の両方でビルド・動作確認し、CYD の空き RAM を記録する。M2 までは仕様確認のため、Raspberry Pi 上の本物の Klipper と SKR Pico の通信をロジックアナライザで記録して比較すると確実。

SKR Pico が手元にないため、M2・M3 は「仮想 MCU」で検証した。Klipper 本家の MCU コード（sched、command、stepper、endstop、trsync など）を改造せずに Mac 上のプロセスとしてビルドし、時刻・シリアル・GPIO だけを差し替えたもの。ピン名とクロックは RP2040 と同じで、GPIO の変化回数を数えられ、tmcuart には TMC2209 の模擬が応答する。CYD での試験は、CYD の USB シリアルを中継スクリプトで仮想 MCU につないで行う。SKR Pico を入手したら、同じ試験を実機で行う。

仮想 MCU で描いた内容は絵で確かめられる（2026-11-13）。仮想 MCU が出力ピンの変化を記録し、`tools/plot_view.py` が printer.cfg に従って CoreXY の位置とペンの上げ下げを復元して、PNG・SVG にするか、ブラウザでリアルタイムに表示する。card\_opt.gcode の絵・文字・QR コードが正しく再現された。

仮想 MCU の原点復帰は、DIAG（エンドストップ）が入った位置を 0 とし直すので、ピンの変化だけを積算すると描画がベッドの外へずれていく。そこで仮想 MCU は強制入力（DIAG の発火・解除）も記録に残し、`plot_view.py` は発火のたびにその軸の位置を printer.cfg の `position_endstop` に合わせ直す（2026-11-13）。これで CoreS3 の microSD から印刷した `square.gcode`（先頭に `G28 X Y` を追加）が、正しい位置に描かれることを PNG で確かめた。

ライブ表示（`--serve`、既定ポート 8770）には Clear ボタンを付けた。ビューワを起動し直すと、開いたままのブラウザは自動で読み込み直す。指定したポートを別のプログラムが使っているときは、起動時に知らせて止まる。仮想 MCU の起動手順（ビルド、書き込み、中継、表示）は README の「仮想 MCU で動かす手順（CoreS3）」にまとめた。

## 13. リスク・未決事項

最大のリスクは、CYD の RAM 不足。倍精度演算の速度は M1 で問題ないと分かったが、運動系のメモリが見積もりの約 3 倍（110KB）だった。RAM はM3 の時点で実測し、足りなければ CYD は機能を制限した版とし、全機能は CoreS3 で提供する。

**リスク**

| リスク | 影響 | 対策 | 確認時期 |
| --- | --- | --- | --- |
| 倍精度演算が遅く、ステップ計算が間に合わない（M1 で解消） | 低 | M1 で実測。既存機の設定（ZV）でコア 1 固定でも 2.8 倍速。WiFi とマクロを載せた M3 で再測定 | M1 |
| CYD の RAM 不足 | 高 | M1 で運動系 110KB を実測し、見積もり合計は約 365KB と予算超過。メッセージと trapq のプール化、先行時間の短縮、LVGL バッファ縮小、データ辞書を先に解放、Bluetooth 無効化、接続数制限。最終手段は CYD の機能制限。M3 時点（WiFi・UI なし）の最低空きヒープは 142KB。M4 の SD 印刷中は UI なし 62KB、UI あり 38KB で、このままでは WiFi（約 60KB）が載らない。CYD の機能範囲（A/B）を M5 の前に決める | M3、M5 |
| センサレスホーミングの誤検出・未検出 | 高 | 電流を下げて当てる、感度を機体ごとに調整、調整手順をドキュメント化。M4 では仮想 MCU で手順（TMC の設定切り替え、trsync、戻り）だけ確認。実際の検出は SKR Pico とモータが届いてから | M4 |
| ステップ供給の遅れ（Timer too close） | 中 | klippy をコア 1 に固定、先行時間を増やす、負荷の実測。M4 で、MCU の応答待ちの間にステップ送信が止まり、印刷を続けて始めると発生した。待ちの間も送信を続けるようにして解消（klippy の reactor と同じ動き） | M3 |
| Klipper の版による MCU コマンドの差異 | 中 | 最新版に追従。接続時にコマンド書式を検証し、Klipper の更新ごとに接続テスト | M1 |
| Mainsail の更新で必要 API が変わる | 中 | 最新版に追従し、リリースごとに回帰テスト | M5 |
| Jinja2 サブセットでマクロが動かない | 中 | M4 で既存機の printer.cfg、M5 で `mainsail.cfg` を合格ラインにし、足りない構文を追加。既存機の printer.cfg は M4 で合格 | M4、M5 |
| ソレノイドの発熱・逆起電力 | 中 | 逆起電力吸収ダイオード、ゲートのプルダウン、PWM で保持電流を下げる | M4 |
| CYD のロット違い | 低 | BSP の設定で吸収 | M0 |
| GPLv3 の義務 | 低 | GPLv3 の OSS として公開。同梱物（LVGL：MIT、esp-bsp：Apache 2.0、Mainsail：GPLv3）のライセンス表記を揃える | 公開前 |

**未決事項**

- [ ] Laser 端子の IO20 が RP2040 に直結かバッファ経由か（回路図または実測で確認）
- [ ] ソレノイドの仕様（電圧、電流、吸引時間、通電でペンが上がるか下がるか）と、トランジスタ・ダイオードの選定
- [ ] 最新版への追従をどう検証するか（実機での手動テストか、プロトコル部分を CI で自動テストするか）
- [ ] 公開先のリポジトリ名と公開開始時期
- [ ] CYD の機能範囲（A：画面と SD のみ、B：WiFi 使用中は画面を簡略化）。M5 は CoreS3 で先に作り、CYD はその後に決める（2026-11-12 決定）
- [ ] 画面の向き・色・タッチ位置の実機確認（UI デモで 10 分）

決定済みの項目（機構、原点復帰、ペン昇降、基準バージョン、公開、USB Host）は第 1 章の決定事項に移した。
