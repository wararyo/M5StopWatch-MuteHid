# Phase 0 検証記録

[仕様書](specification.md) §9「Phase 0: 通信だけのPoC」の実測記録。設計判断の根拠となった観測だけを残し、推測と観測を区別する。未取得の項目は`未取得`と明記し、埋まったふりをしない。

最終更新: 2026-09-09

## 1. 検証機材

| 項目 | 値 |
|---|---|
| 実施日 | 2026-09-09 |
| デバイス | M5StopWatch（ESP32-S3 QFN56 rev v0.2、PSRAM 8 MB、Flash 16 MB） |
| デバイスBD_ADDR | `28:84:85:43:a7:c0`（Serial Number特性にも同じ値を使用） |
| 書き込み・ログ | USB Serial/JTAG、COM11、115200 bps |
| PC OS | Windows 11（ビルド番号: 未取得） |
| ブラウザ | Google Chrome（版: 未取得） |
| 会議アプリ | Google Meet（版表記なし。実施日時で識別） |
| Bluetoothアダプター | 未取得（内蔵／外付けの別、チップ、ドライバ版） |
| Meet側の通話コントロール設定 | 未取得（設定→音声の候補にどう表示され、どの権限を許可したか） |

未取得の欄は再現性の確認に必要なため、次回の検証時に必ず埋める。

## 2. ファームウェア

| 項目 | 値 |
|---|---|
| ビルド | PlatformIO `espressif32 @ 6.12.0` / ESP-IDF 5.5.0 / env `m5stopwatch-coexist` |
| PROJECT_VER | `0.0.1-phase0` |
| 双方向動作・往復時間を測定したビルド | commit `6f9e2c5`（CCCD永続化の修正前） |
| 現在ota_1へ書き込み済みのビルド | commit `9deb88b`（CCCD永続化あり）、1,016,592 bytes、SHA256 `edae3c1adf75dc901b8dade0577e438dbc5a05121d8e09928fff25c2f8dd5b8b` |
| 書き込み方法 | `pio run -e m5stopwatch-coexist -t update`（`tools/device.py update`を呼ぶ。ポートと保存済みバックアップは自動解決、ota_1のみ書き換え、`0x00510000`へ verify OK） |

## 3. HID構成

### Report Map

```
05 0B        Usage Page (Telephony Devices)
09 01        Usage (Phone)
A1 01        Collection (Application)
85 01          Report ID (1)
09 2F          Usage (Phone Mute)
15 00 25 01    Logical Minimum (0) / Maximum (1)
75 01 95 01    Report Size (1) / Count (1)
81 02          Input (Data,Var,Abs)      -- bit 0 = mute
75 07 95 01    Report Size (7) / Count (1)
81 03          Input (Cnst,Var,Abs)      -- padding
05 08          Usage Page (LEDs)
09 09          Usage (Mute)              -- bit 0
09 17          Usage (Off-Hook)          -- bit 1
09 18          Usage (Ring)              -- bit 2
15 00 25 01 75 01 95 03
91 02          Output (Data,Var,Abs)
75 05 95 01
91 03          Output (Cnst,Var,Abs)     -- padding
C0           End Collection
```

Input・Outputともに1バイト、Report ID 1。実体は`src/ble/ReportMap.h`。

### PnP ID

| 項目 | 値 |
|---|---|
| Vendor ID | `0x0000`（実験用の未割り当て。配布不可） |
| Product ID | `0x0000`（同上） |
| Version | `0x0001` |
| Manufacturer | `MuteHid Phase0 (unassigned IDs)` |
| Device Name | `M5StopWatch MuteHid` |

ゼロIDのままでもWindowsは列挙し、Meetも候補として扱った。仕様書§4.4の正式なID選定は未着手。

### GATTハンドル（この構成での実測値）

| 属性 | ハンドル |
|---|---|
| Input Report CCCD | `0x0041` |
| Output Report | `0x0044` |
| Input Report 値 | `0x0040`（CCCDの直前。ログ上は`HANDLES`行で確認する） |

ホストは接続後に`0x0037`〜`0x0045`をまとめてReadしていた。ハンドルはReport Mapや属性テーブルを変えると移動するため、固定値として扱わない。

## 4. 実測結果

### 4.1 互換性

| 環境 | HID列挙 | デバイス選択 | 本体→会議 | 会議→本体 | 備考 |
|---|---|---|---|---|---|
| Windows 11 / Chrome / Meet / BLE直結 | 成立 | 成立 | 成立 | 成立（変化時のみ） | 公式のBluetooth対応範囲外だが双方向動作。機材の版は未取得 |
| Windows 11 / Edge / Meet / BLE直結 | 未検証 | 未検証 | 未検証 | 未検証 | |
| Windows / Teams・Zoom | 未検証 | 未検証 | 未検証 | 未検証 | |

### 4.2 ペアリング

LE Secure Connections、数値比較（IO capability = `ESP_IO_CAP_IO`、`ESP_LE_AUTH_REQ_SC_MITM_BOND`）で成立。

```
E (100666) BT_SMP: Value for numeric comparison = 858192
W (107159) BT_SMP: FOR LE SC LTK IS USED INSTEAD OF STK
I (107479) Telephony: AUTH success=1 reason=0x00 mode=0x0d bonds=1
```

`mode=0x0d` はLE SC + MITM + Bonding。ボンドは1件。

### 4.3 Inputの解釈は絶対状態値

Input=1でミュート、Input=0で解除。押下／解放のエッジをトグルとして解釈する挙動は観測されなかった。

```
I (124668) Telephony: INPUT id=1 len=1 value=1 result=ESP_OK
I (124689) Telephony: OUTPUT id=1 len=1 value=0x03   -- Mute + Off-Hook
I (125515) Telephony: INPUT id=1 len=1 value=0 result=ESP_OK
I (125543) Telephony: OUTPUT id=1 len=1 value=0x02   -- Off-Hook のみ
```

これにより仕様書§4.3の二択（状態値方式／押下→解放方式）は状態値方式で確定した。

### 4.4 Outputはミュート状態の変化時にしか届かない

- すでにミュート中にInput=1を送っても、解除中にInput=0を送ってもOutputは返らない。本体は1秒の反映待ちののち`Unknown`表示になる。
- 会議に参加する前のInputにも応答がない。

```
I (111459) Telephony: INPUT id=1 len=1 value=0 result=ESP_OK
W (112465) Probe: HOST_TIMEOUT: no automatic toggle retry
I (112850) Telephony: INPUT id=1 len=1 value=1 result=ESP_OK
W (113856) Probe: HOST_TIMEOUT: no automatic toggle retry
```

会議参加時には`0x00`に続いて`0x02`（Off-Hook）が届いた。これは参加という状態変化の通知であり、任意のタイミングで現在値を取得できるという根拠にはならない。

```
I (123624) Telephony: OUTPUT id=1 len=1 value=0x00
I (123624) Telephony: OUTPUT id=1 len=1 value=0x02
```

現在値を問い合わせる手段はないため、接続直後や会議参加前は最初の変化まで`Unknown`のままになる。仕様書§5にこの前提と、「既知状態と同じ値を送った場合は`Unknown`へ落とさない」規則を追加した。

### 4.5 往復時間

本体のInput送信ログからMeetのOutput受信ログまでの時間。ESP側のログタイムスタンプによる測定で、ホスト内部の処理境界は含まない。

| 回 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| ms | 20 | 28 | 22 | 32 | 19 | 32 | 29 | 26 | 23 | 31 |

n=10、最小19 ms、最大32 ms、平均26.2 ms。仕様書§9の目標（往復500 ms以内）に対して十分な余裕がある。

### 4.6 NVS使用量

```
I (91887) Probe: NVS used=96 free=408 total=504 namespaces=4
```

ボンド1件＋アプリ設定なしの状態。504エントリ＝16 KBパーティションのうち96使用。CCCD永続化（`mutehid`名前空間、`input_ccc`キー）追加後の再測定は未取得。仕様書§8のNVS容量確認は、UserDemo側の使用量と合わせて改めて行う。

## 5. 判明した問題と対処

### 5.1 再起動後にInputだけ送れなくなる（対処済み）

**症状**: ペアリングとMeetでの双方向動作を確認したあと本体を再起動すると、以降PCへの送信ができない。受信は継続するため気付きにくい。画面のTXが増えず、`INPUT rejected: ready=0`が出続ける。ホスト側でデバイスを削除して再ペアリングすると回復する。

**観測**: 再接続したセッション（gen=1）で`rx`が26まで増える一方、`ccc`は0のままだった。暗号化は成立しており（Outputを受理しているため）、欠けていたのはInput CCCDへの書き込みだけ。

```
I (91887) Probe: STATUS conn=0 auth=0 ccc=0 ready=0 host=-1 pending=0 tx=0 rx=26 gen=1
```

**原因**: CCCDの値はボンド済みクライアントごとにGATTサーバーが永続化する必要がある。Windowsはボンド済みデバイスのGATTをキャッシュし、再接続時にCCCDを書き直さない。実装は接続のたびに購読状態をfalseへ戻し、CCCD書き込みでしか復帰しなかったため、再起動後は恒久的に`ready=0`になっていた。ESP-IDFの`esp_hid_device`例も同様にCCCDを永続化しない。

**対処**（commit `9deb88b`）:

1. CCCDへの書き込みを`{相手アドレス, CCC値}`としてNVS（`mutehid` / `input_ccc`）へ保存。
2. 暗号化完了（`ESP_GAP_BLE_AUTH_CMPL_EVT` success）時に保存値を復元し、CCCD属性値も`0x0001`へ戻す。
3. CCCD書き込みの受理条件からアプリ側の暗号化フラグを外す。属性のパーミッションで暗号化は保証されており、認証イベント到着前に届いた書き込みを取りこぼさないため。
4. ボンド削除時に保存値も消去する。

**確認**: 修正後のビルドで、再ペアリングなしに再起動→再接続→送信が成功することを確認済み。

### 5.2 切断理由

```
W (91883) BT_HCI: hcif disc complete: hdl 0x1, rsn 0x13
```

`0x13` = Remote User Terminated Connection。ホスト側の操作による正常な切断。

## 6. 次に取得する記録

- 検証機材（§1）の未取得欄。特にWindowsビルド、Chrome版、Bluetoothアダプターとドライバ版。
- Meetの通話コントロールでの表示名と権限付与の手順、スクリーンショット。
- CCCD永続化を含めたNVS使用量の再測定。
- Edge、Teams、Zoomでの同じ4項目。
- 長時間接続（仕様書§9 Phase 1の1時間試験）とスリープ復帰の挙動。
- Report Mapを変更した場合のWindows側キャッシュの影響（再ペアリングの要否）。
