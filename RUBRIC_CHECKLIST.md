# Rubric Checklist — CS4101 (20 marks)

Tick each only when there is a **screenshot/photo proof** saved for the report.

## Report format
- [ ] File name: `grpXX_CS4101_26.pdf` (XX = our group number)
- [ ] Submitted by group leader only, via the given link, before the deadline
- [ ] Sections: a) Objective b) Implemented attributes c) Setup diagram d) Logical workflow e) Sample outputs for each scenario f) User manual
- [ ] No code in the report

## 1. Features (5)
| # | Requirement | How we meet it | Proof |
|---|---|---|---|
| 1a | Sensor senses | 4 IR sensors count vehicles per lane | Serial Monitor screenshot |
| 1b | Actuator reacts to a command | LEDs change when a cloud command arrives (override/emergency) | Photo before/after + MQTT screenshot |
| 1c | IoT network → Internet → Cloud | NodeMCU WiFi → hotspot → AWS IoT Core (MQTT) | MQTT test client screenshot |
| 1d | Data visualization | Web dashboard (S3 website) with live counts, signal states, predictions | Dashboard screenshot |
| 1e | Cloud app controls full system | Dashboard buttons: Auto / Manual lane / Emergency mode / change thresholds | Dashboard + LED photos |

## 2. Components (5)
| # | Requirement | Ours |
|---|---|---|
| 2a | Sensor + actuator | IR sensors + traffic LEDs |
| 2b | MCU collecting sensor data | Arduino Mega 2560 |
| 2c | IoT networking device (not phone/laptop) | NodeMCU ESP8266 |
| 2d | Cloud app: store, visualize, send commands | AWS IoT Core, DynamoDB, Lambda, S3, API Gateway, SNS |
| 2e | Data analytics model | Congestion classifier (Decision Tree / Random Forest) |

## 3. Data flows (5)
| # | Flow | Ours |
|---|---|---|
| 3a | Sensor → Arduino → Serial Monitor | Mega prints counts, scores, signal state |
| 3b | Device → router → cloud → displayed | NodeMCU → hotspot → IoT Core → DynamoDB → dashboard |
| 3c | Cloud → router → MCU → actuator | Dashboard → API Gateway → Lambda → MQTT `traffic/sub` → NodeMCU → Mega → LEDs |
| 3d | Email/SMS alert | SNS email when congestion HIGH for long / emergency mode / sensor fault |
| 3e | Serial Arduino → ESP | Mega Serial1 TX1 (D18) → NodeMCU D6; return NodeMCU D5 → Mega RX1 (D19) |

## 4. Data analytics (1 + 2 + 2)
| # | Requirement | Ours |
|---|---|---|
| 4a | **Own collected** dataset + details | Our sensor data from DynamoDB → S3 CSV. Report: no. of rows, columns, sampling interval, sessions, dates, label rule |
| 4b | Model trained on that dataset; name + purpose | Decision Tree / Random Forest; predicts congestion level (Low/Med/High) per lane. Report accuracy + confusion matrix |
| 4c | Real-time inference on live cloud data, shown on dashboard | IoT rule triggers Lambda on every message → prediction stored → dashboard shows it live |

## Scenarios to screenshot (report section e)
1. Normal flow (weighted priority choosing lanes)
2. Heavy lane gets longer green
3. Anti-starvation (long-waiting lane served)
4. Manual override from dashboard
5. Emergency mode from dashboard
6. Email alert received
7. Live ML prediction on dashboard
8. DynamoDB table + S3 CSV + model accuracy
