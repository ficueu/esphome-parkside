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
| Kodowanie | NRZ, MSB first, bit nominalnie 1 ms (zmierzone 0,96–1,005 ms zależnie od baterii) |
| Ramka | 40 bitów = 5 bajtów, bajt 0 = `0x00` (preambuła, 8 bitów w dole) |
| Przerwa | ok. 203 bity w stanie wysokim, okres ramek 241–243 bity (233–242 ms) |
| Kierunek | tylko bateria → ładowarka, ładowarka nic nie odpowiada |

Zmierzone parametry (ESP32, `parkside_bus`):

| Bateria | Czas bitu | Okres ramek | Jitter zboczy |
|---|---|---|---|
| PAP 20 B1 2 Ah | 960,6 µs | 233,3 ms (242,9 bitu) | σ 14 µs, maks. 46 µs |
| PAP 20 B3 4 Ah (uszkodzona) | 994,6 µs | 241,6 ms (243,0 bitu) | – |
| PAPS 204 A1 4 Ah Smart | 1004,8 µs | 242,2 ms (241,0 bitu) | – |

Zegar różni się między bateriami o kilka procent, dlatego dekoder domyślnie dopasowuje czas bitu
(`auto_bit_time`) z krótkich odcinków każdej ramki i pamięta go między ramkami.

Znane ramki:

| Ramka | Znaczenie | Bateria / źródło |
|---|---|---|
| `00 88 33 44 77` | żądanie ładowania | PAP 20 A1 2 Ah (Aifryz), PAP 20 B1 2 Ah (pomiar) |
| `00 88 33 22 CF` | pełna – ładowarka przerywa ładowanie | PAP 20 B1 2 Ah (pomiar) |
| `00 88 CC 44 77` | żądanie ładowania | PAPS 204 A1 4 Ah (hackaday, pomiar) |
| `00 88 CC 22 CF` | pełna | PAPS 204 A1 4 Ah (hackaday, pomiar) |
| `00 88 CC 11 C3` | błąd? – 2 pierwsze ramki po wybudzeniu | PAP 20 B3 4 Ah, uszkodzona (nie ładuje, nie zasila) |
| `00 88 CC 06 3C` | błąd? – potem nadawana stale | jw. |

Interpretacja bajtów:

| Bajt | Wartości | Hipoteza |
|---|---|---|
| 0 | `00` | preambuła |
| 1 | `88` | stały we wszystkich zebranych ramkach |
| 2 | `33` / `CC` | klasa baterii: `33` = 2 Ah, `CC` = 4 Ah (potwierdzone na 2+2 bateriach różnych generacji); być może prąd ładowania |
| 3 | `44`, `22`, `11`, `06` | stan; `44`/`22`/`11` to jeden bit w półbajcie powtórzonym dwa razy (0100, 0010, 0001), `06` łamie ten schemat |
| 4 | `77`, `CF`, `C3`, `3C` | zawsze w parze z bajtem 3 (`44↔77`, `22↔CF`, `11↔C3`, `06↔3C`), nie zależy od bajtu 2; nie pasuje do prostej sumy/XOR |

Jeśli masz inne baterie (8 Ah, 12 Ah, nowa generacja PAP 202/204, inne usterki) – ramki z logu mile widziane w issues.

## Linia T

Wg positron96: bateria w porządku ściąga T do masy przez ok. 10k (NTC + tranzystor), przy rozładowaniu (ok. 13 V na pakiecie) lub przeciążeniu puszcza linię.
W normalnej pracy MCU baterii kilka razy na sekundę puszcza T na ułamek sekundy, więc alarm trzeba filtrować (w przykładzie 1 s).

## Konfiguracja

```yaml
parkside_bus:
  - id: bus_cs
    pin: GPIO4
    bit_time: 1ms          # nominalny czas bitu
    auto_bit_time: true    # dopasowanie do zegara baterii (domyślnie włączone)
    frame_bits: 40         # długość ramki (wielokrotność 8, max 64)
    frame_gap: 100ms       # cisza zamykająca ramkę
    glitch_filter: 200us   # szpilki krótsze niż to są usuwane
    status_timeout: 1s     # brak ramek dłużej -> idle_high / line_low
    frame:                 # text_sensor, ramka hex, publikowana przy zmianie
      name: "CS ramka"
    frame_binary:          # text_sensor, bajty rozbite na bity, np. "00000000 10001000 ..."
      name: "CS ramka bity"
    frame_us:              # text_sensor (debug), surowe czasy odcinków w us
      name: "CS ramka us"
    status:                # text_sensor
      name: "CS status"
    frame_interval:        # sensor, okres ramek w ms
      name: "CS okres ramek"
    measured_bit_time:     # sensor diagnostyczny, zmierzony czas bitu w µs
      name: "CS czas bitu"
    on_frame:              # x = std::vector<uint8_t> (5 bajtów)
      - lambda: 'ESP_LOGI("cs", "bajt2=0x%02X", x[2]);'
```

Wartości `status`:

| Status | Znaczenie |
|---|---|
| `charge_request` | bajty 3–4 = `44 77` |
| `charge_done` | bajty 3–4 = `22 CF` (pełna, ładowarka przerywa) |
| `unknown_frame` | poprawna ramka, nieznana treść |
| `invalid_frame` | nie da się zdekodować (nieparzysta liczba zboczy, brak preambuły, za długa) |
| `idle_high` | brak ramek, linia w górze – MCU baterii milczy |
| `line_low` | brak ramek, linia ściągnięta na stałe – np. zwarty stopień wyjściowy |

`idle_high` / `line_low` przydają się przy diagnozie uszkodzonej baterii.

Log na poziomie `DEBUG` pokazuje każdą ramkę jako bajty i bity:

```
[D][parkside_bus]: Ramka [00 88 33 44 77] bity [00000000 10001000 00110011 01000100 01110111]
```

Na poziomie `VERBOSE` dochodzi okres ramek, zmierzony czas bitu i surowe czasy odcinków w µs.
Niepoprawne ramki (`invalid_frame`) zawsze są logowane z czasami w µs, bo bez bajtów to jedyna informacja.

Kolejne instancje (`MULTI_CONF`) pozwalają podglądać kilka linii naraz.

## Stan

- Przetestowane na prawdziwych bateriach: PAP 20 B1 2 Ah, PAPS 204 A1 4 Ah Smart, PAP 20 B3 4 Ah (uszkodzona) – wszystkie ramki zdekodowane.
- Dekoder (`frame_decoder.h`, bez zależności od ESPHome) przetestowany na PC na 64 zarejestrowanych ramkach (64/64)
  oraz na syntetycznych ramkach z odcinkami do 22 bitów: 100% przy zegarze baterii od −8% do +8% i jitterze ±50 µs.
- Firmware kompiluje się dla ESP32 (ESP-IDF, ESPHome 2026.10).

Test dekodera na PC (zarejestrowane ramki w `tests/captured_frames.txt`, format `ramka hex|odcinki w us`):

```sh
g++ -std=c++17 -I components/parkside_bus -o /tmp/test_decoder tests/test_decoder.cpp
/tmp/test_decoder tests/captured_frames.txt
```

## Źródła

- [Aifryz/parkside-battery-hack](https://github.com/Aifryz/parkside-battery-hack) – generator ramki `00 88 33 44 77`, 1 ms/bit (MIT)
- [LIDL PARKSIDE Battery hacks (hackaday.io)](https://hackaday.io/project/188089-lidl-parkside-battery-hacks) – pinout, pull-up ładowarki, ramki z komentarzy
- [positron96: Parkside battery protection](https://positron96.gitlab.io/projects/parkside-battery-protection) – zachowanie linii T
- [mmame/parkside-x20](https://github.com/mmame/parkside-x20) – zabezpieczenie oparte na linii T
