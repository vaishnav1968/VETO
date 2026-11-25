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
 */

#define SSD1306_NO_SPLASH
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Pin Definitions
const uint8_t PIN_BTN_APPROVE = 2;
const uint8_t PIN_BTN_REJECT  = 3;
const uint8_t PIN_BTN_ESTOP   = 4;

const uint8_t PIN_LED_RED   = 9;
const uint8_t PIN_LED_GREEN = 10;
const uint8_t PIN_LED_BLUE  = 11;

// Debounce timing
const unsigned long DEBOUNCE_DELAY_MS = 50;
unsigned long lastDebounceTimeApprove = 0;
unsigned long lastDebounceTimeReject  = 0;
unsigned long lastDebounceTimeEstop   = 0;

uint8_t lastBtnStateApprove = HIGH;
uint8_t lastBtnStateReject  = HIGH;
uint8_t lastBtnStateEstop   = HIGH;

uint8_t btnStateApprove = HIGH;
uint8_t btnStateReject  = HIGH;
uint8_t btnStateEstop   = HIGH;

// System States
enum SystemState {
  STATE_NORMAL,
  STATE_PENDING_APPROVAL,
  STATE_DECISION_FEEDBACK,
  STATE_EMERGENCY_STOP,
  STATE_CUSTOM_MESSAGE
};

SystemState currentState = STATE_NORMAL;

// Compact buffers to preserve SRAM for 1024-byte OLED frame
char currentApprovalId[12] = "";
char currentOperation[12]  = "";
char currentTarget[16]     = "";
char currentReason[16]     = "REVIEW REQUIRED";
char systemStatusText[16]  = "SYSTEM NOMINAL";
char lastDecisionText[10]  = "";

char customMsgLine1[18] = "VETO SECURITY";
char customMsgLine2[18] = "OLED ACTIVE";
char customMsgLine3[18] = "";
unsigned long customMsgTimer = 0;

unsigned long feedbackTimer = 0;
unsigned long lastHeartbeatSend = 0;
bool oledHardwareReady = false;

// Fixed serial rx buffer (no heap String allocations)
char serialRxBuf[64];
uint8_t serialRxIdx = 0;

int getFreeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}

void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(PIN_LED_RED, r);
  analogWrite(PIN_LED_GREEN, g);
  analogWrite(PIN_LED_BLUE, b);
}

void updateOLED() {
  if (!oledHardwareReady) return;

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

void processIncomingCommand(char* cmd) {
  if (cmd[0] == '\0') return;

  if (strncmp(cmd, "STATUS:", 7) == 0) {
    strncpy(systemStatusText, cmd + 7, sizeof(systemStatusText) - 1);
    systemStatusText[sizeof(systemStatusText) - 1] = '\0';
    if (currentState == STATE_NORMAL) updateOLED();
  }
  else if (strncmp(cmd, "MSG:", 4) == 0) {
    char* p1 = cmd + 4;
    char* p2 = strchr(p1, ':');
    char* p3 = NULL;

    if (p2) {
      *p2 = '\0';
      p2++;
      p3 = strchr(p2, ':');
      if (p3) {
        *p3 = '\0';
        p3++;
      }
    }

    strncpy(customMsgLine1, p1, sizeof(customMsgLine1) - 1);
    customMsgLine1[sizeof(customMsgLine1) - 1] = '\0';

    if (p2) {
      strncpy(customMsgLine2, p2, sizeof(customMsgLine2) - 1);
      customMsgLine2[sizeof(customMsgLine2) - 1] = '\0';
    } else {
      customMsgLine2[0] = '\0';
    }

    if (p3) {
      strncpy(customMsgLine3, p3, sizeof(customMsgLine3) - 1);
      customMsgLine3[sizeof(customMsgLine3) - 1] = '\0';
    } else {
      customMsgLine3[0] = '\0';
    }

    currentState = STATE_CUSTOM_MESSAGE;
    customMsgTimer = millis();
    updateOLED();
    Serial.println(F("MSG:OK"));
  }
  else if (strncmp(cmd, "PENDING:", 8) == 0) {
    char* id = cmd + 8;
    char* op = strchr(id, ':');
    char* tgt = NULL;
    char* why = NULL;

    if (op) {
      *op = '\0';
      op++;
      tgt = strchr(op, ':');
      if (tgt) {
        *tgt = '\0';
        tgt++;
        why = strchr(tgt, ':');
        if (why) {
          *why = '\0';
          why++;
        }
      }
    }

    if (op && tgt) {
      strncpy(currentApprovalId, id, sizeof(currentApprovalId) - 1);
      strncpy(currentOperation, op, sizeof(currentOperation) - 1);
      strncpy(currentTarget, tgt, sizeof(currentTarget) - 1);
      if (why) {
        strncpy(currentReason, why, sizeof(currentReason) - 1);
      } else {
        strncpy(currentReason, "REVIEW REQUIRED", sizeof(currentReason) - 1);
      }

      currentApprovalId[sizeof(currentApprovalId) - 1] = '\0';
      currentOperation[sizeof(currentOperation) - 1] = '\0';
      currentTarget[sizeof(currentTarget) - 1] = '\0';
      currentReason[sizeof(currentReason) - 1] = '\0';

      currentState = STATE_PENDING_APPROVAL;
      setLEDColor(255, 180, 0); // Yellow
      updateOLED();
    }
  }
  else if (strncmp(cmd, "DECISION:", 9) == 0) {
    char* id = cmd + 9;
    char* dec = strchr(id, ':');
    if (dec) {
      *dec = '\0';
      dec++;
      strncpy(lastDecisionText, dec, sizeof(lastDecisionText) - 1);
      lastDecisionText[sizeof(lastDecisionText) - 1] = '\0';

      currentState = STATE_DECISION_FEEDBACK;
      feedbackTimer = millis();

      if (strcmp(dec, "APPROVED") == 0) {
        setLEDColor(0, 255, 0); // Solid Green
      } else {
        setLEDColor(255, 0, 0); // Solid Red
      }
      updateOLED();
    }
  }
  else if (strncmp(cmd, "PING:", 5) == 0) {
    Serial.print(F("PONG:"));
    Serial.println(cmd + 5);
  }
  else if (strcmp(cmd, "ESTOP_RESET") == 0) {
    currentState = STATE_NORMAL;
    setLEDColor(0, 255, 0);
    updateOLED();
  }
  else if (strcmp(cmd, "SCAN") == 0) {
    runI2CScan();
  }
}

void checkButtons() {
  unsigned long now = millis();

  // 1. APPROVE Button
  uint8_t readingApprove = digitalRead(PIN_BTN_APPROVE);
  if (readingApprove != lastBtnStateApprove) {
    lastDebounceTimeApprove = now;
  }
  if ((now - lastDebounceTimeApprove) > DEBOUNCE_DELAY_MS) {
    if (readingApprove != btnStateApprove) {
      btnStateApprove = readingApprove;
      if (btnStateApprove == LOW && currentState == STATE_PENDING_APPROVAL) {
        strncpy(lastDecisionText, "APPROVED", sizeof(lastDecisionText) - 1);
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
  uint8_t readingReject = digitalRead(PIN_BTN_REJECT);
  if (readingReject != lastBtnStateReject) {
    lastDebounceTimeReject = now;
  }
  if ((now - lastDebounceTimeReject) > DEBOUNCE_DELAY_MS) {
    if (readingReject != btnStateReject) {
      btnStateReject = readingReject;
      if (btnStateReject == LOW && currentState == STATE_PENDING_APPROVAL) {
        strncpy(lastDecisionText, "REJECTED", sizeof(lastDecisionText) - 1);
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
  uint8_t readingEstop = digitalRead(PIN_BTN_ESTOP);
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
  Serial.begin(115200);
  while (!Serial && millis() < 500);

  pinMode(PIN_BTN_APPROVE, INPUT_PULLUP);
  pinMode(PIN_BTN_REJECT,  INPUT_PULLUP);
  pinMode(PIN_BTN_ESTOP,   INPUT_PULLUP);

  pinMode(PIN_LED_RED,   OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_BLUE,  OUTPUT);

  setLEDColor(0, 255, 0);

  Serial.print(F("FREE_RAM_START:"));
  Serial.println(getFreeRam());

  Wire.begin();
  Wire.setClock(100000);
  delay(300); // Allow OLED internal charge pump capacitors to reach operating voltage

  // Initialize display immediately to lock contiguous 1024-byte RAM buffer
  oledHardwareReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledHardwareReady) {
    oledHardwareReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  }

  Serial.print(F("FREE_RAM_POST_OLED:"));
  Serial.println(getFreeRam());

  if (oledHardwareReady) {
    Serial.println(F("OLED:INIT_SUCCESS"));
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(0xFF);
    display.dim(false);

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(4, 10);
    display.println(F("VETO GUARD"));
    display.setTextSize(1);
    display.setCursor(8, 34);
    display.println(F("OLED DISPLAY ONLINE"));
    display.setCursor(14, 48);
    display.println(F("ZERO-TRUST ACTIVE"));
    display.display();
    delay(1000);
  } else {
    Serial.println(F("OLED:INIT_FAILED:CHECK_RAM_OR_WIRING"));
  }

  currentState = STATE_NORMAL;
  updateOLED();

  Serial.println(F("READY"));
}

void loop() {
  checkButtons();

  // Fast non-blocking serial reading into fixed buffer
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialRxIdx > 0) {
        serialRxBuf[serialRxIdx] = '\0';
        processIncomingCommand(serialRxBuf);
        serialRxIdx = 0;
      }
    } else if (serialRxIdx < sizeof(serialRxBuf) - 1) {
      serialRxBuf[serialRxIdx++] = c;
    }
  }

  // Handle feedback display timeout (return to normal state after 3.5s)
  if (currentState == STATE_DECISION_FEEDBACK) {
    if (millis() - feedbackTimer > 3500) {
      currentState = STATE_NORMAL;
      setLEDColor(0, 255, 0); // Return to Green
      updateOLED();
    }
  }

  // Handle custom message display timeout (return to normal after 15s)
  if (currentState == STATE_CUSTOM_MESSAGE) {
    if (millis() - customMsgTimer > 15000) {
      currentState = STATE_NORMAL;
      updateOLED();
    }
  }

  // Periodic Heartbeat every 3 seconds
  if (millis() - lastHeartbeatSend > 3000) {
    lastHeartbeatSend = millis();
    Serial.println(F("HEARTBEAT"));
  }
}
