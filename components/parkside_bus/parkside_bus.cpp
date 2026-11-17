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

  if (millis() - this->last_frame_ms_ > this->status_timeout_ms_)
    this->set_status_(this->pin_->digital_read() ? STATUS_IDLE_HIGH : STATUS_LINE_LOW);
}

void ParksideBus::process_frame_(const uint32_t *edges, uint8_t count, bool overflow) {
  if (count < 2) {
    ESP_LOGD(TAG, "Pojedyncze zbocze, linia teraz %s", this->pin_->digital_read() ? "HIGH" : "LOW");
    return;
  }

  // Surowe czasy (us) do diagnostyki.
  std::string us_str;
  char buf[12];
  for (uint8_t i = 1; i < count; i++) {
    if (i > 1)
      us_str += ',';
    snprintf(buf, sizeof(buf), "%" PRIu32, edges[i] - edges[i - 1]);
    us_str += buf;
  }

  // Dekodowanie: odcinki naprzemiennie dół/góra, zaczynając od dołu (spoczynek = góra,
  // ramka zaczyna się zboczem opadającym i kończy narastającym -> parzysta liczba zboczy).
  // Długość odcinka / czas bitu = liczba bitów. Ostatni odcinek w górze zlewa się z ciszą,
  // więc ramkę dopełniamy jedynkami do frame_bits_.
  bool valid = !overflow && (count % 2) == 0;
  const char *reason = overflow ? "przepełnienie bufora" : ((count % 2) ? "nieparzysta liczba zboczy" : nullptr);
  uint64_t acc = 0;
  uint16_t nbits = 0;
  uint8_t level = 0;
  for (uint8_t i = 1; i < count && valid; i++) {
    const uint32_t dt = edges[i] - edges[i - 1];
    const uint32_t n = (dt + this->bit_time_us_ / 2) / this->bit_time_us_;
    if (n == 0) {
      valid = false;
      reason = "odcinek krótszy niż pół bitu";
      break;
    }
    for (uint32_t k = 0; k < n; k++) {
      if (nbits < this->frame_bits_)
        acc = (acc << 1) | level;
      nbits++;
    }
    level ^= 1;
  }
  if (valid && nbits > this->frame_bits_) {
    valid = false;
    reason = "ramka dłuższa niż frame_bits";
  }

  std::vector<uint8_t> bytes;
  std::string hex;
  if (valid) {
    while (nbits < this->frame_bits_) {
      acc = (acc << 1) | 1;
      nbits++;
    }
    const uint8_t nbytes = this->frame_bits_ / 8;
    bytes.reserve(nbytes);
    for (uint8_t j = 0; j < nbytes; j++) {
      const uint8_t b = (acc >> (this->frame_bits_ - 8 * (j + 1))) & 0xFF;
      bytes.push_back(b);
      snprintf(buf, sizeof(buf), j ? " %02X" : "%02X", b);
      hex += buf;
    }
    if (bytes[0] != 0x00) {
      valid = false;
      reason = "brak preambuły 0x00";
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

  ESP_LOGD(TAG, "Ramka [%s] co %.1f ms, us: [%s]", hex.c_str(), interval_ms, us_str.c_str());

  // Do HA tylko przy zmianie treści, żeby nie zalewać recordera ramką co ~240 ms.
  if (hex != this->last_frame_str_) {
    this->last_frame_str_ = hex;
    if (this->frame_ts_ != nullptr)
      this->frame_ts_->publish_state(hex);
    if (this->frame_us_ts_ != nullptr)
      this->frame_us_ts_->publish_state(us_str);
  }

  // Znane ramki (bajt 2 to 0x33 lub 0xCC zależnie od baterii, nie wpływa na status).
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
  ESP_LOGCONFIG(TAG, "  Bit time: %" PRIu32 " us, frame: %u bits", this->bit_time_us_, this->frame_bits_);
  ESP_LOGCONFIG(TAG, "  Frame gap: %" PRIu32 " us", this->frame_gap_us_);
  ESP_LOGCONFIG(TAG, "  Glitch filter: %" PRIu32 " us", this->store_.glitch_us);
  ESP_LOGCONFIG(TAG, "  Status timeout: %" PRIu32 " ms", this->status_timeout_ms_);
  LOG_TEXT_SENSOR("  ", "Frame", this->frame_ts_);
  LOG_TEXT_SENSOR("  ", "Frame us", this->frame_us_ts_);
  LOG_TEXT_SENSOR("  ", "Status", this->status_ts_);
  LOG_SENSOR("  ", "Frame interval", this->frame_interval_s_);
}

}  // namespace parkside_bus
}  // namespace esphome
