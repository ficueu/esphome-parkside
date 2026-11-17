#pragma once

// Dekoder ramki CS baterii Parkside X20V, bez zależności od ESPHome (testowalny na PC).
//
// Ramka: NRZ, MSB first, frame_bits bitów (40 = 5 bajtów), bajt 0 = 0x00.
// Spoczynek = stan wysoki, więc ramka zaczyna się zboczem opadającym, a ostatni
// odcinek w górze zlewa się z ciszą. Wejście to długości odcinków w us,
// naprzemiennie dół/góra, zaczynając od dołu (liczba odcinków jest nieparzysta).
//
// Czas bitu różni się między bateriami (zmierzone: 960,6 us PAP 20 B1, 994,6 us PAP 20 B3,
// 1004,8 us PAPS 204 A1), dlatego przy auto_bit estymujemy go z krótkich odcinków ramki,
// startując od wartości zapamiętanej z poprzednich ramek.

#include <cmath>
#include <cstdint>

namespace esphome {
namespace parkside_bus {

enum class DecodeError : uint8_t {
  NONE = 0,
  EVEN_RUNS,     // parzysta liczba odcinków -> ramka nie kończy się powrotem do stanu wysokiego
  ZERO_RUN,      // odcinek krótszy niż pół bitu
  TOO_LONG,      // więcej bitów niż frame_bits
  NO_PREAMBLE,   // bajt 0 != 0x00
};

struct DecodeResult {
  DecodeError error{DecodeError::NONE};
  uint8_t bytes[8]{};
  uint8_t nbytes{0};
  float bit_time_us{0.0f};
};

inline const char *decode_error_str(DecodeError e) {
  switch (e) {
    case DecodeError::NONE:
      return "ok";
    case DecodeError::EVEN_RUNS:
      return "parzysta liczba odcinków";
    case DecodeError::ZERO_RUN:
      return "odcinek krótszy niż pół bitu";
    case DecodeError::TOO_LONG:
      return "ramka dłuższa niż frame_bits";
    case DecodeError::NO_PREAMBLE:
      return "brak preambuły 0x00";
  }
  return "?";
}

inline uint32_t runs_in_bits(uint32_t d, float tb) {
  return static_cast<uint32_t>(std::lround(static_cast<double>(d) / tb));
}

// Estymata czasu bitu tylko z krótkich odcinków (1-4 bity): przy odchyłce zegara do ~10%
// zaokrąglają się jednoznacznie, a długi odcinek nie przeciąga wyniku. Zwraca 0, gdy
// krótkich bitów jest za mało (< min_bits), żeby estymacie ufać.
inline float estimate_bit_time(const uint32_t *runs, uint8_t n, float tb, uint32_t min_bits = 6) {
  uint64_t dsum = 0;
  uint32_t nsum = 0;
  for (uint8_t i = 0; i < n; i++) {
    const uint32_t k = runs_in_bits(runs[i], tb);
    if (k >= 1 && k <= 4) {
      dsum += runs[i];
      nsum += k;
    }
  }
  return nsum >= min_bits ? static_cast<float>(dsum) / nsum : 0.0f;
}

// start_tb_us: punkt startowy (np. czas bitu zapamiętany z poprzednich ramek tej baterii).
inline DecodeResult decode_frame(const uint32_t *runs, uint8_t n, float start_tb_us, uint32_t nominal_us,
                                 uint8_t frame_bits, bool auto_bit) {
  DecodeResult r;
  const float lo = 0.85f * nominal_us, hi = 1.15f * nominal_us;
  float tb = static_cast<float>(nominal_us);
  if (auto_bit && start_tb_us >= lo && start_tb_us <= hi)
    tb = start_tb_us;

  if (n == 0 || (n % 2) == 0) {
    r.error = DecodeError::EVEN_RUNS;
    r.bit_time_us = tb;
    return r;
  }

  if (auto_bit) {
    for (int pass = 0; pass < 2; pass++) {
      const float est = estimate_bit_time(runs, n, tb);
      if (est >= lo && est <= hi)
        tb = est;
    }
  }
  r.bit_time_us = tb;

  uint64_t acc = 0;
  uint16_t nbits = 0;
  uint8_t level = 0;
  for (uint8_t i = 0; i < n; i++) {
    const uint32_t k = runs_in_bits(runs[i], tb);
    if (k == 0) {
      r.error = DecodeError::ZERO_RUN;
      return r;
    }
    for (uint32_t j = 0; j < k; j++) {
      if (nbits < frame_bits)
        acc = (acc << 1) | level;
      nbits++;
    }
    level ^= 1;
  }
  if (nbits > frame_bits) {
    r.error = DecodeError::TOO_LONG;
    return r;
  }
  while (nbits < frame_bits) {
    acc = (acc << 1) | 1;
    nbits++;
  }

  r.nbytes = frame_bits / 8;
  for (uint8_t j = 0; j < r.nbytes; j++)
    r.bytes[j] = (acc >> (frame_bits - 8 * (j + 1))) & 0xFF;
  if (r.bytes[0] != 0x00)
    r.error = DecodeError::NO_PREAMBLE;
  return r;
}

}  // namespace parkside_bus
}  // namespace esphome
