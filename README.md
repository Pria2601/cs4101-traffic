# CS4101 – Smart Traffic Signal (IoT + AWS + ML)

IR sensors count vehicles on 4 lanes. An Arduino Mega picks which lane gets green using
**weighted priority**. A NodeMCU sends the data to **AWS**, where a trained model **forecasts
congestion** live. A web dashboard shows everything and can **control the signal**, and alerts
are sent by **email**.

## How the pieces connect

```
IR sensors ─► Arduino Mega ──Serial1──► NodeMCU ──WiFi/MQTT──► AWS IoT Core ──rule──┬─► DynamoDB (TrafficReadings)
LEDs ◄─────────┘    ▲                     │                                           └─► Lambda traffic_infer ─► TrafficPredictions
                    └──── commands ◄──────┘◄── traffic/sub ◄── Lambda traffic_api ◄── API Gateway ◄── Dashboard (S3 website)
                                                                                         SNS email ◄─┘ (alerts)
DynamoDB ─► Lambda traffic_export ─► S3 CSV ─► train_model.py (laptop) ─► S3 model JSON ─► traffic_infer
```

## What each piece of code does

| File | Runs on | Job |
|---|---|---|
| `code/mega_traffic/mega_traffic.ino` | Arduino Mega | Counts vehicles, weighted-priority signal control, drives LEDs, prints status on Serial Monitor, sends telemetry JSON every 3 s, obeys commands |
| `code/nodemcu_gateway/nodemcu_gateway.ino` | NodeMCU ESP8266 | WiFi + AWS bridge only: Mega → `traffic/pub`, `traffic/sub` → Mega |
| `code/nodemcu_gateway/secrets.h` | NodeMCU | WiFi name/password, AWS endpoint, certificates (**never share once filled**) |
| `code/cloud/lambda_infer/` | AWS Lambda | For every reading: runs the model per lane, saves the forecast, sends congestion / sensor-fault emails |
| `code/cloud/lambda_api/` | AWS Lambda | Dashboard backend: `GET /state` (live data) and `POST /command` (control, needs PIN, emails on override) |
| `code/cloud/lambda_export/` | AWS Lambda | Exports all readings from DynamoDB to S3 as the training CSV |
| `code/ml/train_model.py` | Laptop | Trains + compares models on our data, exports the best one, makes report graphs |
| `code/deploy/setup.sh` + `traffic_aws_bundle.zip` | AWS CloudShell | Creates the whole AWS side in one go and generates `secrets.h` |
| `code/deploy/fake_device.sh` | AWS CloudShell | Pretends to be the NodeMCU for testing without hardware |
| `code/dashboard/index.html` | S3 website | Live lanes, forecast, queue chart, alerts, control buttons, settings |

### Signal logic (Mega) in plain words
- A car passing a red lane's sensor joins that lane's **queue**. During green, one queued car leaves every 2 s.
- **Score = 2 × waiting cars + 0.5 × seconds the first car has waited.**
- When a green ends, the waiting lane with the highest score goes next.
- **Green time = 5 s + 2 s per waiting car** (max 20 s).
- **Gap-out:** if the green lane empties and another lane is waiting, switch early (after the 5 s minimum).
- **Anti-starvation:** a lane waiting 40 s or more is served next, whatever the scores.
- Always Green → Yellow (3 s) → All-red (1 s) → next Green.
- Cloud modes: **AUTO**, **MANUAL lane N** (hold that lane green), **ALLRED** (emergency stop).
- Every number above can be changed from the dashboard.

### ML model in plain words
- **Purpose:** forecast each lane's congestion **30 s ahead** (LOW ≤ 2, MEDIUM 3–5, HIGH ≥ 6 waiting cars).
- **Features:** queue, wait time, is-green, arrivals in the last 9 s, queue change, other lanes' queue.
- **Models compared:** "no change" baseline vs Decision Tree vs Random Forest. The best macro-F1 is deployed.
- **Live inference:** runs in Lambda on every reading. The dashboard shows a forecast badge for each lane, and a predicted HIGH triggers an email.

## Build order
1. **Me (code side):** AWS setup + test without hardware → `docs/AWS_SETUP.md` Steps 1–10 + "Testing without hardware"
2. **Teammate (hardware):** upload `mega_traffic`, then `nodemcu_gateway` with my filled `secrets.h`, and test on the real setup
3. **Together:** collect data (3+ sessions), then I export, train and upload the model → `docs/AWS_SETUP.md` Step 11
4. Take screenshots for every scenario → `RUBRIC_CHECKLIST.md`

Both sketches compile cleanly with: AVR Boards 1.8.8, esp8266 3.1.2, PubSubClient 2.8, ArduinoJson 7.x.

## Folder
```
course_docs/         prof's PDFs
reference_repo/      CircuitDigest repo (idea reference only, not used as code)
code/                all our code
docs/                AWS setup guide (incl. testing without hardware, model training)
HARDWARE_ACTUAL.md   real board + pin map from teammate
RUBRIC_CHECKLIST.md  marks checklist + screenshots needed
```
