/**
 * VETO Hardware Controller Firmware
 * Platform: Arduino Uno R3 (ATmega328P)
 * 
 * Hardware Peripherals:
 * - 0.96" SSD1306 OLED (128x64 I2C, Address 0x3C / 0x3D): SDA -> A4, SCL -> A5
 * - APPROVE Button (Momentary NO): Pin 2 (INPUT_PULLUP)
 * - REJECT Button (Momentary NO): Pin 3 (INPUT_PULLUP)
 * - EMERGENCY STOP Button (NC Safety Switch): Pin 4 (INPUT_PULLUP)
 * - RGB LED (Common Cathode with 220 Ohm series resistors):
 *     Red:   Pin 9  (PWM)
 *     Green: Pin 10 (PWM)
 *     Blue:  Pin 11 (PWM)
 * 
 * Serial Protocol (115200 Baud):
 * Host -> Arduino:
 *   STATUS:<text>                     Update idle display status text
 *   MSG:<line1>[:<line2>[:<line3>]]   Display custom message on OLED
 *   PENDING:<id>:<op>:<target>[:<why>] New operation pending review
 *   DECISION:<id>:<state>             Result of review (APPROVED / REJECTED)
 *   PING:<nonce>                      Heartbeat request from host
 *   ESTOP_RESET                       Reset emergency stop lock
 *   SCAN                              Perform live I2C bus scan
 * 
 * Arduino -> Host:
 *   READY                             Device booted and ready
 *   I2C:FOUND:<addr>                  I2C peripheral detected
 *   I2C:NONE_FOUND                    No I2C device responded
 *   OLED:READY:<addr>                 OLED initialized successfully
 *   APPROVE:<id>                      Approve button pressed
 *   REJECT:<id>                       Reject button pressed
 *   ESTOP                             Emergency stop button pressed / open
 *   PONG:<nonce>                      Heartbeat response to host
 *   HEARTBEAT                         Periodic keepalive
 */

#define SSD1306_NO_SPLASH
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define DEFAULT_SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Pin Definitions
const int PIN_BTN_APPROVE = 2;
const int PIN_BTN_REJECT  = 3;
const int PIN_BTN_ESTOP   = 4;

const int PIN_LED_RED   = 9;
const int PIN_LED_GREEN = 10;
const int PIN_LED_BLUE  = 11;

// Debounce timing
const unsigned long DEBOUNCE_DELAY_MS = 50;
unsigned long lastDebounceTimeApprove = 0;
unsigned long lastDebounceTimeReject  = 0;
unsigned long lastDebounceTimeEstop   = 0;

int lastBtnStateApprove = HIGH;
int lastBtnStateReject  = HIGH;
int lastBtnStateEstop   = HIGH;

int btnStateApprove = HIGH;
int btnStateReject  = HIGH;
int btnStateEstop   = HIGH;

// System States
enum SystemState {
  STATE_NORMAL,
  STATE_PENDING_APPROVAL,
  STATE_DECISION_FEEDBACK,
  STATE_EMERGENCY_STOP,
  STATE_CUSTOM_MESSAGE
};

SystemState currentState = STATE_NORMAL;

// Pending Operation Details
char currentApprovalId[16] = "";
char currentOperation[16]  = "";
char currentTarget[24]     = "";
char currentReason[24]     = "REVIEW REQUIRED";
char systemStatusText[32]  = "SYSTEM NOMINAL";
char lastDecisionText[16]  = "";

// Custom Message Display
char customMsgLine1[22] = "VETO SECURITY";
char customMsgLine2[22] = "OLED ACTIVE";
char customMsgLine3[22] = "";
unsigned long customMsgTimer = 0;

unsigned long feedbackTimer = 0;
unsigned long lastHeartbeatSend = 0;
unsigned long lastOledRetry = 0;
bool oledHardwareReady = false;

void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(PIN_LED_RED, r);
  analogWrite(PIN_LED_GREEN, g);
  analogWrite(PIN_LED_BLUE, b);
}

bool ensureOLEDReady() {
  if (oledHardwareReady) return true;
  delay(20);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledHardwareReady = true;
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(0xFF);
    display.dim(false);
    Serial.println(F("OLED:ATTACHED:0x3C"));
    return true;
  }
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    oledHardwareReady = true;
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(0xFF);
    display.dim(false);
    Serial.println(F("OLED:ATTACHED:0x3D"));
    return true;
  }
  return false;
}

void updateOLED() {
  if (!oledHardwareReady && !ensureOLEDReady()) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Top Banner
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("VETO RUNTIME GUARD"));
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  if (currentState == STATE_EMERGENCY_STOP) {
    display.setTextSize(2);
    display.setCursor(8, 16);
    display.print(F("!! ESTOP !!"));
    display.setTextSize(1);
    display.setCursor(0, 38);
    display.print(F("EXECUTION HALTED"));
    display.setCursor(0, 50);
    display.print(F("Reset via Dashboard"));
  }
  else if (currentState == STATE_PENDING_APPROVAL) {
    display.setTextSize(1);
    display.setCursor(0, 11);
    display.print(F("REQ: "));
    display.print(currentApprovalId);

    display.setCursor(0, 21);
    display.print(F("OP:  "));
    display.print(currentOperation);

    display.setCursor(0, 31);
    display.print(F("TGT: "));
    display.print(currentTarget);

    display.setCursor(0, 41);
    display.print(F("WHY: "));
    display.print(currentReason);

    display.drawLine(0, 50, 127, 50, SSD1306_WHITE);
    display.setCursor(4, 54);
    display.print(F("[APPROVE]   [REJECT]"));
  }
  else if (currentState == STATE_DECISION_FEEDBACK) {
    bool isApproved = (strcmp(lastDecisionText, "APPROVED") == 0);

    // High-contrast inverted banner
    display.fillRect(0, 12, 128, 26, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.setTextSize(2);
    if (isApproved) {
      display.setCursor(16, 17);
      display.print(F("APPROVED"));
    } else {
      display.setCursor(16, 17);
      display.print(F("REJECTED"));
    }

    // Normal info text below banner
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 42);
    display.print(F("ID:  "));
    display.print(currentApprovalId);

    display.setCursor(0, 53);
    if (isApproved) {
      display.print(F("Action Authorized."));
    } else {
      display.print(F("Action Blocked."));
    }
  }
  else if (currentState == STATE_CUSTOM_MESSAGE) {
    display.setTextSize(1);
    display.setCursor(0, 14);
    display.print(customMsgLine1);

    display.setCursor(0, 27);
    display.print(customMsgLine2);

    if (strlen(customMsgLine3) > 0) {
      display.setCursor(0, 40);
      display.print(customMsgLine3);
    }

    display.drawLine(0, 51, 127, 51, SSD1306_WHITE);
    display.setCursor(0, 55);
    display.print(F("MSG: VETO CONTROLLER"));
  }
  else { // STATE_NORMAL
    display.setTextSize(1);
    display.setCursor(0, 14);
    display.print(F("STATUS: "));
    display.print(systemStatusText);

    display.setCursor(0, 27);
    display.print(F("OLED DISPLAY ONLINE"));

    display.setCursor(0, 39);
    display.print(F("ZERO-TRUST ACTIVE"));

    display.drawLine(0, 51, 127, 51, SSD1306_WHITE);
    display.setCursor(0, 55);
    display.print(F("Mode: Hardware Leased"));
  }

  display.display();
}

void runI2CScan() {
  uint8_t count = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("I2C:FOUND:0x"));
      if (addr < 16) Serial.print(F("0"));
      Serial.println(addr, HEX);
      count++;
    }
  }
  if (count == 0) {
    Serial.println(F("I2C:NONE_FOUND:CHECK_SDA_A4_SCL_A5"));
  }
}

void processIncomingCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd.startsWith("STATUS:")) {
    String txt = cmd.substring(7);
    txt.toCharArray(systemStatusText, sizeof(systemStatusText));
    if (currentState == STATE_NORMAL) updateOLED();
  }
  else if (cmd.startsWith("MSG:")) {
    // Format: MSG:<line1>[:<line2>[:<line3>]]
    int idx1 = cmd.indexOf(':', 4);
    int idx2 = (idx1 > 0) ? cmd.indexOf(':', idx1 + 1) : -1;

    String l1 = (idx1 > 0) ? cmd.substring(4, idx1) : cmd.substring(4);
    String l2 = (idx1 > 0) ? ((idx2 > 0) ? cmd.substring(idx1 + 1, idx2) : cmd.substring(idx1 + 1)) : "";
    String l3 = (idx2 > 0) ? cmd.substring(idx2 + 1) : "";

    l1.toCharArray(customMsgLine1, sizeof(customMsgLine1));
    l2.toCharArray(customMsgLine2, sizeof(customMsgLine2));
    l3.toCharArray(customMsgLine3, sizeof(customMsgLine3));

    currentState = STATE_CUSTOM_MESSAGE;
    customMsgTimer = millis();
    updateOLED();
    Serial.println(F("MSG:OK"));
  }
  else if (cmd.startsWith("PENDING:")) {
    // Format: PENDING:<id>:<operation>:<target>[:<reason>]
    int idx1 = cmd.indexOf(':', 8);
    int idx2 = cmd.indexOf(':', idx1 + 1);
    int idx3 = cmd.indexOf(':', idx2 + 1);

    if (idx1 > 0 && idx2 > 0) {
      String id = cmd.substring(8, idx1);
      String op = cmd.substring(idx1 + 1, idx2);
      String tgt = (idx3 > 0) ? cmd.substring(idx2 + 1, idx3) : cmd.substring(idx2 + 1);
      String why = (idx3 > 0) ? cmd.substring(idx3 + 1) : "REVIEW REQUIRED";

      id.toCharArray(currentApprovalId, sizeof(currentApprovalId));
      op.toCharArray(currentOperation, sizeof(currentOperation));
      tgt.toCharArray(currentTarget, sizeof(currentTarget));
      why.toCharArray(currentReason, sizeof(currentReason));

      currentState = STATE_PENDING_APPROVAL;
      // Yellow LED for pending review
      setLEDColor(255, 180, 0);
      updateOLED();
    }
  }
  else if (cmd.startsWith("DECISION:")) {
    // Format: DECISION:<id>:<decision>
    int idx = cmd.indexOf(':', 9);
    if (idx > 0) {
      String id = cmd.substring(9, idx);
      String dec = cmd.substring(idx + 1);

      dec.toCharArray(lastDecisionText, sizeof(lastDecisionText));
      currentState = STATE_DECISION_FEEDBACK;
      feedbackTimer = millis();

      if (dec == "APPROVED") {
        setLEDColor(0, 255, 0); // Solid Green
      } else {
        setLEDColor(255, 0, 0); // Solid Red
      }
      updateOLED();
    }
  }
  else if (cmd.startsWith("PING:")) {
    String nonce = cmd.substring(5);
    Serial.print(F("PONG:"));
    Serial.println(nonce);
  }
  else if (cmd == "ESTOP_RESET") {
    currentState = STATE_NORMAL;
    setLEDColor(0, 255, 0);
    updateOLED();
  }
  else if (cmd == "SCAN") {
    runI2CScan();
  }
}

void checkButtons() {
  unsigned long now = millis();

  // 1. APPROVE Button
  int readingApprove = digitalRead(PIN_BTN_APPROVE);
  if (readingApprove != lastBtnStateApprove) {
    lastDebounceTimeApprove = now;
  }
  if ((now - lastDebounceTimeApprove) > DEBOUNCE_DELAY_MS) {
    if (readingApprove != btnStateApprove) {
      btnStateApprove = readingApprove;
      if (btnStateApprove == LOW && currentState == STATE_PENDING_APPROVAL) {
        strncpy(lastDecisionText, "APPROVED", sizeof(lastDecisionText));
        currentState = STATE_DECISION_FEEDBACK;
        feedbackTimer = millis();
        setLEDColor(0, 255, 0); // Solid Green
        updateOLED();
        Serial.print(F("APPROVE:"));
        Serial.println(currentApprovalId);
      }
    }
  }
  lastBtnStateApprove = readingApprove;

  // 2. REJECT Button
  int readingReject = digitalRead(PIN_BTN_REJECT);
  if (readingReject != lastBtnStateReject) {
    lastDebounceTimeReject = now;
  }
  if ((now - lastDebounceTimeReject) > DEBOUNCE_DELAY_MS) {
    if (readingReject != btnStateReject) {
      btnStateReject = readingReject;
      if (btnStateReject == LOW && currentState == STATE_PENDING_APPROVAL) {
        strncpy(lastDecisionText, "REJECTED", sizeof(lastDecisionText));
        currentState = STATE_DECISION_FEEDBACK;
        feedbackTimer = millis();
        setLEDColor(255, 0, 0); // Solid Red
        updateOLED();
        Serial.print(F("REJECT:"));
        Serial.println(currentApprovalId);
      }
    }
  }
  lastBtnStateReject = readingReject;

  // 3. EMERGENCY STOP Button
  int readingEstop = digitalRead(PIN_BTN_ESTOP);
  if (readingEstop != lastBtnStateEstop) {
    lastDebounceTimeEstop = now;
  }
  if ((now - lastDebounceTimeEstop) > DEBOUNCE_DELAY_MS) {
    if (readingEstop != btnStateEstop) {
      btnStateEstop = readingEstop;
      if (btnStateEstop == LOW) {
        currentState = STATE_EMERGENCY_STOP;
        setLEDColor(255, 0, 0); // Red
        updateOLED();
        Serial.println(F("ESTOP"));
      }
    }
  }
  lastBtnStateEstop = readingEstop;
}

void setup() {
  // Initialize Serial
  Serial.begin(115200);
  Serial.setTimeout(50); // Fast non-blocking serial timeout
  while (!Serial && millis() < 1000);

  // Pin Modes
  pinMode(PIN_BTN_APPROVE, INPUT_PULLUP);
  pinMode(PIN_BTN_REJECT,  INPUT_PULLUP);
  pinMode(PIN_BTN_ESTOP,   INPUT_PULLUP);

  pinMode(PIN_LED_RED,   OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_BLUE,  OUTPUT);

  // Initial Green
  setLEDColor(0, 255, 0);

  // Initialize I2C Bus
  Wire.begin();
  Wire.setClock(100000); // 100kHz standard I2C speed for maximum cable noise tolerance
  delay(500); // Give OLED power-on reset capacitor time to charge

  // Auto-detect OLED I2C Address
  uint8_t detectedAddr = 0;
  for (uint8_t addr = 0x3C; addr <= 0x3D; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      detectedAddr = addr;
      break;
    }
  }
  if (detectedAddr == 0) {
    // Scan all valid I2C addresses
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        detectedAddr = addr;
        break;
      }
    }
  }

  uint8_t finalAddr = (detectedAddr != 0) ? detectedAddr : DEFAULT_SCREEN_ADDRESS;

  // Initialize Adafruit SSD1306 Display
  oledHardwareReady = display.begin(SSD1306_SWITCHCAPVCC, finalAddr);
  if (!oledHardwareReady && finalAddr != 0x3D) {
    oledHardwareReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3D);
    if (oledHardwareReady) finalAddr = 0x3D;
  }

  if (oledHardwareReady) {
    // Maximize display contrast
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(0xFF);
    display.dim(false);

    Serial.print(F("OLED:READY:0x"));
    if (finalAddr < 16) Serial.print(F("0"));
    Serial.println(finalAddr, HEX);

    // Initial Splash Screen
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(12, 16);
    display.println(F("VETO RUNTIME GUARD"));
    display.setCursor(10, 30);
    display.println(F("OLED DISPLAY ONLINE"));
    display.setCursor(14, 44);
    display.println(F("ZERO-TRUST ACTIVE"));
    display.display();
    delay(800);
  } else {
    Serial.println(F("OLED:NOT_DETECTED:CHECK_SDA_SCL"));
  }

  currentState = STATE_NORMAL;
  updateOLED();

  // Announce ready to host
  Serial.println(F("READY"));
}

void loop() {
  // Check buttons
  checkButtons();

  // Handle serial input from host
  if (Serial.available() > 0) {
    String incoming = Serial.readStringUntil('\n');
    processIncomingCommand(incoming);
  }

  // Handle feedback display timeout (return to normal state after 3.5s)
  if (currentState == STATE_DECISION_FEEDBACK) {
    if (millis() - feedbackTimer > 3500) {
      currentState = STATE_NORMAL;
      setLEDColor(0, 255, 0); // Return to Green
      updateOLED();
    }
  }

  // Handle custom message display timeout (return to normal after 10s)
  if (currentState == STATE_CUSTOM_MESSAGE) {
    if (millis() - customMsgTimer > 10000) {
      currentState = STATE_NORMAL;
      updateOLED();
    }
  }

  // Periodic Heartbeat every 3 seconds
  if (millis() - lastHeartbeatSend > 3000) {
    lastHeartbeatSend = millis();
    Serial.println(F("HEARTBEAT"));
  }

  // Auto-retry OLED initialization if not yet attached
  if (!oledHardwareReady && (millis() - lastOledRetry > 2000)) {
    lastOledRetry = millis();
    if (ensureOLEDReady()) {
      updateOLED();
    }
  }
}
