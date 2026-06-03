// ╔══════════════════════════════════════════════════════════════╗
// ║        STM32F411 BLACK PILL — SECURITY SYSTEM               ║
// ║        FreeRTOS Edition v5.1 — ESP32 WiFi UI Integration    ║
// ║        FIX: trim() on all ESP commands, debug echo,         ║
// ║             xTestSmsSem binary safe-give, PIN sanity check  ║
// ╚══════════════════════════════════════════════════════════════╝

#include <STM32FreeRTOS.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <Keypad.h>
#include <SoftwareSerial.h>

SoftwareSerial ESPSerial(PA4, PA5);  // RX=PA4, TX=PA5
#define ESP_SERIAL ESPSerial

// ═══════════════════════════════════════════════════════════════
//  LCD
// ═══════════════════════════════════════════════════════════════
hd44780_I2Cexp lcd;

// ═══════════════════════════════════════════════════════════════
//  KEYPAD
// ═══════════════════════════════════════════════════════════════
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {PB9, PB8, PB5, PB4};
byte colPins[COLS] = {PB3, PB10, PB1, PB0};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ═══════════════════════════════════════════════════════════════
//  PINS
// ═══════════════════════════════════════════════════════════════
#define PIR_PIN       PA1
#define MAG_PIN       PA12
#define BUZZER_PIN    PB12
#define LED_PIN       PC13
#define RST_PIN       PA8
#define SIM_SERIAL    Serial1

// ═══════════════════════════════════════════════════════════════
//  SECURITY CODES & TIMING
//  Mutable — updated remotely via ESP32 CMD:PIN= and CMD:PHONE=
// ═══════════════════════════════════════════════════════════════
String ARM_CODE    = "1234";
String DISARM_CODE = "5678";
String SMS_TARGET  = "+201553071798";

const unsigned long ALERT_TIMEOUT   = 8000;
const unsigned long SIREN_LOW_FREQ  = 800;
const unsigned long SIREN_HIGH_FREQ = 1800;
const unsigned long SIREN_INTERVAL  = 400;

// ═══════════════════════════════════════════════════════════════
//  FSM
// ═══════════════════════════════════════════════════════════════
enum State {
  STATE_ARMED,
  STATE_ALERT,
  STATE_ALARM,
  STATE_DISARMED
};

volatile State         currentState   = STATE_DISARMED;
volatile unsigned long alertStartTime = 0;

// ═══════════════════════════════════════════════════════════════
//  FLAGS
// ═══════════════════════════════════════════════════════════════
volatile bool userIsTyping     = false;
volatile bool smsArmRequest    = false;
volatile bool smsDisarmRequest = false;
volatile bool espArmRequest    = false;
volatile bool espDisarmRequest = false;

// ═══════════════════════════════════════════════════════════════
//  LCD MESSAGE STRUCT
// ═══════════════════════════════════════════════════════════════
struct LcdMsg {
  char line1[17];
  char line2[17];
};

// ═══════════════════════════════════════════════════════════════
//  RTOS HANDLES
// ═══════════════════════════════════════════════════════════════
QueueHandle_t     xLcdQueue;
SemaphoreHandle_t xAlarmSem;
SemaphoreHandle_t xSmsSem;
SemaphoreHandle_t xTestSmsSem;
SemaphoreHandle_t xDisarmSem;
SemaphoreHandle_t xStateMutex;
SemaphoreHandle_t xI2CMutex;
SemaphoreHandle_t xBuzzerMutex;
SemaphoreHandle_t xEspSerialMutex;

// ═══════════════════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════════════════
void lcdSend(const char* l1, const char* l2) {
  LcdMsg msg;
  strncpy(msg.line1, l1, 16); msg.line1[16] = '\0';
  strncpy(msg.line2, l2, 16); msg.line2[16] = '\0';
  xQueueSend(xLcdQueue, &msg, 0);
}

void lcdFlushAndSend(const char* l1, const char* l2) {
  LcdMsg flush;
  while (xQueueReceive(xLcdQueue, &flush, 0) == pdTRUE) {}
  lcdSend(l1, l2);
}

void reportToESP(State s, bool pir, bool door) {
  xSemaphoreTake(xEspSerialMutex, portMAX_DELAY);
  ESP_SERIAL.println(s == STATE_DISARMED ? "DISARMED" : "ARMED");
  ESP_SERIAL.println(pir  ? "PIR:1" : "PIR:0");
  ESP_SERIAL.println(door ? "DOOR:1" : "DOOR:0");
  if (s == STATE_ALARM) ESP_SERIAL.println("ALARM");
  xSemaphoreGive(xEspSerialMutex);
}

String sendAT(const String& cmd, uint32_t timeout = 3000) {
  while (SIM_SERIAL.available()) SIM_SERIAL.read();
  SIM_SERIAL.println(cmd);
  String resp = "";
  uint32_t start = millis();
  while (millis() - start < timeout) {
    while (SIM_SERIAL.available()) resp += (char)SIM_SERIAL.read();
    if (resp.indexOf("OK")    != -1 ||
        resp.indexOf("ERROR") != -1) break;
  }
  return resp;
}

void safeBeep(int times, int ms) {
  xSemaphoreTake(xBuzzerMutex, portMAX_DELAY);
  noTone(BUZZER_PIN);
  for (int i = 0; i < times; i++) {
    tone(BUZZER_PIN, 1200, ms);
    vTaskDelay(pdMS_TO_TICKS(ms * 2));
  }
  xSemaphoreGive(xBuzzerMutex);
}

// FIX: helper to check if a string is exactly 4 decimal digits
bool isFourDigits(const String& s) {
  if (s.length() != 4) return false;
  for (int i = 0; i < 4; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

bool sendSmsTo(const String& target, const char* text) {
  sendAT("AT+CMGF=1");
  vTaskDelay(pdMS_TO_TICKS(300));
  while (SIM_SERIAL.available()) SIM_SERIAL.read();
  SIM_SERIAL.println("AT+CMGS=\"" + target + "\"");
  String buf = "";
  uint32_t t = millis();
  bool gotPrompt = false;
  while (millis() - t < 10000) {
    while (SIM_SERIAL.available()) buf += (char)SIM_SERIAL.read();
    if (buf.indexOf(">") != -1) { gotPrompt = true; break; }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!gotPrompt) return false;
  SIM_SERIAL.print(text);
  SIM_SERIAL.write(26);
  buf = "";
  t   = millis();
  while (millis() - t < 30000) {
    while (SIM_SERIAL.available()) buf += (char)SIM_SERIAL.read();
    if (buf.indexOf("+CMGS") != -1 || buf.indexOf("ERROR") != -1) break;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return buf.indexOf("+CMGS") != -1;
}

void sendReplySms(const char* text) {
  sendSmsTo(SMS_TARGET, text);
}

void parseSmsCommands() {
  sendAT("AT+CMGF=1");
  vTaskDelay(pdMS_TO_TICKS(200));
  while (SIM_SERIAL.available()) SIM_SERIAL.read();
  SIM_SERIAL.println("AT+CMGL=\"REC UNREAD\"");
  String resp = "";
  uint32_t t = millis();
  while (millis() - t < 5000) {
    while (SIM_SERIAL.available()) resp += (char)SIM_SERIAL.read();
    if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (resp.indexOf("+CMGL:") == -1) return;

  int searchPos = 0;
  while (true) {
    int hdrStart = resp.indexOf("+CMGL:", searchPos);
    if (hdrStart == -1) break;
    int commaAfterIdx = resp.indexOf(',', hdrStart + 6);
    String idxStr = resp.substring(hdrStart + 6, commaAfterIdx);
    idxStr.trim();
    int msgIndex = idxStr.toInt();
    int bodyStart = resp.indexOf('\n', hdrStart);
    if (bodyStart == -1) break;
    bodyStart++;
    int bodyEnd = resp.indexOf('\n', bodyStart);
    if (bodyEnd == -1) bodyEnd = resp.length();
    String body = resp.substring(bodyStart, bodyEnd);
    body.trim();

    // ── Keep a copy before uppercasing for value extraction ──
    String bodyRaw = body;
    body.toUpperCase();

    // ── ARM / DISARM system (no value) ───────────────────────
    if (body == "ARM") {
      smsArmRequest = true;
      lcdSend("SMS: ARM cmd", "Arming...");
      sendReplySms("Security system ARMED via SMS.");
    }
    else if (body == "DISARM") {
      smsDisarmRequest = true;
      lcdSend("SMS: DISARM cmd", "Disarming...");
      sendReplySms("Security system DISARMED via SMS.");
    }

    // ── ARM=XXXX  →  update arm PIN ──────────────────────────
    else if (body.startsWith("ARM=")) {
      String newPin = body.substring(4);  // already uppercased, digits only anyway
      newPin.trim();
      if (isFourDigits(newPin)) {
        ARM_CODE = newPin;
        lcdFlushAndSend("Arm PIN updated", newPin.c_str());
        sendReplySms("Arm code updated successfully.");
      } else {
        sendReplySms("Invalid arm code. Send ARM=XXXX (4 digits).");
      }
    }

    // ── DISARM=XXXX  →  update disarm PIN ────────────────────
    else if (body.startsWith("DISARM=")) {
      String newPin = body.substring(7);
      newPin.trim();
      if (isFourDigits(newPin)) {
        DISARM_CODE = newPin;
        lcdFlushAndSend("Disarm PIN upd.", newPin.c_str());
        sendReplySms("Disarm code updated successfully.");
      } else {
        sendReplySms("Invalid disarm code. Send DISARM=XXXX (4 digits).");
      }
    }

    // ── PHONE=+XXXXXXXXXXX  →  update SMS target ─────────────
    else if (body.startsWith("PHONE=")) {
      // Use bodyRaw here so the '+' and number case are preserved
      String newPhone = bodyRaw.substring(6);
      newPhone.trim();
      if (newPhone.length() >= 10) {
        SMS_TARGET = newPhone;
        char l2[17];
        newPhone.toCharArray(l2, 17);
        lcdFlushAndSend("Phone updated:", l2);
        sendReplySms("Phone number updated successfully.");
      } else {
        sendReplySms("Invalid phone. Send PHONE=+201XXXXXXXXX");
      }
    }

    // ── Unknown ───────────────────────────────────────────────
    else {
      sendReplySms("Unknown cmd. Valid: ARM, DISARM, ARM=XXXX, DISARM=XXXX, PHONE=+XX");
    }

    sendAT("AT+CMGD=" + String(msgIndex), 2000);
    searchPos = bodyEnd + 1;
  }
}
// ═══════════════════════════════════════════════════════════════
//  TASK 1 — SENSOR TASK
// ═══════════════════════════════════════════════════════════════
void SensorTask(void *pvParameters) {
  bool lastPir  = false;
  bool lastDoor = false;

  for (;;) {
    xSemaphoreTake(xStateMutex, portMAX_DELAY);
    State s = currentState;
    xSemaphoreGive(xStateMutex);

    bool pir  = (digitalRead(PIR_PIN) == HIGH);
    bool door = (digitalRead(MAG_PIN) == HIGH);

    if (pir != lastPir || door != lastDoor) {
      lastPir  = pir;
      lastDoor = door;
      reportToESP(s, pir, door);
    }

    if (s == STATE_ARMED && (pir || door)) {
      xSemaphoreGive(xAlarmSem);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ═══════════════════════════════════════════════════════════════
//  TASK 2 — ALARM TASK
// ═══════════════════════════════════════════════════════════════
void AlarmTask(void *pvParameters) {
  bool     sirenHigh   = false;
  bool     ledState    = false;
  uint32_t sirenToggle = 0;
  uint32_t ledToggle   = 0;
  State    lastReportedState = STATE_DISARMED;

  for (;;) {
    xSemaphoreTake(xStateMutex, portMAX_DELAY);
    State s = currentState;
    xSemaphoreGive(xStateMutex);

    if (s != lastReportedState) {
      lastReportedState = s;
      reportToESP(s,
        digitalRead(PIR_PIN) == HIGH,
        digitalRead(MAG_PIN) == HIGH);
    }

    // ── ESP32 ARM
    if (espArmRequest) {
      espArmRequest = false;
      if (s == STATE_DISARMED) {
        noTone(BUZZER_PIN);
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        currentState = STATE_ARMED;
        xSemaphoreGive(xStateMutex);
        lcdFlushAndSend("Armed via App", "Monitoring...");
        safeBeep(2, 100);
      }
    }

    // ── ESP32 DISARM
    if (espDisarmRequest) {
      espDisarmRequest = false;
      if (s == STATE_ARMED || s == STATE_ALERT || s == STATE_ALARM) {
        userIsTyping = false;
        xSemaphoreTake(xBuzzerMutex, portMAX_DELAY);
        noTone(BUZZER_PIN);
        xSemaphoreGive(xBuzzerMutex);
        digitalWrite(LED_PIN, HIGH);
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        currentState = STATE_DISARMED;
        xSemaphoreGive(xStateMutex);
        lcdFlushAndSend("Disarmed via App", "Safe");
        safeBeep(3, 100);
        s = STATE_DISARMED;
      }
    }

    // ── SMS ARM
    if (smsArmRequest) {
      smsArmRequest = false;
      if (s == STATE_DISARMED) {
        noTone(BUZZER_PIN);
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        currentState = STATE_ARMED;
        xSemaphoreGive(xStateMutex);
        lcdFlushAndSend("Armed via SMS", "Monitoring...");
        safeBeep(2, 100);
      }
    }

    // ── SMS DISARM
    if (smsDisarmRequest) {
      smsDisarmRequest = false;
      if (s == STATE_ARMED || s == STATE_ALERT || s == STATE_ALARM) {
        userIsTyping = false;
        xSemaphoreTake(xBuzzerMutex, portMAX_DELAY);
        noTone(BUZZER_PIN);
        xSemaphoreGive(xBuzzerMutex);
        digitalWrite(LED_PIN, HIGH);
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        currentState = STATE_DISARMED;
        xSemaphoreGive(xStateMutex);
        lcdFlushAndSend("Disarmed via SMS", "Safe");
        safeBeep(3, 100);
        s = STATE_DISARMED;
      }
    }

    // ── ARMED
    if (s == STATE_ARMED) {
      digitalWrite(LED_PIN,    HIGH);
      digitalWrite(BUZZER_PIN, LOW);
      if (xSemaphoreTake(xAlarmSem, pdMS_TO_TICKS(200)) == pdTRUE) {
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        currentState   = STATE_ALERT;
        alertStartTime = millis();
        xSemaphoreGive(xStateMutex);
        userIsTyping = false;
        lcdFlushAndSend("!! ALERT !!", "Enter code:");
        safeBeep(1, 300);
      }
      continue;
    }

    // ── ALERT
    if (s == STATE_ALERT) {
      uint32_t elapsed = millis() - alertStartTime;
      if (!userIsTyping) {
        int rem = max(0, (int)((ALERT_TIMEOUT - elapsed) / 1000));
        char l1[17];
        snprintf(l1, sizeof(l1), "!! ALERT !! %2ds", rem);
        lcdSend(l1, "Enter code:");
      }
      if (elapsed >= ALERT_TIMEOUT) {
        userIsTyping = false;
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        currentState = STATE_ALARM;
        xSemaphoreGive(xStateMutex);
        sirenToggle = millis();
        sirenHigh   = false;
        xSemaphoreGive(xSmsSem);
        lcdFlushAndSend("** ALARM **", "Enter code:");
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      if (xSemaphoreTake(xDisarmSem, 0) == pdTRUE) {
        userIsTyping = false;
        noTone(BUZZER_PIN);
        digitalWrite(LED_PIN, HIGH);
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        currentState = STATE_DISARMED;
        xSemaphoreGive(xStateMutex);
        lcdFlushAndSend("System Disarmed", "Safe");
        safeBeep(3, 100);
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // ── ALARM
    if (s == STATE_ALARM) {
      uint32_t now = millis();
      if (now - sirenToggle >= SIREN_INTERVAL) {
        sirenToggle = now;
        sirenHigh   = !sirenHigh;
        xSemaphoreTake(xBuzzerMutex, portMAX_DELAY);
        tone(BUZZER_PIN, sirenHigh ? SIREN_HIGH_FREQ : SIREN_LOW_FREQ);
        xSemaphoreGive(xBuzzerMutex);
      }
      if (now - ledToggle >= 500) {
        ledToggle = now;
        ledState  = !ledState;
        digitalWrite(LED_PIN, ledState ? LOW : HIGH);
      }
      if (xSemaphoreTake(xDisarmSem, 0) == pdTRUE) {
        userIsTyping = false;
        xSemaphoreTake(xBuzzerMutex, portMAX_DELAY);
        noTone(BUZZER_PIN);
        xSemaphoreGive(xBuzzerMutex);
        digitalWrite(LED_PIN, HIGH);
        xSemaphoreTake(xStateMutex, portMAX_DELAY);
        currentState = STATE_DISARMED;
        xSemaphoreGive(xStateMutex);
        lcdFlushAndSend("System Disarmed", "Safe");
        safeBeep(3, 100);
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // ── DISARMED
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ═══════════════════════════════════════════════════════════════
//  TASK 3 — GSM TASK
// ═══════════════════════════════════════════════════════════════
void GSMTask(void *pvParameters) {
  SIM_SERIAL.begin(9600);
  vTaskDelay(pdMS_TO_TICKS(100));
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, LOW);
  vTaskDelay(pdMS_TO_TICKS(200));
  digitalWrite(RST_PIN, HIGH);
  lcdSend("GSM Booting", "Please wait...");
  vTaskDelay(pdMS_TO_TICKS(8000));

  bool atOk = false;
  for (int i = 0; i < 5; i++) {
    if (sendAT("AT").indexOf("OK") != -1) { atOk = true; break; }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  if (!atOk) {
    lcdSend("GSM: No response", "SMS disabled");
    vTaskDelay(pdMS_TO_TICKS(2000));
    for (;;) {
      xSemaphoreTake(xSmsSem,     portMAX_DELAY);
      xSemaphoreTake(xTestSmsSem, 0);
      lcdSend("GSM unavailable", "");
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }

  sendAT("ATE0");
  sendAT("AT+CMGF=1");
  sendAT("AT+CMGDA=\"DEL ALL\"", 5000);

  bool netOk = false;
  for (int i = 0; i < 10; i++) {
    String creg = sendAT("AT+CREG?");
    if (creg.indexOf(",1") != -1 || creg.indexOf(",5") != -1) { netOk = true; break; }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  lcdSend(netOk ? "GSM Ready" : "GSM: No network",
          netOk ? "Network OK"  : "SMS may fail");
  vTaskDelay(pdMS_TO_TICKS(1500));
  lcdSend("System Disarmed", "Enter code: ARM");

  xSemaphoreTake(xEspSerialMutex, portMAX_DELAY);
  ESP_SERIAL.println(netOk ? "LOG:GSM OK" : "LOG:GSM fail");
  xSemaphoreGive(xEspSerialMutex);

  const uint32_t SMS_POLL_INTERVAL = 5000;
  uint32_t lastPollTime = millis();

  for (;;) {
    bool alarmSmsNeeded = (xSemaphoreTake(xSmsSem,     pdMS_TO_TICKS(SMS_POLL_INTERVAL)) == pdTRUE);
    bool testSmsNeeded  = (xSemaphoreTake(xTestSmsSem, 0) == pdTRUE);

    if (millis() - lastPollTime >= SMS_POLL_INTERVAL || alarmSmsNeeded || testSmsNeeded) {
      parseSmsCommands();
      lastPollTime = millis();
    }

    if (testSmsNeeded) {
      lcdSend("Sending test SMS", "");
      bool ok = sendSmsTo(SMS_TARGET, "NEXUS Security: Test SMS OK. System is online.");
      lcdSend(ok ? "Test SMS sent!" : "Test SMS failed", "");
      xSemaphoreTake(xEspSerialMutex, portMAX_DELAY);
      ESP_SERIAL.println(ok ? "LOG:Test SMS OK" : "LOG:Test SMS fail");
      xSemaphoreGive(xEspSerialMutex);
      vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (alarmSmsNeeded) {
      lcdSend("Sending SMS...", "");
      bool ok = sendSmsTo(SMS_TARGET, "ALARM! Intruder detected at your property.");
      lcdSend(ok ? "SMS Sent OK!" : "SMS Failed", "");
      xSemaphoreTake(xEspSerialMutex, portMAX_DELAY);
      ESP_SERIAL.println(ok ? "LOG:Alarm SMS sent" : "LOG:Alarm SMS fail");
      xSemaphoreGive(xEspSerialMutex);
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  TASK 4 — UI TASK
// ═══════════════════════════════════════════════════════════════
void UITask(void *pvParameters) {
  String enteredCode   = "";
  int    wrongAttempts = 0;

  for (;;) {
    LcdMsg msg;
    if (xQueueReceive(xLcdQueue, &msg, 0) == pdTRUE) {
      xSemaphoreTake(xI2CMutex, portMAX_DELAY);
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print(msg.line1);
      lcd.setCursor(0, 1); lcd.print(msg.line2);
      xSemaphoreGive(xI2CMutex);
    }

    char key = keypad.getKey();
    if (!key) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    xSemaphoreTake(xStateMutex, portMAX_DELAY);
    State s = currentState;
    xSemaphoreGive(xStateMutex);

    if (s == STATE_DISARMED) {
      if (key == '*') {
        userIsTyping = false;
        enteredCode  = "";
        lcdFlushAndSend("System Disarmed", "Enter code: ARM");
      } else {
        userIsTyping = true;
        enteredCode += key;
        LcdMsg flush;
        while (xQueueReceive(xLcdQueue, &flush, 0) == pdTRUE) {}
        char disp[17] = {0};
        for (int i = 0; i < (int)enteredCode.length() && i < 16; i++) disp[i] = '*';
        LcdMsg m;
        strncpy(m.line1, "DISARMED", 16);  m.line1[16] = '\0';
        strncpy(m.line2, disp, 16);        m.line2[16] = '\0';
        xQueueSend(xLcdQueue, &m, 0);
        if (enteredCode.length() == 4) {
          userIsTyping = false;
          if (enteredCode == ARM_CODE) {
            xSemaphoreTake(xStateMutex, portMAX_DELAY);
            currentState = STATE_ARMED;
            xSemaphoreGive(xStateMutex);
            lcdFlushAndSend("System Armed", "Monitoring...");
            safeBeep(2, 100);
          } else {
            lcdFlushAndSend("Wrong Code", "Try again");
          }
          enteredCode   = "";
          wrongAttempts = 0;
        }
      }
    }
    else if (s == STATE_ALERT || s == STATE_ALARM) {
      const char* header = (s == STATE_ALERT) ? "!! ALERT !!" : "** ALARM **";
      if (key == '*') {
        userIsTyping = false;
        enteredCode  = "";
        lcdFlushAndSend(header, "Enter code:");
      } else {
        userIsTyping = true;
        enteredCode += key;
        LcdMsg flush;
        while (xQueueReceive(xLcdQueue, &flush, 0) == pdTRUE) {}
        char disp[17] = {0};
        for (int i = 0; i < (int)enteredCode.length() && i < 16; i++) disp[i] = '*';
        LcdMsg m;
        strncpy(m.line1, header, 16); m.line1[16] = '\0';
        strncpy(m.line2, disp,   16); m.line2[16] = '\0';
        xQueueSend(xLcdQueue, &m, 0);
        if (enteredCode.length() == 4) {
          userIsTyping = false;
          if (enteredCode == DISARM_CODE) {
            wrongAttempts = 0;
            xSemaphoreGive(xDisarmSem);
          } else {
            wrongAttempts++;
            safeBeep(2, 150);
            if (wrongAttempts >= 3 && s == STATE_ALERT) {
              xSemaphoreTake(xStateMutex, portMAX_DELAY);
              currentState = STATE_ALARM;
              xSemaphoreGive(xStateMutex);
              xSemaphoreGive(xSmsSem);
              wrongAttempts = 0;
              lcdFlushAndSend("** ALARM **", "Enter code:");
            } else {
              lcdFlushAndSend("Wrong Code!", "Try again");
            }
          }
          enteredCode = "";
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ═══════════════════════════════════════════════════════════════
//  TASK 5 — ESP TASK
//  Parses all commands from the ESP32 web dashboard
//
//  KEY FIXES vs v5:
//  1. cmd.trim() is now the FIRST thing done — strips the \r
//     that ESP32 println() appends (causes all strcmp to fail)
//  2. Debug echo: every received command is echoed back as
//     LOG:RX=... so you can see it in the web dashboard log
//  3. isFourDigits() used instead of length()==4 to reject
//     non-numeric PINs that could crash keypad comparison
//  4. xTestSmsSem give wrapped in a uxSemaphoreGetCount guard
//     to avoid counting above 1 on repeated TEST SMS presses
// ═══════════════════════════════════════════════════════════════
void ESPTask(void *pvParameters) {
  for (;;) {
    if (ESP_SERIAL.available()) {
      String cmd = ESP_SERIAL.readStringUntil('\n');

      // FIX 1: Strip \r and any surrounding whitespace FIRST.
      // ESP32 Serial2.println() sends "CMD:xxx\r\n".
      // readStringUntil('\n') stops at \n but keeps the \r,
      // so without trim() every string comparison fails silently.
      cmd.trim();

      if (cmd.length() == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }

      // FIX 2: Echo received command back to web log for debugging.
      // You will see "RX=CMD:PIN=1234:5678" in the event log,
      // confirming the STM32 received the command intact.
      xSemaphoreTake(xEspSerialMutex, portMAX_DELAY);
      ESP_SERIAL.println("LOG:RX=" + cmd);
      xSemaphoreGive(xEspSerialMutex);

      // ── Basic arm/disarm ────────────────────────────────────
      if (cmd == "CMD:ARM") {
        espArmRequest = true;
      }

      if (cmd == "CMD:DISARM") {
        espDisarmRequest = true;
      }

      // ── Test SMS: CMD:TESTSMS ───────────────────────────────
      // FIX 4: Only give semaphore if not already pending,
      // preventing counter from accumulating on rapid clicks.
      if (cmd == "CMD:TESTSMS") {
        if (uxSemaphoreGetCount(xTestSmsSem) == 0) {
          xSemaphoreGive(xTestSmsSem);
        }
        lcdSend("Test SMS queued", "");
      }

      // ── Update phone number: CMD:PHONE=+201XXXXXXXXX ────────
      if (cmd.startsWith("CMD:PHONE=")) {
        String newPhone = cmd.substring(10);  // skip "CMD:PHONE="
        newPhone.trim();
        if (newPhone.length() >= 10) {
          SMS_TARGET = newPhone;
          // Show on LCD (truncate to 16 chars)
          char l2[17];
          newPhone.toCharArray(l2, 17);
          lcdFlushAndSend("Phone updated:", l2);
          xSemaphoreTake(xEspSerialMutex, portMAX_DELAY);
          ESP_SERIAL.println("LOG:Phone updated");
          xSemaphoreGive(xEspSerialMutex);
        } else {
          lcdSend("Phone too short", "");
          xSemaphoreTake(xEspSerialMutex, portMAX_DELAY);
          ESP_SERIAL.println("LOG:Phone invalid");
          xSemaphoreGive(xEspSerialMutex);
        }
      }

      // ── Update PINs: CMD:PIN=ARMCODE:DISARMCODE ─────────────
      // e.g. CMD:PIN=1234:5678
      if (cmd.startsWith("CMD:PIN=")) {
        String payload = cmd.substring(8);   // skip "CMD:PIN="
        payload.trim();
        int sep = payload.indexOf(':');

        if (sep != -1) {
          String newArm    = payload.substring(0, sep);
          String newDisarm = payload.substring(sep + 1);
          newArm.trim();
          newDisarm.trim();

          // FIX 3: Use isFourDigits() which validates both length
          // AND that every character is 0-9, not just length == 4
          if (isFourDigits(newArm) && isFourDigits(newDisarm)) {
            ARM_CODE    = newArm;
            DISARM_CODE = newDisarm;
            lcdFlushAndSend("PINs updated!", "ARM & DISARM OK");
            xSemaphoreTake(xEspSerialMutex, portMAX_DELAY);
            ESP_SERIAL.println("LOG:PINs updated");
            xSemaphoreGive(xEspSerialMutex);
          } else {
            // Tell the dashboard exactly what was rejected
            String errMsg = "LOG:PIN bad: [" + newArm + "][" + newDisarm + "]";
            lcdSend("PIN invalid", "Need 4 digits");
            xSemaphoreTake(xEspSerialMutex, portMAX_DELAY);
            ESP_SERIAL.println(errMsg);
            ESP_SERIAL.println("LOG:PIN format error");
            xSemaphoreGive(xEspSerialMutex);
          }
        } else {
          // No ':' separator found — malformed command
          lcdSend("PIN cmd malformed", "");
          xSemaphoreTake(xEspSerialMutex, portMAX_DELAY);
          ESP_SERIAL.println("LOG:PIN no separator");
          xSemaphoreGive(xEspSerialMutex);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  pinMode(PIR_PIN,    INPUT);
  pinMode(MAG_PIN,    INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN,    OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN,    HIGH);

  Wire.setSDA(PB7);
  Wire.setSCL(PB6);
  Wire.begin();
  lcd.begin(16, 2);
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Booting...");

  ESP_SERIAL.begin(9600);

  xLcdQueue        = xQueueCreate(8, sizeof(LcdMsg));
  xAlarmSem        = xSemaphoreCreateBinary();
  xSmsSem          = xSemaphoreCreateBinary();
  xTestSmsSem      = xSemaphoreCreateBinary();
  xDisarmSem       = xSemaphoreCreateBinary();
  xStateMutex      = xSemaphoreCreateMutex();
  xI2CMutex        = xSemaphoreCreateMutex();
  xBuzzerMutex     = xSemaphoreCreateMutex();
  xEspSerialMutex  = xSemaphoreCreateMutex();

  xTaskCreate(GSMTask,    "GSM",    640,  NULL, 2, NULL);
  xTaskCreate(SensorTask, "Sensor", 256,  NULL, 3, NULL);
  xTaskCreate(AlarmTask,  "Alarm",  512,  NULL, 3, NULL);
  xTaskCreate(UITask,     "UI",     512,  NULL, 1, NULL);
  xTaskCreate(ESPTask,    "ESP",    256,  NULL, 1, NULL);

  vTaskStartScheduler();
}

void loop() {}