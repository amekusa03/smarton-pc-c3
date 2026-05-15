# SmartOn PC

> **WIP** — 現在開発中です。動作保証はありません。

ESP32-C3 を使い、Ubuntu PCをスマートホームデバイスとして **Matter** プロトコルで制御するファームウェアです。  
Google Home / Nest Hub からの音声操作や自動化でPCの電源をON/OFFできます。

---

## 機能

| 機能 | 手段 |
|------|------|
| 電源ON | フォトカプラ → 電源ボタン短押し（500ms） |
| フリーズ回復 | フォトカプラ → 電源ボタン長押し（4〜5秒） |
| 死活監視 | Ping（ホスト名 or mDNS） |
| ファクトリーリセット | GPIO9 長押し（3秒） |

- **WoL（Wake on LAN）は不採用** — 信頼性の問題による
- シャットダウンはSSH等ソフトウェアで対応（リレーのメインフローには含めない）
- ネット未接続時の監視失敗は許容（スマートホーム自体が機能しないため）

---

## ハードウェア構成

- **マイコン**: Generic ESP32-C3
- **ホストPC**: Ubuntu
- **スイッチング**: フォトカプラ（PC817）経由でマザーボードの PWR SW ピンを短絡
- **コントローラー**: Google Nest Hub / Android スマートフォン

### GPIO割り当て

| GPIO | 役割 | 備考 |
|------|------|------|
| GPIO 20 | OUTPUT: PWR SW制御 | フォトカプラ（PC817）経由 |
| GPIO 4  | INPUT: PWR LED読み取り | 参考値（Ping監視がメイン） |
| GPIO 9  | INPUT: ファクトリーリセット | 内部プルアップ、LOW=押下 |

> **フォトカプラ回路（アクティブLOW）**: GPIO 20 → 200Ω → PC817 LED → GND  
> 極性は `pc_control.c` の `PWR_SW_ACTIVE_HIGH` マクロで切り替え可能。

---

## ソフトウェアスタック

| レイヤー | 名称 | バージョン |
|---------|------|-----------|
| OS / SDK | ESP-IDF | 5.4.1 |
| Matter ラッパー | esp-matter | `$ESP_MATTER_PATH` で指定 |
| Matter 本体 | ConnectedHomeIP (CHIP) | esp-matter サブモジュール |

Matterデバイスタイプ: **On/Off Plug-in Unit**

---

## ビルド

### 前提条件

- ESP-IDF 5.4.1 がセットアップ済み
- esp-matter がセットアップ済み（`ESP_MATTER_PATH` 環境変数に設定）

### WiFi認証情報の設定

`main/wifi_creds.h` を作成してSSIDとパスワードを記載します（`.gitignore` 済み）:

```c
#define WIFI_SSID     "your-ssid"
#define WIFI_PASSWORD "your-password"
```

### ビルド & フラッシュ

```bash
. $IDF_PATH/export.sh
. $ESP_MATTER_PATH/export.sh

idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

### Matterコミッショニング

フラッシュ後、シリアルモニタにQRコードとセットアップPINが出力されます。  
Google Home アプリ等でスキャンしてデバイスを登録してください。

開発・テスト段階ではテスト用DAC（Device Attestation Certificate）を使用します。

---

## ライセンス

本プロジェクトのアプリケーションコード（`main/`）は [Apache License 2.0](LICENSE) です。  
使用している依存コンポーネントも同ライセンス（一部 MIT）です。詳細は [doc/spec.md](doc/spec.md) を参照。
