/*
 * ==========================================================================
 *   ____    ___    ____   _   _      _      _     
 *  / ___|  |_ _|  / ___| | \ | |    / \    | |    
 *  \___ \   | |  | |  _  |  \| |   / _ \   | |    
 *   ___) |  | |  | |_| | | |\  |  / ___ \  | |___ 
 *  |____/  |___|  \____| |_| \_| /_/   \_\ |_____|
 
 * ==========================================================================
 *  SIGNAL — Off-Grid Emergency Signaling Device
 *  Platform: ESP32 Dev Module
 *  Display : 0.96" SSD1306 I2C OLED (128x64, addr 0x3C, SDA21/SCL22)
 *  Input   : Single multifunction button (GPIO4, INPUT_PULLUP)
 *  Output  : Buzzer (GPIO19) + on-board guide indicator via OLED
 *  Radio   : ESP-NOW broadcast beacon (peerless, FF:FF:FF:FF:FF:FF)
 *
 *  Libraries required (install via Library Manager):
 *   - U8g2 (by oliver / olikraus)
 *  ESP-NOW and WiFi come bundled with the ESP32 board package.
 * ==========================================================================
 */

#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ==========================================================================
//  CONFIGURATION
// ==========================================================================

// ---- Buzzer hardware toggle ---------------------------------------------
#define BUZZER_IS_PASSIVE true   // true = passive (PWM tone), false = active (on/off)

// ---- Pin map --------------------------------------------------------------
#define BUZZER_PIN      19
#define BUTTON_PIN      4
#define OLED_SDA        21
#define OLED_SCL        22
#define OLED_ADDR       0x3C
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64

// ---- Morse timing -----------------------------------------------------
#define MORSE_UNIT_MS   1000
#define TONE_FREQ_HZ    2400

// ---- Button timing ------------------------------------------------------
#define DEBOUNCE_MS         35
#define LONG_PRESS_MS        600
#define DOUBLE_CLICK_MS      300

// ---- ESP-NOW ----------------------------------------------------------
#define BEACON_INTERVAL_MS  2500UL
#define DEVICE_NAME         "SIGNAL-01"

// ---- PWM channel for passive buzzer ------------------------------------
#define BUZZER_PWM_CHANNEL   0
#define BUZZER_PWM_RES_BITS  8

// ==========================================================================
//  TYPE DEFINITIONS (must precede all functions — Arduino auto-generates
//  function prototypes right after the #includes, so any custom type used
//  in a function signature must already be defined above that point)
// ==========================================================================

enum AppState {
  STATE_SPLASH,
  STATE_MENU,
  STATE_MORSE_PLAY,
  STATE_BEACON_PLAY
};

enum ButtonEvent {
  BTN_NONE,
  BTN_SINGLE_CLICK,
  BTN_DOUBLE_CLICK,
  BTN_LONG_PRESS
};

enum MorseSymbolType { SYM_DOT, SYM_DASH, SYM_INTRA_GAP, SYM_CHAR_GAP, SYM_WORD_GAP };

struct MorseSymbol {
  MorseSymbolType type;
};

struct Preset {
  const char* label;   // menu label
  const char* text;    // text to morse-encode (nullptr for special modes)
  bool isBeacon;        // true => ESP-NOW beacon mode instead of morse playback
};

typedef struct __attribute__((packed)) {
  char deviceName[16];
  char status[16];
  uint32_t packetCounter;
} EmergencyPacket;

// ==========================================================================
//  GLOBAL OBJECTS
// ==========================================================================

// Full-frame-buffer mode, hardware I2C. _F_ = full buffer (needed since we
// redraw incrementally with mixed fonts/shapes before a single sendBuffer()).
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ==========================================================================
//  MORSE CODE TABLE (ITU standard)
// ==========================================================================

struct MorseEntry {
  char c;
  const char* code;
};

static const MorseEntry MORSE_TABLE[] = {
  {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},
  {'E', "."},     {'F', "..-."},  {'G', "--."},   {'H', "...."},
  {'I', ".."},    {'J', ".---"},  {'K', "-.-"},   {'L', ".-.."},
  {'M', "--"},    {'N', "-."},    {'O', "---"},   {'P', ".--."},
  {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
  {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},
  {'Y', "-.--"},  {'Z', "--.."},
  {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
  {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
  {'8', "---.."}, {'9', "----."},
  {'.', ".-.-.-"} , {',', "--..--"}
};
static const int MORSE_TABLE_LEN = sizeof(MORSE_TABLE) / sizeof(MorseEntry);

const char* morseLookup(char c) {
  c = toupper(c);
  for (int i = 0; i < MORSE_TABLE_LEN; i++) {
    if (MORSE_TABLE[i].c == c) return MORSE_TABLE[i].code;
  }
  return nullptr; // unsupported char (e.g. space handled separately)
}

// ==========================================================================
//  PRESET MESSAGES
// ==========================================================================

static const Preset PRESETS[] = {
  {"SOS",         "SOS",  false},
  {"MED",         "MED",  false},
  {"FIRE",        "FIRE", false},
  {"LOST",        "LOST", false},
  {"HERE",        "HERE", false},
  {"OK",          "OK",   false},
  {"2.4G BEACON", nullptr, true}
};
static const int PRESET_COUNT = sizeof(PRESETS) / sizeof(Preset);

// ==========================================================================
//  APPLICATION STATE MACHINE
// ==========================================================================

volatile AppState appState = STATE_SPLASH;
int menuIndex = 0;

// ==========================================================================
//  BUTTON INPUT ENGINE (non-blocking)
// ==========================================================================

// Raw debounced level tracking
bool btnLastRawState = HIGH;
bool btnStableState = HIGH;
unsigned long btnLastDebounceTime = 0;

// Click / press tracking
bool btnPressActive = false;
unsigned long btnPressStartTime = 0;
bool btnLongPressFired = false;

unsigned long lastReleaseTime = 0;
bool awaitingSecondClick = false;

ButtonEvent pollButton() {
  ButtonEvent evt = BTN_NONE;
  bool raw = digitalRead(BUTTON_PIN); // LOW = pressed (INPUT_PULLUP)

  unsigned long now = millis();

  if (raw != btnLastRawState) {
    btnLastDebounceTime = now;
  }

  if ((now - btnLastDebounceTime) > DEBOUNCE_MS) {
    if (raw != btnStableState) {
      btnStableState = raw;

      if (btnStableState == LOW) {
        // Button just pressed
        btnPressActive = true;
        btnPressStartTime = now;
        btnLongPressFired = false;
      } else {
        // Button just released
        if (btnPressActive && !btnLongPressFired) {
          unsigned long heldFor = now - btnPressStartTime;
          if (heldFor < LONG_PRESS_MS) {
            // Could be a single or the second half of a double click
            if (awaitingSecondClick && (now - lastReleaseTime) <= DOUBLE_CLICK_MS) {
              evt = BTN_DOUBLE_CLICK;
              awaitingSecondClick = false;
            } else {
              awaitingSecondClick = true;
              lastReleaseTime = now;
            }
          }
        }
        btnPressActive = false;
      }
    }
  }

  // Long-press detection while held (fires once)
  if (btnPressActive && !btnLongPressFired && btnStableState == LOW) {
    if ((now - btnPressStartTime) >= LONG_PRESS_MS) {
      btnLongPressFired = true;
      awaitingSecondClick = false; // long press cancels pending click logic
      evt = BTN_LONG_PRESS;
    }
  }

  // Resolve a pending single click if the double-click window expired
  if (awaitingSecondClick && (now - lastReleaseTime) > DOUBLE_CLICK_MS) {
    awaitingSecondClick = false;
    evt = BTN_SINGLE_CLICK;
  }

  btnLastRawState = raw;
  return evt;
}

// ==========================================================================
//  BUZZER CONTROL
// ==========================================================================

void buzzerInit() {
#if BUZZER_IS_PASSIVE
  ledcSetup(BUZZER_PWM_CHANNEL, TONE_FREQ_HZ, BUZZER_PWM_RES_BITS);
  ledcAttachPin(BUZZER_PIN, BUZZER_PWM_CHANNEL);
  ledcWrite(BUZZER_PWM_CHANNEL, 0);
#else
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
#endif
}

void buzzerOn() {
#if BUZZER_IS_PASSIVE
  ledcWriteTone(BUZZER_PWM_CHANNEL, TONE_FREQ_HZ);
#else
  digitalWrite(BUZZER_PIN, HIGH);
#endif
}

void buzzerOff() {
#if BUZZER_IS_PASSIVE
  ledcWriteTone(BUZZER_PWM_CHANNEL, 0);
#else
  digitalWrite(BUZZER_PIN, LOW);
#endif
}

// ==========================================================================
//  ESP-NOW BEACON
// ==========================================================================

uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint32_t beaconPacketCount = 0;
unsigned long lastBeaconTime = 0;
bool espNowReady = false;
bool lastSendOk = false;

void onEspNowSent(const uint8_t* mac, esp_now_send_status_t status) {
  lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
}

bool beaconInit() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    return false;
  }
  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;

  if (!esp_now_is_peer_exist(broadcastMac)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      return false;
    }
  }
  return true;
}

void beaconTeardown() {
  if (esp_now_is_peer_exist(broadcastMac)) {
    esp_now_del_peer(broadcastMac);
  }
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  espNowReady = false;
}

void sendBeaconPacket() {
  EmergencyPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  strncpy(pkt.deviceName, DEVICE_NAME, sizeof(pkt.deviceName) - 1);
  strncpy(pkt.status, "EMERGENCY", sizeof(pkt.status) - 1);
  pkt.packetCounter = ++beaconPacketCount;

  esp_now_send(broadcastMac, (uint8_t*)&pkt, sizeof(pkt));
}

// ==========================================================================
//  MORSE PLAYBACK ENGINE (non-blocking state machine)
// ==========================================================================

// Playback symbol stream is built once per preset activation.
String morseStream;        // e.g. "...---... " with spaces as inter-char/word markers already expanded via symbol list
String morseSymbols;       // flattened symbol list: '.' , '-', 'c' (intra-char pause end), ' ' (word space) etc.

#define MAX_MORSE_SYMBOLS 512
MorseSymbol morseSeq[MAX_MORSE_SYMBOLS];
int morseSeqLen = 0;
int morsePlayIndex = 0;

bool morsePulseActive = false;      // true while a dot/dash tone+light is ON
unsigned long morseSymbolStart = 0;
unsigned long morseSymbolDuration = 0;

const char* currentPresetLabel = "";
char currentSymbolDesc[16] = "";

void buildMorseSequence(const char* text) {
  morseSeqLen = 0;
  int len = strlen(text);

  for (int i = 0; i < len && morseSeqLen < MAX_MORSE_SYMBOLS - 8; i++) {
    char c = text[i];

    if (c == ' ') {
      morseSeq[morseSeqLen++].type = SYM_WORD_GAP;
      continue;
    }

    const char* code = morseLookup(c);
    if (!code) continue;

    int codeLen = strlen(code);
    for (int j = 0; j < codeLen; j++) {
      if (code[j] == '.') {
        morseSeq[morseSeqLen++].type = SYM_DOT;
      } else if (code[j] == '-') {
        morseSeq[morseSeqLen++].type = SYM_DASH;
      }
      // intra-character gap after every symbol except the last in this letter
      if (j < codeLen - 1 && morseSeqLen < MAX_MORSE_SYMBOLS) {
        morseSeq[morseSeqLen++].type = SYM_INTRA_GAP;
      }
    }
    // inter-character gap after each letter (unless next is a space, handled above)
    if (i < len - 1 && text[i + 1] != ' ' && morseSeqLen < MAX_MORSE_SYMBOLS) {
      morseSeq[morseSeqLen++].type = SYM_CHAR_GAP;
    }
  }
}

unsigned long durationForSymbol(MorseSymbolType t) {
  switch (t) {
    case SYM_DOT:       return MORSE_UNIT_MS * 1;
    case SYM_DASH:       return MORSE_UNIT_MS * 3;
    case SYM_INTRA_GAP:  return MORSE_UNIT_MS * 1;
    case SYM_CHAR_GAP:   return MORSE_UNIT_MS * 3;
    case SYM_WORD_GAP:   return MORSE_UNIT_MS * 7;
  }
  return MORSE_UNIT_MS;
}

bool symbolIsPulse(MorseSymbolType t) {
  return (t == SYM_DOT || t == SYM_DASH);
}

void startMorsePlayback(const Preset& p) {
  buildMorseSequence(p.text);
  morsePlayIndex = 0;
  currentPresetLabel = p.label;
  morsePulseActive = false;
  morseSymbolStart = 0;
  morseSymbolDuration = 0;
  strcpy(currentSymbolDesc, "READY");
  appState = STATE_MORSE_PLAY;
}

// Advances / drives the morse playback. Called every loop() iteration.
// Loops the message continuously until interrupted.
void updateMorsePlayback() {
  unsigned long now = millis();

  if (morseSeqLen == 0) {
    // Nothing to play (unsupported text) — bail to menu
    appState = STATE_MENU;
    return;
  }

  if (morseSymbolDuration == 0 || (now - morseSymbolStart) >= morseSymbolDuration) {
    // finish previous pulse if any
    if (morsePulseActive) {
      buzzerOff();
      morsePulseActive = false;
    }

    // advance to next symbol, loop around at end (with a word gap breather)
    if (morsePlayIndex >= morseSeqLen) {
      morsePlayIndex = 0;
    }

    MorseSymbolType t = morseSeq[morsePlayIndex].type;
    morseSymbolDuration = durationForSymbol(t);
    morseSymbolStart = now;

    if (symbolIsPulse(t)) {
      buzzerOn();
      morsePulseActive = true;
      strcpy(currentSymbolDesc, (t == SYM_DOT) ? "DOT ." : "DASH -");
    } else {
      morsePulseActive = false;
      strcpy(currentSymbolDesc, "PAUSE");
    }

    morsePlayIndex++;
  }
}

void stopMorsePlayback() {
  buzzerOff();
  morsePulseActive = false;
  morseSeqLen = 0;
  appState = STATE_MENU;
}

// ==========================================================================
//  DISPLAY RENDERING
// ==========================================================================

// U8g2 draws bottom-up from a baseline y-coordinate (not top-left like GFX).
// centerTextX() returns the x needed to center a string using the currently
// selected font.
int centerTextX(const char* text) {
  int w = u8g2.getStrWidth(text);
  return (SCREEN_WIDTH - w) / 2;
}

void drawDivider(int y) {
  u8g2.drawHLine(0, y, SCREEN_WIDTH);
}

void renderSplash() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  u8g2.setFont(u8g2_font_logisoso24_tr); // large bold font for the title
  u8g2.drawStr(centerTextX("SIGNAL"), 30, "SIGNAL");

  u8g2.setFont(u8g2_font_6x10_tr);
  const char* sub = "EMERGENCY BEACON";
  u8g2.drawStr(centerTextX(sub), 44, sub);

  drawDivider(54);
  u8g2.drawStr(2, 63, "Click=Next  Hold=Select");

  u8g2.sendBuffer();
}

void renderMenu() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 8, "SIGNAL - MAIN MENU");
  drawDivider(10);

  // Show up to 4 items per page, centered around current selection
  const int visibleRows = 4;
  const int rowHeight = 12;
  int startIdx = menuIndex - 1;
  if (startIdx < 0) startIdx = 0;
  if (startIdx > PRESET_COUNT - visibleRows) startIdx = max(0, PRESET_COUNT - visibleRows);

  int rowY = 13; // top of first row
  for (int i = startIdx; i < min(PRESET_COUNT, startIdx + visibleRows); i++) {
    bool selected = (i == menuIndex);
    if (selected) {
      u8g2.setDrawColor(1);
      u8g2.drawBox(0, rowY, SCREEN_WIDTH, rowHeight - 1);
      u8g2.setDrawColor(0); // inverted text on filled bar
    } else {
      u8g2.setDrawColor(1);
    }
    u8g2.drawStr(4, rowY + 9, PRESETS[i].label);
    rowY += rowHeight;
  }

  u8g2.setDrawColor(1);
  drawDivider(62);
  u8g2.sendBuffer();
}

void renderMorsePlayback() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  // Top bar: preset name
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 8, currentPresetLabel);
  drawDivider(10);

  // Center: light guide block
  bool lightOn = morsePulseActive;
  int boxX = 14, boxY = 16, boxW = 100, boxH = 30;

  if (lightOn) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(boxX, boxY, boxW, boxH);
    u8g2.setDrawColor(0); // inverted text on filled block
    u8g2.setFont(u8g2_font_7x14B_tr);
    u8g2.drawStr(boxX + 8, boxY + 20, "LIGHT ON");
  } else {
    u8g2.setDrawColor(1);
    u8g2.drawFrame(boxX, boxY, boxW, boxH);
    u8g2.drawFrame(boxX + 1, boxY + 1, boxW - 2, boxH - 2);
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(boxX + 20, boxY + 18, "LIGHT OFF");
  }

  // Bottom: current symbol + hint
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_6x10_tr);
  drawDivider(50);
  u8g2.drawStr(0, 62, currentSymbolDesc);
  u8g2.drawStr(70, 62, "2xClk=Exit");

  u8g2.sendBuffer();
}

void renderBeaconPlayback() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 8, "2.4G DISASTER BEACON");
  drawDivider(10);

  u8g2.drawStr(0, 24, "Status:");
  u8g2.drawStr(48, 24, espNowReady ? "TRANSMITTING" : "INIT FAILED");

  u8g2.drawStr(0, 36, "Packets sent:");
  u8g2.setFont(u8g2_font_7x14B_tr);
  char countBuf[12];
  snprintf(countBuf, sizeof(countBuf), "%lu", (unsigned long)beaconPacketCount);
  u8g2.drawStr(0, 52, countBuf);

  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(80, 24, lastSendOk ? "TX OK" : "TX ...");

  drawDivider(56);
  u8g2.drawStr(0, 63, "2xClick = Exit");

  u8g2.sendBuffer();
}

// ==========================================================================
//  BEACON MODE DRIVER (non-blocking)
// ==========================================================================

void enterBeaconMode() {
  beaconPacketCount = 0;
  lastSendOk = false;
  espNowReady = beaconInit();
  lastBeaconTime = 0; // force immediate first send
  appState = STATE_BEACON_PLAY;
}

void exitBeaconMode() {
  buzzerOff();
  beaconTeardown();
  appState = STATE_MENU;
}

void updateBeaconMode() {
  if (!espNowReady) return;

  unsigned long now = millis();
  if (now - lastBeaconTime >= BEACON_INTERVAL_MS) {
    lastBeaconTime = now;
    sendBeaconPacket();

    // Brief chirp per broadcast
    buzzerOn();
    delayMicroseconds(1); // no-op, keep structure explicit
    // Short non-blocking chirp handled via short blocking pulse (<10ms, imperceptible to UI)
    // Using a very short blocking delay here is acceptable: it is far shorter than any
    // debounce/UI interval and keeps the chirp crisp and audible.
    delay(8);
    buzzerOff();
  }
}

// ==========================================================================
//  STATE TRANSITIONS DRIVEN BY BUTTON EVENTS
// ==========================================================================

void handleButtonEvent(ButtonEvent evt) {
  if (evt == BTN_NONE) return;

  switch (appState) {

    case STATE_SPLASH:
      // Any interaction skips splash
      if (evt == BTN_SINGLE_CLICK || evt == BTN_LONG_PRESS || evt == BTN_DOUBLE_CLICK) {
        appState = STATE_MENU;
      }
      break;

    case STATE_MENU:
      if (evt == BTN_SINGLE_CLICK) {
        menuIndex = (menuIndex + 1) % PRESET_COUNT;
      } else if (evt == BTN_LONG_PRESS) {
        const Preset& p = PRESETS[menuIndex];
        if (p.isBeacon) {
          enterBeaconMode();
        } else {
          startMorsePlayback(p);
        }
      }
      // double click on menu: no-op (already at top level)
      break;

    case STATE_MORSE_PLAY:
      if (evt == BTN_DOUBLE_CLICK) {
        stopMorsePlayback();
      }
      break;

    case STATE_BEACON_PLAY:
      if (evt == BTN_DOUBLE_CLICK) {
        exitBeaconMode();
      }
      break;
  }
}

// ==========================================================================
//  SETUP
// ==========================================================================

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  buzzerInit();

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000UL); // fast-mode I2C for snappier redraws

  u8g2.setI2CAddress(OLED_ADDR << 1); // U8g2 wants the 8-bit (shifted) address
  if (!u8g2.begin()) {
    Serial.println("U8g2 SSD1306 init failed");
    for (;;) { delay(1000); } // halt — display is essential to this device's UI
  }
  u8g2.setBusClock(400000UL);
  u8g2.setContrast(255);
  u8g2.setFontMode(1);      // transparent background for mixed draw-color text
  u8g2.setFontPosBaseline(); // ensure y coordinates passed to drawStr are the text baseline

  renderSplash();
}

// ==========================================================================
//  MAIN LOOP (fully non-blocking except an 8ms audible beacon chirp)
// ==========================================================================

unsigned long lastRenderTime = 0;
#define RENDER_INTERVAL_MS 40 // ~25 FPS UI refresh, cheap on OLED

void loop() {
  ButtonEvent evt = pollButton();
  handleButtonEvent(evt);

  switch (appState) {
    case STATE_MORSE_PLAY:
      updateMorsePlayback();
      break;
    case STATE_BEACON_PLAY:
      updateBeaconMode();
      break;
    default:
      break;
  }

  unsigned long now = millis();
  if (now - lastRenderTime >= RENDER_INTERVAL_MS) {
    lastRenderTime = now;
    switch (appState) {
      case STATE_SPLASH:        renderSplash();          break;
      case STATE_MENU:          renderMenu();            break;
      case STATE_MORSE_PLAY:    renderMorsePlayback();   break;
      case STATE_BEACON_PLAY:   renderBeaconPlayback();  break;
    }
  }
}
