// Needle car - board B (ESP32): receives plans from the S3 (ESP-NOW radio or
// wired UART), queues them, and drives the TB6612FNG after you confirm.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>

// ---- pins ----
const int PIN_PWMA = 32, PIN_AIN1 = 18, PIN_AIN2 = 25;   // left motor
const int PIN_PWMB = 14, PIN_BIN1 = 26, PIN_BIN2 = 27;   // right motor
const int PIN_STBY = -1;   // GPIO wired to STBY, or -1 if STBY is tied high
const int PIN_BTN  = 0;    // BOOT button: GO when a plan waits, STOP while running
const int PIN_RX2  = 16;   // from S3 GPIO17 (wired fallback)
const int PIN_TX2  = 17;   // to S3 GPIO18 (wired fallback, optional)

// ---- tuning ----
const bool  INVERT_LEFT  = false;
const bool  INVERT_RIGHT = false;
const int   SPEED_PCT    = 60;
const float LEFT_SCALE   = 1.1f;
const float RIGHT_SCALE  = 1.07f;        // use your calibrated value
const float TURN_DEG_PER_SEC = 120.0f;   // use your calibrated value
const bool  REQUIRE_CONFIRM  = true;

// ---- safety limits ----
const float    MAX_MOVE_S   = 10.0f;
const float    MAX_TURN_DEG = 360.0f;
const int      MAX_STEPS    = 8;
const uint32_t PLAN_GAP_MS  = 1500;
const uint32_t PAUSE_MS     = 400;
const uint32_t WAIT_TIMEOUT_MS = 60000;
const int PWM_FREQ = 20000, PWM_BITS = 8;

// ---- types (kept above all functions on purpose) ----
struct Step { char kind; char dir; float amount; };
struct LineReader { char buf[32]; int len; };
enum State { ST_IDLE, ST_PENDING, ST_RUNNING };

Step plan[MAX_STEPS];
int nSteps = 0, cur = 0;
State state = ST_IDLE;
bool inStep = false;
uint32_t lastLineMs = 0, stepEndMs = 0, pauseEndMs = 0;
LineReader usbIn = {{0}, 0};
LineReader botIn = {{0}, 0};

// radio mailbox: filled in the callback, handled in loop()
volatile bool rxFlag = false;
char rxBuf[250];
uint8_t rxSrc[6];

void pwmInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_PWMA, PWM_FREQ, PWM_BITS);
  ledcAttach(PIN_PWMB, PWM_FREQ, PWM_BITS);
#else
  ledcSetup(0, PWM_FREQ, PWM_BITS);
  ledcAttachPin(PIN_PWMA, 0);
  ledcSetup(1, PWM_FREQ, PWM_BITS);
  ledcAttachPin(PIN_PWMB, 1);
#endif
}

void pwmWrite(int side, int duty) {   // side 0 = left, 1 = right
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(side == 0 ? PIN_PWMA : PIN_PWMB, duty);
#else
  ledcWrite(side, duty);
#endif
}

void driveMotor(int side, int dir) {  // dir: +1 forward, -1 backward
  const int in1 = side == 0 ? PIN_AIN1 : PIN_BIN1;
  const int in2 = side == 0 ? PIN_AIN2 : PIN_BIN2;
  const bool inv = side == 0 ? INVERT_LEFT : INVERT_RIGHT;
  const float scale = side == 0 ? LEFT_SCALE : RIGHT_SCALE;
  if (inv) dir = -dir;
  int duty = constrain((int)(255.0f * SPEED_PCT / 100.0f * scale), 0, 255);
  digitalWrite(in1, dir > 0 ? HIGH : LOW);
  digitalWrite(in2, dir > 0 ? LOW : HIGH);
  pwmWrite(side, duty);
}

void stopMotors() {                   // short brake
  digitalWrite(PIN_AIN1, HIGH); digitalWrite(PIN_AIN2, HIGH); pwmWrite(0, 255);
  digitalWrite(PIN_BIN1, HIGH); digitalWrite(PIN_BIN2, HIGH); pwmWrite(1, 255);
}

void abortPlan(const char *why) {
  stopMotors();
  nSteps = 0; cur = 0; inStep = false; state = ST_IDLE;
  Serial.printf("ABORT: %s\n", why);
}

void startPlan() {
  cur = 0; inStep = false; state = ST_RUNNING; pauseEndMs = millis();
  Serial.printf("RUN %d steps\n", nSteps);
}

void beginStep(const Step &s) {
  uint32_t dur;
  if (s.kind == 'M') {
    int d = (s.dir == 'F') ? 1 : -1;
    driveMotor(0, d); driveMotor(1, d);
    dur = (uint32_t)(s.amount * 1000.0f);
  } else {
    int d = (s.dir == 'R') ? 1 : -1;  // right turn: left wheel fwd, right wheel back
    driveMotor(0, d); driveMotor(1, -d);
    dur = (uint32_t)(s.amount / TURN_DEG_PER_SEC * 1000.0f);
  }
  stepEndMs = millis() + dur;
  inStep = true;
  Serial.printf("STEP %d/%d %c %c %.1f\n", cur + 1, nSteps, s.kind, s.dir, s.amount);
}

void runPlan() {
  uint32_t now = millis();
  if (inStep) {
    if ((int32_t)(now - stepEndMs) < 0) return;
    stopMotors(); inStep = false; cur++; pauseEndMs = now + PAUSE_MS;
    return;
  }
  if ((int32_t)(now - pauseEndMs) < 0) return;
  if (cur >= nSteps) {
    Serial.println("DONE");
    nSteps = 0; cur = 0; state = ST_IDLE;
    return;
  }
  beginStep(plan[cur]);
}

bool parseStep(const char *s, Step &st) {
  char k = 0, d = 0; float a = 0;
  if (sscanf(s, " %c %c %f", &k, &d, &a) != 3) return false;
  if (k == 'M' && (d == 'F' || d == 'B')) { st = {k, d, constrain(a, 0.5f, MAX_MOVE_S)}; return true; }
  if (k == 'T' && (d == 'L' || d == 'R')) { st = {k, d, constrain(a, 1.0f, MAX_TURN_DEG)}; return true; }
  return false;
}

// ---- wired path: one step per line ----
void addStep(const Step &st) {
  uint32_t now = millis();
  if (state == ST_RUNNING) { Serial.println("BUSY: ignored"); return; }
  if (now - lastLineMs > PLAN_GAP_MS) nSteps = 0;
  lastLineMs = now;
  if (nSteps >= MAX_STEPS) return;
  plan[nSteps++] = st;
  state = ST_PENDING;
  Serial.printf("plan: %d step(s) waiting\n", nSteps);
}

void handleLine(const char *line) {
  if (line[0] == 'S' || line[0] == 's') { abortPlan("stop command"); return; }
  Step st;
  if (parseStep(line, st)) addStep(st);
  else Serial.printf("ignored: %s\n", line);
}

void pollStream(Stream &s, LineReader &r) {
  while (s.available()) {
    char c = s.read();
    if (c == '\r') continue;
    if (c == '\n') { r.buf[r.len] = 0; if (r.len) handleLine(r.buf); r.len = 0; }
    else if (r.len < (int)sizeof(r.buf) - 1) r.buf[r.len++] = c;
  }
}

// ---- radio path: whole plan in one packet "P<id>,<n>|M F 4.0;T R 90.0;" ----
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (rxFlag || len <= 0 || len >= (int)sizeof(rxBuf)) return;
  memcpy(rxBuf, data, len);
  rxBuf[len] = 0;
  memcpy(rxSrc, info->src_addr, 6);
  rxFlag = true;
}

void sendReply(const uint8_t *dst, char kind, unsigned id) {
  if (!esp_now_is_peer_exist(dst)) {
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, dst, 6);
    p.channel = 0;
    p.encrypt = false;
    esp_now_add_peer(&p);
  }
  char msg[16];
  int n = snprintf(msg, sizeof(msg), "%c%u", kind, id);
  esp_now_send(dst, (const uint8_t *)msg, n);
}

void handlePacket() {
  char buf[sizeof(rxBuf)];
  memcpy(buf, rxBuf, sizeof(buf));
  unsigned id = 0, n = 0;
  char *bar = strchr(buf, '|');
  if (buf[0] != 'P' || !bar || sscanf(buf, "P%u,%u|", &id, &n) != 2) {
    Serial.println("radio: bad packet ignored");
    return;
  }
  if (state == ST_RUNNING) {
    sendReply(rxSrc, 'B', id);
    Serial.printf("radio: plan %u refused, car is busy\n", id);
    return;
  }
  Step tmp[MAX_STEPS];
  int cnt = 0;
  char *p = bar + 1;
  while (*p) {
    char *semi = strchr(p, ';');
    if (!semi || cnt >= MAX_STEPS) { cnt = -1; break; }
    *semi = 0;
    if (!parseStep(p, tmp[cnt])) { cnt = -1; break; }
    cnt++;
    p = semi + 1;
  }
  if (cnt < 1 || cnt != (int)n) {
    sendReply(rxSrc, 'N', id);
    Serial.printf("radio: plan %u rejected (steps do not match)\n", id);
    return;
  }
  for (int i = 0; i < cnt; i++) plan[i] = tmp[i];
  nSteps = cnt; cur = 0; inStep = false;
  lastLineMs = millis();
  state = ST_PENDING;
  sendReply(rxSrc, 'A', id);
  Serial.printf("radio: plan %u accepted, %d step(s) waiting\n", id, cnt);
}

void pollButton() {
  static bool prev = HIGH;
  static uint32_t lastMs = 0;
  bool b = digitalRead(PIN_BTN);
  uint32_t now = millis();
  if (b == LOW && prev == HIGH && now - lastMs > 50) {
    lastMs = now;
    if (state == ST_PENDING && now - lastLineMs > 300) startPlan();
    else if (state == ST_RUNNING) abortPlan("button");
  }
  prev = b;
}

void setup() {
  const int outs[] = {PIN_AIN1, PIN_AIN2, PIN_BIN1, PIN_BIN2};
  for (int p : outs) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }   // brake at boot
  if (PIN_STBY >= 0) { pinMode(PIN_STBY, OUTPUT); digitalWrite(PIN_STBY, HIGH); }
  pinMode(PIN_BTN, INPUT_PULLUP);
  pwmInit();
  stopMotors();
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, PIN_RX2, PIN_TX2);

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onRecv);
    Serial.println("radio ready (channel 1)");
  } else {
    Serial.println("radio FAILED - wired link still works");
  }
  Serial.println("car ready. BOOT button = go / stop. Wired: M F 2 / T R 90 / S.");
}

void loop() {
  pollStream(Serial2, botIn);
  pollStream(Serial, usbIn);
  if (rxFlag) { handlePacket(); rxFlag = false; }
  pollButton();
  uint32_t now = millis();
  if (state == ST_PENDING) {
    if (!REQUIRE_CONFIRM && now - lastLineMs > PLAN_GAP_MS) startPlan();
    else if (REQUIRE_CONFIRM && now - lastLineMs > WAIT_TIMEOUT_MS) abortPlan("not confirmed in time");
  } else if (state == ST_RUNNING) {
    runPlan();
  }
}