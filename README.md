# esphome-parkside

Komponent zewnętrzny ESPHome do odczytu sygnałów, które akumulator **Parkside X20V** wysyła do ładowarki i narzędzia:

- **linia CS** (w ładowarce opisana „ID”) – ramki cyfrowe z baterii do ładowarki, dekodowane do bajtów,
- **linia T** – temperatura + alarm BMS dla narzędzia (przykład z ADC w `examples/`).

Tylko odczyt. Komponent niczego nie nadaje na linie baterii.

## Instalacja

```yaml
external_components:
  - source: github://ficueu/esphome-parkside@main
    components: [parkside_bus]
```

Pełny przykład: [`examples/parkside-reader.yaml`](examples/parkside-reader.yaml).

## Podłączenie

| Bateria | Gdzie |
|---|---|
| P- | GND ESP i GND konwertera poziomów |
| CS | strona 5 V konwertera poziomów (np. BSS138) |
| T (opcjonalnie) | 10k do 5 V oraz dzielnik 100k/100k do pinu ADC |

Strona 3,3 V konwertera idzie na pin wejściowy ESP (w przykładzie GPIO4).
Pull-up do 5 V po stronie baterii udaje ładowarkę (ładowarka ma ok. 7 kΩ do 5 V) – bez niego bateria nie nadaje.

## Protokół linii CS

| Parametr | Wartość |
|---|---|
| Fizycznie | otwarty dren po stronie baterii, pull-up 5 V po stronie ładowarki, spoczynek = stan wysoki |
| Kodowanie | NRZ, 1 ms na bit, MSB first |
| Ramka | 40 bitów = 5 bajtów, bajt 0 = `0x00` (preambuła, 8 ms w dole) |
| Przerwa | ok. 200 ms w stanie wysokim, okres ramek ok. 240 ms |

Znane ramki:

| Ramka | Znaczenie | Źródło |
|---|---|---|
| `00 88 33 44 77` | żądanie ładowania, bateria 2 Ah (PAP 20 A1) | Aifryz, hackaday |
| `00 88 CC 44 77` | żądanie ładowania, PAPS 204 A1 | hackaday |
| `00 88 33 22 CF` | naładowana, bateria 2 Ah | pomiar własny |
| `00 88 CC 22 CF` | naładowana, PAPS 204 A1 | hackaday |

Interpretacja bajtów (hipotezy, niepotwierdzone): bajt 1 stały, bajt 2 zależy od typu baterii (być może klasa prądu ładowania), bajty 3–4 to stan.
Jeśli masz inne baterie (4 Ah bez Smart, 8 Ah, 12 Ah, nowa generacja PAP 202/204) – ramki z logu mile widziane w issues.

## Linia T

Wg positron96: bateria w porządku ściąga T do masy przez ok. 10k (NTC + tranzystor), przy rozładowaniu (ok. 13 V na pakiecie) lub przeciążeniu puszcza linię.
W normalnej pracy MCU baterii kilka razy na sekundę puszcza T na ułamek sekundy, więc alarm trzeba filtrować (w przykładzie 1 s).

## Konfiguracja

```yaml
parkside_bus:
  - id: bus_cs
    pin: GPIO4
    bit_time: 1ms          # czas bitu
    frame_bits: 40         # długość ramki (wielokrotność 8, max 64)
    frame_gap: 100ms       # cisza zamykająca ramkę
    glitch_filter: 200us   # szpilki krótsze niż to są usuwane
    status_timeout: 1s     # brak ramek dłużej -> idle_high / line_low
    frame:                 # text_sensor, ramka hex, publikowana przy zmianie
      name: "CS ramka"
    frame_us:              # text_sensor, surowe czasy odcinków w us
      name: "CS ramka us"
    status:                # text_sensor
      name: "CS status"
    frame_interval:        # sensor, okres ramek w ms
      name: "CS okres ramek"
    on_frame:              # x = std::vector<uint8_t> (5 bajtów)
      - lambda: 'ESP_LOGI("cs", "bajt2=0x%02X", x[2]);'
```

Wartości `status`:

| Status | Znaczenie |
|---|---|
| `charge_request` | bajty 3–4 = `44 77` |
| `charge_done` | bajty 3–4 = `22 CF` |
| `unknown_frame` | poprawna ramka, nieznana treść |
| `invalid_frame` | nie da się zdekodować (nieparzysta liczba zboczy, brak preambuły, za długa) |
| `idle_high` | brak ramek, linia w górze – MCU baterii milczy |
| `line_low` | brak ramek, linia ściągnięta na stałe – np. zwarty stopień wyjściowy |

`idle_high` / `line_low` przydają się przy diagnozie uszkodzonej baterii.

Kolejne instancje (`MULTI_CONF`) pozwalają podglądać kilka linii naraz.

## Stan

- Konfiguracja przechodzi `esphome config`, firmware kompiluje się dla ESP32 (ESP-IDF, ESPHome 2026.10).
- Dekoder przetestowany poza urządzeniem na syntetycznych ramkach: 100% przy dryfcie zegara baterii ±3% i jitterze zboczy do ±100 µs.
- Na prawdziwej baterii jeszcze nieprzetestowane.

## Źródła

- [Aifryz/parkside-battery-hack](https://github.com/Aifryz/parkside-battery-hack) – generator ramki `00 88 33 44 77`, 1 ms/bit (MIT)
- [LIDL PARKSIDE Battery hacks (hackaday.io)](https://hackaday.io/project/188089-lidl-parkside-battery-hacks) – pinout, pull-up ładowarki, ramki z komentarzy
- [positron96: Parkside battery protection](https://positron96.gitlab.io/projects/parkside-battery-protection) – zachowanie linii T
- [mmame/parkside-x20](https://github.com/mmame/parkside-x20) – zabezpieczenie oparte na linii T
