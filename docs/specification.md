# M5StopWatch-MuteHid アプリ仕様書

作成日: 2026-09-09 / 版: 0.3（Phase 0実測反映）

2026-09-09の変更: ユーザー指定によりmacOSを当面の要件・検証対象から除外。Windows 11を対象とする。Phase 0の実測結果は[検証記録](phase0-results.md)で管理する。

2026-09-09の追記: Phase 0でWindows 11＋Chrome＋Google Meetの双方向動作を実測した。Inputが絶対状態値として扱われること、Outputがミュート状態の変化時にしか届かないことを§4.3・§5・§6・§9へ反映した。

## 1. 目的と実現性

M5StopWatchをBluetooth接続のマイクミュートコントローラーとして使用する。物理ボタンまたは画面タップで会議アプリのミュートを切り替え、PC側の操作結果を画面へ反映する。

第一候補は **BLE HID over GATT Profile（HOGP）上のHID Telephony Device**。PCに専用ソフトを追加しない利用を目指す。ただし、HIDとしてOSに認識されても、会議アプリがその入力と出力に対応するとは限らない。

Google公式ヘルプは、通話コントロールをChromium系ブラウザに限定し、Bluetoothでの対応をChromeOSのみとしている。USB参考実装があることは、WindowsのBluetooth対応の根拠にはならない。[S1][S2]

この公式対応範囲外の条件下で、Phase 0のPoCによりWindows 11＋Chrome＋Google Meetの双方向動作を1台の検証機で実測した。ただし対応が公式に保証されたわけではなく、Meetやブラウザの更新で失われうる。検証機・版の記録と再現性の確認は継続する。

ここでいう「PCのミュート状態」は、連携中の会議アプリがHIDへ通知した状態を指す。OS全体の録音デバイスのミュート、他の会議アプリ、マイク本体の物理スイッチと一致する保証はない。

## 2. 要求と対象範囲

| ID | 要求 | 優先度・扱い |
|---|---|---|
| R1 | KantanPlayを基にした技術スタック | 必須 |
| R2 | WindowsでBLE HID Telephonyとして列挙・接続できる | 必須、PoCで判定 |
| R3 | StopWatchから会議アプリのミュートを切り替える | 必須、環境別に判定 |
| R4 | 会議アプリ側のミュート変更をStopWatchへ反映 | 最優先の実現目標、未対応環境を明示 |
| R5 | Material Design Iconsのマイクアイコンを使用 | 必須 |
| R6 | UserDemoと共存し相互に起動を切り替える | 推奨、初期設計に含める |
| R7 | ペアリング情報を保持し再接続する | 必須 |

初期対象はWindows 11の検証機、Google Meet＋Chrome。OS・ブラウザの正確なバージョンは検証時に記録する。Edge、Teams、Zoomは追加の互換性確認対象とし、一つの環境の成功を他へ一般化しない。macOSとSafariは当面の対象外とする。[S1]

初期版では音声送受信、Bluetoothヘッドセット機能、録音、複数PCの同時接続、押している間だけ解除するPTT、キーボードショートカット代替を含めない。USBは給電・書き込み・ログ取得に使用する。USB HIDへの変更やPC常駐ソフトはPoC不成立時の別案とする。

## 3. 技術スタックと再利用

ローカルのKantanPlayの実ファイルを基準とする。調査時のHEADは `86309befbc8fe8de97084f4b9356b8f878cd0729`。将来変更された場合も、この調査時点と区別する。

| 項目 | 採用方針 |
|---|---|
| 言語・実行基盤 | C++、ESP-IDF、FreeRTOS |
| ビルド | PlatformIO、`framework = espidf`、`espressif32 @ 6.12.0` |
| ESP-IDF | KantanPlayで使用している5.5.0を初期基準とし、実際の解決バージョンを記録 |
| ボード設定 | `m5stack-cores3`を流用し、M5UnifiedのStopWatch自動認識を使用 |
| ハードウェアAPI | `M5Unified @ 0.2.16` |
| 描画 | `M5GFX @ 0.2.28`、直接描画。LVGLは追加しない |
| メモリ設定 | Flash 16 MB、DIO/80 MHz、Octal PSRAM/80 MHz |
| BLE | ESP-IDF付属の`esp_hid_device`例を起点に`esp_hid`を使用。Bluedroid BLEを第一候補とし、PoCで確定 |
| 設定保存 | NVS。アプリ用名前空間は`mutehid` |
| アセット | 必要なアイコンだけビルド時に変換してファームウェアへ埋め込む |

ESP32-S3ではBluetooth LEを使用し、Bluetooth Classic HID/HFPを前提にしない。[S3][S4]

KantanPlayからは、初期化、画面サイズに追従するレイアウト、ボタン・タッチ入力、振動、設定保存、UserDemo切り替え、共存書き込みガードを再利用する。和音エンジン、音声エンジン、音色データは不要。内蔵マイク・スピーカー・IMU・RTCは本アプリで使わない構成を基本とする。

参照したローカルファイル: `platformio.ini`、`sdkconfig.defaults`、`docs/02-hardware.md`、`docs/10-coexistence.md`、`docs/11-coexistence-testing.md`、`src/app/FirmwareSwitch.cpp`、`partitions.coexist.csv`。共存については古い設計文書より実装と検証手順を優先する。

## 4. Bluetooth・HID仕様

### 4.1 接続

- デバイス表示名は`M5StopWatch MuteHid`。OSが表示するデバイス分類名そのものは保証せず、TelephonyのApplication Collectionの列挙で確認する。
- BLE Peripheralとして動作し、同時接続は1台。初期版はボンド先も1台に限定する。
- 初回起動はペアリング待ち。登録後は保存済みPCへの再接続待ちとする。
- 新規登録は設定画面から明示的に開始する。登録解除は対象ボンドだけを消去する。
- ペアリング時の暗号化・ボンディングに対応し、対応ホストではLE Secure Connectionsと画面による数値確認を使用する。必要なI/O capabilityと互換性はPoCで確定する。
- 接続成立、暗号化完了、Inputの通知購読がそろうまで操作レポートを送らない。
- Input CCCDの値はボンドごとにNVSへ永続化し、暗号化完了時に復元する。ボンド済みホストは再接続時にCCCDを書き直さないため、「今回の接続でCCCD書き込みを受けたか」を購読の判定に使わない。ボンド削除時は保存値も消去する。
- 切断中の操作は送信予約しない。復帰時に古い操作を送信しない。
- Report Map変更後はOSのキャッシュの影響を確認し、必要ならPCと本体の登録を解除して再登録する。

### 4.2 GATT構成

HID Service（`0x1812`）、Device Information Service（`0x180A`）、Battery Service（`0x180F`）を設ける。HID Information、Report Map、HID Control Point、Input/Output ReportとReport Referenceを整合させる。InputはRead/Notify、OutputはRead/Writeを基本とし、Write Without Responseの要否をホストと使用APIで確認する。[S4][S5]

キーボード／マウスのBoot Reportは追加しない。Report IDとペイロード先頭へのID付加は使用ライブラリの規約に従い、Report Map・Report Reference・送受信処理を一致させる。USB版のバイト列をGATTへそのまま送る実装にしない。

### 4.3 レポートの第一候補

push-to-talk-picoの`src/usb_descriptors.c`と`src/main.c`を確認した。以下のレポート構成と、ホスト出力による表示更新を参考にする。USB/TinyUSB層およびPTTの操作仕様は移植しない。[S2]

| 方向 | Usage | データ案 |
|---|---|---|
| Application Collection | Telephony Devices `0x0B` / Phone `0x01` | 1 collection |
| Input: 本体→PC | Phone Mute `0x2F` | 1 byte、bit 0、残りはpadding |
| Output: PC→本体 | LEDs `0x08` / Mute `0x09` | 1 byteのbit 0、1=ミュート表示 |
| 同じOutput | Off-Hook `0x17`、Ring `0x18` | bit 1/2、診断用に保持。初期UIでは使わない |

参考実装はInputを`Data,Var,Abs`とし、ローカルのミュート値を送る。Phase 0のWindows 11＋Chrome＋Google Meetでは、これが**絶対状態値**として扱われることを確認した。Input=1でミュート、Input=0で解除となり、押下／解放のエッジをトグルとして解釈する挙動は観測していない。初期版は状態値方式を採用し、押下→解放方式は実装しない。他の会議アプリでは同じ確認を改めて行う。

同じ検証で、**Meetはミュート状態が変化したときにしかOutputを送らない**ことも確認した。既にミュート中にInput=1を送っても、解除中にInput=0を送っても、Outputは返らない。無反応は送信失敗ではなく「変化なし」を意味しうるため、これを理由に別方式へ切り替えたり、同じ操作を再送したりしない。押下→解放の送信はPhase 0の診断コマンドとしてのみ残し、通常操作の経路には載せない。

Outputの実測値は`0x02`／`0x03`で、bit 0がミュート、bit 1（Off-Hook）は会議参加中に1だった。会議参加直後は`0x00`に続いて`0x02`が届いた。この初期通知は会議参加時にだけ観測されたものであり、接続だけで現在値が得られる根拠にはしない。

2026-09-10のmacOS実測では、ホストがOutput characteristicへ**Report IDを前置した2バイト**（`01 01`、`01 02`、`01 03`）を書き込んだ。BLE HIDではReport IDはReport Reference記述子側のメタデータであり、Windowsはペイロード1バイトだけを書く。ペイロードの意味はWindowsと同じでbit 0がミュート、bit 1がOff-Hookだった。本体は1バイトと「Report ID＋1バイト」の両方を受け付け、状態にはペイロードだけを渡す。長さだけで弾くと受信経路が丸ごと無言で失われるため、この差はホストごとに確認する。

### 4.4 VID/PID

BLEでもDevice InformationのPnP IDを設計対象とする。「USB端子を使わないのでVID/PIDは不要」とは決めない。USB VID由来の値を使う場合、Vendor ID SourceはUSB-IFを示す`0x02`とし、Bluetooth SIGのCompany Identifierと混同しない。採用するHOGP版とESP-IDF実装に照らして検証する。[S5]

V-USBの共有ID表にある、名前で区別する汎用HID用の **VID `0x16C0` / PID `0x05DF`** を採用する。同資料はUSBデバイスを想定した条件であり、BLEのPnP IDへ転用した事例としての正当性は保証されないが、名前で区別するという同じ前提を満たす形で使用する。[S6]

条件として、管理下にあるドメインまたはメールアドレスを含むメーカー文字列、メーカー内で一意の製品文字列、標準クラスドライバの使用が必要。採用値は次のとおり。

| 項目 | 値 |
|---|---|
| Vendor ID / Product ID | `0x16C0` / `0x05DF` |
| Vendor ID Source | `0x02`（USB-IF）。ESP-IDFの`ble_hidd.c`が固定値で発行する |
| Manufacturer Name String（`0x2A29`） | `wararyo(contact@wararyo.com)` |
| Model Number String（`0x2A24`） | `M5StopWatch MuteHid` |
| Serial Number String（`0x2A25`） | 本体BD_ADDRの16進表記 |

ESP-IDFのDevice Information ServiceはModel Number Stringを持たないため、属性テーブル生成をラップして追加する。ホストがHIDの製品名をどこから得るかは環境依存であり、追加の効果は実測で確認する。アプリが識別するときはVID/PIDだけでなく文字列も照合する。USBで使用する場合は英語（0x0409）文字列の条件も満たす。[S6]

参考実装や市販会議デバイスのVID/PIDをコピーしない。識別値を変更した場合、Windowsはボンドごとにキャッシュするため、PC側の登録解除と再ペアリングが必要になる。

Windows + ChromeのWebHIDデバイス選択画面では、BLE HIDの製品名が表示されない。ChromiumがWindowsで使う`HidD_GetProductString`がBLE HIDに未対応であるためで、デバイス側の設定では解決できない。この環境ではVID/PIDが唯一の識別材料になる。Model Number Stringは他のホストのために保持する。[S8]

## 5. 双方向同期の状態管理

接続状態、ホスト報告状態、送信中の操作を別々に保持する。ホスト報告状態は`Unknown / Muted / Unmuted`の3値とし、起動・再接続時は必ず`Unknown`から開始する。最終受信時刻と接続世代も保持する。

| イベント | 動作 |
|---|---|
| 有効なOutput受信 | Mute bitを表示へ反映。受信内容をInputとして送り返さない |
| 既知状態で切り替え操作 | 状態値方式で反転した値を1回送信し、「反映待ち」にする |
| 操作後のOutput受信 | 現在のホスト報告を優先する。期待と異なれば「PC側状態を反映」とし、成功と断定しない |
| 既知状態と同じ値の送信 | ホスト側で状態が変化せずOutputは返らない。既知状態を維持し、`Unknown`へ落とさない。成功とも断定しない |
| 状態が変わる要求の反映待ちが1秒超過 | `Unknown`へ移行し「状態を確認できません」。自動トグル再送しない |
| 不正な長さ・種別・IDのOutput | 無視して診断ログへ記録。状態は更新しない |
| 切断・再起動 | 保留操作と既知状態を破棄。新接続へ持ち越さない |
| 同一状態の繰り返し通知 | 再描画・振動を繰り返さない |

1秒は初期の調整可能値である。HID Outputには本アプリの操作番号がないため、操作後の通知を厳密なACKとみなさない。PC操作と本体操作が競合したときも、最後に受信したホスト報告を表示の根拠にする。

ホスト報告は状態が変化したときだけ届き、現在値を問い合わせる手段はない。したがって接続直後や会議参加前は、最初の変化があるまで`Unknown`のままになる。これは異常ではなく、UIも同期失敗として扱わない。

状態値方式と確認できたため、`Unknown`では現在値を反転できない。代わりに絶対値の「ミュートする」（Input=1）だけを1回送れる操作とし、「切り替え」は既知状態でのみ有効にする。`Unknown`で送信してOutputが来ない場合、「すでにミュート済みで変化しなかった」と「ホストが処理していない」を区別できないため、`Unknown`のまま据え置き、推測でミュート値を表示しない。この差は接続プロファイルで定義する。

ホスト出力は物理マイクの遮断を保証せず、アプリ終了がBLE切断として検出されるとも限らない。UIは「PC報告のミュート」として扱う。ホスト出力の周期送信や会議終了通知は前提にしない。終了時に古い値が残る環境では、完全同期対応に分類せず、その制約を互換性表に記載する。

## 6. 操作と画面

### 6.1 通常操作

| 操作 | 挙動 |
|---|---|
| KEY.A短押し | ミュート切り替え要求。`Unknown`ではミュート要求（Input=1）に読み替える |
| 中央マイクアイコンのタップ | 同じ切り替え要求 |
| KEY.B短押し | 設定画面の開閉 |
| A+Bを3秒保持、画面タッチなし | 共存版ではUserDemoへ戻る |
| KEY.Bを押しながら起動 | 共存版では初期化前にUserDemoへ戻る |

物理ボタンは解放時に短押しを確定する。長押しリピートはしない。A+Bが成立した操作では個別短押しを抑止し、途中で復帰ジェスチャを中断してもミュートを切り替えない。タップは同一領域内で押して離したときに確定し、ドラッグを除外する。

反映待ちの間は追加操作を抑止し、PCへのトグル連射を防ぐ。入力のデバウンスはM5Unifiedを基本とし、物理ボタンとタッチの同時操作も1操作に集約する。

### 6.2 状態表示

円形画面の中央に大きなマイクアイコンとラベル、上部にBluetooth接続状態と電池残量、下部に補助説明を配置する。画面座標は`M5.Display.width()/height()`から算出し、466/468 pxの差と円形の見切れに対応する。

| 状態 | アイコン・色 | ラベル例 |
|---|---|---|
| ホスト報告ミュート | `microphone-off`、赤 | `MUTED` / `PC報告` |
| ホスト報告解除 | `microphone`、緑 | `MIC ON` / `PC報告` |
| BLE接続済み・未同期 | マイク＋疑問符、黄 | `UNKNOWN` / `PC状態未取得` |
| 操作送信後 | 最終状態を薄く表示＋進行表示 | `反映待ち` |
| 既知状態と同じ値を送信 | 変化前と同じ表示 | `変化なし` |
| 切断 | グレー | `未接続` |
| ペアリング待ち | Bluetooth表示 | `PCから接続してください` |

色だけで区別せず、アイコン形状とテキストも変える。振動は有効／無効を設定可能とし、ホスト報告が変化した際に短い振動を1回出す。送信成功だけで反映完了の振動を出さない。

Material Design Iconsは **Pictogrammers / TemplarianのMaterialDesign** と解釈し、`microphone`と`microphone-off`を採用候補とする。SVG原本、取得元、固定した版、ライセンスを保持し、必要サイズのマスクまたはRGB565アセットへ変換する。実行時のSVGパーサーは不要。[S7]

設定項目は、輝度、振動、ペアリング、登録解除、接続・同期診断、共存版のUserDemo復帰。アプリ設定のみを初期化する処理と、ボンド削除は分離する。接続維持中は画面を減光できるが、初期版では自動Deep Sleepに入らない。減光解除の最初のタップはミュート操作に使わない。

## 7. 内部構成

| モジュール案 | 責務 |
|---|---|
| `app/MuteApp` | イベント処理、操作の調停 |
| `domain/MuteState` | ホスト報告、反映待ち、期限、接続世代 |
| `ble/TelephonyHid` | GATT・Report Map・入出力、接続管理 |
| `ui/Renderer` | 円形レイアウト、アイコン、設定画面 |
| `storage/Settings` | NVS設定、バージョン管理 |
| `app/FirmwareSwitch` | UserDemoへの復帰と起動時脱出口 |

BLEコールバックは受信内容をコピーしてキューに渡し、描画や`M5.*`を呼ばない。M5Unified、M5GFX、タッチ、電源、振動のAPIは単一のアプリタスクから呼ぶ。NVSは設定変更時のみ保存し、ミュート状態は永続化しない。

キューあふれ・送信失敗・NVS容量不足をログへ残す。状態更新を失った場合は同期不明に戻す。ログは接続、認証結果、通知購読、レポート種別・値、入力、状態遷移、送信エラーを対象とし、ボンド鍵を出力しない。

## 8. UserDemoとの共存

KantanPlayと同じく、独立ファームウェアをOTAスロットに置き、再起動で切り替える。UserDemoへのLVGLアプリ組み込みは行わない。Vibe Watchと同じUserDemo＋追加アプリ3本の共存テーブルを使用する。

| 領域 | Offset | Size | 用途 |
|---|---|---|---|
| nvs | `0x009000` | `0x004000` | 共有設定領域 |
| otadata | `0x00D000` | `0x002000` | 起動先 |
| phy_init | `0x00F000` | `0x001000` | PHY |
| ota_0 | `0x020000` | `0x4F0000` | UserDemo |
| ota_1 | `0x510000` | `0x190000` | App1 |
| ota_2 | `0x6A0000` | `0x190000` | App2 |
| ota_3 | `0x830000` | `0x190000` | App3 |
| storage | `0xA00000` | `0x400000` | UserDemoの画像など |
| coredump | `0xE00000` | `0x010000` | 共有 |

この構成は **UserDemo＋追加アプリ3本の共存**。MuteHidの書き込み先は `--slot 1/2/3`（既定1）で選ぶ。各スロットの上限は1,638,400 bytes。同じイメージをどのスロットにも配置できる。

- UserDemo側ランチャーはApp1〜App3に対応した版を使用し、各スロットのイメージ情報を確認する。別アプリをMuteHidと誤表示しない。
- 起動時のKEY.B判定はM5初期化とBLE初期化より前に実行する。
- 復帰先が存在し、有効なイメージであることを確認し、起動先変更API成功時のみ再起動する。失敗時はアプリに留まる。
- 通常復帰では入力受付停止、必要な解放レポート送信、BLE切断、振動停止を行う。会議側のミュート値は自動変更しない。
- 単独ビルドではUserDemo復帰操作を無効にする。
- 共存用ビルドと単独用ビルドを分け、共存版の通常uploadをガードする。更新は実機パーティション照合後に選択スロットだけを書き換える。
- 初回導入は既存16 MB Flashのバックアップとハッシュを保存し、書き込み範囲を表示する。storageを上書きしない。既存配置が一致していれば不必要な全消去をしない。

NVSは16 KBしかないため、アプリ設定とBLEボンドを含めた容量をPoCで確認する。BLEスタックの保存名はアプリ用名前空間とは別管理になる可能性があり、単に`mutehid`を使うだけで衝突回避完了とはしない。NVS初期化エラー時の自動全消去は禁止し、診断と復旧手順を用意する。

現行UserDemoの工場出荷リセットはNVS全体を消去するため、MuteHid設定とBLEボンドも失われる前提で再登録手順を用意する。通常のファームウェア切り替えでは設定とボンドを維持する。

## 9. 実装順序と受け入れ条件

### Phase 0: 通信だけのPoC

最小画面とBLE TelephonyのInput/Outputを実装し、まずWindowsで列挙、通知購読、ホストからのOutput書き込みを検証する。診断用ホストから書けることと、Meetが自動で書くことを区別する。

Meetでは、利用可能なら設定→音声→通話コントロールからデバイス接続・ブラウザ権限付与を行う。候補に表示されない場合はその結果を記録し、成功扱いにしない。[S1]

| 環境 | HID列挙 | デバイス選択 | 本体→会議 | 会議→本体 | 判定 |
|---|---|---|---|---|---|
| Windows 11 / Chrome / Meet / BLE直結 | 成立 | 成立 | 成立 | 成立（変化時のみ） | 公式Bluetooth対応範囲外だが双方向動作を実測。OS・ブラウザ版などの記録は要補完 |
| Windows 11 / Edge / Meet / BLE直結 | 未検証 | 未検証 | 未検証 | 未検証 | 追加確認 |
| Windows / Teams・Zoom | 未検証 | 対象アプリの設定で確認 | 未検証 | 未検証 | アプリごとに記録 |

検証記録にはOSビルド、ブラウザ・会議アプリ版、Bluetoothアダプター、ファームウェア版、Report Map、PnP ID、権限設定、送受信ログを含める。

Phase 0で判明した実装上の要件を記録する。ボンド済みホストは再接続時にInput CCCDを書き直さないため、CCCDの永続化がないと本体の再起動後にInputだけ送れなくなる（Outputの受信は続くため気付きにくい）。この症状はホスト側の登録解除・再ペアリングでのみ回復していた。

### Phase 1: 操作・同期・UI

PoCで実用的な入力経路が確認できたら通常画面と状態管理を実装する。

| 試験 | 合格条件 |
|---|---|
| 本体ボタン・タップ各20回 | 1操作につき会議側が1回だけ切り替わる |
| PC側のミュート操作20回 | 有効なホスト出力に応じ本体表示が追従する |
| 初期状態がミュート／解除の両方 | 接続だけで会議側の状態を変更しない |
| 初回Outputなし・応答停止 | 誤って既知状態を表示せず、1秒の反映待ち後は不明となる |
| 既知状態と同じ値の送信 | 変化通知が来なくても`Unknown`へ落ちず、既知状態を維持する |
| 同時操作・連打 | 二重トグル、フィードバックループ、永久反映待ちがない |
| 切断・PCスリープ・再起動後の再接続10回 | 古い操作を再生せず、初回通知まで不明表示。再ペアリングなしでInputを送信できる |
| 会議終了・再参加・別タブ・デバイス権限解除 | 表示の有効範囲と通知挙動を確認し、制約を記録 |
| 1時間の接続維持 | クラッシュや意図しないミュート変更がない |

性能目標は入力確定から送信開始まで100 ms以内、Output受信から描画まで100 ms以内。会議アプリまで含む往復は通常500 ms以内を目標に計測するが、ホスト依存のためファームウェア単体で保証しない。

完全同期は「対象環境で両方向を検証済み」の場合のみ表記する。片方向だけ動く場合は限定モードとし、要求R4達成とは扱わない。実装検証では状態遷移のホストテスト、Report Map解析、実機ログを使う。

### Phase 2: 共存・配布準備

単独・共存両ビルドの成功、イメージサイズ`0x4F0000`以内、パーティション一致、通常uploadガード、UserDemoとの10往復、起動時KEY.B脱出、ボンドと設定の保持、NVS不足時に共有設定を消さないことを確認する。アイコンと移植コードのライセンス表記、書き込み・再ペアリング・復旧手順を整備する。

### PoC不成立時

Windows＋Meetで本体から操作できなければ、BLE直結方式では主要用途未達と記録する。UIだけを完成させて対応済みにしない。次の案を具体的な追加仕様として比較する。

1. PC常駐ソフト／ブラウザ拡張を介する方式。会議アプリ状態の取得と操作経路の両方が必要。OS入力デバイスのミュートだけを変更しても同等とは扱わない。
2. BLEとUSB HIDを中継する外付け受信機。PCにはUSB Telephonyとして接続し、本体との無線通信と出力の返送を行う。追加ハードウェアが必要。
3. 本体をUSB HIDへ変更する方式。無線の要求を変更する案であり、USB配線・書き込み経路を別途検証する。

いずれも本ドラフトで導入を決定するものではなく、主要要件を満たせないことが判明した後の判断材料とする。

## 10. 実装前に確定する事項

1. ~~WindowsでのBLE Telephony列挙とMeetへの入出力の成立可否。~~ Phase 0で成立を確認（Windows 11＋Chrome＋Meet）。他アプリ・他ブラウザは未確認。
2. Phone Muteの解釈はPhase 0で状態値方式と確認。最終Report Mapは未確定。
3. ~~BLE PnP IDとして使用可能なVID/PIDとメーカー識別文字列。~~ V-USB共有ID `0x16C0`/`0x05DF` と識別文字列を§4.4に確定。Windows + Chromeでは製品名が表示されないホスト側の制約も確認済み。
4. 実機のOS・ブラウザの検証対象バージョン。
5. BLEボンドと永続化したCCCDを含む共有NVS容量と、UserDemo側の保存領域との衝突有無。

これらは仕様作成を止める前提条件ではなく、Phase 0で解決する技術課題とする。

## 11. 参照資料

外部資料の確認日: 2026-09-09。動作確認済みという意味ではない。

- **[S1]** [Google Meet: Use call controls](https://support.google.com/meet/answer/12562325?hl=en) — OS・ブラウザの公式対応範囲と接続手順。
- **[S2]** [push-to-talk-pico](https://github.com/masawada/push-to-talk-pico)、[HID descriptor](https://github.com/masawada/push-to-talk-pico/blob/main/src/usb_descriptors.c)、[main.c](https://github.com/masawada/push-to-talk-pico/blob/main/src/main.c) — USBレポートとホスト出力受信。取得時Git blob SHAはdescriptorが`cf421cd2c9dc50b78a27cd189f2ed80bf4814c8e`、mainが`b686f7df221d34e9368cebc576b1da3492f6831c`。READMEはMITと記載。移植時に必要な著作権・許諾表示を保持する。
- **[S3]** [Espressif ESP32-S3 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf) — BLEのハードウェア前提。
- **[S4]** [ESP-IDF v5.5 esp_hid_device](https://github.com/espressif/esp-idf/tree/v5.5/examples/bluetooth/esp_hid_device) — ESP32-S3対応のBLE HIDサンプル。Telephony互換性を保証する資料ではない。
- **[S5]** [Bluetooth SIG HOGP Test Suite](https://files.bluetooth.com/wp-content/uploads/2024/10/HOGP.TS_.p11.pdf) — HID・Device Information・BatteryとPnP IDの検証項目。実装時には採用版の規範仕様も照合する。
- **[S6]** [V-USB USB-IDs-for-free.txt](https://raw.githubusercontent.com/obdev/v-usb/refs/heads/master/usbdrv/USB-IDs-for-free.txt) — 共有IDの用途と条件。
- **[S7]** [MaterialDesign](https://github.com/Templarian/MaterialDesign)、[ライセンス](https://github.com/Templarian/MaterialDesign/blob/master/LICENSE) — アイコン候補と利用条件。Google Material Iconsとは区別する。
- **[S8]** [WebHID: Unknown Device（WICG/webhid#112）](https://github.com/WICG/webhid/issues/112) — WindowsのBLE HIDでChromiumが製品名を取得できない件。`HidD_GetProductString`がBLE HIDに未対応で、Chromium側の対応（`Windows.Devices.Bluetooth`の使用）が必要。Chromium側の追跡は crbug.com/1455500。確認日 2026-09-10、issueはopen。
