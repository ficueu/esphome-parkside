#include "parkside_bus.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace parkside_bus {

static const char *const TAG = "parkside_bus";

static const char *const STATUS_CHARGE_REQUEST = "charge_request";  // ... 44 77
static const char *const STATUS_CHARGE_DONE = "charge_done";        // ... 22 CF
static const char *const STATUS_UNKNOWN = "unknown_frame";          // poprawna ramka, nieznana treść
static const char *const STATUS_INVALID = "invalid_frame";          // nie da się zdekodować
static const char *const STATUS_IDLE_HIGH = "idle_high";            // linia w górze, brak ramek -> MCU milczy
static const char *const STATUS_LINE_LOW = "line_low";              // linia ściągnięta na stałe

void IRAM_ATTR ISRStore::gpio_intr(ISRStore *arg) {
  const uint32_t now = micros();
  const uint8_t n = arg->count;
  // Szpilka: zbocze zbyt blisko poprzedniego -> usuń poprzednie, pomiń bieżące.
  // Nieparzysta liczba drgań na zboczu daje w efekcie jedno zbocze (ostatnie).
  if (n > 0 && (now - arg->edges[n - 1]) < arg->glitch_us) {
    arg->count = n - 1;
    return;
  }
  if (n >= MAX_EDGES) {
    arg->overflow = true;
    return;
  }
  arg->edges[n] = now;
  arg->count = n + 1;
}

void ParksideBus::setup() {
  this->pin_->setup();
  this->pin_->attach_interrupt(ISRStore::gpio_intr, &this->store_, gpio::INTERRUPT_ANY_EDGE);
  this->last_frame_ms_ = millis();
}

void ParksideBus::loop() {
  uint32_t edges[MAX_EDGES];
  uint8_t count = 0;
  bool overflow = false;
  {
    InterruptLock lock;
    const uint8_t n = this->store_.count;
    if (n > 0 && (this->store_.overflow || (micros() - this->store_.edges[n - 1]) > this->frame_gap_us_)) {
      for (uint8_t i = 0; i < n; i++)
        edges[i] = this->store_.edges[i];
      count = n;
      overflow = this->store_.overflow;
      this->store_.count = 0;
      this->store_.overflow = false;
    }
  }

  if (count > 0)
    this->process_frame_(edges, count, overflow);

  if (millis() - this->last_frame_ms_ > this->status_timeout_ms_) {
    this->set_status_(this->pin_->digital_read() ? STATUS_IDLE_HIGH : STATUS_LINE_LOW);
    this->bit_time_est_us_ = 0.0f;  // brak ramek -> pewnie inna bateria, kalibracja od nowa
  }
}

void ParksideBus::process_frame_(const uint32_t *edges, uint8_t count, bool overflow) {
  if (count < 2) {
    ESP_LOGD(TAG, "Pojedyncze zbocze, linia teraz %s", this->pin_->digital_read() ? "HIGH" : "LOW");
    return;
  }

  // Odcinki (us) między zboczami: naprzemiennie dół/góra, zaczynając od dołu.
  uint32_t runs[MAX_EDGES];
  const uint8_t nruns = count - 1;
  std::string us_str;
  char buf[12];
  for (uint8_t i = 0; i < nruns; i++) {
    runs[i] = edges[i + 1] - edges[i];
    if (i > 0)
      us_str += ',';
    snprintf(buf, sizeof(buf), "%" PRIu32, runs[i]);
    us_str += buf;
  }

  const DecodeResult res = decode_frame(runs, nruns, this->bit_time_est_us_, this->bit_time_us_, this->frame_bits_,
                                        this->auto_bit_time_);
  const bool valid = !overflow && res.error == DecodeError::NONE;
  if (valid && this->auto_bit_time_) {
    // Zegar baterii jest stabilny (±0,2% między ramkami), więc uśredniamy po ramkach.
    this->bit_time_est_us_ = this->bit_time_est_us_ > 0.0f
                                 ? 0.8f * this->bit_time_est_us_ + 0.2f * res.bit_time_us
                                 : res.bit_time_us;
  }
  const char *reason = overflow ? "przepełnienie bufora" : decode_error_str(res.error);

  std::vector<uint8_t> bytes;
  std::string hex;
  if (valid) {
    bytes.assign(res.bytes, res.bytes + res.nbytes);
    for (uint8_t j = 0; j < res.nbytes; j++) {
      snprintf(buf, sizeof(buf), j ? " %02X" : "%02X", res.bytes[j]);
      hex += buf;
    }
  }

  float interval_ms = NAN;
  if (this->have_frame_)
    interval_ms = (edges[0] - this->last_frame_start_us_) / 1000.0f;
  this->last_frame_start_us_ = edges[0];
  this->have_frame_ = true;
  this->last_frame_ms_ = millis();

  if (this->frame_interval_s_ != nullptr && !std::isnan(interval_ms))
    this->frame_interval_s_->publish_state(interval_ms);

  if (!valid) {
    ESP_LOGW(TAG, "Niepoprawna ramka (%s), %u zboczy, us: [%s]", reason, count, us_str.c_str());
    if (this->frame_us_ts_ != nullptr)
      this->frame_us_ts_->publish_state(us_str);
    this->set_status_(STATUS_INVALID);
    return;
  }

  ESP_LOGD(TAG, "Ramka [%s] co %.1f ms, bit %.1f us, us: [%s]", hex.c_str(), interval_ms, res.bit_time_us,
           us_str.c_str());

  if (this->bit_time_s_ != nullptr)
    this->bit_time_s_->publish_state(res.bit_time_us);

  // Do HA tylko przy zmianie treści, żeby nie zalewać recordera ramką co ~240 ms.
  if (hex != this->last_frame_str_) {
    this->last_frame_str_ = hex;
    if (this->frame_ts_ != nullptr)
      this->frame_ts_->publish_state(hex);
    if (this->frame_us_ts_ != nullptr)
      this->frame_us_ts_->publish_state(us_str);
  }

  // Znane ramki (bajt 2 = 0x33 dla 2 Ah, 0xCC dla 4 Ah; nie wpływa na status).
  // 44 77 = bateria chce ładowania, 22 CF = pełna, ładowarka przerywa.
  if (bytes.size() >= 5 && bytes[3] == 0x44 && bytes[4] == 0x77) {
    this->set_status_(STATUS_CHARGE_REQUEST);
  } else if (bytes.size() >= 5 && bytes[3] == 0x22 && bytes[4] == 0xCF) {
    this->set_status_(STATUS_CHARGE_DONE);
  } else {
    this->set_status_(STATUS_UNKNOWN);
  }

  this->frame_callback_.call(bytes);
}

void ParksideBus::set_status_(const char *status) {
  if (this->status_ == status)
    return;
  this->status_ = status;
  ESP_LOGI(TAG, "Status: %s", status);
  if (this->status_ts_ != nullptr)
    this->status_ts_->publish_state(status);
}

void ParksideBus::dump_config() {
  ESP_LOGCONFIG(TAG, "Parkside bus reader:");
  LOG_PIN("  Pin: ", this->pin_);
  ESP_LOGCONFIG(TAG, "  Bit time: %" PRIu32 " us (auto: %s), frame: %u bits", this->bit_time_us_,
                YESNO(this->auto_bit_time_), this->frame_bits_);
  ESP_LOGCONFIG(TAG, "  Frame gap: %" PRIu32 " us", this->frame_gap_us_);
  ESP_LOGCONFIG(TAG, "  Glitch filter: %" PRIu32 " us", this->store_.glitch_us);
  ESP_LOGCONFIG(TAG, "  Status timeout: %" PRIu32 " ms", this->status_timeout_ms_);
  LOG_TEXT_SENSOR("  ", "Frame", this->frame_ts_);
  LOG_TEXT_SENSOR("  ", "Frame us", this->frame_us_ts_);
  LOG_TEXT_SENSOR("  ", "Status", this->status_ts_);
  LOG_SENSOR("  ", "Frame interval", this->frame_interval_s_);
  LOG_SENSOR("  ", "Measured bit time", this->bit_time_s_);
}

}  // namespace parkside_bus
}  // namespace esphome
