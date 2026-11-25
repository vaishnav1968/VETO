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

// Pin Definitions (#define to eliminate SRAM usage)
#define PIN_BTN_APPROVE 2
#define PIN_BTN_REJECT  3
#define PIN_BTN_ESTOP   4

#define PIN_LED_RED   9
#define PIN_LED_GREEN 10
#define PIN_LED_BLUE  11

#define DEBOUNCE_DELAY_MS 50
#define ANIM_INTERVAL_MS  65

// Debounce timing
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
enum SystemState : uint8_t {
  STATE_NORMAL = 0,
  STATE_PENDING_APPROVAL,
  STATE_DECISION_FEEDBACK,
  STATE_EMERGENCY_STOP,
  STATE_CUSTOM_MESSAGE,
  STATE_VETO_ALERT,
  STATE_DEMO_ALERT
};

SystemState currentState = STATE_NORMAL;

// Compact static buffers (strictly minimized to allow 1024-byte OLED framebuffer allocation)
char currentApprovalId[10] = "";
char currentOperation[10]  = "";
char currentTarget[14]     = "";
char currentReason[14]     = "REVIEW REQUIRED";
char systemStatusText[14]  = "NOMINAL";
char lastDecisionText[10]  = "";

// Shared auxiliary text buffers (used across VETO, DEMO, and CUSTOM_MESSAGE to save 120+ bytes SRAM)
char auxText1[16] = "";
char auxText2[16] = "";
char auxText3[14] = "";

unsigned long alertTimer = 0;
unsigned long feedbackTimer = 0;
unsigned long lastHeartbeatSend = 0;
bool oledHardwareReady = false;

// Animation state
uint8_t animStep = 0;
unsigned long lastAnimTime = 0;

// Compact serial rx buffer
char serialRxBuf[48];
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

  // Animated mini-shield icon in top-right header (x=114..124, y=0..7)
  display.drawRect(114, 0, 10, 8, SSD1306_WHITE);
  display.drawPixel(114, 0, SSD1306_BLACK);
  display.drawPixel(123, 0, SSD1306_BLACK);
  display.drawPixel(114, 7, SSD1306_BLACK);
  display.drawPixel(123, 7, SSD1306_BLACK);
  if ((animStep / 6) % 2 == 0) {
    display.drawPixel(118, 3, SSD1306_WHITE);
    display.drawPixel(119, 3, SSD1306_WHITE);
    display.drawPixel(118, 4, SSD1306_WHITE);
  }
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  if (currentState == STATE_EMERGENCY_STOP) {
    bool flash = ((animStep / 4) % 2 == 0);
    if (flash) {
      display.fillRect(0, 11, 128, 24, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    }
    display.setTextSize(2);
    display.setCursor(8, 15);
    display.print(F("!! ESTOP !!"));
    display.setTextColor(SSD1306_WHITE);

    display.setTextSize(1);
    display.setCursor(0, 39);
    display.print(F("EXECUTION HALTED"));
    display.setCursor(0, 51);
    display.print(F("Reset via Dashboard"));
  }
  else if (currentState == STATE_VETO_ALERT) {
    // Inverted high-contrast AUTO-VETO banner
    display.fillRect(0, 11, 128, 14, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(4, 14);
    display.print(F(">> AUTO-VETO BLOCKED <<"));
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 27);
    display.print(F("WHY: "));
    display.print(auxText1);

    display.setCursor(0, 38);
    display.print(F("TGT: "));
    display.print(auxText2);

    display.drawLine(0, 48, 127, 48, SSD1306_WHITE);

    display.setCursor(0, 53);
    if ((animStep / 4) % 2 == 0) {
      display.print(F("[!] ZERO-TRUST ENFORCED"));
    } else {
      display.print(F("[-] OPERATION DENIED   "));
    }
  }
  else if (currentState == STATE_DEMO_ALERT) {
    // Inverted Demo Banner
    display.fillRect(0, 11, 128, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(6, 13);
    display.print(F("RUNNING DEMO SCENARIO"));
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 26);
    display.print(F("SCN: "));
    display.print(auxText1);

    display.setCursor(0, 38);
    display.print(F("ACT: "));
    display.print(auxText2);

    display.drawLine(0, 49, 127, 49, SSD1306_WHITE);

    // Animated scanner bar across bottom
    uint8_t barPos = (animStep * 3) % 110;
    display.drawRect(0, 53, 127, 8, SSD1306_WHITE);
    display.fillRect(barPos, 54, 16, 6, SSD1306_WHITE);
  }
  else if (currentState == STATE_PENDING_APPROVAL) {
    display.setTextSize(1);
    display.setCursor(0, 11);
    display.print(F("REQ: "));
    display.print(currentApprovalId);

    display.setCursor(0, 20);
    display.print(F("OP:  "));
    display.print(currentOperation);

    display.setCursor(0, 29);
    display.print(F("TGT: "));
    display.print(currentTarget);

    display.setCursor(0, 38);
    display.print(F("WHY: "));
    display.print(currentReason);

    // Animated button callout boxes
    display.drawLine(0, 48, 127, 48, SSD1306_WHITE);
    bool blinkBox = ((animStep / 6) % 2 == 0);

    display.drawRect(2, 51, 58, 12, SSD1306_WHITE);
    display.drawRect(67, 51, 58, 12, SSD1306_WHITE);

    if (blinkBox) {
      display.fillRect(2, 51, 58, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      display.setCursor(8, 53);
      display.print(F("APPROVE"));
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(77, 53);
      display.print(F("REJECT"));
    } else {
      display.fillRect(67, 51, 58, 12, SSD1306_WHITE);
      display.setCursor(8, 53);
      display.print(F("APPROVE"));
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      display.setCursor(77, 53);
      display.print(F("REJECT"));
      display.setTextColor(SSD1306_WHITE);
    }
  }
  else if (currentState == STATE_DECISION_FEEDBACK) {
    bool isApproved = (strcmp(lastDecisionText, "APPROVED") == 0);

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
    display.print(auxText1);

    display.setCursor(0, 27);
    display.print(auxText2);

    if (strlen(auxText3) > 0) {
      display.setCursor(0, 40);
      display.print(auxText3);
    }

    display.drawLine(0, 51, 127, 51, SSD1306_WHITE);
    display.setCursor(0, 55);
    display.print(F("MSG: VETO CONTROLLER"));
  }
  else { // STATE_NORMAL with Cyber Animated Waveform
    display.setTextSize(1);
    display.setCursor(0, 12);
    display.print(F("STATUS: "));
    display.print(systemStatusText);

    display.setCursor(0, 23);
    display.print(F("SENTINEL: ARMED"));

    display.setCursor(0, 33);
    display.print(F("ZERO-TRUST RUNTIME"));

    // Dynamic animated cyber radar pulse / heartbeat across screen
    display.drawLine(0, 46, 127, 46, SSD1306_WHITE);

    uint8_t pulseX = (animStep * 2) % 126;
    if (pulseX > 10 && pulseX < 118) {
      display.drawPixel(pulseX - 6, 46, SSD1306_BLACK);
      display.drawPixel(pulseX - 5, 45, SSD1306_WHITE);
      display.drawPixel(pulseX - 4, 43, SSD1306_WHITE);
      display.drawPixel(pulseX - 3, 49, SSD1306_WHITE);
      display.drawPixel(pulseX - 2, 51, SSD1306_WHITE);
      display.drawPixel(pulseX - 1, 46, SSD1306_WHITE);
      display.drawPixel(pulseX,     44, SSD1306_WHITE);
      display.drawPixel(pulseX + 1, 46, SSD1306_WHITE);
      display.drawCircle(pulseX, 46, 2, SSD1306_WHITE);
    }

    display.drawLine(0, 53, 127, 53, SSD1306_WHITE);
    display.setCursor(0, 56);
    display.print(F("Lease: Host Active"));
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
  else if (strncmp(cmd, "VETO:", 5) == 0) {
    char* r = cmd + 5;
    char* t = strchr(r, ':');
    if (t) {
      *t = '\0';
      t++;
      strncpy(auxText2, t, sizeof(auxText2) - 1);
    } else {
      strncpy(auxText2, "BLOCKED", sizeof(auxText2) - 1);
    }
    strncpy(auxText1, r, sizeof(auxText1) - 1);
    auxText1[sizeof(auxText1) - 1] = '\0';
    auxText2[sizeof(auxText2) - 1] = '\0';

    currentState = STATE_VETO_ALERT;
    alertTimer = millis();
    setLEDColor(255, 0, 0); // Solid RED for VETO
    updateOLED();
    Serial.println(F("VETO:ACK"));
  }
  else if (strncmp(cmd, "DEMO:", 5) == 0) {
    char* t = cmd + 5;
    char* s = strchr(t, ':');
    if (s) {
      *s = '\0';
      s++;
      strncpy(auxText2, s, sizeof(auxText2) - 1);
    } else {
      strncpy(auxText2, "ACTIVE", sizeof(auxText2) - 1);
    }
    strncpy(auxText1, t, sizeof(auxText1) - 1);
    auxText1[sizeof(auxText1) - 1] = '\0';
    auxText2[sizeof(auxText2) - 1] = '\0';

    currentState = STATE_DEMO_ALERT;
    alertTimer = millis();
    setLEDColor(0, 180, 255); // Cyan / Blue for Demo
    updateOLED();
    Serial.println(F("DEMO:ACK"));
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

    strncpy(auxText1, p1, sizeof(auxText1) - 1);
    auxText1[sizeof(auxText1) - 1] = '\0';

    if (p2) {
      strncpy(auxText2, p2, sizeof(auxText2) - 1);
      auxText2[sizeof(auxText2) - 1] = '\0';
    } else {
      auxText2[0] = '\0';
    }

    if (p3) {
      strncpy(auxText3, p3, sizeof(auxText3) - 1);
      auxText3[sizeof(auxText3) - 1] = '\0';
    } else {
      auxText3[0] = '\0';
    }

    currentState = STATE_CUSTOM_MESSAGE;
    alertTimer = millis();
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
  Wire.setClock(400000); // 400kHz fast I2C mode
  delay(300);

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

    // Dynamic Boot Animation with loading bar
    for (int frame = 0; frame <= 10; frame++) {
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(2);
      display.setCursor(4, 8);
      display.println(F("VETO GUARD"));

      display.setTextSize(1);
      display.setCursor(6, 30);
      display.println(F("ZERO-TRUST SENTINEL"));

      display.drawRect(8, 46, 112, 10, SSD1306_WHITE);
      display.fillRect(10, 48, frame * 10, 6, SSD1306_WHITE);
      display.display();
      delay(50);
    }
    delay(300);
  } else {
    Serial.println(F("OLED:INIT_FAILED:CHECK_RAM_OR_WIRING"));
  }

  currentState = STATE_NORMAL;
  updateOLED();

  Serial.println(F("READY"));
}

void loop() {
  checkButtons();

  // Non-blocking serial reading
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

  // Animation frame update (~15 FPS)
  unsigned long now = millis();
  if (now - lastAnimTime >= ANIM_INTERVAL_MS) {
    lastAnimTime = now;
    animStep++;
    updateOLED();
  }

  // Feedback display timeout (return to normal after 3.5s)
  if (currentState == STATE_DECISION_FEEDBACK) {
    if (now - feedbackTimer > 3500) {
      currentState = STATE_NORMAL;
      setLEDColor(0, 255, 0);
      updateOLED();
    }
  }

  // VETO or DEMO alert timeout (return to normal after 4.0s)
  if (currentState == STATE_VETO_ALERT || currentState == STATE_DEMO_ALERT) {
    if (now - alertTimer > 4000) {
      currentState = STATE_NORMAL;
      setLEDColor(0, 255, 0);
      updateOLED();
    }
  }

  // Custom message timeout (return to normal after 15s)
  if (currentState == STATE_CUSTOM_MESSAGE) {
    if (now - alertTimer > 15000) {
      currentState = STATE_NORMAL;
      setLEDColor(0, 255, 0);
      updateOLED();
    }
  }

  // Heartbeat every 3 seconds
  if (now - lastHeartbeatSend > 3000) {
    lastHeartbeatSend = now;
    Serial.println(F("HEARTBEAT"));
  }
}
