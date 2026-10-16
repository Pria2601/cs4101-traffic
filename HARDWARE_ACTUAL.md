# Actual Hardware (from teammate, 2026-10-15)

Replaces the UNO/ESP32 plan in HARDWARE_CHECKLIST.md.

## Boards
- Main controller: **Arduino Mega 2560**
- WiFi board: **NodeMCU ESP8266** (ESP-12E, CP2102 USB)

## Pin map (Mega)
| Lane | IR | Green | Yellow | Red |
|---|---|---|---|---|
| 1 | D2 | D22 | D28 | D34 |
| 2 | D3 | D24 | D30 | D36 |
| 3 | D4 | D52 | D32 | D38 |
| 4 | D5 | D40 | D48 | D44 |

## Serial link
- Mega TX1 (D18) → voltage divider → NodeMCU D6 (GPIO12). Tested OK at 9600 baud, JSON telemetry.
- Common GND: yes.
- **Missing:** NodeMCU → Mega return wire (needed for cloud commands, rubric 3c).
  Proposed: NodeMCU D5 (GPIO14) → Mega RX1 (D19), direct wire.

## Confirmed
- 4 lanes; IR obstacle sensors, active-LOW, INPUT_PULLUP; 80 ms debounce in code
- LEDs through series resistors, common GND
- No buzzer/button/LCD

## Still pending from teammate
- [ ] Mega + NodeMCU code she has already written
- [ ] Top-view photo with lane numbers
- [ ] IR sensor exact model
- [ ] Sensor position vs stop line (before / at)
- [ ] Trigger duration for a passing toy car + false triggers under light
- [ ] LED polarity photo (HIGH = ON?)
- [ ] Power for final demo (USB / power bank)
- [ ] Add the return serial wire
- [ ] Photos, circuit diagram, working video for report
