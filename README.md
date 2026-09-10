# M5StopWatch-MuteHid

M5StopWatchを、BLE HID over GATT（HOGP）のTelephony Deviceとして動かし、PCの会議アプリのマイクミュートを操作・表示するアプリです。  
PC側に常駐ソフトを入れる必要はありません。

物理ボタンまたは画面タップでミュートを切り替え、PCが報告してきたミュート状態を画面へ反映します。  
表示するのは「連携中の会議アプリがHIDへ通知した状態」であり、OSの録音デバイスやマイク本体のスイッチではありません。

## ビルドと書き込み

### このファームウェアのみを書き込む場合

```
pio run -e m5stopwatch -t upload
```

### M5StopWatch-UserDemoと共存させる場合

M5StopWatchのFlashにUserDemoとMuteHidを共存させ、再起動で切り替えます。  
MuteHidはota_1（`0x510000`）だけを使います。  
なお、UserDemo側にota_1を起動する機能を追加する必要があります。

```
pio run -e m5stopwatch-coexist            # ビルド
pio run -e m5stopwatch-coexist -t update  # ビルドしてota_1のみ書き込み
pio run -e m5stopwatch-coexist -t backup  # 16 MB全体のバックアップ
```

`-t update` は `tools/device.py` を呼び、シリアルポートと保存済みバックアップを自動で解決します。  
その後、実機のブートローダ・パーティション表・UserDemo領域がバックアップと一致することを確認してからota_1を書き換えます。

直接叩く場合は次のとおりです。

```
python tools/device.py update --execute
```

> **共存させる場合、`pio run -t upload` と `-t erase` は使わないでください。** uploadはアプリを `0x10000` から書くためUserDemo（ota_0）を破壊し、eraseはBLEボンドと共有設定を含むNVSごと消します。`tools/upload_guard.py` が共存env（`m5stopwatch-coexist`）でこれらのターゲットを停止します。単独env（`m5stopwatch`）では、デバイス全体をMuteHid専用にする前提でuploadを許可しています。

`update` は既にota_1にMuteHidが入っている場合の更新用で、実機の保護領域だけをバックアップと照合します。初回は `tools/device.py install --execute` を使い、こちらはFlash全体がバックアップと一致することを確認します（`--backup` を省くと16 MBを読み出して新しいバックアップを作ります）。Flashの全消去や共存レイアウトの新規構築は実装していません。

ota_1へ切り替えて起動させるには `--boot` を付けます。付けない場合はotadataを変更せず、書き込みだけを行います。

状態遷移のホストテストは実機なしで動きます。

```
g++ -std=c++17 -I src tools/test_mute.cpp -o test_mute && ./test_mute
```

## 操作

| 操作 | 動作 |
|---|---|
| KEY.A 短押し | ミュート切り替え要求。`UNKNOWN` のときは絶対値のミュート要求（Input=1） |
| 中央アイコンのタップ | 同じ切り替え要求 |
| KEY.B 短押し | 設定画面の開閉 |
| A+B を3秒保持 | UserDemoへ戻る |
| KEY.Bを押しながら起動 | 初期化前にUserDemoへ戻る |

30秒無操作で画面が減光します。  
ボタンは減光中でもそのまま動作しますが、減光を解除した最初のタップはミュート操作になりません。

USBシリアルからの診断コマンド: `a`/`b`＝ボタン相当、`0`/`1`＝生のInput送信、`s`＝状態ログ、`y`/`n`＝ペアリング応答、`u`＝UserDemo復帰、`h`＝ヘルプ。

消費電力の測定用コマンド: `p`＝電源の状態を1行出力、`L`/`l`＝その1秒ごとの出力を開始／停止、`D`＝表示の固定状態を切り替え、`A`＝アドバタイズの一時停止、`X`＝固定の解除、`R`/`r`＝電池電圧の記録を開始／停止、`O`＝記録の吸い出し。手順は[省電力化のアイデア集](docs/power-saving-ideas.md)の§4を参照してください。

## 画面

中央にマイクアイコンと状態ラベル、上部に接続を示すドットと電池残量を表示します。  
ステータスは `MUTED` / `MIC ON` / `UNKNOWN` / `LINKING` / `OFFLINE` の5種類です。

## リポジトリ構成

| パス | 内容 |
|---|---|
| `src/app/MuteApp.cpp` | 入力の調停、画面遷移、振動、減光、電池 |
| `src/domain/MuteState.h` | ホスト報告・反映待ち・接続世代。BLEとUIに依存せず単体でテスト可能 |
| `src/ble/TelephonyHid.cpp` | GATT、Report Map、接続とボンド、CCCDの永続化 |
| `src/ui/Renderer.cpp` | 円形レイアウトの描画 |
| `src/storage/Settings.cpp` | NVS（名前空間 `mutehid`）のアプリ設定 |
| `src/app/FirmwareSwitch.cpp` | UserDemoへの復帰と起動時の脱出口 |
| `src/app/PowerProbe.cpp` | 消費電力測定用の電圧出力と、電池電圧の記録 |
| `tools/icons.py` | SVGを8bitアルファマスクへ変換（ビルド時に実行、実行時のSVGパーサーは不要） |
| `tools/device.py` | バックアップ照合付きのota_1書き込み |
| `tools/ble_scan.py` / `hid_probe.py` / `serial_probe.py` | 広告の確認、Windows HIDの列挙、シリアルログ取得 |
| `tools/power_measure.py` | USBテスターを使った状態ごとの消費電力の記録と、電池電圧の記録の吸い出し |
| `tools/test_mute.cpp` | 状態遷移のホストテスト |
| `tools/test_device.py` / `test_power_measure.py` | 書き込みツールと測定ツールの単体テスト |

## ドキュメント

- [アプリ仕様書](docs/specification.md)
- [Phase 0 検証記録](docs/phase0-results.md)
- [省電力化のアイデア集と測定手順](docs/power-saving-ideas.md)
- [Google Meetの通話コントロール対応条件](https://support.google.com/meet/answer/12562325?hl=en)

## サードパーティー

アイコンは [Pictogrammers Material Design Icons](https://github.com/Templarian/MaterialDesign) 使用しています。  
Pictogrammers Material Design Icons は Apache License 2.0 で公開されています。[LICENSE](assets/icons/LICENSE)  
HIDレポート構成の参考として [push-to-talk-pico](https://github.com/masawada/push-to-talk-pico) を参照しました。
push-to-talk-pico は MIT ライセンスで公開されています。
