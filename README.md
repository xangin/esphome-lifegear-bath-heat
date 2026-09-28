# ESPHome Lifegear 樂奇浴室暖風機

用一片 ESP32 取代樂奇（Lifegear）浴室暖風機的原廠有線面板，直接與暖風機主機通訊，把所有模式、照明、定時與濾網提醒整合進 Home Assistant。

這是一個 ESPHome [external component](https://esphome.io/components/external_components.html)，協定由逆向工程取得，在 **BD-125W2** 實機運作。

<p align="center">
  <img src="images/product.jpg" alt="樂奇浴室暖風機與原廠面板" width="300">
</p>

> [!WARNING]
> 暖風機是 220V 電器。拆裝面板與接線前，請先關閉該迴路的斷路器。本專案為非官方改裝，與樂奇電器無關，風險請自行評估。

## 功能

- **模式**：乾燥（節電／快速）、暖房（沐浴／溫控）、涼風（強／弱）、換氣（強／中／弱）、24 小時換氣、關機
- **暖房溫控溫度**：25–35 °C
- **照明**：左燈、右燈各自開關，並回報實際狀態
- **定時**：八個模式各自獨立的自動關機時間（5–720 分鐘），並回報剩餘時間
- **濾網**：累計運轉時數、清潔提醒門檻（720／1440／2160 小時）、重設
- **主機回報**：從主機送回的訊號解碼實際運轉狀態，不只是「面板送了什麼」
- **按鍵音**：可選擇指令是否附帶主機的「嗶」聲
- **斷電記憶**：定時、濾網門檻、24h 換氣、按鍵音等設定重開機後保留
- **顯示語言**：模式與狀態字串可選繁體中文或英文

## 支援硬體

| 項目 | 狀態 |
|---|---|
| 暖風機 | BD-125W2 已實測。使用相同 4-pin 面板排線的其他樂奇機型可能相容，但未驗證 |
| ESP32、ESP32-S3 | 支援（需使用 `esp-idf` 框架） |
| ESP32-C3 等 RISC-V 晶片 | 目前不支援（元件直接存取 GPIO 暫存器） |
| ESPHome | 在 2025.11.5 開發與測試 |

GPIO 必須在 0–31 之間。

## 接線

原廠面板背後有一組 4-pin 排針（JK2），主機透過一條排線接上來。拔掉原廠面板，把這條排線改接到 ESP32 即可。

<p align="center">
  <img src="images/panel-original.jpg" alt="原廠面板 BD-125W2" width="32%">
  <img src="images/panel-pcb-pinout.jpg" alt="原廠面板電路板 JK2 排針：TX、RX、VDD、GND" width="64%">
</p>

面板電路板上的絲印由左至右為 TX、RX、VDD、GND，是**面板的視角**：TX 是面板送往主機的線，RX 是主機送往面板的線。主機端排線的對應如下：

<p align="center">
  <img src="images/cable-pinout.jpg" alt="主機排線定義：GND、3V、RX、TX" width="320">
</p>

| 排線 | 用途 | 接到 ESP32（範例） |
|---|---|---|
| GND | 地 | GND |
| 3V | 原廠面板的電源 | 不接，ESP32 請用獨立電源（USB 5V） |
| TX | 面板 → 主機 RX | GPIO19（`machine_tx_pin`） |
| RX | 主機 TX → 面板 | GPIO4（`rmt_rx_pin`） |

訊號為 3.3V 準位，可直接接 ESP32 GPIO。3V 腳的電流容量未經驗證，不建議拿來供電給 ESP32。排線顏色可能因批次不同，請以排針位置為準。

### 使用 Guition ESP32-S3-4848S040 觸控面板

如果想要一片有螢幕的面板取代原廠面板，Guition 4 吋 480×480 開發板背面的 relay 排針就能直接接暖風機：

<p align="center">
  <img src="images/guition-4848s040-header.png" alt="Guition ESP32-S3-4848S040 relay 排針定義" width="360">
</p>

| 排針 | GPIO | 接到排線 | 元件設定 |
|---|---|---|---|
| relay2 | GPIO2 | TX | `machine_tx_pin: 2` |
| relay3 | GPIO1 | RX | `rmt_rx_pin: 1` |
| GND | — | GND | — |

本 repo 只提供暖風機元件；觸控面板的 LVGL 介面不在範圍內。

## 安裝

在你的 ESPHome 設定加入：

```yaml
esp32:
  board: esp32dev
  framework:
    type: esp-idf

external_components:
  - source: github://xangin/esphome-lifegear-bath-heat@main
    components: [ lifegear_bath_heat ]

lifegear_bath_heat:
  id: erv
  machine_tx_pin: 19   # → 排線 TX（主機 RX）
  rmt_rx_pin: 4        # ← 排線 RX（主機 TX）
```

完整可編譯的範例（含所有 Home Assistant 實體）在 [`example/lifegear-bath-heat.yaml`](example/lifegear-bath-heat.yaml)。範例需要 `wifi_ssid` 與 `wifi_password` 兩個 secrets，格式可參考 [`example/secrets.yaml.example`](example/secrets.yaml.example)。

## 設定選項

| 選項 | 必填 | 說明 |
|---|---|---|
| `machine_tx_pin` | 是 | ESP 送往主機的 GPIO，接排線 TX |
| `rmt_rx_pin` | 建議 | 接收主機訊號的 GPIO，接排線 RX。用 RMT 硬體擷取並解碼主機回報；不設就沒有主機實際狀態 |
| `display_language` | 否 | 模式與狀態字串語言：`zh-TW`（預設）或 `en` |
| `timer_defaults` | 否 | 八個模式的預設定時（分鐘，5–720）：`heat_bath`、`heat_temp`、`cool_high`、`cool_low`、`vent_high`、`vent_low`、`dry_eco`、`dry_fast` |
| `panel_rx_pin`、`panel_tx_pin`、`machine_rx_pin` | 中繼模式 | 三個一起設定才會啟用中繼模式，見下方說明 |

### 中繼模式（保留原廠面板）

不想拆掉原廠面板時，可以把 ESP32 串在面板與主機中間，面板照常可用，HA 也能控制：

```
面板 TX ──→ panel_rx_pin ──→ machine_tx_pin ──→ 主機 RX
主機 TX ──→ machine_rx_pin ──→ panel_tx_pin ──→ 面板 RX
主機 TX 另分接 rmt_rx_pin（解碼用）
```

```yaml
lifegear_bath_heat:
  id: erv
  panel_rx_pin: 17
  panel_tx_pin: 18
  machine_rx_pin: 16
  machine_tx_pin: 19
  rmt_rx_pin: 4
```

中繼轉發對時序敏感，`logger` 請不要設成 `DEBUG` 等級。

## 範例提供的 Home Assistant 實體

[`example/lifegear-bath-heat.yaml`](example/lifegear-bath-heat.yaml) 用 template 實體呼叫元件方法，建立以下實體：

| 類型 | 實體 |
|---|---|
| 按鈕 | 關機、八個模式（暖房沐浴、暖房溫控、涼風強／弱、換氣強／弱、乾燥節電／快速）、重設濾網 |
| 開關 | 左燈、右燈、蜂鳴確認、24h 換氣啟用 |
| 選擇 | 暖房溫控溫度、濾網提醒時數 |
| 數值 | 八個模式的定時（分鐘） |
| 感測器 | 濾網運轉時數、定時剩餘 |
| 文字感測器 | 目前模式、機器實際模式 |
| 二元感測器 | 濾網清潔通知、左燈狀態、右燈狀態 |

### 元件方法（lambda 用）

| 方法 | 說明 |
|---|---|
| `cmd_mode(key)` | 切換模式。`key`：`heat_bath`、`heat_temp`、`cool_hi`、`cool_lo`、`vent_hi`、`vent_mid`、`vent_lo`、`dry_eco`、`dry_fast`、`vent24` |
| `request_off()` | 關機；24h 換氣啟用時改為進入 24h 換氣 |
| `set_heat_temp(t)` | 暖房溫控溫度 25–35。只在暖房溫控模式中生效，其他模式會被忽略 |
| `get_temp_str()` | 目前溫控溫度；非溫控模式回傳空值 |
| `set_state_bit(pos, on)` / `get_light(pos)` | 照明：`23` 左燈、`22` 右燈 |
| `set_mode_timer(idx, min)` | 設定定時。`idx`：0 暖房沐浴、1 暖房溫控、2 涼風強、3 涼風弱、4 換氣強（換氣中共用）、5 換氣弱、6 乾燥節電、7 乾燥快速 |
| `get_timer_left_min()` | 定時剩餘分鐘；沒有定時回傳 -1 |
| `set_vent24_enabled(on)` | 24h 換氣啟用 |
| `set_buzzer_enabled(on)` | 指令是否附帶主機按鍵音 |
| `set_filter_threshold_h(h)` / `is_filter_due()` | 濾網提醒門檻與是否該清潔 |
| `get_runtime_h()` / `reset_filter()` | 濾網累計運轉時數與歸零 |
| `get_mode_name()` | 目前送出的模式名稱 |
| `get_machine_mode()` | 從主機回報解碼的實際模式 |

## 原生實體平台（免寫 lambda）

元件也內建各類實體平台，不用寫任何 lambda。只宣告需要的項目，沒宣告的就不會編進韌體；每個實體都能用 ESPHome 標準的 `name`、`id`、`icon`、`entity_category` 等選項。

```yaml
lifegear_bath_heat:
  id: bath_heat
  machine_tx_pin: 19
  rmt_rx_pin: 4

select:
  - platform: lifegear_bath_heat
    lifegear_bath_heat_id: bath_heat
    mode:
      name: "Mode 模式"
    heat_temperature:
      name: "Heat Temp 溫控溫度"

switch:
  - platform: lifegear_bath_heat
    lifegear_bath_heat_id: bath_heat
    left_light:
      name: "Left Light 左燈"
    vent24:
      name: "Vent24 24h換氣"
```

<p align="center">
  <img src="images/ha-entities.png" alt="使用原生實體平台時的 Home Assistant 裝置頁面" width="300">
  <br>
  <sub>使用原生實體平台時的 Home Assistant 裝置頁面。此截圖來自 Guition 面板版，其中「面板背光」是面板本身的實體。</sub>
</p>

各平台可用的項目：

| 平台 | 項目 |
|---|---|
| `select` | `mode`、`heat_temperature`、`filter_threshold` |
| `number` | `heat_bath_timer`、`heat_temp_timer`、`cool_high_timer`、`cool_low_timer`、`vent_high_timer`、`vent_low_timer`、`dry_eco_timer`、`dry_fast_timer` |
| `switch` | `left_light`、`right_light`、`buzzer`、`vent24` |
| `button` | `off`、`filter_reset`、`return_to_panel` |
| `sensor` | `filter_hours`、`timer_left` |
| `text_sensor` | `current_mode`、`host_status`、`raw_state` |
| `binary_sensor` | `override`、`filter_due`、`left_light_state`、`right_light_state` |

原生的 `heat_temperature` 會把溫度存成偏好值：不在溫控模式時也能先設定，切進暖房溫控時自動套用。

## 行為說明

- **開機保護**：上電後 15 秒內收到的控制指令不會被丟掉，會暫存最後一道，保護解除後自動送出。開機時預設送「關機」狀態。
- **24h 換氣**：關機狀態下啟用會立即進入 24h 換氣；其他模式運轉中啟用不會打斷，定時到了改回 24h 換氣。停用時若正處於 24h 換氣會立即關機。開機還原設定時不會送出任何指令。
- **定時**：切進有定時的模式即開始倒數，時間到自動關機（24h 換氣啟用時改回 24h 換氣）。運轉中修改定時會以新值重新起算。
- **暖房溫控溫度**：協定裡沒有獨立的溫度欄位，每個溫度都是一組完整的模式碼，所以非溫控模式下無法「只改溫度」。
- **濾網時數**：只在實際運轉時累計，節流寫入 NVS 以保護 flash。

## 已知限制

- 只在 BD-125W2 實測過。
- 不支援 ESP32-C3 等 RISC-V 晶片。
- 在 ESPHome 2025.11.5 開發與測試，新版 ESPHome 未驗證。

## English

An ESPHome external component that replaces the wired wall panel of a Lifegear bathroom heater/ventilator (tested on BD-125W2) with an ESP32, talking to the heater's main unit directly. It exposes every mode (dry, heat, cool, vent, 24h ventilation), heat set-point (25–35 °C), both lights, per-mode auto-off timers, filter runtime reminders, and the machine's decoded actual state to Home Assistant.

Wire the 4-pin panel cable to an ESP32 or ESP32-S3 (esp-idf): cable **TX** → `machine_tx_pin`, cable **RX** → `rmt_rx_pin`, common GND, and power the ESP separately. See [`example/lifegear-bath-heat.yaml`](example/lifegear-bath-heat.yaml) for a complete configuration. A relay mode keeps the original panel in the loop. ESP32-C3 is not supported. Developed against ESPHome 2025.11.5. Display strings are selectable with `display_language: en`.

This is an unofficial, reverse-engineered project and is not affiliated with Lifegear. Mains voltage is involved — switch off the breaker before opening anything.

## 授權

程式碼以 [MIT License](LICENSE) 釋出。產品照片版權屬樂奇電器所有，僅用於識別機型。
