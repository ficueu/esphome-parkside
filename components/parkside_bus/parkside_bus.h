#pragma once

#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "frame_decoder.h"

namespace esphome {
namespace parkside_bus {

static const uint8_t MAX_EDGES = 64;

// Ramka CS: 40 bitów NRZ, MSB first, bajt 0 = 0x00 (preambuła, 8 bitów w dole),
// potem ~203 bity ciszy w górze; okres ~241-243 bity (233-242 ms zależnie od zegara baterii).
// Bateria ściąga linię otwartym drenem, ładowarka podciąga do 5 V (~7 kΩ).

// Bufor zboczy wypełniany w przerwaniu.
struct ISRStore {
  volatile uint32_t edges[MAX_EDGES];
  volatile uint8_t count{0};
  volatile bool overflow{false};
  uint32_t glitch_us{200};

  static void gpio_intr(ISRStore *arg);
};

class ParksideBus : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_pin(InternalGPIOPin *pin) { this->pin_ = pin; }
  void set_frame_gap(uint32_t us) { this->frame_gap_us_ = us; }
  void set_glitch_filter(uint32_t us) { this->store_.glitch_us = us; }
  void set_status_timeout(uint32_t ms) { this->status_timeout_ms_ = ms; }
  void set_bit_time(uint32_t us) { this->bit_time_us_ = us; }
  void set_frame_bits(uint8_t bits) { this->frame_bits_ = bits; }
  void set_auto_bit_time(bool en) { this->auto_bit_time_ = en; }

  void set_frame_text_sensor(text_sensor::TextSensor *s) { this->frame_ts_ = s; }
  void set_frame_us_text_sensor(text_sensor::TextSensor *s) { this->frame_us_ts_ = s; }
  void set_status_text_sensor(text_sensor::TextSensor *s) { this->status_ts_ = s; }
  void set_frame_interval_sensor(sensor::Sensor *s) { this->frame_interval_s_ = s; }
  void set_measured_bit_time_sensor(sensor::Sensor *s) { this->bit_time_s_ = s; }

  void add_on_frame_callback(std::function<void(const std::vector<uint8_t> &)> &&callback) {
    this->frame_callback_.add(std::move(callback));
  }

 protected:
  void process_frame_(const uint32_t *edges, uint8_t count, bool overflow);
  void set_status_(const char *status);

  InternalGPIOPin *pin_{nullptr};
  ISRStore store_{};
  uint32_t frame_gap_us_{100000};
  uint32_t status_timeout_ms_{1000};
  uint32_t bit_time_us_{1000};
  uint8_t frame_bits_{40};
  bool auto_bit_time_{true};
  float bit_time_est_us_{0.0f};  // zapamiętany czas bitu bieżącej baterii (0 = brak)

  uint32_t last_frame_start_us_{0};
  uint32_t last_frame_ms_{0};
  bool have_frame_{false};
  const char *status_{nullptr};
  std::string last_frame_str_;

  text_sensor::TextSensor *frame_ts_{nullptr};
  text_sensor::TextSensor *frame_us_ts_{nullptr};
  text_sensor::TextSensor *status_ts_{nullptr};
  sensor::Sensor *frame_interval_s_{nullptr};
  sensor::Sensor *bit_time_s_{nullptr};

  CallbackManager<void(const std::vector<uint8_t> &)> frame_callback_;
};

class FrameTrigger : public Trigger<std::vector<uint8_t>> {
 public:
  explicit FrameTrigger(ParksideBus *parent) {
    parent->add_on_frame_callback([this](const std::vector<uint8_t> &frame) { this->trigger(frame); });
  }
};

}  // namespace parkside_bus
}  // namespace esphome
