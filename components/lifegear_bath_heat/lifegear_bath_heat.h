#pragma once
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include <array>
#include <map>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <driver/rmt_rx.h>

namespace esphome {
namespace lifegear_bath_heat {

// =============================================================================
//  中繼(man-in-the-middle)架構
//    面板 TX ─→ panel_rx_pin   (ESP 收面板) ──鏡射──→ machine_tx_pin ─→ 機器 RX
//    機器 TX ─→ machine_rx_pin (ESP 收機器) ──鏡射──→ panel_tx_pin   ─→ 面板 RX
//  鏡射在 GPIO ISR 內完成(數 us),面板↔機器照常對話;ESP 同時解碼面板幀顯示狀態。
//  override 模式: 停止 面板→機器 的鏡射,改由本元件 bit-bang 送出指定狀態幀。
//
//  線路格式: 封包 = 81 wire bits, 脈衝 0→500us 1→1000us, 電位交替, idle=LOW
//            wire[0:21]=固定表頭, wire[21:81]=Manchester → 30 bits 資料
// =============================================================================

// ⚠ 真正固定表頭只有 17 wire bits;舊 21 位表頭誤含前 2 個資料位(過去恰好恆為"01")。
//   乾燥等運轉模式下前 2 位會變(實測見過"11") → RX 用 17 位表頭 + 32 資料位(前2=旗標,後30=狀態)。
//   TX 仍送 21 位表頭+30 位(等同旗標"01"),為已實測可控的組合。
static const char HEADER_BITS[] = "101100101010110010110";   // TX 用 (17 表頭 + 旗標"01"的 Manchester)
static const char HEADER17[] = "10110010101011001";          // RX 表頭 (17 wire bits)
static const int WIRE_BITS = 81;
static const int FRAME_GAP_US = 3000;  // 大於此值視為封包間隔

struct ModeEntry {
  const char *bits;
  const char *name;
};

class LifegearBathHeat;

enum LifegearSwitchKind : uint8_t {
  SWITCH_LEFT_LIGHT = 0,
  SWITCH_RIGHT_LIGHT = 1,
  SWITCH_BUZZER = 2,
  SWITCH_VENT24 = 3,
};

enum LifegearButtonKind : uint8_t {
  BUTTON_OFF = 0,
  BUTTON_FILTER_RESET = 1,
  BUTTON_RETURN_TO_PANEL = 2,
};

class LifegearModeSelect : public select::Select {
 public:
  explicit LifegearModeSelect(LifegearBathHeat *parent) : parent_(parent) {}

 protected:
  void control(size_t index) override;
  LifegearBathHeat *parent_;
};

class LifegearFilterThresholdSelect : public select::Select, public Component {
 public:
  explicit LifegearFilterThresholdSelect(LifegearBathHeat *parent) : parent_(parent) {}
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void control(size_t index) override;
  LifegearBathHeat *parent_;
  ESPPreferenceObject pref_;
};

class LifegearHeatTemperatureSelect : public select::Select, public Component {
 public:
  explicit LifegearHeatTemperatureSelect(LifegearBathHeat *parent) : parent_(parent) {}
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  // 溫控模式中由機器實際值同步偏好 (發佈 + 保存); 其他路徑一律走 control()。
  void sync_actual(size_t index);

 protected:
  void control(size_t index) override;
  LifegearBathHeat *parent_;
  ESPPreferenceObject pref_;
};

class LifegearModeTimerNumber : public number::Number, public Component {
 public:
  LifegearModeTimerNumber(LifegearBathHeat *parent, int index, int initial_minutes)
      : parent_(parent), index_(index), initial_minutes_(initial_minutes) {}
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void control(float value) override;
  LifegearBathHeat *parent_;
  int index_;
  int initial_minutes_;
  ESPPreferenceObject pref_;
};

class LifegearSwitch : public switch_::Switch {
 public:
  LifegearSwitch(LifegearBathHeat *parent, LifegearSwitchKind kind) : parent_(parent), kind_(kind) {}

 protected:
  void write_state(bool state) override;
  LifegearBathHeat *parent_;
  LifegearSwitchKind kind_;
};

class LifegearButton : public button::Button {
 public:
  LifegearButton(LifegearBathHeat *parent, LifegearButtonKind kind) : parent_(parent), kind_(kind) {}

 protected:
  void press_action() override;
  LifegearBathHeat *parent_;
  LifegearButtonKind kind_;
};

class LifegearBathHeat : public Component {
 public:
  void set_panel_rx_pin(int p) { this->panel_rx_pin_ = p; }
  void set_panel_tx_pin(int p) { this->panel_tx_pin_ = p; }
  void set_machine_rx_pin(int p) { this->machine_rx_pin_ = p; }
  void set_machine_tx_pin(int p) { this->machine_tx_pin_ = p; }
  void set_rmt_rx_pin(int p) { this->rmt_rx_pin_ = p; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;   // 重開機前結算濾網時數 (修 OTA 歸零 bug)
  float get_setup_priority() const override { return setup_priority::LATE; }

  // --- 狀態查詢 (供 HA 顯示) ---
  std::string get_panel_state() const {
    if (this->mtx_ == nullptr) return "";   // setup 前防禦
    xSemaphoreTake(this->mtx_, portMAX_DELAY);
    std::string s = this->panel_state_;
    xSemaphoreGive(this->mtx_);
    return s;
  }
  std::string get_mode_name() const;
  uint32_t get_panel_frames() const { return this->panel_frames_; }
  optional<std::string> get_temp_str();      // 由幀解出目前溫控設定 (25-35), 供 select 回填
  bool is_heat_temp_mode();                  // 目前是否為暖房溫控模式
  uint32_t get_machine_frames() const { return this->machine_frames_; }
  uint32_t get_panel_edges() const { return this->p_edges_; }
  uint32_t get_machine_edges() const { return this->m_edges_; }

  // --- 24h 換氣記憶 + 各模式定時 (斷電記憶由 YAML entity restore 推入) ---
  void set_vent24_enabled(bool en);
  bool get_vent24_enabled() const { return this->vent24_enabled_; }
  void request_off();                        // 關機請求: 24h 設定開啟時改送 24h換氣
  void set_mode_timer(int idx, int minutes); // idx: 0暖沐 1暖控 2涼強 3涼弱 4換強(中共用) 5換弱 6乾節 7乾快
  int get_timer_left_min();                  // 定時剩餘(分,無條件進位), 無定時=-1
  void tick_timer();                         // worker 1Hz 呼叫

  // --- 濾網清潔提醒 (運轉時數累計; 起訖時間相減法, NVS 節流寫入) ---
  void set_filter_threshold_h(int hours);    // 提醒門檻 (小時, 由 yaml number restore 推入)
  void reset_filter();                       // 濾網已清潔: 累計歸零並立即存檔
  float get_runtime_h();                     // 自上次清潔以來的運轉小時 (含進行中區間)
  bool is_filter_due();
  void tick_runtime();                       // worker 1Hz 呼叫

  // --- 控制 (yaml 用語意 API, 位元碼集中在 cpp 的 CMDS/TEMPS 表) ---
  void cmd_mode(const std::string &key);  // heat_bath/heat_temp/cool_hi/cool_lo/vent_hi/vent_mid/vent_lo/dry_eco/dry_fast/vent24
  void set_heat_temp(int t);              // 暖房溫控溫度 25-35 (非溫控模式自動忽略)
  void set_override_state(const std::string &data_bits);
  void trigger_pulse(const std::string &data_bits);   // 送指定幀 N 次後復原
  void toggle_bits(const std::string &positions);     // 在「當前狀態」上翻轉指定 bit 後送出
  void set_state_bit(int pos, bool val);              // 在「當前狀態」上設定某 bit 並持續送出(照明用)
  bool get_light(int pos);                            // 讀燈態: pos 22=右燈, 23=左燈
  void toggle_light(int pos);                         // 切換燈態 (override 持續送出)
  void trigger_keypress();                            // 手動送一幀按鍵事件幀 (旗標11+pos14=0=機器按鍵音)
  void set_buzzer_enabled(bool en);                   // 蜂鳴確認: HA 指令是否附帶按鍵音
  bool get_buzzer_enabled() const { return this->buzzer_enabled_; }
  void send_keypress_();
  void queue_keypress_(const std::string &state30);
  void queue_light_keypress_(const std::string &state30);   // 燈專用: b0=幀內燈位(22|23)
  volatile bool kp_pending_{false};
  char kp_flag0_{'1'};   // 按鍵幀旗標 b0 (= 右燈狀態反轉)
  void set_override(bool on);
  bool is_override() const { return this->override_; }

  // --- Native ESPHome entities (created by this component; no YAML templates) ---
  void set_translation(const std::string &key, const std::string &value) { this->translations_[key] = value; }
  void set_mode_select(LifegearModeSelect *entity) { this->mode_select_ = entity; }
  void set_filter_threshold_select(LifegearFilterThresholdSelect *entity) {
    this->filter_threshold_select_ = entity;
  }
  void set_heat_temperature_select(LifegearHeatTemperatureSelect *entity) {
    this->heat_temperature_select_ = entity;
  }
  void set_left_light_switch(LifegearSwitch *entity) { this->left_light_switch_ = entity; }
  void set_right_light_switch(LifegearSwitch *entity) { this->right_light_switch_ = entity; }
  void set_buzzer_switch(LifegearSwitch *entity) { this->buzzer_switch_ = entity; }
  void set_vent24_switch(LifegearSwitch *entity) { this->vent24_switch_ = entity; }
  void set_mode_timer_number(int index, LifegearModeTimerNumber *entity) {
    if (index >= 0 && index < 8) this->mode_timer_numbers_[index] = entity;
  }
  void set_filter_hours_sensor(sensor::Sensor *entity) { this->filter_hours_sensor_ = entity; }
  void set_timer_left_sensor(sensor::Sensor *entity) { this->timer_left_sensor_ = entity; }
  void set_host_status_text_sensor(text_sensor::TextSensor *entity) { this->host_status_text_sensor_ = entity; }
  void set_current_mode_text_sensor(text_sensor::TextSensor *entity) { this->current_mode_text_sensor_ = entity; }
  void set_raw_state_text_sensor(text_sensor::TextSensor *entity) { this->raw_state_text_sensor_ = entity; }
  void set_filter_due_binary_sensor(binary_sensor::BinarySensor *entity) {
    this->filter_due_binary_sensor_ = entity;
  }
  void set_override_binary_sensor(binary_sensor::BinarySensor *entity) {
    this->override_binary_sensor_ = entity;
  }
  void set_left_light_state_binary_sensor(binary_sensor::BinarySensor *entity) {
    this->left_light_state_binary_sensor_ = entity;
  }
  void set_right_light_state_binary_sensor(binary_sensor::BinarySensor *entity) {
    this->right_light_state_binary_sensor_ = entity;
  }
  int get_mode_index() const;
  std::string get_localized_mode_name();
  std::string get_localized_machine_status() const;

  // --- 內部 (ISR / task 需存取) ---
  volatile bool override_{false};
  bool standalone_{false};   // true = 取代面板模式(無面板側腳位, 恆為 master 送幀)
  volatile bool accept_cmd_{false};  // 開機保護: 啟動後才接受控制
  std::string boot_pending_bits_;    // 開機保護期間暫存的最後一道控制命令 (主迴圈單執行緒存取)
  volatile int64_t p_last_edge_{0};
  volatile int p_idx_{-1};
  volatile uint16_t p_dur_[WIRE_BITS + 4]{};
  volatile bool p_ready_{false};
  volatile uint32_t machine_frames_{0};
  volatile uint32_t manch_fail_{0};   // Manchester 違規丟棄計數 (診斷)
  volatile uint32_t hdr_fail_{0};     // 表頭不符計數 (診斷)
  volatile uint32_t p_edges_{0};
  volatile uint32_t m_edges_{0};
  volatile int64_t m_last_edge_{0};
  volatile int m_idx_{-1};
  volatile uint16_t m_dur_[160]{};
  volatile int m_len_{0};
  volatile bool m_ready_{false};
  int panel_rx_pin_{-1};    // -1 = 獨立模式(取代面板, 無中繼)
  int panel_tx_pin_{-1};
  int machine_rx_pin_{-1};
  int machine_tx_pin_{19};
  int rmt_rx_pin_{4};   // RMT 專用擷取腳(機器TX分接), 與 GPIO16 ISR 並存
  SemaphoreHandle_t sem_{nullptr};
  SemaphoreHandle_t mtx_{nullptr};  // 控制狀態互斥鎖: main task(API) vs worker task

  // --- RMT RX: 機器方向硬體精準擷取 (與 ISR 鏡射並存) ---
  rmt_channel_handle_t rx_chan_{nullptr};
  rmt_symbol_word_t rx_raw_[256]{};
  rmt_symbol_word_t rx_done_buf_[256]{};
  volatile int rx_done_num_{0};
  volatile bool rx_done_{false};
  volatile uint32_t rx_cb_count_{0};
  volatile int64_t m_cb_us_{0};    // RMT callback 時刻 (機器幀結束≈此值-30ms)
  volatile int64_t p_done_us_{0};  // 最近一次面板幀完成時刻
  bool rx_ok_{false};
  volatile int rx_arm_err_{-99};
  rmt_receive_config_t rx_cfg_{};

  void decode_machine_rmt();
  void decode_panel_frame();
  void decode_machine_frame();
  std::string get_machine_state() const { return this->machine_state_; }
  std::string get_machine_mode() const {
    if (this->mtx_ == nullptr) return this->machine_mode_;
    xSemaphoreTake(this->mtx_, portMAX_DELAY);
    std::string s = this->machine_mode_;
    xSemaphoreGive(this->mtx_);
    return s;
  }
  bool get_machine_autovent() const { return this->machine_autovent_; }
  float get_machine_temp_raw() const {   // 機器溫度遙測原始值 (500µs 單位, 未校準; 0=尚無讀值)
    uint32_t us = this->machine_temp_us_;
    return us ? us / 500.0f : 0.0f;
  }
  void transmit_override();
  void tick_tx();
  void trigger_pulse_raw(const std::string &data_bits);

 protected:
  std::string panel_state_;
  std::string panel_flags_;   // 面板幀前 2 資料位 (旗標, 意義建表中)
  std::string machine_state_;
  std::string machine_mode_{"等待中"};
  // 公開的「主機回報」只採用模式切換後的新主機封包，且回報超時後回到等待回報。
  // 這些欄位都由 mtx_ 保護，避免 main task 與 RMT worker 跨核心讀寫不同步。
  uint32_t mode_request_seq_{1};
  uint32_t machine_confirmed_request_seq_{0};
  uint32_t autovent_confirmed_request_seq_{0};
  uint32_t machine_frame_request_seq_{0};
  uint32_t last_machine_frame_ms_{0};
  const char *m_cand_{nullptr};  // 長頁投票候選 (連續一致計數)
  int m_cand_cnt_{0};
  int m_unmatched_{0};           // 連續尚未建表的輪播頁數 (僅供診斷，不覆寫有效狀態)
  uint32_t ht_win_{0};           // 溫控熱態統計視窗 (近20頁 burst2 單字符旗標)
  volatile uint32_t machine_temp_us_{0};  // 長頁溫度欄位原始 µs (最近一次)
  bool machine_autovent_{false};  // 機器自主低速換氣中 (24h運轉或停機餘轉, 由心跳頁尾段判別)
  bool vent24_enabled_{false};              // 24h 換氣設定 (由 yaml switch restore 推入)
  bool buzzer_enabled_{false};              // 蜂鳴確認設定 (由 yaml switch restore 推入)
  std::string kp_bits_;                     // 待送的按鍵事件幀內容
  int mode_timer_min_[8]{60, 60, 60, 60, 60, 180, 180, 60};  // 出廠預設 (手冊 p.4)
  volatile int32_t timer_deadline_s_{0};    // 定時到期時刻 (esp_timer 秒), 0=無
  volatile int cur_timer_idx_{-1};          // 目前計時中的模式 idx
  int filter_threshold_h_{200};             // 濾網提醒門檻 (小時)
  volatile uint32_t runtime_total_s_{0};    // 已結算累計運轉秒數 (NVS 持久)
  volatile int32_t run_start_s_{-1};        // 本次運轉開始時刻 (uptime 秒), -1=未運轉
  uint32_t last_saved_total_s_{0};          // 上次寫入 NVS 時的累計值 (節流用)
  ESPPreferenceObject runtime_pref_;
  std::string m_last_raw_;
  std::string last_raw_;
  uint32_t panel_frames_{0};
  std::string override_bits_{"100011111111111110000100111111"};
  std::string pulse_bits_;
  volatile int pulse_count_{0};
  bool saved_override_{false};
  uint16_t tx_dur_[WIRE_BITS]{};
  bool tx_ready_{false};
  void build_tx_(const std::string &data_bits);
  void update_entities_();
  std::string translate_(const std::string &key) const;

  std::map<std::string, std::string> translations_;
  LifegearModeSelect *mode_select_{nullptr};
  LifegearFilterThresholdSelect *filter_threshold_select_{nullptr};
  LifegearHeatTemperatureSelect *heat_temperature_select_{nullptr};
  LifegearSwitch *left_light_switch_{nullptr};
  LifegearSwitch *right_light_switch_{nullptr};
  LifegearSwitch *buzzer_switch_{nullptr};
  LifegearSwitch *vent24_switch_{nullptr};
  std::array<LifegearModeTimerNumber *, 8> mode_timer_numbers_{};
  sensor::Sensor *filter_hours_sensor_{nullptr};
  sensor::Sensor *timer_left_sensor_{nullptr};
  text_sensor::TextSensor *host_status_text_sensor_{nullptr};
  text_sensor::TextSensor *current_mode_text_sensor_{nullptr};
  text_sensor::TextSensor *raw_state_text_sensor_{nullptr};
  binary_sensor::BinarySensor *filter_due_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *override_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *left_light_state_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *right_light_state_binary_sensor_{nullptr};
  uint32_t last_entity_update_ms_{0};
  uint32_t last_filter_hours_check_ms_{0};
  uint32_t last_raw_state_publish_ms_{0};
  bool filter_hours_dirty_{true};
};

}  // namespace lifegear_bath_heat
}  // namespace esphome
