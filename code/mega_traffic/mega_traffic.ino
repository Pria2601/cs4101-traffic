/*
 * CS4101 Smart Traffic Signal - Arduino Mega 2560
 * ------------------------------------------------
 * Job of this board:
 *   1. Read 4 IR sensors and count vehicles per lane
 *   2. Decide which lane gets green (weighted priority)
 *   3. Drive the 12 traffic LEDs
 *   4. Print readable status on the Serial Monitor (USB)
 *   5. Send telemetry JSON to the NodeMCU over Serial1 every 3 s
 *   6. Accept commands from the NodeMCU (cloud) or the Serial Monitor
 *
 * Commands (one per line, "Newline" line ending in Serial Monitor):
 *   MODE AUTO | MODE MANUAL <1-4> | MODE ALLRED
 *   SET WQ|WW|MINGREEN|MAXGREEN|PERVEH|MAXWAIT <value>
 *   RESET | PING
 */

// ---------------- Hardware (see HARDWARE_ACTUAL.md) ----------------
const uint8_t NUM_LANES = 4;
const uint8_t IR_PIN[NUM_LANES]     = {2, 3, 4, 5};
const uint8_t GREEN_PIN[NUM_LANES]  = {22, 24, 52, 40};
const uint8_t YELLOW_PIN[NUM_LANES] = {28, 30, 32, 48};
const uint8_t RED_PIN[NUM_LANES]    = {34, 36, 38, 44};

const bool SENSOR_ACTIVE_LOW = true;   // IR output is LOW when a vehicle is in front
const bool LED_ACTIVE_HIGH   = true;   // pin HIGH turns the LED on

// ---------------- Timing ----------------
const unsigned long DEBOUNCE_MS     = 80;     // ignore IR flicker shorter than this
const unsigned long SENSOR_FAULT_MS = 10000;  // IR blocked this long => sensor fault
const unsigned long YELLOW_MS       = 3000;
const unsigned long ALL_RED_MS      = 1000;   // safety gap between two greens
const unsigned long DISCHARGE_MS    = 2000;   // one queued vehicle leaves per 2 s of green
const unsigned long TELEMETRY_MS    = 3000;
const unsigned long STATUS_MS       = 1000;

// ---------------- Tunable from the cloud (SET command) ----------------
float    wQueue      = 2.0;  // score points per waiting vehicle
float    wWait       = 0.5;  // score points per second the first vehicle has waited
uint8_t  minGreenS   = 5;
uint8_t  maxGreenS   = 20;
uint8_t  perVehicleS = 2;    // extra green seconds per waiting vehicle
uint16_t maxWaitS    = 40;   // anti-starvation: a lane waiting this long is served next

// ---------------- State ----------------
enum Mode  { MODE_AUTO, MODE_MANUAL, MODE_ALLRED };
enum Phase { PH_GREEN, PH_YELLOW, PH_ALLRED };

Mode    mode       = MODE_AUTO;
uint8_t manualLane = 0;
Phase   phase      = PH_ALLRED;
int8_t  curLane    = -1;   // lane that is green/yellow (or was last served)
int8_t  nextLane   = -1;   // lane chosen to go green after yellow + all-red
unsigned long phaseStart = 0, phaseDur = ALL_RED_MS, lastDischarge = 0;
const char* lastReason = "start";

uint8_t       queueLen[NUM_LANES];   // vehicles waiting at red
unsigned int  arrivals[NUM_LANES];   // detections since last telemetry
unsigned long totalCount[NUM_LANES]; // detections since boot / RESET
unsigned long waitStart[NUM_LANES];  // when the first waiting vehicle arrived (0 = none)
bool          fault[NUM_LANES];

bool          rawState[NUM_LANES], stableState[NUM_LANES];
unsigned long rawChange[NUM_LANES], activeSince[NUM_LANES];

unsigned long seq = 0, lastTelemetry = 0, lastStatus = 0;

// Serial1 output queue: lets the loop keep reading sensors while 9600 baud sends
char     txBuf[600];
uint16_t txLen = 0, txPos = 0;

char    line[400];
uint16_t lineLen = 0;

char    cmdBuf[2][64];
uint8_t cmdLen[2] = {0, 0};

// ====================================================================
// Small helpers
// ====================================================================
void lineReset() { lineLen = 0; line[0] = 0; }
void lineAdd(const char* s) {
  while (*s && lineLen < sizeof(line) - 1) line[lineLen++] = *s++;
  line[lineLen] = 0;
}
void lineAddInt(long v)    { char b[12]; ltoa(v, b, 10); lineAdd(b); }
void lineAddFloat(float v) { char b[16]; dtostrf(v, 0, 1, b); lineAdd(b); }
void lineAddKey(const char* k) { lineAdd(",\""); lineAdd(k); lineAdd("\":"); }
void lineAddLaneKey(char prefix, uint8_t lane) {
  char k[3] = {prefix, (char)('1' + lane), 0};
  lineAddKey(k);
}

// Queue the current `line` (plus newline) for sending to the NodeMCU
void queueLineToNode() {
  if (txPos == txLen) { txPos = 0; txLen = 0; }
  if (txLen + lineLen + 1 > sizeof(txBuf)) {
    Serial.println(F("[WARN] Serial1 queue full, message dropped"));
    return;
  }
  memcpy(txBuf + txLen, line, lineLen);
  txLen += lineLen;
  txBuf[txLen++] = '\n';
}

void pumpSerial1() {
  while (txPos < txLen && Serial1.availableForWrite() > 0) Serial1.write(txBuf[txPos++]);
}

void writeLed(uint8_t pin, bool on) {
  digitalWrite(pin, (on == LED_ACTIVE_HIGH) ? HIGH : LOW);
}

char laneLight(uint8_t i) {
  if ((int8_t)i != curLane) return 'R';
  if (phase == PH_GREEN)  return 'G';
  if (phase == PH_YELLOW) return 'Y';
  return 'R';
}

void applyLights() {
  for (uint8_t i = 0; i < NUM_LANES; i++) {
    char c = laneLight(i);
    writeLed(GREEN_PIN[i],  c == 'G');
    writeLed(YELLOW_PIN[i], c == 'Y');
    writeLed(RED_PIN[i],    c == 'R');
  }
}

bool sensorActive(uint8_t i) {
  int r = digitalRead(IR_PIN[i]);
  return SENSOR_ACTIVE_LOW ? (r == LOW) : (r == HIGH);
}

unsigned long waitSec(uint8_t i, unsigned long now) {
  return waitStart[i] ? (now - waitStart[i]) / 1000UL : 0;
}

float laneScore(uint8_t i, unsigned long now) {
  return wQueue * queueLen[i] + wWait * waitSec(i, now);
}

bool othersWaiting(int8_t except) {
  for (uint8_t i = 0; i < NUM_LANES; i++)
    if ((int8_t)i != except && queueLen[i] > 0) return true;
  return false;
}

const char* modeName() {
  return mode == MODE_AUTO ? "AUTO" : mode == MODE_MANUAL ? "MANUAL" : "ALLRED";
}

// ====================================================================
// Signal phases
// ====================================================================
void startGreen(uint8_t lane, unsigned long now) {
  curLane = lane;
  nextLane = -1;
  phase = PH_GREEN;
  phaseStart = now;
  lastDischarge = now;
  waitStart[lane] = 0;  // vehicles are moving now

  unsigned int g = minGreenS + (unsigned int)perVehicleS * queueLen[lane];
  if (g > maxGreenS) g = maxGreenS;
  if (g < minGreenS) g = minGreenS;
  phaseDur = g * 1000UL;
  applyLights();

  Serial.print(F("[GREEN] Lane ")); Serial.print(lane + 1);
  Serial.print(F(" for ")); Serial.print(g);
  Serial.print(F("s (queue ")); Serial.print(queueLen[lane]);
  Serial.print(F(", reason ")); Serial.print(lastReason); Serial.println(F(")"));
}

void startYellow(unsigned long now) {
  phase = PH_YELLOW;
  phaseStart = now;
  phaseDur = YELLOW_MS;
  if (curLane >= 0 && queueLen[curLane] > 0 && waitStart[curLane] == 0)
    waitStart[curLane] = now ? now : 1;  // leftover vehicles start waiting again
  applyLights();
  Serial.print(F("[YELLOW] Lane ")); Serial.println(curLane + 1);
}

void startAllRed(unsigned long now) {
  phase = PH_ALLRED;
  phaseStart = now;
  phaseDur = ALL_RED_MS;
  applyLights();
  Serial.println(F("[ALL RED]"));
}

// Weighted priority: pick the lane that should go green next.
// exclude = lane to skip (the current green lane), or -1.
int8_t chooseNext(int8_t exclude, unsigned long now) {
  int8_t best = -1;

  // 1. Anti-starvation: lane waiting longer than maxWaitS goes first
  unsigned long longest = 0;
  for (uint8_t i = 0; i < NUM_LANES; i++) {
    if ((int8_t)i == exclude || queueLen[i] == 0) continue;
    unsigned long w = waitSec(i, now);
    if (w >= maxWaitS && w > longest) { longest = w; best = i; }
  }
  if (best >= 0) { lastReason = "maxwait"; return best; }

  // 2. Highest score among lanes that have vehicles
  float bestScore = -1;
  for (uint8_t i = 0; i < NUM_LANES; i++) {
    if ((int8_t)i == exclude || queueLen[i] == 0) continue;
    float s = laneScore(i, now);
    if (s > bestScore) { bestScore = s; best = i; }
  }
  if (best >= 0) { lastReason = "score"; return best; }

  // 3. Nobody waiting: keep current green, or move on in order
  lastReason = "idle";
  if (exclude >= 0) return exclude;
  return (curLane + 1) % NUM_LANES;
}

void onPhaseEnd(unsigned long now) {
  if (phase == PH_GREEN) {
    int8_t target;
    if (mode == MODE_ALLRED)      { target = -1; lastReason = "allred"; }
    else if (mode == MODE_MANUAL) { target = manualLane; lastReason = "manual"; }
    else                          target = chooseNext(curLane, now);

    if (target == curLane) { startGreen(curLane, now); return; }  // extend green
    nextLane = target;
    startYellow(now);
  } else if (phase == PH_YELLOW) {
    startAllRed(now);
  } else {  // PH_ALLRED
    if (mode == MODE_ALLRED) { phaseStart = now; phaseDur = ALL_RED_MS; return; }  // hold
    int8_t target;
    if (mode == MODE_MANUAL)  { target = manualLane; lastReason = "manual"; }
    else if (nextLane >= 0)   target = nextLane;
    else                      target = chooseNext(-1, now);
    startGreen(target, now);
  }
}

void updateSignal(unsigned long now) {
  // Vehicles leave the green lane at a steady rate
  if (phase == PH_GREEN && curLane >= 0 && now - lastDischarge >= DISCHARGE_MS) {
    lastDischarge += DISCHARGE_MS;
    if (queueLen[curLane] > 0) queueLen[curLane]--;
  }

  // Gap-out: green lane is empty and someone else is waiting -> end green early
  if (mode == MODE_AUTO && phase == PH_GREEN &&
      now - phaseStart >= minGreenS * 1000UL &&
      queueLen[curLane] == 0 && othersWaiting(curLane)) {
    Serial.println(F("[GAP-OUT] Green lane empty, switching early"));
    onPhaseEnd(now);
    return;
  }

  if (now - phaseStart >= phaseDur) onPhaseEnd(now);
}

// ====================================================================
// IR sensors
// ====================================================================
void onVehicle(uint8_t i, unsigned long now) {
  arrivals[i]++;
  totalCount[i]++;
  bool isGreen = ((int8_t)i == curLane && phase == PH_GREEN);
  if (!isGreen) {
    if (queueLen[i] < 99) queueLen[i]++;
    if (waitStart[i] == 0) waitStart[i] = now ? now : 1;
  }
  Serial.print(F("[COUNT] Lane ")); Serial.print(i + 1);
  Serial.print(isGreen ? F(" vehicle passed on green") : F(" vehicle waiting, queue="));
  if (!isGreen) Serial.print(queueLen[i]);
  Serial.println();
}

void readSensors(unsigned long now) {
  for (uint8_t i = 0; i < NUM_LANES; i++) {
    bool r = sensorActive(i);
    if (r != rawState[i]) { rawState[i] = r; rawChange[i] = now; }

    if (now - rawChange[i] >= DEBOUNCE_MS && stableState[i] != rawState[i]) {
      stableState[i] = rawState[i];
      if (stableState[i]) {
        activeSince[i] = now;
        onVehicle(i, now);
      } else if (fault[i]) {
        fault[i] = false;
        Serial.print(F("[FAULT CLEARED] Lane ")); Serial.println(i + 1);
      }
    }

    if (stableState[i] && !fault[i] && now - activeSince[i] >= SENSOR_FAULT_MS) {
      fault[i] = true;
      Serial.print(F("[FAULT] Lane ")); Serial.print(i + 1);
      Serial.println(F(" IR sensor blocked for 10 s"));
    }
  }
}

// ====================================================================
// Commands (from NodeMCU on Serial1, or typed in Serial Monitor)
// ====================================================================
void sendAck(const char* cmd, bool ok) {
  lineReset();
  lineAdd("{\"ack\":\""); lineAdd(cmd); lineAdd("\"");
  lineAddKey("ok"); lineAddInt(ok ? 1 : 0);
  lineAddKey("mode"); lineAdd("\""); lineAdd(modeName()); lineAdd("\"}");
  queueLineToNode();
}

bool setParam(const char* key, float v) {
  if (!strcmp(key, "WQ")       && v >= 0 && v <= 10)  { wQueue = v; return true; }
  if (!strcmp(key, "WW")       && v >= 0 && v <= 10)  { wWait = v; return true; }
  if (!strcmp(key, "MINGREEN") && v >= 3 && v <= maxGreenS) { minGreenS = (uint8_t)v; return true; }
  if (!strcmp(key, "MAXGREEN") && v >= minGreenS && v <= 120) { maxGreenS = (uint8_t)v; return true; }
  if (!strcmp(key, "PERVEH")   && v >= 0 && v <= 10)  { perVehicleS = (uint8_t)v; return true; }
  if (!strcmp(key, "MAXWAIT")  && v >= 10 && v <= 300) { maxWaitS = (uint16_t)v; return true; }
  return false;
}

void handleCommand(char* cmd) {
  for (char* p = cmd; *p; p++) *p = toupper(*p);
  char original[64];
  strncpy(original, cmd, sizeof(original) - 1);
  original[sizeof(original) - 1] = 0;

  char* w1 = strtok(cmd, " ");
  char* w2 = strtok(NULL, " ");
  char* w3 = strtok(NULL, " ");
  if (!w1) return;

  unsigned long now = millis();
  bool ok = false;

  if (!strcmp(w1, "MODE") && w2) {
    if (!strcmp(w2, "AUTO")) {
      mode = MODE_AUTO;
      if (phase == PH_ALLRED) phaseDur = 0;  // leave all-red hold right away
      ok = true;
    } else if (!strcmp(w2, "MANUAL") && w3) {
      int l = atoi(w3);
      if (l >= 1 && l <= NUM_LANES) {
        mode = MODE_MANUAL;
        manualLane = l - 1;
        lastReason = "manual";
        if (phase == PH_GREEN && curLane != (int8_t)manualLane) startYellow(now);
        else if (phase == PH_ALLRED) phaseDur = 0;
        ok = true;
      }
    } else if (!strcmp(w2, "ALLRED")) {
      mode = MODE_ALLRED;
      nextLane = -1;
      lastReason = "allred";
      if (phase == PH_GREEN) startYellow(now);
      ok = true;
    }
  } else if (!strcmp(w1, "SET") && w2 && w3) {
    ok = setParam(w2, atof(w3));
  } else if (!strcmp(w1, "RESET")) {
    for (uint8_t i = 0; i < NUM_LANES; i++) {
      queueLen[i] = 0; arrivals[i] = 0; totalCount[i] = 0; waitStart[i] = 0;
    }
    ok = true;
  } else if (!strcmp(w1, "PING")) {
    ok = true;
  }

  Serial.print(F("[CMD] ")); Serial.print(original);
  Serial.println(ok ? F(" -> OK") : F(" -> REJECTED"));
  sendAck(original, ok);
}

void pollCommands(Stream& s, uint8_t idx) {
  while (s.available()) {
    char c = s.read();
    if (c == '\r') continue;
    if (c == '\n') {
      cmdBuf[idx][cmdLen[idx]] = 0;
      if (cmdLen[idx] > 0) handleCommand(cmdBuf[idx]);
      cmdLen[idx] = 0;
    } else if (cmdLen[idx] < sizeof(cmdBuf[idx]) - 1) {
      cmdBuf[idx][cmdLen[idx]++] = c;
    }
  }
}

// ====================================================================
// Telemetry (to NodeMCU) and status (to Serial Monitor)
// ====================================================================
void sendTelemetry(unsigned long now) {
  unsigned long rem = (now - phaseStart >= phaseDur) ? 0 : (phaseDur - (now - phaseStart)) / 1000UL;
  if (phase == PH_ALLRED && mode == MODE_ALLRED) rem = 0;

  lineReset();
  lineAdd("{\"seq\":"); lineAddInt(seq++);
  lineAddKey("up");     lineAddInt(now / 1000UL);
  lineAddKey("mode");   lineAdd("\""); lineAdd(modeName()); lineAdd("\"");
  lineAddKey("ml");     lineAddInt(mode == MODE_MANUAL ? manualLane + 1 : 0);
  lineAddKey("green");  lineAddInt(phase == PH_GREEN ? curLane + 1 : 0);
  lineAddKey("phase");  lineAdd(phase == PH_GREEN ? "\"G\"" : phase == PH_YELLOW ? "\"Y\"" : "\"R\"");
  lineAddKey("rem");    lineAddInt(rem);
  lineAddKey("rsn");    lineAdd("\""); lineAdd(lastReason); lineAdd("\"");

  char lights[NUM_LANES + 1];
  for (uint8_t i = 0; i < NUM_LANES; i++) lights[i] = laneLight(i);
  lights[NUM_LANES] = 0;
  lineAddKey("lights"); lineAdd("\""); lineAdd(lights); lineAdd("\"");

  for (uint8_t i = 0; i < NUM_LANES; i++) {
    lineAddLaneKey('q', i); lineAddInt(queueLen[i]);
    lineAddLaneKey('w', i); lineAddInt(waitSec(i, now));
    lineAddLaneKey('a', i); lineAddInt(arrivals[i]);
    lineAddLaneKey('t', i); lineAddInt(totalCount[i]);
    lineAddLaneKey('s', i); lineAddFloat(laneScore(i, now));
    lineAddLaneKey('f', i); lineAddInt(fault[i] ? 1 : 0);
    arrivals[i] = 0;
  }

  lineAddKey("wq");   lineAddFloat(wQueue);
  lineAddKey("ww");   lineAddFloat(wWait);
  lineAddKey("ming"); lineAddInt(minGreenS);
  lineAddKey("maxg"); lineAddInt(maxGreenS);
  lineAddKey("pv");   lineAddInt(perVehicleS);
  lineAddKey("mw");   lineAddInt(maxWaitS);
  lineAdd("}");
  queueLineToNode();
}

void printStatus(unsigned long now) {
  Serial.print(F("[STATUS] ")); Serial.print(modeName());
  for (uint8_t i = 0; i < NUM_LANES; i++) {
    Serial.print(F(" | L")); Serial.print(i + 1); Serial.print(' ');
    Serial.print(laneLight(i));
    Serial.print(F(" q"));  Serial.print(queueLen[i]);
    Serial.print(F(" w"));  Serial.print(waitSec(i, now));
    Serial.print(F(" s"));  Serial.print(laneScore(i, now), 1);
    if (fault[i]) Serial.print(F(" FAULT"));
  }
  Serial.println();
}

// ====================================================================
// Setup & loop
// ====================================================================
void setup() {
  Serial.begin(115200);
  Serial1.begin(9600);

  unsigned long now = millis();
  for (uint8_t i = 0; i < NUM_LANES; i++) {
    pinMode(GREEN_PIN[i], OUTPUT);
    pinMode(YELLOW_PIN[i], OUTPUT);
    pinMode(RED_PIN[i], OUTPUT);
    pinMode(IR_PIN[i], INPUT_PULLUP);
    rawState[i] = stableState[i] = sensorActive(i);  // a blocked sensor at boot is not a vehicle
    rawChange[i] = activeSince[i] = now;
  }

  Serial.println(F("\n=== CS4101 Smart Traffic Signal (Mega) ==="));
  Serial.println(F("Commands: MODE AUTO | MODE MANUAL <1-4> | MODE ALLRED | SET <key> <value> | RESET"));
  startAllRed(now);
}

void loop() {
  unsigned long now = millis();

  readSensors(now);
  pollCommands(Serial1, 0);
  pollCommands(Serial, 1);
  updateSignal(now);

  if (now - lastTelemetry >= TELEMETRY_MS) { lastTelemetry = now; sendTelemetry(now); }
  if (now - lastStatus >= STATUS_MS)       { lastStatus = now; printStatus(now); }

  pumpSerial1();
}
