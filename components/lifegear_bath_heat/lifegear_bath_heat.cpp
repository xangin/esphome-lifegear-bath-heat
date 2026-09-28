#include "lifegear_bath_heat.h"
#include "esphome/core/log.h"
#include <driver/gpio.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <soc/gpio_struct.h>
#include <hal/gpio_ll.h>   // C3 移植: 跨目標 GPIO 暫存器存取 (always_inline, ISR 安全)
#include <sdkconfig.h>     // C3 移植: CONFIG_FREERTOS_UNICORE
#include <cmath>

namespace esphome {
namespace lifegear_bath_heat {

static const char *const TAG = "lifegear_bath_heat";
static constexpr uint32_t FILTER_HOURS_CHECK_INTERVAL_MS = 60000;
static constexpr uint32_t RAW_STATE_PUBLISH_INTERVAL_MS = 5000;
static constexpr uint32_t MACHINE_CONFIRM_TIMEOUT_MS = 10000;

static void publish_select_if_changed_(select::Select *entity, size_t index) {
  if (entity == nullptr || !entity->has_index(index)) return;
  auto current = entity->active_index();
  if (!current.has_value() || current.value() != index) entity->publish_state(index);
}

static void publish_sensor_if_changed_(sensor::Sensor *entity, float value) {
  if (entity == nullptr) return;
  const bool old_nan = std::isnan(entity->state);
  const bool new_nan = std::isnan(value);
  if (!entity->has_state() || old_nan != new_nan || (!new_nan && entity->state != value))
    entity->publish_state(value);
}

static void publish_text_if_changed_(text_sensor::TextSensor *entity, const std::string &value) {
  if (entity != nullptr && (!entity->has_state() || entity->get_state() != value)) entity->publish_state(value);
}

static void publish_binary_if_changed_(binary_sensor::BinarySensor *entity, bool value) {
  if (entity != nullptr && (!entity->has_state() || entity->get_state() != value)) entity->publish_state(value);
}

// =============================================================================
//  Native ESPHome entities
// =============================================================================

void LifegearModeSelect::control(size_t index) {
  static const char *const COMMANDS[9] = {
      "heat_bath", "heat_temp", "cool_hi", "cool_lo",
      "vent_hi",   "vent_mid",  "vent_lo", "dry_eco", "dry_fast",
  };
  if (index == 0) {
    this->parent_->request_off();
  } else if (index <= 9) {
    this->parent_->cmd_mode(COMMANDS[index - 1]);
  } else if (index == 10) {
    this->parent_->cmd_mode("vent24");
  }
}

void LifegearFilterThresholdSelect::setup() {
  this->pref_ = global_preferences->make_preference<size_t>(this->get_preference_hash());
  size_t index = 0;
  if (!this->pref_.load(&index) || !this->has_index(index)) index = 0;
  this->control(index);
}

void LifegearFilterThresholdSelect::control(size_t index) {
  static const int HOURS[3] = {720, 1440, 2160};
  if (index >= 3) return;
  this->parent_->set_filter_threshold_h(HOURS[index]);
  this->publish_state(index);
  this->pref_.save(&index);
}

void LifegearFilterThresholdSelect::dump_config() {
  LOG_SELECT("", "Lifegear Filter Threshold", this);
}

void LifegearHeatTemperatureSelect::setup() {
  this->pref_ = global_preferences->make_preference<size_t>(this->get_preference_hash());
  size_t index = 5;  // 預設 30°C
  if (!this->pref_.load(&index) || !this->has_index(index)) index = 5;
  // 開機只發佈偏好值不套用; 進入暖房溫控時由 cmd_mode 補套。
  this->publish_state(index);
}

void LifegearHeatTemperatureSelect::control(size_t index) {
  if (index >= 11) return;
  // 溫度是偏好值 (協定無獨立溫度欄位): 一律保存+發佈; 溫控模式中才立即送到機器。
  this->publish_state(index);
  this->pref_.save(&index);
  if (this->parent_->is_heat_temp_mode())
    this->parent_->set_heat_temp(25 + static_cast<int>(index));
}

void LifegearHeatTemperatureSelect::sync_actual(size_t index) {
  if (index >= 11) return;
  auto current = this->active_index();
  if (current.has_value() && current.value() == index) return;
  this->publish_state(index);
  this->pref_.save(&index);
}

void LifegearModeTimerNumber::setup() {
  this->pref_ = global_preferences->make_preference<float>(this->get_preference_hash());
  float value = static_cast<float>(this->initial_minutes_);
  float restored;
  if (this->pref_.load(&restored) && std::isfinite(restored) && restored >= 5.0f && restored <= 720.0f)
    value = restored;
  this->parent_->set_mode_timer(this->index_, static_cast<int>(std::lround(value)));
  this->publish_state(value);
}

void LifegearModeTimerNumber::control(float value) {
  int minutes = static_cast<int>(std::lround(value));
  if (minutes < 5) minutes = 5;
  if (minutes > 720) minutes = 720;
  float saved = static_cast<float>(minutes);
  this->parent_->set_mode_timer(this->index_, minutes);
  this->publish_state(saved);
  this->pref_.save(&saved);
}

void LifegearModeTimerNumber::dump_config() {
  LOG_NUMBER("", "Lifegear Mode Timer", this);
  ESP_LOGCONFIG(TAG, "  Timer index: %d", this->index_);
}

void LifegearSwitch::write_state(bool state) {
  switch (this->kind_) {
    case SWITCH_LEFT_LIGHT:
      this->parent_->set_state_bit(23, state);
      this->publish_state(this->parent_->get_light(23));
      break;
    case SWITCH_RIGHT_LIGHT:
      this->parent_->set_state_bit(22, state);
      this->publish_state(this->parent_->get_light(22));
      break;
    case SWITCH_BUZZER:
      this->parent_->set_buzzer_enabled(state);
      this->publish_state(this->parent_->get_buzzer_enabled());
      break;
    case SWITCH_VENT24:
      this->parent_->set_vent24_enabled(state);
      this->publish_state(this->parent_->get_vent24_enabled());
      break;
  }
}

void LifegearButton::press_action() {
  switch (this->kind_) {
    case BUTTON_OFF:
      this->parent_->request_off();
      break;
    case BUTTON_FILTER_RESET:
      this->parent_->reset_filter();
      break;
    case BUTTON_RETURN_TO_PANEL:
      this->parent_->set_override(false);
      break;
  }
}

// 線上有 7-20us 振鈴, 遠小於 500us 位元時間 → 視為雜訊合併
static const int64_t GLITCH_US = 150;

// 已實測建表的狀態 (30-bit 資料 → 名稱)
static const ModeEntry MODES[] = {
    {"100011111111111110000100111111", "關機"},
    {"001110111111001110000100101111", "暖房沐浴"},
    {"001111111110111110000100101111", "暖房溫控"},
    {"000111111110101101110000101111", "涼風強"},
    {"001110111110011101110000110111", "涼風弱"},
    {"111010111110001110000100011111", "換氣強"},   // 26.08.31 實測確認: 面板真正的「強」
    {"000111111110001101110000101111", "換氣中"},   // 26.08.31 更正: 舊表誤標為換氣強, 實為中檔
    {"001110111101111101110000110111", "換氣弱"},
    {"000010111111101110000100011111", "乾燥節電"},
    {"111111111111011110000100011111", "乾燥快速"},
    {"010010111101101110000100110111", "24h換氣"},
    {"010011111110011110000100110111", "涼風弱"},
    {"001010111110001110000100101111", "換氣中"},   // 26.08.31 更正(記憶位變體)
    {"010011111101111110000100110111", "換氣弱"},
    {"100011111111110110000110111111", "照明右(切換)"},
    {"110001111111111011000010111111", "照明左(切換)"},
};

// 24h 換氣記憶 + 定時用的狀態碼與模式表
static const char CODE_OFF[] = "100011111111111110000100111111";
static const char CODE_VENT24[] = "010010111101101110000100110111";
// 換氣中不設獨立定時, 與換氣強共用 idx4 (tick_timer 特判)
static const char *const TIMER_MODES[8] = {"暖房沐浴", "暖房溫控", "涼風強",   "涼風弱",
                                           "換氣強",   "換氣弱",   "乾燥節電", "乾燥快速"};

// HA 指令鍵 → 30-bit 狀態碼 (yaml 只用語意鍵, 位元碼集中於此)
static const ModeEntry CMDS[] = {
    {"001110111111001110000100101111", "heat_bath"},   // 暖房沐浴
    {"001111111110111110000100101111", "heat_temp"},   // 暖房溫控
    {"000111111110101101110000101111", "cool_hi"},     // 涼風強
    {"001110111110011101110000110111", "cool_lo"},     // 涼風弱
    {"111010111110001110000100011111", "vent_hi"},     // 換氣強 (26.08.31 更正為真正的強)
    {"000111111110001101110000101111", "vent_mid"},    // 換氣中 (26.08.31 更正: 舊表誤標為強)
    {"001110111101111101110000110111", "vent_lo"},     // 換氣弱
    {"000010111111101110000100011111", "dry_eco"},     // 乾燥節電
    {"111111111111011110000100011111", "dry_fast"},    // 乾燥快速
    {"010010111101101110000100110111", "vent24"},      // 24h換氣
};

// 暖房溫控各溫度 (pos16-21 = 63 - 溫度)
static const ModeEntry TEMPS[] = {
    {"010010111110111110011000101111", "暖房溫控 25°C"},
    {"010011111110111110010100101111", "暖房溫控 26°C"},
    {"010010111110111110010000101111", "暖房溫控 27°C"},
    {"001111111110111110001100101111", "暖房溫控 28°C"},
    {"001110111110111110001000101111", "暖房溫控 29°C"},
    {"001111111110111110000100101111", "暖房溫控 30°C"},
    {"001110111110111110000000101111", "暖房溫控 31°C"},
    {"001011111110111101111100101111", "暖房溫控 32°C"},
    {"001010111110111101111000101111", "暖房溫控 33°C"},
    {"001011111110111101110100101111", "暖房溫控 34°C"},
    {"001010111110111101110000101111", "暖房溫控 35°C"},
};

// --- 快速 GPIO 存取 (GPIO < 32) ---

// 模式特徵位元 (排除記憶/溫度欄位 pos0-3,5,16-21, 以及照明狀態位 pos22=右燈/pos23=左燈)
static const int SIG_BITS[] = {4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29};
static const int SIG_LEN = 17;
static const ModeEntry SIGS[] = {
    {"11111111111111111", "關機"},
    {"11111110011101111", "暖房沐浴"},
    {"11111101111101111", "暖房溫控"},
    {"11111101011101111", "涼風強"},
    {"11111100111110111", "涼風弱"},
    {"11111100011011111", "換氣強"},   // 26.08.31 實測: 真正的強
    {"11111100011101111", "換氣中"},   // 26.08.31 更正: 舊誤標換氣強
    {"11111011111110111", "換氣弱"},
    {"11111111011011111", "乾燥節電"},
    {"11111110111011111", "乾燥快速"},
    {"11111011011110111", "24h換氣"},
};

static std::string signature_of(const std::string &d) {
  std::string s;
  if ((int) d.size() < 30) return s;
  for (int i = 0; i < SIG_LEN; i++) s += d[SIG_BITS[i]];
  return s;
}

static std::string mode_name_from_state_(const std::string &state) {
  if (state.empty()) return "等待中";
  for (auto &mode : TEMPS)
    if (state == mode.bits) return mode.name;
  for (auto &mode : MODES)
    if (state == mode.bits) return mode.name;
  const std::string signature = signature_of(state);
  for (auto &mode : SIGS)
    if (signature == mode.bits) return mode.name;
  return "未知";
}

static int mode_index_from_name_(const std::string &mode) {
  if (mode == "關機") return 0;
  if (mode.rfind("暖房沐浴", 0) == 0) return 1;
  if (mode.rfind("暖房溫控", 0) == 0) return 2;
  if (mode.rfind("涼風強", 0) == 0) return 3;
  if (mode.rfind("涼風弱", 0) == 0) return 4;
  if (mode.rfind("換氣強", 0) == 0) return 5;
  if (mode.rfind("換氣中", 0) == 0) return 6;
  if (mode.rfind("換氣弱", 0) == 0) return 7;
  if (mode.rfind("乾燥節電", 0) == 0) return 8;
  if (mode.rfind("乾燥快速", 0) == 0) return 9;
  if (mode == "24h換氣") return 10;
  return -1;
}

// =============================================================================
//  機器方向 PPM 訊息辨識 (2026-08-25 依實測 raw 資料重寫)
//  真實編碼: 高脈衝三種寬 h≈1000/N≈1500/W≈2000us + 離散低電平 gap (500us 倍數);
//  每幀前導 "W _2 N N N W _2" (三連 N 之間為 2~3us 微縫), 之後為訊息頁, 每秒輪播多頁。
//  簽章格式: 脈衝→h/N/W, gap→"_單位數"; 比對時 gap 容差 ±1 單位。
//  已建立實測可確認的長頁風量碼與 burst2 致動狀態碼;
//  未建表的輪播頁僅略過，不得將已確認的主機狀態覆寫成未知。
// =============================================================================
// ★ 2026-08-25 晚間 22 分鐘連續關聯實測(1300+幀, 面板 ground truth)推翻舊頁面表:
//   - 「長頁」(N _6 開頭)才是穩態運轉的權威模式頁, 第 4-5 符號 = 模式欄位:
//       N _6 N W h → 換氣強 (99.9% 純度) / N _6 N W N → 換氣弱 / N _6 W → 關機閒置(暫定,樣本少)
//   - B群頁(_31/_22 開頭, 舊表誤標為關機/狀態頁)在穩定運轉約 6 分鐘後也會混入,
//     不代表關機, 意義待解 → 不投票, 僅 DEBUG M頁 記錄供建表。
//   - 暖房/涼風/乾燥的長頁欄位值未知, 待自然使用累積 log 後補規則。
// 心跳頁 (不含模式; 尾段 _21 h = 自主低速換氣中(24h運轉或餘轉), _20 W = 非自主換氣)
static const char HB_NORMAL[] = "N _31 N _20 W";
static const char HB_AUTOVENT[] = "N _31 N _21 h";
static const char MPRE[] = "W _2 N N N W _2 ";   // 前導簽章 (含結尾空白)

// 簽章比對: 脈衝類別需相等, gap 容差 ±1 單位; prefix=true 時 b 只需為 a 的開頭
static bool sig_match(const char *pa, const char *pb, bool prefix) {
  for (;;) {
    while (*pa == ' ') pa++;
    while (*pb == ' ') pb++;
    if (!*pb) return prefix || !*pa;
    if (!*pa) return false;
    if (*pa == '_' && *pb == '_') {
      int va = atoi(pa + 1), vb = atoi(pb + 1);
      if (va - vb > 1 || vb - va > 1) return false;
      while (*pa && *pa != ' ') pa++;
      while (*pb && *pb != ' ') pb++;
    } else {
      if (*pa != *pb) return false;
      pa++; pb++;
    }
  }
}

// C3 移植: ESP32 原版 GPIO.out_w1ts/out_w1tc/in 是 uint32_t, ESP32-C3 的 gpio_struct.h 改為
// union (要寫 .val)。改用 hal/gpio_ll.h 的 always_inline 函式, ESP32 與 C3 都編得過且 ISR 安全,
// 產生的暫存器寫入與原本完全相同 (w1ts/w1tc 單次寫入, in 單次讀取)。
static inline void IRAM_ATTR fast_write(int pin, int val) {
  gpio_ll_set_level(&GPIO, (uint32_t) pin, val ? 1U : 0U);
}
static inline int IRAM_ATTR fast_read(int pin) { return gpio_ll_get_level(&GPIO, (uint32_t) pin); }

// =============================================================================
//  RMT RX 完成 callback: 機器方向硬體精準擷取 (含 glitch filter)
// =============================================================================
static bool IRAM_ATTR rmt_rx_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *ed,
                                  void *user) {
  auto *self = static_cast<LifegearBathHeat *>(user);
  int n = ed->num_symbols;
  if (n > 256) n = 256;
  for (int i = 0; i < n; i++)
    self->rx_done_buf_[i] = ed->received_symbols[i];
  self->rx_done_num_ = n;
  self->m_cb_us_ = esp_timer_get_time();
  self->rx_cb_count_++;
  self->rx_done_ = true;
  // re-arm (在 callback 內重新啟動接收); 失敗要記下來, worker 會在 task context 重試
  self->rx_arm_err_ = rmt_receive(ch, self->rx_raw_, sizeof(self->rx_raw_), &self->rx_cfg_);
  return false;
}

// =============================================================================
//  ISR: 面板 → 機器 (鏡射 + 記錄脈衝供解碼)
// =============================================================================
static void IRAM_ATTR isr_panel(void *arg) {
  auto *self = static_cast<LifegearBathHeat *>(arg);
  self->p_edges_++;
  int lvl = fast_read(self->panel_rx_pin_);
  if (!self->override_)
    fast_write(self->machine_tx_pin_, lvl);  // 即時鏡射給機器

  int64_t now = esp_timer_get_time();
  int64_t dt = now - self->p_last_edge_;
  if (dt < GLITCH_US) return;          // 毛刺: 不記錄也不更新, 讓脈衝繼續累積
  self->p_last_edge_ = now;

  if (dt > FRAME_GAP_US) {
    // 長間隔後的這個邊緣 = 新封包開始 (idle LOW → HIGH)
    self->p_idx_ = 0;
  } else if (self->p_idx_ >= 0 && self->p_idx_ < WIRE_BITS) {
    self->p_dur_[self->p_idx_++] = (uint16_t) (dt > 65535 ? 65535 : dt);
    if (self->p_idx_ >= WIRE_BITS) {
      self->p_idx_ = -1;
      self->p_done_us_ = now;
      self->p_ready_ = true;
      BaseType_t hp = pdFALSE;
      xSemaphoreGiveFromISR(self->sem_, &hp);
      if (hp == pdTRUE)
        portYIELD_FROM_ISR();
    }
  }
}

// =============================================================================
//  ISR: 機器 → 面板 (鏡射)
// =============================================================================
static void IRAM_ATTR isr_machine(void *arg) {
  auto *self = static_cast<LifegearBathHeat *>(arg);
  self->m_edges_++;
  fast_write(self->panel_tx_pin_, fast_read(self->machine_rx_pin_));
  int64_t now = esp_timer_get_time();
  int64_t dt = now - self->m_last_edge_;
  if (dt < GLITCH_US) return;          // 毛刺濾除
  self->m_last_edge_ = now;
  if (dt > FRAME_GAP_US) {
    self->machine_frames_++;
    if (self->m_idx_ > 20 && !self->m_ready_) {   // 前一幀結束
      self->m_len_ = self->m_idx_;
      self->m_ready_ = true;
    }
    self->m_idx_ = 0;
  } else if (self->m_idx_ >= 0 && self->m_idx_ < 160) {
    self->m_dur_[self->m_idx_++] = (uint16_t) (dt > 65535 ? 65535 : dt);
  }
}

// =============================================================================
//  背景任務: 解碼面板幀 / override 時定期送出自訂幀
// =============================================================================
static void worker_task(void *arg) {
  auto *self = static_cast<LifegearBathHeat *>(arg);
  TickType_t last_tx = xTaskGetTickCount();
  for (;;) {
    if (xSemaphoreTake(self->sem_, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (self->p_ready_) {
        self->p_ready_ = false;
        self->decode_panel_frame();
      }
    }
    if (self->m_ready_) {
      self->m_ready_ = false;
      self->decode_machine_frame();
    }
    if (self->rx_done_) {
      self->rx_done_ = false;
      self->decode_machine_rmt();
    }
    if (self->kp_pending_) {
      self->kp_pending_ = false;
      self->send_keypress_();
    }
    if ((xTaskGetTickCount() - last_tx) >= pdMS_TO_TICKS(1000)) {
      last_tx = xTaskGetTickCount();
      self->tick_tx();
      self->tick_timer();
      self->tick_runtime();
      // RMT re-arm 曾失敗 → 在 task context 重試, 避免 RX 靜默停擺
      if (self->rx_ok_ && self->rx_arm_err_ != 0) {
        self->rx_arm_err_ = rmt_receive(self->rx_chan_, self->rx_raw_, sizeof(self->rx_raw_), &self->rx_cfg_);
        ESP_LOGW(TAG, "RMT re-arm 重試: %d", self->rx_arm_err_);
      }
      static uint32_t k = 0;
      if ((k++ % 3) == 0) {
        // 取樣 GPIO4 電平 20 次看有無變化(確認訊號到腳)
        int hi = 0, lo = 0;
        for (int i = 0; i < 20; i++) {
          if (fast_read(self->rmt_rx_pin_)) hi++; else lo++;
          esp_rom_delay_us(50);
        }
        ESP_LOGI(TAG, "RMT: cb=%u arm_err=%d  GPIO%d電平(hi=%d lo=%d)",
                 (unsigned) self->rx_cb_count_, self->rx_arm_err_, self->rmt_rx_pin_, hi, lo);
        ESP_LOGI(TAG, "面板診斷: 邊緣=%u 成功幀=%u 表頭錯=%u Manch錯=%u 收集中idx=%d",
                 (unsigned) self->p_edges_, (unsigned) self->get_panel_frames(),
                 (unsigned) self->hdr_fail_, (unsigned) self->manch_fail_, self->p_idx_);
      }
    }
  }
}

void LifegearBathHeat::decode_panel_frame() {
  // 脈衝寬度 → wire bits (短 500us=0, 長 1000us=1)
  std::string wire;
  wire.reserve(WIRE_BITS);
  for (int i = 0; i < WIRE_BITS; i++)
    wire += (this->p_dur_[i] < 750) ? '0' : '1';

  if (wire.compare(0, 17, HEADER17) != 0) {
    this->hdr_fail_++;
    static uint32_t nh = 0;
    if ((nh++ % 100) == 0)
      ESP_LOGW(TAG, "表頭不符: %s", wire.c_str());
    return;
  }

  // Manchester: 每 2 wire bit = 1 資料 bit (17 表頭後共 32 位: 前 2 旗標 + 後 30 狀態)
  std::string data;
  for (int i = 17; i + 1 < WIRE_BITS; i += 2) {
    if (wire[i] == '0' && wire[i + 1] == '1')
      data += '0';
    else if (wire[i] == '1' && wire[i + 1] == '0')
      data += '1';
    else {
      this->manch_fail_++;
      if ((this->manch_fail_ % 10) == 1)
        ESP_LOGW(TAG, "Manchester違規@wire[%d]: %s", i, wire.c_str());
      return;  // Manchester 違規,丟棄
    }
  }
  if (data.size() != 32)
    return;

  // 前 2 位 = 旗標 (關機/舊建表="01", 乾燥運轉實測="11"; 意義待建表), 後 30 位 = 狀態
  std::string flags = data.substr(0, 2);
  data = data.substr(2);
  if (flags != this->panel_flags_) {
    this->panel_flags_ = flags;
    ESP_LOGI(TAG, "面板旗標位變更: %s", flags.c_str());
  }

  this->panel_frames_++;
  ESP_LOGD(TAG, "P: %s [%s]", data.c_str(), flags.c_str());
  // 按鍵事件幀 (pos14=0) 只在實際按鍵時出現, INFO 記錄供建立各按鍵旗標規則
  if (data[14] == '0')
    ESP_LOGI(TAG, "面板按鍵幀: 旗標%s %s", flags.c_str(), data.c_str());

  // jitter 濾波: 連續兩幀相同才採信 (單幀 bit 抖動不更新)
  if (data != this->last_raw_) {
    this->last_raw_ = data;
    return;  // 首次出現, 等下一幀確認
  }
  // 已連續兩幀一致 (jitter 已濾除), 不論是否已知模式都採信
  if (data != this->panel_state_) {
    xSemaphoreTake(this->mtx_, portMAX_DELAY);
    if (!this->override_ && signature_of(this->panel_state_) != signature_of(data)) {
      if (++this->mode_request_seq_ == 0) this->mode_request_seq_ = 1;
    }
    this->panel_state_ = data;
    xSemaphoreGive(this->mtx_);
    // 24h 換氣狀態改由機器方向判別(decode_machine_rmt), 面板方向不推論
    ESP_LOGI(TAG, "面板狀態: %s (%s)", data.c_str(), this->get_mode_name().c_str());
  }
}

optional<std::string> LifegearBathHeat::get_temp_str() {
  if (this->mtx_ == nullptr) return {};
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  std::string s = this->override_ ? this->override_bits_ : this->panel_state_;
  xSemaphoreGive(this->mtx_);
  if (s.size() != 30) return {};
  // pos16-21 只有暖房溫控模式才有意義; 其他模式此欄位是雜訊, 且剛好會解成 30/35
  // 落在有效範圍內, 必須先以模式特徵閘控 (鎖已釋放, 直接比對即可, 不可再呼叫
  // is_heat_temp_mode() 重複取鎖)。
  if (signature_of(s) != "11111101111101111") return {};
  int v = 0;
  for (int i = 16; i < 22; i++) v = (v << 1) | (s[i] == '1');
  int t = 63 - v;                       // pos16-21 = 63 - 溫度
  if (t < 25 || t > 35) return {};
  return std::to_string(t);
}

bool LifegearBathHeat::is_heat_temp_mode() {
  if (this->mtx_ == nullptr) return false;
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  std::string s = this->override_ ? this->override_bits_ : this->panel_state_;
  xSemaphoreGive(this->mtx_);
  if (s.size() != 30) return false;
  return signature_of(s) == "11111101111101111";   // 暖房溫控特徵 (17-bit, 已移除燈位)
}

void LifegearBathHeat::decode_machine_frame() {
  // 舊 ISR 版擷取已停用, 改由 RMT (decode_machine_rmt)。保留空殼避免其他引用。
}

// RMT 硬體擷取的機器方向脈寬 (乾淨, 含 glitch filter)
void LifegearBathHeat::decode_machine_rmt() {
  int n = this->rx_done_num_;
  if (n < 4) return;
  uint32_t cb0 = this->rx_cb_count_;   // 快照: 解碼期間若有新幀進來, 本輪資料視為污染丟棄
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  const uint32_t request_seq_at_frame = this->mode_request_seq_;
  xSemaphoreGive(this->mtx_);

  // 未濾波原始 symbol 傾印 (建表用, 每 30 幀一次)
  static uint32_t dumpn = 0;
  if ((dumpn++ % 30) == 0) {
    int64_t mc, pd;   // 64-bit volatile 在 32-bit 平台非原子 → 重讀到一致
    do { mc = this->m_cb_us_; } while (mc != this->m_cb_us_);
    do { pd = this->p_done_us_; } while (pd != this->p_done_us_);
    int64_t dp = (mc - 30000 - pd) / 1000;   // 機器幀結束 vs 面板幀完成 (ms)
    for (int base = 0; base < n && base < 90; base += 30) {
      std::string r;
      int lim = (base + 30 < n) ? base + 30 : n;
      for (int i = base; i < lim; i++) {
        auto &sy = this->rx_done_buf_[i];
        r += std::to_string(sy.level0); r += ':'; r += std::to_string(sy.duration0); r += ' ';
        r += std::to_string(sy.level1); r += ':'; r += std::to_string(sy.duration1); r += ' ';
      }
      ESP_LOGI(TAG, "RMTraw n=%d dp=%lld [%d..%d): %s", n, (long long) dp, base, lim, r.c_str());
    }
  }

  // 符號化: 依 symbol 實際 level 分類; <150us(微縫/毛刺)跳過
  std::string sig;
  int bad = 0;
  int low_cnt = 0;
  uint32_t temp_cand = 0;
  std::string bursts[12];   // 叢 = 微縫相連的脈衝群 (26.08.27 網格實驗: 叢2 = 4脈衝模式碼)
  int nburst = 0;
  for (int i = 0; i < n; i++) {
    auto &sy = this->rx_done_buf_[i];
    const unsigned lv[2] = {sy.level0, sy.level1};
    const unsigned du[2] = {sy.duration0, sy.duration1};
    for (int k = 0; k < 2; k++) {
      if (du[k] < 150) continue;
      if (!lv[k]) {
        low_cnt++;
        // 長頁第3個真實 low = 機體溫度類比欄位候選 (僅在頁面被辨識後才提交, 避免混讀他頁欄位)
        if (low_cnt == 3 && du[k] >= 2000 && du[k] <= 9900)
          temp_cand = du[k];
      }
      if (lv[k]) {
        char pc;
        if (du[k] < 1300) pc = 'h';
        else if (du[k] <= 1650) pc = 'N';
        else if (du[k] <= 2150) pc = 'W';
        else { pc = 'X'; bad++; }
        sig += pc;
        if (nburst < 12) bursts[nburst] += pc;
      } else {
        char b[8];
        snprintf(b, sizeof(b), "_%u", (unsigned) ((du[k] + 250) / 500));
        sig += b;
        if (nburst < 11 && !bursts[nburst].empty()) nburst++;   // 真 gap = 叢邊界
      }
      sig += ' ';
    }
  }
  if (this->rx_cb_count_ != cb0) return;   // 解碼中被新幀覆寫 → 丟棄
  if (!sig.empty() && sig.back() == ' ') sig.pop_back();

  // 前導驗證 "W _2 N N N W _2" (不符=殘幀/雜訊, 靜默丟棄)
  size_t plen = sizeof(MPRE) - 1;
  if (sig.size() <= plen || sig.compare(0, plen, MPRE) != 0) return;
  std::string payload = sig.substr(plen);
  ESP_LOGD(TAG, "M頁: %s", payload.c_str());   // 每幀簽章 (建表/關聯分析用)
  if (bad) {
    static uint32_t nb = 0;
    if ((nb++ % 20) == 0)
      ESP_LOGW(TAG, "機器頁含未知脈寬: %s", payload.c_str());
    return;
  }

  // 詳細致動碼可能在暖機、恆溫或冷卻階段暫時消失；合法主機頁仍可證明
  // 主機在本次模式要求後持續回應。關機/24h 仍由下方明確碼另外確認。
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  this->last_machine_frame_ms_ = millis();
  this->machine_frame_request_seq_ = request_seq_at_frame;
  xSemaphoreGive(this->mtx_);

  // 心跳頁 (prefix 比對, 尾段可能掛額外欄位): 只更新自主換氣旗標, 不動模式
  if (sig_match(payload.c_str(), HB_AUTOVENT, true)) {
    this->m_unmatched_ = 0;
    xSemaphoreTake(this->mtx_, portMAX_DELAY);
    const bool changed = !this->machine_autovent_;
    this->machine_autovent_ = true;
    this->autovent_confirmed_request_seq_ = request_seq_at_frame;
    xSemaphoreGive(this->mtx_);
    if (changed) ESP_LOGI(TAG, "機器自主換氣: 開始");
    return;
  }
  if (sig_match(payload.c_str(), HB_NORMAL, true)) {
    this->m_unmatched_ = 0;
    xSemaphoreTake(this->mtx_, portMAX_DELAY);
    const bool changed = this->machine_autovent_;
    this->machine_autovent_ = false;
    xSemaphoreGive(this->mtx_);
    if (changed) ESP_LOGI(TAG, "機器自主換氣: 結束");
    return;
  }

  // 長頁投票: 連續 3 幀同判定才發布 (過渡期/B群頁混雜時保持原判)
  // ★ 26.08.25 晚 override 全模式實測: 長頁欄位=風量/致動狀態, 非使用者模式:
  //   涼風強=換氣強、涼風弱=換氣弱 (機器端不可分) → 標籤改用送風強/中/弱。
  //   「送風中」= override 換氣強/涼風強碼實測值(N N W), 與面板實按換氣強(N W h)不同,
  //   疑為機器內部中檔, 待驗證。b4(面板失聯)不影響長頁; B群頁=面板互動連動。
  // ★ 26.08.31 面板側更正: 舊「換氣強」碼實為換氣「中」, 真正的強是新碼(見 MODES)。
  //   因此本表 26.08.25 的「送風強」(N W h, 當晚 ground truth=舊碼=中) 與「送風中」(N N W)
  //   標籤存疑, 可能都是中檔的不同頁; 真換氣強僅抓到 4 幀新頁型
  //   "W _6 N N h _6 W _9 N h N N _12 N N N _4 h" (當時面板失聯干擾, 未達建表標準, 待驗證後補)。
  const char *vote = nullptr;
  if (sig_match(payload.c_str(), "N _6 W N N", true)) vote = "關機";        // 閒置
  else if (sig_match(payload.c_str(), "N _7 h N N", true)) vote = "關機";   // 剛停機/餘轉
  // ⚠ "N _6 W h N"→24h換氣 已撤回(26.08.31): 面板控換氣弱的新頁型 N _6 [WhNW] _3 N _9 [hWNN]…
  //   攤平後前綴完全相同 → 弱被誤判 24h → 與要求矛盾 → 主機回報卡「等待回報」。
  //   24h 偵測交給心跳頁 HB_AUTOVENT (更可靠)；此頁家族(叢2=WhNW)與 24h/暖房溫控互撞已三度應驗。
  else if (sig_match(payload.c_str(), "N _6 N W h", true)) vote = "送風強";
  else if (sig_match(payload.c_str(), "N _6 N N W", true)) vote = "送風中";
  else if (sig_match(payload.c_str(), "N _6 N W N", true)) vote = "送風弱";
  if (vote != nullptr) {
    this->m_unmatched_ = 0;
    if (temp_cand)
      this->machine_temp_us_ = temp_cand;   // 已辨識頁面的機體溫度欄位才採信
    if (this->m_cand_ == vote) {
      if (this->m_cand_cnt_ < 99) this->m_cand_cnt_++;
    } else {
      this->m_cand_ = vote;
      this->m_cand_cnt_ = 1;
    }
    if (this->m_cand_cnt_ >= 3) {
      xSemaphoreTake(this->mtx_, portMAX_DELAY);
      const bool changed = this->machine_mode_ != vote;
      if (changed) this->machine_mode_ = vote;
      this->machine_confirmed_request_seq_ = request_seq_at_frame;
      xSemaphoreGive(this->mtx_);
      if (changed) ESP_LOGI(TAG, "機器狀態(長頁): %s", vote);
    }
    return;
  }
  // ★ burst2 模式碼表 (26.08.27 網格實驗: payload 第二叢 = 4脈衝模式碼, 溫度免疫)
  //   前導後結構 = [叢1] gap(機體溫度類比) [叢2=模式碼] ...
  //   注意: 熱機時模式碼頁會碎裂消失(暖房沐浴約起動2分鐘內可見), 碎裂後靠下方統計特徵或未知回退。
  // 全幀叢序: [0]=前導W [1]=前導NNNW(NNN與W微縫相連) [2]=payload叢1 [3]=payload叢2=模式碼
  if (nburst >= 4) {
    // 26.08.27 冷機補收修正: 4碼=「致動狀態」非使用者模式 (涼強=換強=NNWN 強風,
    // 涼弱=換弱=沐浴起動=NWNN 弱風)。
    // ⚠ WhNW→暖房溫控 已撤回(26.08.27晚): 與 24h換氣頁的溫度變體相撞(24h 被誤判溫控),
    //   溫控辨識退回「暖房」統計特徵。
    static const ModeEntry BURST2_MODES[] = {
        {"WNNN", "關機"},      {"NNNW", "乾燥節電"}, {"NNNN", "乾燥快速"},
        {"NNWN", "送風強"},    {"NWNN", "送風弱"},
    };
    for (auto &m : BURST2_MODES) {
      if (bursts[3] == m.bits) {
        this->m_unmatched_ = 0;
        if (temp_cand) this->machine_temp_us_ = temp_cand;
        if (this->m_cand_ == m.name) {
          if (this->m_cand_cnt_ < 99) this->m_cand_cnt_++;
        } else { this->m_cand_ = m.name; this->m_cand_cnt_ = 1; }
        if (this->m_cand_cnt_ >= 3) {
          xSemaphoreTake(this->mtx_, portMAX_DELAY);
          const bool changed = this->machine_mode_ != m.name;
          if (changed) this->machine_mode_ = m.name;
          this->machine_confirmed_request_seq_ = request_seq_at_frame;
          xSemaphoreGive(this->mtx_);
          if (changed) ESP_LOGI(TAG, "機器狀態(模式碼): %s", m.name);
        }
        return;
      }
    }
    // 暖房熱態統計特徵 (沐浴/溫控通用): 熱機加熱中 burst2 塌縮為單字符 h/W/Wh
    // (實測: 熱機沐浴 W:91%, 溫控27-35°C h/W 92-98%; 熱機乾燥/涼風=N 不會誤中)
    bool hw = (bursts[3] == "h" || bursts[3] == "W" || bursts[3] == "Wh");
    this->ht_win_ = ((this->ht_win_ << 1) | (hw ? 1 : 0)) & 0xFFFFF;   // 20-bit 視窗
    int pop = __builtin_popcount(this->ht_win_);
    if (pop >= 14) {
      this->m_unmatched_ = 0;
      if (temp_cand) this->machine_temp_us_ = temp_cand;
      xSemaphoreTake(this->mtx_, portMAX_DELAY);
      const bool changed = this->machine_mode_ != "暖房";
      if (changed) this->machine_mode_ = "暖房";
      this->machine_confirmed_request_seq_ = request_seq_at_frame;
      xSemaphoreGive(this->mtx_);
      if (changed) ESP_LOGI(TAG, "機器狀態(統計): 暖房加熱中 (%d/20)", pop);
      return;
    }
  }

  // 其他頁(B群/尚未建表的遙測頁): 不投票。機器每秒會輪播多種頁面，
  // 因此「這一頁無法辨識」不等於「主機狀態未知」。保留最後一個有效回報，
  // 直到後續長頁或 burst2 模式碼確認新狀態。這也避免乾燥模式碼被辨識後，
  // 立即被同一輪尚未建表的其他頁覆寫成「未知」。
  if (this->m_unmatched_ < 99) this->m_unmatched_++;
}


std::string LifegearBathHeat::get_mode_name() const {
  if (this->mtx_ == nullptr)
    return "等待中";   // setup 前防禦 (template lambda 可能早於本元件 setup 執行)
  // 生效狀態: HA 接管時 = override 設定, 否則 = 面板送出
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  std::string s = this->override_ ? this->override_bits_ : this->panel_state_;
  xSemaphoreGive(this->mtx_);
  return mode_name_from_state_(s);
}

int LifegearBathHeat::get_mode_index() const {
  return mode_index_from_name_(this->get_mode_name());
}

std::string LifegearBathHeat::translate_(const std::string &key) const {
  auto it = this->translations_.find(key);
  return it == this->translations_.end() ? key : it->second;
}

std::string LifegearBathHeat::get_localized_mode_name() {
  const int index = this->get_mode_index();
  static const char *const KEYS[11] = {
      "off",      "heat_bath", "heat_temp", "cool_high", "cool_low", "vent_high",
      "vent_mid", "vent_low",  "dry_eco",   "dry_fast",  "vent24",
  };
  if (index >= 0 && index < 11) {
    std::string result = this->translate_(KEYS[index]);
    if (index == 2) {
      auto temperature = this->get_temp_str();
      if (temperature.has_value()) result += " " + *temperature + "°C";
    }
    return result;
  }
  const std::string mode = this->get_mode_name();
  if (mode == "等待中") return this->translate_("waiting");
  return this->translate_("unknown");
}

std::string LifegearBathHeat::get_localized_machine_status() const {
  if (this->mtx_ == nullptr) return this->translate_("waiting_response");

  // 同一把鎖內快照「目前要求」與「主機確認」，避免模式剛切換時混用新舊資料。
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  const std::string effective_state = this->override_ ? this->override_bits_ : this->panel_state_;
  const std::string machine_status = this->machine_mode_;
  const bool machine_autovent = this->machine_autovent_;
  const uint32_t request_seq = this->mode_request_seq_;
  const uint32_t confirmed_seq = this->machine_confirmed_request_seq_;
  const uint32_t autovent_seq = this->autovent_confirmed_request_seq_;
  const uint32_t machine_frame_seq = this->machine_frame_request_seq_;
  const uint32_t machine_frame_ms = this->last_machine_frame_ms_;
  xSemaphoreGive(this->mtx_);

  const uint32_t now = millis();
  const bool machine_link_fresh = machine_frame_ms != 0 && machine_frame_seq == request_seq &&
                                  now - machine_frame_ms <= MACHINE_CONFIRM_TIMEOUT_MS;
  const bool machine_fresh = machine_link_fresh && confirmed_seq == request_seq;
  const bool autovent_fresh = machine_link_fresh && machine_autovent && autovent_seq == request_seq;
  const int requested_mode = mode_index_from_name_(mode_name_from_state_(effective_state));

  if (requested_mode == 0 && machine_fresh && machine_status == "關機") return this->translate_("off");
  if (requested_mode == 10 &&
      (autovent_fresh || (machine_fresh && machine_status == "24h換氣")))
    return this->translate_("vent24");
  if (requested_mode >= 1 && requested_mode <= 9 && machine_link_fresh) {
    // 溫控已達標、暖機及尾段冷卻時可能沒有致動碼。只要本次模式要求後主機
    // 持續回傳合法頁面，且沒有明確回報相反的關機/24h 狀態，即視為程式運轉中。
    const bool explicit_contradiction =
        machine_fresh && (machine_status == "關機" || machine_status == "24h換氣");
    if (!explicit_contradiction) return this->translate_("running");
  }
  return this->translate_("waiting_response");
}

void LifegearBathHeat::set_buzzer_enabled(bool en) {
  if (this->buzzer_enabled_ == en) return;
  this->buzzer_enabled_ = en;
  ESP_LOGI(TAG, "蜂鳴確認=%s", en ? "開" : "關");
}

// 穩態旗標 b0 (26.08.31 面板實測修正語意): 非單純右燈位 —
//   右燈開=1、乾燥運轉=1、「換氣強」運轉=1, 其餘=0。
//   換氣強/中 的差異一半在 30-bit 碼、一半在旗標: 強=碼A+旗標11, 中=碼B+旗標01;
//   機器對真強碼只認旗標 11, override 送 01 會被忽略 (實測: 交還面板立即增強)。
static const char SIG_VENT_HIGH[] = "11111100011011111";
static char steady_flag0_(const std::string &data_bits) {
  if (data_bits.size() == 30 && data_bits[22] == '1') return '1';   // 右燈開
  if (signature_of(data_bits) == SIG_VENT_HIGH) return '1';         // 換氣強 (穩態旗標必須 11)
  return '0';
}

// 燈的按鍵事件幀: 機器接受條件實測 = b0 等於幀內燈位 (pos22|pos23)。
// (換氣強下送穩態反轉0: 開燈不嗶關燈嗶; 早期固定送1: 開嗶關不嗶 → 合併得此規則)
void LifegearBathHeat::queue_light_keypress_(const std::string &state30) {
  if (state30.size() != 30) return;
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  this->kp_bits_ = state30;
  this->kp_bits_[14] = '0';
  this->kp_flag0_ = (state30[22] == '1' || state30[23] == '1') ? '1' : '0';
  xSemaphoreGive(this->mtx_);
  this->kp_pending_ = true;
}

void LifegearBathHeat::queue_keypress_(const std::string &state30) {
  // 排入一幀「按鍵事件幀」: pos14=0 + 旗標 b0=右燈反轉 (翻轉=按鍵事件標記, 26.08.26 由面板實測解出)
  if (state30.size() != 30) return;
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  this->kp_bits_ = state30;
  this->kp_bits_[14] = '0';
  this->kp_flag0_ = (steady_flag0_(state30) == '1') ? '0' : '1';   // 按鍵幀 = 穩態旗標 b0 反轉
  xSemaphoreGive(this->mtx_);
  this->kp_pending_ = true;
}

void LifegearBathHeat::send_keypress_() {
  // 按鍵事件幀 = 17位表頭 + 旗標"11" + kp_bits_ (pos14=0), 立即送一幀 (實測會觸發機器按鍵音)
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  std::string base = this->kp_bits_;
  if (base.size() != 30) { xSemaphoreGive(this->mtx_); return; }
  std::string wire = HEADER17;
  wire += (this->kp_flag0_ == '0') ? "01" : "10";
  wire += "10";   // b1 恆為 1
  for (char b : base) wire += (b == '0') ? "01" : "10";
  for (int i = 0; i < WIRE_BITS; i++)
    this->tx_dur_[i] = (wire[i] == '0') ? 500 : 1000;
  bool prev = this->override_;
  this->override_ = true;              // 暫停鏡射避免撞幀
  this->tx_ready_ = true;
  this->transmit_override();
  this->build_tx_(this->override_bits_);   // 復原正常 TX 緩衝
  this->override_ = prev;
  xSemaphoreGive(this->mtx_);
  ESP_LOGI(TAG, "按鍵事件幀已送: 旗標11 + pos14=0 (%s)", base.c_str());
}

void LifegearBathHeat::trigger_keypress() {
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  std::string base = this->override_ ? this->override_bits_ : this->panel_state_;
  xSemaphoreGive(this->mtx_);
  this->queue_keypress_(base);
}

void LifegearBathHeat::build_tx_(const std::string &data_bits) {
  // 穩態幀 = 17位表頭 + 旗標(b0=steady_flag0_, b1=1) + 30位資料 (仿面板)
  std::string wire = HEADER17;
  const char f0 = steady_flag0_(data_bits);
  wire += (f0 == '0') ? "01" : "10";
  wire += "10";   // b1 恆為 1
  for (char b : data_bits)
    wire += (b == '0') ? "01" : "10";
  if ((int) wire.size() != WIRE_BITS) {
    ESP_LOGW(TAG, "wire 長度錯誤 %d", (int) wire.size());
    this->tx_ready_ = false;
    return;
  }
  for (int i = 0; i < WIRE_BITS; i++)
    this->tx_dur_[i] = (wire[i] == '0') ? 500 : 1000;
  this->tx_ready_ = true;
}

void LifegearBathHeat::transmit_override() {
  if (!this->tx_ready_)
    return;
  int pin = this->machine_tx_pin_;
  int lvl = 1;  // 由 HIGH 起,電位交替
  for (int i = 0; i < WIRE_BITS; i++) {
    fast_write(pin, lvl);
    esp_rom_delay_us(this->tx_dur_[i]);
    lvl ^= 1;
  }
  fast_write(pin, 0);  // 回到 idle LOW
}

void LifegearBathHeat::tick_tx() {
  if (this->kp_pending_)
    return;   // 按鍵事件幀優先出門 (worker 下一輪 <200ms 內送), 穩態幀下個 tick 再送
  // 整段持鎖: tx_dur_/pulse_bits_/override_bits_ 與 main task(API) 共用
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  if (this->pulse_count_ > 0) {
    this->build_tx_(this->pulse_bits_);
    this->transmit_override();
    if (--this->pulse_count_ <= 0) {
      // 脈衝結束: 復原先前狀態
      this->build_tx_(this->override_bits_);
      this->override_ = this->saved_override_;
      ESP_LOGI(TAG, "toggle 脈衝結束, 復原");
    }
    xSemaphoreGive(this->mtx_);
    return;
  }
  bool ov = this->override_;
  if (ov)
    this->transmit_override();
  xSemaphoreGive(this->mtx_);
}

bool LifegearBathHeat::get_light(int pos) {
  if (this->mtx_ == nullptr) return false;
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  std::string s = this->override_ ? this->override_bits_ : this->panel_state_;
  xSemaphoreGive(this->mtx_);
  return s.size() == 30 && s[pos] == '1';
}

void LifegearBathHeat::toggle_light(int pos) {
  // pos 22=右燈, 23=左燈: 反轉當前生效狀態的該位, 進入 override 持續送出
  this->set_state_bit(pos, !this->get_light(pos));
}

void LifegearBathHeat::set_state_bit(int pos, bool val) {
  if (!this->accept_cmd_) {
    ESP_LOGW(TAG, "開機保護中,忽略");
    return;
  }
  if (pos < 0 || pos >= 30) return;
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  std::string base = this->override_ ? this->override_bits_ : this->panel_state_;
  if (base.size() != 30) {
    xSemaphoreGive(this->mtx_);
    ESP_LOGW(TAG, "尚未取得目前狀態");
    return;
  }
  base[pos] = val ? '1' : '0';
  // 持續 override (不是脈衝),否則面板會把值蓋回去
  this->override_bits_ = base;
  this->build_tx_(base);
  this->override_ = true;
  xSemaphoreGive(this->mtx_);
  if (this->buzzer_enabled_)
    this->queue_light_keypress_(base);  // 燈按鍵幀規則與模式不同 (26.08.31 實測)
  ESP_LOGI(TAG, "設定 bit%d=%d → %s (持續送出)", pos, (int) val, base.c_str());
}

void LifegearBathHeat::toggle_bits(const std::string &positions) {
  if (!this->accept_cmd_) {
    ESP_LOGW(TAG, "開機保護中,忽略 toggle");
    return;
  }
  // 基準 = 目前生效狀態 (override 時用 override_bits_, 否則用面板實際狀態)
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  std::string base = this->override_ ? this->override_bits_ : this->panel_state_;
  xSemaphoreGive(this->mtx_);
  if (base.size() != 30) {
    ESP_LOGW(TAG, "尚未取得目前狀態,無法 toggle");
    return;
  }
  // positions: 逗號分隔的 bit 位置, 例如 "23"
  size_t start = 0;
  while (start < positions.size()) {
    size_t comma = positions.find(',', start);
    std::string tok = positions.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    int p = atoi(tok.c_str());
    if (p >= 0 && p < 30)
      base[p] = (base[p] == '0') ? '1' : '0';
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  ESP_LOGI(TAG, "toggle bits[%s] → %s", positions.c_str(), base.c_str());
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  this->trigger_pulse_raw(base);
  xSemaphoreGive(this->mtx_);
}

void LifegearBathHeat::trigger_pulse(const std::string &data_bits) {
  if (!this->accept_cmd_) {
    ESP_LOGW(TAG, "開機保護中,忽略 toggle");
    return;
  }
  if (data_bits.size() != 30) return;
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  this->trigger_pulse_raw(data_bits);
  xSemaphoreGive(this->mtx_);
}

void LifegearBathHeat::trigger_pulse_raw(const std::string &data_bits) {
  // 呼叫者必須持 mtx_
  if (this->pulse_count_ <= 0)              // 脈衝進行中不覆寫快照, 避免 override 卡死
    this->saved_override_ = this->override_;
  this->pulse_bits_ = data_bits;
  this->pulse_count_ = 2;      // 送 2 次確保收到
  this->override_ = true;
}

void LifegearBathHeat::set_override_state(const std::string &data_bits) {
  if (!this->accept_cmd_) {
    // 保護期內不丟棄: 暫存最後一道命令, 保護解除後自動補發。
    // 開機還原路徑在各 setter 早退不會走到這裡, 會到這裡的都是真實操作。
    this->boot_pending_bits_ = data_bits;
    ESP_LOGW(TAG, "開機保護中,暫存控制指令待解除後套用");
    return;
  }
  if (data_bits.size() != 30) {
    ESP_LOGW(TAG, "狀態需 30 bits,收到 %d", (int) data_bits.size());
    return;
  }
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  std::string pre = this->override_ ? this->override_bits_ : this->panel_state_;
  std::string bits = data_bits;
  if (pre.size() == 30) {
    bits[22] = pre[22];   // ★ 模式/關機指令保留照明狀態 (指令表是燈全關時抓的, 不可覆蓋燈)
    bits[23] = pre[23];
  }
  if (signature_of(pre) != signature_of(bits)) {
    if (++this->mode_request_seq_ == 0) this->mode_request_seq_ = 1;
  }
  this->override_bits_ = bits;
  this->build_tx_(bits);
  this->override_ = true;
  xSemaphoreGive(this->mtx_);
  if (this->buzzer_enabled_)
    this->queue_keypress_(bits);   // 按鍵幀 = 新狀態 + pos14=0 + 旗標反轉 (仿面板)
  ESP_LOGI(TAG, "override 開啟 → %s", bits.c_str());
}

void LifegearBathHeat::set_vent24_enabled(bool en) {
  // 規則:
  //   開啟 + 目前關機   → 立即進入 24h 換氣。
  //   開啟 + 其他模式   → 只記旗標，不中斷現行模式；定時後回 24h。
  //   關閉 + 純 24h 換氣 → 立即送真正關機碼。
  //   關閉 + 其他模式   → 只記旗標，不中斷現行模式；定時後真正關機。
  // 開機 restore 期間 accept_cmd_ 尚未開啟，僅還原旗標，不發送任何命令。
  if (this->vent24_enabled_ == en) return;
  const std::string current_mode = this->get_mode_name();
  this->vent24_enabled_ = en;
  ESP_LOGI(TAG, "24h換氣設定=%s", en ? "開" : "關");
  if (!this->accept_cmd_) return;              // 開機還原: 只記旗標
  if (en && current_mode == "關機") {
    ESP_LOGI(TAG, "關機狀態下開啟 24h → 立即進入 24h 換氣");
    this->set_override_state(CODE_VENT24);
  } else if (!en && current_mode == "24h換氣") {
    ESP_LOGI(TAG, "24h 換氣中關閉設定 → 立即真正關機");
    this->set_override_state(CODE_OFF);
  }
}

void LifegearBathHeat::cmd_mode(const std::string &key) {
  for (auto &m : CMDS) {
    if (key == m.name) {
      this->set_override_state(m.bits);
      // 暖房溫控的基準碼固定是 30°C; 立刻以偏好溫度覆蓋 (溫度=完整模式碼)。
      // set_override_state 後 override 已是溫控碼, set_heat_temp 的模式檢查會通過。
      if (key == "heat_temp" && this->heat_temperature_select_ != nullptr) {
        auto index = this->heat_temperature_select_->active_index();
        if (index.has_value()) this->set_heat_temp(25 + (int) index.value());
      }
      return;
    }
  }
  ESP_LOGW(TAG, "未知模式指令: %s", key.c_str());
}

void LifegearBathHeat::set_heat_temp(int t) {
  if (t < 25 || t > 35) {
    ESP_LOGW(TAG, "溫度超出範圍(25-35): %d", t);
    return;
  }
  if (!this->is_heat_temp_mode()) {
    ESP_LOGW(TAG, "非暖房溫控模式,忽略溫度設定");
    return;
  }
  this->set_override_state(TEMPS[t - 25].bits);
}

void LifegearBathHeat::request_off() {
  if (this->vent24_enabled_) {
    ESP_LOGI(TAG, "關機請求: 24h 設定開啟 → 改送 24h換氣");
    this->set_override_state(CODE_VENT24);
  } else {
    this->set_override_state(CODE_OFF);
  }
}

void LifegearBathHeat::set_mode_timer(int idx, int minutes) {
  if (idx < 0 || idx >= 8) return;
  if (minutes < 5) minutes = 5;
  if (minutes > 720) minutes = 720;
  bool changed = (this->mode_timer_min_[idx] != minutes);
  this->mode_timer_min_[idx] = minutes;
  if (changed)
    ESP_LOGI(TAG, "定時設定[%s]=%d 分", TIMER_MODES[idx], minutes);
  // 手冊 p.4: 定時運轉中變更時間 → 經過時間清零, 以新值重新起算
  if (changed && idx == this->cur_timer_idx_ && this->timer_deadline_s_ > 0)
    this->timer_deadline_s_ = (int32_t) (esp_timer_get_time() / 1000000LL) + minutes * 60;
}

int LifegearBathHeat::get_timer_left_min() {
  if (this->cur_timer_idx_ < 0 || this->timer_deadline_s_ <= 0) return -1;
  int32_t left = this->timer_deadline_s_ - (int32_t) (esp_timer_get_time() / 1000000LL);
  if (left < 0) left = 0;
  return (left + 59) / 60;
}

void LifegearBathHeat::tick_timer() {
  if (!this->override_) {
    // 面板主控時面板有自己的定時, 不搶; 清掉殘留計時
    if (this->cur_timer_idx_ != -1) { this->cur_timer_idx_ = -1; this->timer_deadline_s_ = 0; }
    return;
  }
  std::string cur = this->get_mode_name();
  int idx = -1;
  for (int i = 0; i < 8; i++)
    if (cur.rfind(TIMER_MODES[i], 0) == 0) { idx = i; break; }   // 前綴比對(暖房溫控含溫度)
  if (idx < 0 && cur.rfind("換氣中", 0) == 0) idx = 4;           // 中檔共用換氣強定時
  int32_t now_s = (int32_t) (esp_timer_get_time() / 1000000LL);
  if (idx != this->cur_timer_idx_) {
    this->cur_timer_idx_ = idx;
    if (idx >= 0) {
      this->timer_deadline_s_ = now_s + this->mode_timer_min_[idx] * 60;
      ESP_LOGI(TAG, "定時啟動: %s, %d 分後停止", TIMER_MODES[idx], this->mode_timer_min_[idx]);
    } else {
      this->timer_deadline_s_ = 0;
    }
    return;
  }
  if (idx >= 0 && this->timer_deadline_s_ > 0 && now_s >= this->timer_deadline_s_) {
    ESP_LOGI(TAG, "定時到: %s → %s", TIMER_MODES[idx], this->vent24_enabled_ ? "24h換氣" : "關機");
    this->timer_deadline_s_ = 0;
    this->cur_timer_idx_ = -1;
    this->request_off();
  }
}

// --- 濾網運轉時數: 起訖相減法 (省去每秒累加), NVS 節流 (停止且未存量>=30分, 或長運轉每小時結算) ---
static bool mode_is_running_(const std::string &m) {
  return !(m.empty() || m == "關機" || m == "等待中" || m == "未知");
}

void LifegearBathHeat::set_filter_threshold_h(int hours) {
  if (hours < 10) hours = 10;
  if (hours > 5000) hours = 5000;   // 原廠選項 720/1440/2160
  if (this->filter_threshold_h_ != hours) {
    this->filter_threshold_h_ = hours;
    ESP_LOGI(TAG, "濾網提醒門檻=%d 小時", hours);
  }
}

void LifegearBathHeat::reset_filter() {
  this->runtime_total_s_ = 0;
  if (this->run_start_s_ >= 0)
    this->run_start_s_ = (int32_t) (esp_timer_get_time() / 1000000LL);  // 運轉中歸零則重新起算
  this->filter_hours_dirty_ = true;  // 下一次主 loop 立即發布 0.0h，不等待一分鐘節流
  uint32_t v = 0;
  this->runtime_pref_.save(&v);
  this->last_saved_total_s_ = 0;
  ESP_LOGI(TAG, "濾網已清潔: 運轉時數歸零");
}

float LifegearBathHeat::get_runtime_h() {
  uint32_t s = this->runtime_total_s_;
  int32_t rs = this->run_start_s_;
  if (rs >= 0)
    s += (uint32_t) ((int32_t) (esp_timer_get_time() / 1000000LL) - rs);
  return s / 3600.0f;
}

bool LifegearBathHeat::is_filter_due() {
  return this->get_runtime_h() >= (float) this->filter_threshold_h_;
}

void LifegearBathHeat::on_shutdown() {
  // 重開機(OTA/重啟)前強制結算+存檔, 修正「累計隨重開機歸零」bug
  int32_t now = (int32_t) (esp_timer_get_time() / 1000000LL);
  if (this->run_start_s_ >= 0) {
    this->runtime_total_s_ += (uint32_t) (now - this->run_start_s_);
    this->run_start_s_ = now;
  }
  uint32_t v = this->runtime_total_s_;
  this->runtime_pref_.save(&v);
  this->last_saved_total_s_ = v;
  global_preferences->sync();
  ESP_LOGI(TAG, "關機前濾網時數存檔: %.2f 小時", v / 3600.0f);
}

void LifegearBathHeat::tick_runtime() {
  int32_t now = (int32_t) (esp_timer_get_time() / 1000000LL);
  bool running = mode_is_running_(this->get_mode_name());
  bool save = false;
  if (running && this->run_start_s_ < 0) {
    this->run_start_s_ = now;                          // 開始運轉: 只記時間戳
  } else if (!running && this->run_start_s_ >= 0) {
    this->runtime_total_s_ += (uint32_t) (now - this->run_start_s_);  // 停止: 相減結算
    this->run_start_s_ = -1;
    save = (this->runtime_total_s_ - this->last_saved_total_s_ >= 1800);  // 未存量>=30分才寫
  } else if (this->run_start_s_ >= 0 && now - this->run_start_s_ >= 3600) {
    this->runtime_total_s_ += (uint32_t) (now - this->run_start_s_);  // 長運轉: 每小時結算
    this->run_start_s_ = now;
    save = true;
  }
  if (save) {
    uint32_t v = this->runtime_total_s_;
    this->runtime_pref_.save(&v);
    this->last_saved_total_s_ = v;
    ESP_LOGI(TAG, "濾網時數存檔: %.1f 小時", v / 3600.0f);
  }
}

void LifegearBathHeat::set_override(bool on) {
  if (this->standalone_ && !on) {
    ESP_LOGW(TAG, "獨立模式無面板可交還, 忽略");
    return;
  }
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  const std::string previous = this->override_ ? this->override_bits_ : this->panel_state_;
  const std::string next = on ? this->override_bits_ : this->panel_state_;
  if (signature_of(previous) != signature_of(next)) {
    if (++this->mode_request_seq_ == 0) this->mode_request_seq_ = 1;
  }
  this->override_ = on;
  xSemaphoreGive(this->mtx_);
  ESP_LOGI(TAG, "override %s", on ? "開啟" : "關閉(回面板直通)");
}

void LifegearBathHeat::setup() {
  this->sem_ = xSemaphoreCreateBinary();
  this->mtx_ = xSemaphoreCreateMutex();

  // Reuse the old entity object IDs, so these calls load the settings saved by
  // the previous template switches during the first OTA migration.
  if (this->buzzer_switch_ != nullptr) {
    auto restored = this->buzzer_switch_->get_initial_state_with_restore_mode();
    this->set_buzzer_enabled(restored.value_or(true));
    this->buzzer_switch_->publish_state(this->get_buzzer_enabled());
  }
  if (this->vent24_switch_ != nullptr) {
    auto restored = this->vent24_switch_->get_initial_state_with_restore_mode();
    this->set_vent24_enabled(restored.value_or(false));
    this->vent24_switch_->publish_state(this->get_vent24_enabled());
  }

  this->standalone_ = (this->panel_rx_pin_ < 0);

  // 輸出腳 (獨立模式只有 machine_tx)
  gpio_config_t out = {};
  out.pin_bit_mask = (1ULL << this->machine_tx_pin_);
  if (!this->standalone_)
    out.pin_bit_mask |= (1ULL << this->panel_tx_pin_);
  out.mode = GPIO_MODE_OUTPUT;
  gpio_config(&out);
  fast_write(this->machine_tx_pin_, 0);

  if (this->standalone_) {
    // 取代面板模式: 我們是 master, 開機即持續送出狀態幀 (預設關機)
    this->override_ = true;
    ESP_LOGI(TAG, "獨立模式(取代面板): 恆為 master, 以 1Hz 送出狀態幀");
  } else {
    fast_write(this->panel_tx_pin_, 0);
    // 輸入腳 + 雙邊緣中斷 (機器 + 面板, 鏡射用)
    gpio_config_t in = {};
    in.pin_bit_mask = (1ULL << this->panel_rx_pin_) | (1ULL << this->machine_rx_pin_);
    in.mode = GPIO_MODE_INPUT;
    in.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&in);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add((gpio_num_t) this->panel_rx_pin_, isr_panel, this);
    gpio_isr_handler_add((gpio_num_t) this->machine_rx_pin_, isr_machine, this);
  }

  // --- RMT RX: 機器方向硬體精準擷取, 用獨立腳 (機器TX 硬體分接到 rmt_rx_pin) ---
  rmt_rx_channel_config_t rxcfg = {};
  rxcfg.gpio_num = (gpio_num_t) this->rmt_rx_pin_;
  rxcfg.clk_src = RMT_CLK_SRC_DEFAULT;
  rxcfg.resolution_hz = 1000000;   // 1 tick = 1us
  rxcfg.mem_block_symbols = 96;   // C3: 48 words/channel → 佔 2 個記憶體區塊 (= C3 全部 2 個 RX channel), IDF 5.5 合法
  esp_err_t rerr = rmt_new_rx_channel(&rxcfg, &this->rx_chan_);
  if (rerr != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_rx_channel 失敗: %d", rerr);
  } else {
    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = rmt_rx_done;
    rmt_rx_register_event_callbacks(this->rx_chan_, &cbs, this);
    rmt_enable(this->rx_chan_);
    this->rx_cfg_.signal_range_min_ns = 1000;        // 1us (ESP32 filter 上限~3us);振鈴改軟體濾
    this->rx_cfg_.signal_range_max_ns = 30000000;    // 30ms idle (看完整週期)
    this->rx_arm_err_ = rmt_receive(this->rx_chan_, this->rx_raw_, sizeof(this->rx_raw_), &this->rx_cfg_);
    this->rx_ok_ = true;
    ESP_LOGI(TAG, "RMT RX 已啟動於 GPIO%d (機器TX需分接到此腳)", this->rmt_rx_pin_);
  }

  // 濾網運轉時數 NVS (固定 hash key, 與 devicename 無關)
  this->runtime_pref_ = global_preferences->make_preference<uint32_t>(0x4C464C54UL);
  uint32_t rt = 0;
  if (this->runtime_pref_.load(&rt)) {
    this->runtime_total_s_ = rt;
    this->last_saved_total_s_ = rt;
    ESP_LOGI(TAG, "濾網時數載入: %.1f 小時", rt / 3600.0f);
  }

  this->build_tx_(this->override_bits_);
#if CONFIG_FREERTOS_UNICORE
  // C3 移植: 單核 (ESP32-C3) 綁定 core 1 會觸發 configASSERT(taskVALID_CORE_ID) → 改為不綁核
  const BaseType_t worker_core = tskNO_AFFINITY;
#else
  const BaseType_t worker_core = 1;   // 雙核 ESP32: 維持原本綁定 core 1
#endif
  xTaskCreatePinnedToCore(worker_task, "lg_erv", 8192, this, 19, nullptr, worker_core);
  ESP_LOGI(TAG, "reset原因=%d (1=POR 3=SW 4=PANIC 5=INT_WDT 6=TASK_WDT)", (int) esp_reset_reason());

  if (!this->standalone_)
    ESP_LOGI(TAG, "中繼啟動: 面板(rx=GPIO%d tx=GPIO%d) 機器(rx=GPIO%d tx=GPIO%d)",
             this->panel_rx_pin_, this->panel_tx_pin_, this->machine_rx_pin_, this->machine_tx_pin_);

  // 開機保護: 15 秒後才接受 HA 控制,避免 select/switch 還原初值時誤觸發 override
  this->set_timeout("accept", 15000, [this]() {
    this->accept_cmd_ = true;
    ESP_LOGI(TAG, "開機保護解除,可接受 HA 控制");
    if (!this->boot_pending_bits_.empty()) {
      std::string bits;
      std::swap(bits, this->boot_pending_bits_);
      ESP_LOGI(TAG, "補發開機保護期間暫存的控制指令");
      this->set_override_state(bits);
      // 暫存的暖房溫控是 30°C 基準碼, 補套偏好溫度
      if (this->heat_temperature_select_ != nullptr && this->is_heat_temp_mode()) {
        auto index = this->heat_temperature_select_->active_index();
        if (index.has_value()) this->set_heat_temp(25 + (int) index.value());
      }
    }
  });

}

void LifegearBathHeat::loop() {
  const uint32_t now = millis();
  if (now - this->last_entity_update_ms_ < 1000) return;
  this->last_entity_update_ms_ = now;
  this->update_entities_();
}

void LifegearBathHeat::update_entities_() {
  const uint32_t now = millis();
  if (this->mode_select_ != nullptr) {
    const int mode_index = this->get_mode_index();
    if (mode_index >= 0) publish_select_if_changed_(this->mode_select_, static_cast<size_t>(mode_index));
  }

  if (this->heat_temperature_select_ != nullptr) {
    auto temperature = this->get_temp_str();
    if (temperature.has_value()) {
      auto index = this->heat_temperature_select_->index_of(*temperature);
      if (index.has_value()) this->heat_temperature_select_->sync_actual(index.value());
    }
  }

  bool left_light = false;
  bool right_light = false;
  if (this->left_light_switch_ != nullptr || this->left_light_state_binary_sensor_ != nullptr)
    left_light = this->get_light(23);
  if (this->right_light_switch_ != nullptr || this->right_light_state_binary_sensor_ != nullptr)
    right_light = this->get_light(22);
  if (this->left_light_switch_ != nullptr) this->left_light_switch_->publish_state(left_light);
  if (this->right_light_switch_ != nullptr) this->right_light_switch_->publish_state(right_light);
  if (this->buzzer_switch_ != nullptr) this->buzzer_switch_->publish_state(this->get_buzzer_enabled());
  if (this->vent24_switch_ != nullptr) this->vent24_switch_->publish_state(this->get_vent24_enabled());

  if (this->filter_hours_sensor_ != nullptr &&
      (this->filter_hours_dirty_ || now - this->last_filter_hours_check_ms_ >= FILTER_HOURS_CHECK_INTERVAL_MS)) {
    // 濾網時數只需要 0.1h 顯示精度。每分鐘取樣、量化後再比較，避免運轉中每秒寫入 HA。
    const float runtime_hours = std::round(this->get_runtime_h() * 10.0f) / 10.0f;
    publish_sensor_if_changed_(this->filter_hours_sensor_, runtime_hours);
    this->last_filter_hours_check_ms_ = now;
    this->filter_hours_dirty_ = false;
  }
  if (this->timer_left_sensor_ != nullptr) {
    const int minutes = this->get_timer_left_min();
    publish_sensor_if_changed_(this->timer_left_sensor_, minutes < 0 ? NAN : static_cast<float>(minutes));
  }

  if (this->host_status_text_sensor_ != nullptr)
    publish_text_if_changed_(this->host_status_text_sensor_, this->get_localized_machine_status());
  if (this->current_mode_text_sensor_ != nullptr)
    publish_text_if_changed_(this->current_mode_text_sensor_, this->get_localized_mode_name());
  if (this->raw_state_text_sensor_ != nullptr) {
    const std::string raw_state = this->get_panel_state();
    if (!this->raw_state_text_sensor_->has_state() ||
        (this->raw_state_text_sensor_->get_state() != raw_state &&
         now - this->last_raw_state_publish_ms_ >= RAW_STATE_PUBLISH_INTERVAL_MS)) {
      this->raw_state_text_sensor_->publish_state(raw_state);
      this->last_raw_state_publish_ms_ = now;
    }
  }

  if (this->filter_due_binary_sensor_ != nullptr)
    publish_binary_if_changed_(this->filter_due_binary_sensor_, this->is_filter_due());
  if (this->override_binary_sensor_ != nullptr)
    publish_binary_if_changed_(this->override_binary_sensor_, this->is_override());
  if (this->left_light_state_binary_sensor_ != nullptr)
    publish_binary_if_changed_(this->left_light_state_binary_sensor_, left_light);
  if (this->right_light_state_binary_sensor_ != nullptr)
    publish_binary_if_changed_(this->right_light_state_binary_sensor_, right_light);
}

void LifegearBathHeat::dump_config() {
  if (this->standalone_) {
    ESP_LOGCONFIG(TAG, "Lifegear 浴室暖風機 (獨立/取代面板):");
    ESP_LOGCONFIG(TAG, "  機器: 送 GPIO%d / RMT 收 GPIO%d", this->machine_tx_pin_, this->rmt_rx_pin_);
  } else {
    ESP_LOGCONFIG(TAG, "Lifegear 浴室暖風機 (中繼):");
    ESP_LOGCONFIG(TAG, "  面板: 收 GPIO%d / 送 GPIO%d", this->panel_rx_pin_, this->panel_tx_pin_);
    ESP_LOGCONFIG(TAG, "  機器: 收 GPIO%d / 送 GPIO%d", this->machine_rx_pin_, this->machine_tx_pin_);
  }
}

}  // namespace lifegear_bath_heat
}  // namespace esphome
