#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

Adafruit_SSD1306 display(128, 64, &Wire, -1);

#define BUZZER 12

// =====================================================
//                    EEPROM
// =====================================================
// Notes:   адрес 0   — до 200 байт
// Notes:   адрес 201 — маркер
// Counter: адрес 202 — маркер (4 байта)
// Counter: адрес 206 — score (4 байта)

#define EEPROM_NOTES_ADDR     0
#define EEPROM_NOTES_MAX      200
#define EEPROM_MAGIC_ADDR     201
#define EEPROM_MAGIC_VALUE    0xA5

#define EEPROM_COUNTER_ADDR   202
#define EEPROM_COUNTER_MAGIC  0xA5A5A5A5UL

// ---------------- Своя клавиатура ----------------
const byte ROWS = 4;
const byte COLS = 4;
const char keymap[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
const byte rowPins[ROWS] = {9, 8, 7, 6};
const byte colPins[COLS] = {5, 4, 3, 2};

char lastKey = 0;

char scanKeypad() {
  char result = 0;
  for (byte r = 0; r < ROWS && !result; r++) {
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], LOW);
    for (byte c = 0; c < COLS; c++) {
      pinMode(colPins[c], INPUT_PULLUP);
      if (digitalRead(colPins[c]) == LOW) {
        delay(15);
        if (digitalRead(colPins[c]) == LOW) {
          result = keymap[r][c];
          break;
        }
      }
    }
    pinMode(rowPins[r], INPUT);
  }
  for (byte r = 0; r < ROWS; r++) pinMode(rowPins[r], INPUT);
  for (byte c = 0; c < COLS; c++) pinMode(colPins[c], INPUT);
  return result;
}

// ---------------- Состояние ОС ----------------
enum Screen : uint8_t {
  SCR_BOOT, SCR_DESKTOP, SCR_MENU, SCR_APP_NOTES,
  SCR_APP_CALC, SCR_APP_PAINT, SCR_APP_TIMER,
  SCR_APP_COUNTER, SCR_APP_INFO, SCR_SLEEP
};

Screen  currentScreen = SCR_BOOT;
bool    screenDirty   = true;
bool    soundEnabled  = true;

const char* const menuItems[] = {
  "Notes", "Calculator", "Paint", "Timer", "Counter", "System Info", "Sleep"
};
const uint8_t MENU_COUNT = 7;
uint8_t menuIndex = 0;

// =====================================================
//             NOTES
// =====================================================
#define NOTES_MAX EEPROM_NOTES_MAX
char    notesText[NOTES_MAX + 1];
uint8_t notesLen = 0;

bool    notesDirty = false;
unsigned long notesLastEdit = 0;
#define NOTES_SAVE_DELAY 2000

// =====================================================
//             COUNTER
// =====================================================
unsigned long counterScore = 0;
bool          counterNeedSave = false;
unsigned long counterLastChange = 0;
#define COUNTER_SAVE_DELAY 2000

// Для долгого удержания `0` = сброс
unsigned long counterKey0Start = 0;
bool          counterKey0Reset = false;
#define COUNTER_LONG_PRESS_MS 1500

// =====================================================
//             EEPROM функции
// =====================================================
void eepromSaveNotes() {
  EEPROM.update(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VALUE);
  for (uint8_t i = 0; i < notesLen; i++) {
    EEPROM.update(EEPROM_NOTES_ADDR + i, notesText[i]);
  }
  if (notesLen < EEPROM_NOTES_MAX) {
    EEPROM.update(EEPROM_NOTES_ADDR + notesLen, 0);
  }
}

void eepromLoadNotes() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VALUE) {
    notesLen = 0;
    notesText[0] = 0;
    return;
  }
  notesLen = 0;
  for (uint8_t i = 0; i < EEPROM_NOTES_MAX; i++) {
    char c = EEPROM.read(EEPROM_NOTES_ADDR + i);
    if (c == 0) break;
    notesText[notesLen++] = c;
  }
  notesText[notesLen] = 0;
}

void eepromWipeNotes() {
  EEPROM.update(EEPROM_MAGIC_ADDR, 0);
  notesLen = 0;
  notesText[0] = 0;
}

// Counter
void eepromSaveCounter() {
  uint32_t magic = EEPROM_COUNTER_MAGIC;
  EEPROM.put(EEPROM_COUNTER_ADDR, magic);
  EEPROM.put(EEPROM_COUNTER_ADDR + 4, counterScore);
  counterNeedSave = false;
}

void eepromLoadCounter() {
  uint32_t magic;
  EEPROM.get(EEPROM_COUNTER_ADDR, magic);
  if (magic == EEPROM_COUNTER_MAGIC) {
    EEPROM.get(EEPROM_COUNTER_ADDR + 4, counterScore);
  } else {
    counterScore = 0;
  }
}

void eepromWipeCounter() {
  uint32_t zero = 0;
  EEPROM.put(EEPROM_COUNTER_ADDR, zero);
  counterScore = 0;
  counterNeedSave = false;
}

// ---------------- Calculator ----------------
char    calcBuffer[12];
uint8_t calcLen = 0;
long    calcA = 0;
char    calcOp = 0;
bool    calcNewEntry = true;

// ---------------- Paint ----------------
uint8_t paintCanvas[8][16];
uint8_t paintX = 64, paintY = 32;

// ---------------- Timer ----------------
unsigned long timerStart  = 0;
unsigned long timerTarget = 30000;
bool timerRunning = false;
bool timerDone    = false;

// =====================================================
//                      ЗВУК
// =====================================================
inline void beep(uint16_t freq, uint16_t dur) {
  if (!soundEnabled) return;
  tone(BUZZER, freq);
  delay(dur);
  noTone(BUZZER);
}
inline void clickSound() { beep(1200, 8);  }
inline void okSound()    { beep(1800, 50); }
inline void errorSound() { beep(300,  100); }
void bootSound()  { beep(800, 60); beep(1100, 60); beep(1500, 120); }
void sleepSound() { beep(1500, 80); beep(1000, 80); beep(700, 150); }

// =====================================================
//                      BOOT
// =====================================================
void drawBoot() {
  display.clearDisplay();
  display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 8);
  display.print(F("MicroOS"));
  display.setTextSize(1);
  display.setCursor(28, 30);
  display.print(F("v1.8 final"));
  display.drawRect(20, 46, 88, 8, SSD1306_WHITE);
  display.display();

  for (uint8_t i = 0; i <= 84; i += 6) {
    display.fillRect(22, 48, i, 4, SSD1306_WHITE);
    display.display();
    delay(60);
  }
}

// =====================================================
//                    TOP BAR
// =====================================================
void drawTopBar(const __FlashStringHelper* title, const __FlashStringHelper* hint) {
  display.fillRect(0, 0, 128, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 1);
  display.print(title);
  if (hint) {
    uint8_t hintLen = strlen_P((PGM_P)hint);
    display.setCursor(128 - hintLen * 6 - 2, 1);
    display.print(hint);
  }
  display.setTextColor(SSD1306_WHITE);
}

// =====================================================
//                    DESKTOP
// =====================================================
void drawDesktop() {
  display.clearDisplay();
  drawTopBar(F("MicroOS"), F("M=menu"));

  const char icons[] = {'N','C','P','T','K','i','S'};   // K = Counter
  for (uint8_t i = 0; i < 7; i++) {
    uint8_t cx = 8 + (i % 4) * 30;
    uint8_t cy = 18 + (i / 4) * 24;
    display.drawRect(cx, cy, 26, 18, SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(cx + 7, cy + 1);
    display.print(icons[i]);
  }
  display.setTextSize(1);
  display.setCursor(2, 56);
  display.print(F("1-4 apps  M=menu"));
  display.display();
}

// =====================================================
//                      MENU
// =====================================================
void drawMenu() {
  display.clearDisplay();
  drawTopBar(F("MENU"), F("B=exit"));

  uint8_t start = (menuIndex >= 4) ? menuIndex - 3 : 0;

  for (uint8_t i = 0; i < 4 && (start + i) < MENU_COUNT; i++) {
    uint8_t idx = start + i;
    uint8_t y = 14 + i * 12;
    bool sel = (idx == menuIndex);

    if (sel) {
      display.fillRect(0, y - 1, 128, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, y + 1);
    display.print(idx + 1);
    display.print(F(". "));
    display.print(menuItems[idx]);
  }
  display.display();
}

// =====================================================
//                      NOTES
// =====================================================
void drawNotes() {
  display.clearDisplay();

  if (notesDirty) {
    drawTopBar(F("NOTES*"), F("B=back"));
  } else {
    drawTopBar(F("NOTES"), F("B=back"));
  }

  const uint8_t charsPerRow = 21;
  const uint8_t rowsVisible = 5;
  const uint8_t maxChars = charsPerRow * rowsVisible;

  uint8_t start = (notesLen > maxChars) ? notesLen - maxChars : 0;

  display.setCursor(2, 14);
  uint8_t row = 0;
  for (uint8_t i = start; i < notesLen; i++) {
    char c = notesText[i];
    if (c == '\n' || (i - start) % charsPerRow == 0) {
      if (i != start) {
        row++;
        display.setCursor(2, 14 + row * 8);
        if (row >= rowsVisible) break;
      }
    }
    if (c != '\n') display.print(c);
  }

  uint8_t pos = (notesLen - start) % charsPerRow;
  uint8_t cr  = (notesLen - start) / charsPerRow;
  if (cr < rowsVisible) {
    display.setCursor(2 + pos * 6, 14 + cr * 8);
    display.print('_');
  }

  display.setCursor(2, 56);
  display.print(F("A=save * x2=wipe"));
  display.display();
}

// =====================================================
//                    CALCULATOR
// =====================================================
void drawCalc() {
  display.clearDisplay();
  drawTopBar(F("CALCULATOR"), F("B=bk"));

  display.drawRect(0, 14, 128, 18, SSD1306_WHITE);
  display.setTextSize(2);
  uint8_t start = (calcLen > 10) ? calcLen - 10 : 0;
  display.setCursor(4, 17);
  for (uint8_t i = start; i < calcLen; i++) display.print(calcBuffer[i]);

  display.setTextSize(1);
  display.setCursor(2, 40); display.print(F("0-9  + - * /"));
  display.setCursor(2, 50); display.print(F("#=eq  C=clr  B=bk"));
  display.display();
}

void calcReset() {
  calcLen = 0; calcBuffer[0] = 0;
  calcA = 0; calcOp = 0; calcNewEntry = true;
}

void calcPress(char k) {
  if (k == 'C') { calcReset(); clickSound(); return; }

  if (k == '#') {
    if (calcOp) {
      long b = atol(calcBuffer);
      long r = 0;
      switch (calcOp) {
        case '+': r = calcA + b; break;
        case '-': r = calcA - b; break;
        case '*': r = calcA * b; break;
        case '/': r = (b != 0) ? calcA / b : 0; break;
      }
      char tmp[12];
      int n = 0;
      bool neg = (r < 0);
      unsigned long v = neg ? (unsigned long)(-r) : (unsigned long)r;
      if (v == 0) tmp[n++] = '0';
      while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
      if (neg) tmp[n++] = '-';
      calcLen = 0;
      for (int i = n - 1; i >= 0; i--) calcBuffer[calcLen++] = tmp[i];
      calcBuffer[calcLen] = 0;
      calcOp = 0; calcNewEntry = true;
      okSound();
    }
    return;
  }

  if (k == '+' || k == '-' || k == '*' || k == '/') {
    calcA = atol(calcBuffer); calcOp = k; calcNewEntry = true;
    clickSound(); return;
  }

  if (k >= '0' && k <= '9') {
    if (calcNewEntry) { calcLen = 0; calcNewEntry = false; }
    if (calcLen < 8) { calcBuffer[calcLen++] = k; calcBuffer[calcLen] = 0; }
    clickSound();
  }
}

// =====================================================
//                      PAINT
// =====================================================
void drawPaint() {
  display.clearDisplay();
  for (uint8_t y = 0; y < 64; y++) {
    uint8_t page = y >> 3;
    for (uint8_t xb = 0; xb < 16; xb++) {
      uint8_t bits = paintCanvas[page][xb];
      if (!bits) continue;
      for (uint8_t b = 0; b < 8; b++) {
        if (bits & (1 << b)) display.drawPixel(xb * 8 + b, y, SSD1306_WHITE);
      }
    }
  }
  display.drawRect(paintX - 1, paintY - 1, 3, 3, SSD1306_WHITE);
  display.fillRect(0, 0, 128, 8, SSD1306_BLACK);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 0);
  display.print(F("PAINT  2468=move 5=dr"));
  display.display();
}

void paintSet(int x, int y) {
  if ((unsigned)x > 127 || (unsigned)y > 63) return;
  paintCanvas[y >> 3][x >> 3] |= (1 << (x & 7));
}

void paintClear() {
  for (uint8_t i = 0; i < 8; i++)
    for (uint8_t j = 0; j < 16; j++)
      paintCanvas[i][j] = 0;
}

void paintMove(int dx, int dy) {
  paintX = constrain((int)paintX + dx, 0, 127);
  paintY = constrain((int)paintY + dy, 0, 63);
}

// =====================================================
//                      TIMER
// =====================================================
void drawTimer() {
  display.clearDisplay();
  drawTopBar(F("TIMER"), F("B=back"));

  long remainMs;
  if (timerRunning) {
    unsigned long elapsed = millis() - timerStart;
    if (elapsed >= timerTarget) {
      remainMs = 0;
      timerRunning = false;
      timerDone = true;
    } else {
      remainMs = (long)timerTarget - (long)elapsed;
    }
  } else {
    remainMs = timerDone ? 0 : (long)timerTarget;
  }

  int sec = remainMs / 1000;
  char buf[6];
  buf[0] = '0' + (sec / 60) / 10;
  buf[1] = '0' + (sec / 60) % 10;
  buf[2] = ':';
  buf[3] = '0' + (sec % 60) / 10;
  buf[4] = '0' + (sec % 60) % 10;
  buf[5] = 0;

  display.setTextSize(3);
  display.setCursor(28, 24);
  display.print(buf);

  display.setTextSize(1);
  display.setCursor(2, 54);
  if (timerDone)         display.print(F("DONE! 5=reset"));
  else if (timerRunning) display.print(F("Running... 0=stop"));
  else                   display.print(F("0=start 2=10s 3=30s"));
  display.display();
}

// =====================================================
//                     COUNTER
// =====================================================
unsigned long counterIncrement(unsigned long s) {
  if (s < 10UL)          return 1UL;
  if (s < 100UL)         return 5UL;
  if (s < 500UL)         return 10UL;
  if (s < 5000UL)        return 50UL;
  if (s < 10000UL)       return 100UL;
  if (s < 50000UL)       return 500UL;
  if (s < 100000UL)      return 1000UL;
  if (s < 500000UL)      return 5000UL;
  if (s < 1000000UL)     return 10000UL;
  return 50000UL;
}

void counterLevelSound() {
  beep(1500, 70);
  beep(2000, 90);
}

void drawCounter() {
  display.clearDisplay();
  drawTopBar(F("COUNTER"), F("B=back"));

  display.setTextSize(1);
  display.setCursor(2, 14);
  display.print(F("SCORE:"));

  display.setTextSize(3);
  display.setCursor(2, 24);
  display.print(counterScore);

  display.setTextSize(1);
  display.setCursor(2, 54);
  display.print(F("+"));
  display.print(counterIncrement(counterScore));
  display.print(F("/5  0-hold=reset"));

  if (counterNeedSave) {
    display.setCursor(120, 14);
    display.print(F("*"));
  }
  display.display();
}

void counterReset() {
  counterScore = 0;
  counterNeedSave = true;
  counterLastChange = millis();
}

// =====================================================
//                    SYSTEM INFO
// =====================================================
void drawInfo() {
  display.clearDisplay();
  drawTopBar(F("SYSTEM INFO"), F("B=bk"));
  display.setCursor(2, 14); display.print(F("Chip  : ATmega328P"));
  display.setCursor(2, 24); display.print(F("Clock : 16 MHz"));
  display.setCursor(2, 34); display.print(F("SRAM  : 2048 B"));
  display.setCursor(2, 44); display.print(F("Flash : 32 KB"));
  display.setCursor(2, 54); display.print(F("Uptime: "));
  display.print(millis() / 1000); display.print('s');
  display.display();
}

// =====================================================
//                       SLEEP
// =====================================================
void drawSleep() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(20, 20);
  display.print(F("Zzz..."));
  display.setTextSize(1);
  display.setCursor(10, 50);
  display.print(F("Press any key"));
  display.display();
}

// =====================================================
//                  ОБРАБОТКА КЛАВИШ
// =====================================================
void gotoMenu() { currentScreen = SCR_MENU; screenDirty = true; }

void handleKey(char k) {
  screenDirty = true;
  switch (currentScreen) {
    case SCR_DESKTOP:
      if      (k == 'M' || k == 'A') { gotoMenu(); menuIndex = 0; clickSound(); }
      else if (k == '*')             { currentScreen = SCR_APP_INFO; clickSound(); }
      else if (k == '1')             { currentScreen = SCR_APP_NOTES; clickSound(); }
      else if (k == '2')             { calcReset(); currentScreen = SCR_APP_CALC; clickSound(); }
      else if (k == '3')             { paintClear(); currentScreen = SCR_APP_PAINT; clickSound(); }
      else if (k == '4')             { currentScreen = SCR_APP_TIMER; clickSound(); }
      else                            errorSound();
      break;

    case SCR_MENU:
      if      (k == '2' || k == 'A') { menuIndex = (menuIndex + MENU_COUNT - 1) % MENU_COUNT; clickSound(); }
      else if (k == '8' || k == 'D') { menuIndex = (menuIndex + 1) % MENU_COUNT; clickSound(); }
      else if (k == '5') {
        okSound();
        switch (menuIndex) {
          case 0: currentScreen = SCR_APP_NOTES; break;
          case 1: calcReset(); currentScreen = SCR_APP_CALC; break;
          case 2: paintClear(); currentScreen = SCR_APP_PAINT; break;
          case 3: currentScreen = SCR_APP_TIMER; break;
          case 4: currentScreen = SCR_APP_COUNTER; break;   // ← Counter
          case 5: currentScreen = SCR_APP_INFO; break;
          case 6: currentScreen = SCR_SLEEP; sleepSound(); break;
        }
      }
      else if (k == 'B' || k == '*') { currentScreen = SCR_DESKTOP; clickSound(); }
      else errorSound();
      break;

    case SCR_APP_NOTES:
      if (k == 'A') {
        eepromSaveNotes();
        notesDirty = false;
        okSound();
      }
      else if (k == '*') {
        if (lastKey == '*') {
          eepromWipeNotes();
          notesDirty = false;
          okSound();
        } else {
          if (notesDirty) eepromSaveNotes();
          notesDirty = false;
          currentScreen = SCR_MENU;
          clickSound();
        }
      }
      else if (k == 'B' || k == 'C') {
        if (notesDirty) eepromSaveNotes();
        notesDirty = false;
        currentScreen = SCR_MENU;
        clickSound();
      }
      else if (k == 'D') {
        if (notesLen) {
          notesText[--notesLen] = 0;
          notesDirty = true;
          notesLastEdit = millis();
        }
        clickSound();
      }
      else if (k == '#') {
        if (notesLen < NOTES_MAX) {
          notesText[notesLen++] = '\n';
          notesText[notesLen] = 0;
          notesDirty = true;
          notesLastEdit = millis();
        }
        clickSound();
      }
      else if (notesLen < NOTES_MAX) {
        notesText[notesLen++] = k;
        notesText[notesLen] = 0;
        notesDirty = true;
        notesLastEdit = millis();
        clickSound();
      }
      break;

    case SCR_APP_CALC:
      if (k == 'B' || k == '*') { currentScreen = SCR_MENU; clickSound(); }
      else calcPress(k);
      break;

    case SCR_APP_PAINT:
      if      (k == 'B' || k == '*') { currentScreen = SCR_MENU; clickSound(); }
      else if (k == '2') { paintMove(0, -2); clickSound(); }
      else if (k == '8') { paintMove(0,  2); clickSound(); }
      else if (k == '4') { paintMove(-2, 0); clickSound(); }
      else if (k == '6') { paintMove( 2, 0); clickSound(); }
      else if (k == '5') { paintSet(paintX, paintY); clickSound(); }
      else if (k == '0') { paintClear(); okSound(); }
      else if (k == 'A') {
        paintSet(paintX, paintY);
        paintSet(paintX + 1, paintY);
        paintSet(paintX, paintY + 1);
        paintSet(paintX + 1, paintY + 1);
        clickSound();
      }
      else errorSound();
      break;

    case SCR_APP_TIMER:
      if (k == 'B' || k == '*') {
        timerRunning = false;
        currentScreen = SCR_MENU;
      }
      else if (k == '0') {
        if (!timerRunning) {
          timerStart = millis();
          timerRunning = true;
          timerDone = false;
        } else {
          timerRunning = false;
        }
      }
      else if (k == '1') { timerTarget += 10000; timerRunning = false; timerDone = false; }
      else if (k == '2') { timerTarget = 10000;  timerRunning = false; timerDone = false; }
      else if (k == '3') { timerTarget = 30000;  timerRunning = false; timerDone = false; }
      else if (k == '4') { timerTarget = 60000;  timerRunning = false; timerDone = false; }
      else if (k == '5') { timerTarget = 300000; timerRunning = false; timerDone = false; }
      break;

    // ★ COUNTER
    case SCR_APP_COUNTER:
      if (k == 'B' || k == '*') {
        if (counterNeedSave) eepromSaveCounter();
        currentScreen = SCR_MENU;
        clickSound();
      }
      else if (k == '5') {
        unsigned long before = counterIncrement(counterScore);
        counterScore += before;
        counterNeedSave = true;
        counterLastChange = millis();

        unsigned long after = counterIncrement(counterScore);
        if (after != before) {
          counterLevelSound();     // порог пройден
        } else {
          clickSound();            // обычный клик
        }
      }
      else if (k == '0') {
        counterKey0Start = millis();
        counterKey0Reset = false;
        // ждём в loop — если держат > 1.5 сек, сбросим
      }
      break;

    case SCR_APP_INFO:
      if (k) { currentScreen = SCR_MENU; clickSound(); }
      break;

    case SCR_SLEEP:
      currentScreen = SCR_DESKTOP;
      okSound();
      break;

    default: break;
  }
}

// Обработка отпускания клавиши `0` в Counter
void handleKeyRelease(char prevK) {
  if (currentScreen == SCR_APP_COUNTER && prevK == '0') {
    if (counterKey0Reset) {
      // уже сбросили, ничего
    } else {
      // короткое нажатие — ничего не делаем (можно добавить действие)
      clickSound();
    }
    counterKey0Start = 0;
    counterKey0Reset = false;
  }
}

// =====================================================
//                       SETUP
// =====================================================
void setup() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    pinMode(LED_BUILTIN, OUTPUT);
    while (1) {
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      delay(500);
    }
  }

  pinMode(BUZZER, OUTPUT);
  Wire.setClock(400000);

  eepromLoadNotes();
  notesDirty = false;

  eepromLoadCounter();
  counterNeedSave = false;

  display.clearDisplay();
  display.display();

  drawBoot();
  bootSound();
  delay(200);

  paintClear();
  currentScreen = SCR_DESKTOP;
  screenDirty = true;
}

// =====================================================
//                        LOOP
// =====================================================
void loop() {
  // Автосохранение Notes
  if (notesDirty && millis() - notesLastEdit > NOTES_SAVE_DELAY) {
    eepromSaveNotes();
    notesDirty = false;
  }

  // Автосохранение Counter
  if (counterNeedSave && millis() - counterLastChange > COUNTER_SAVE_DELAY) {
    eepromSaveCounter();
    counterNeedSave = false;
  }

  // ★ Долгое удержание `0` в Counter → сброс
  if (currentScreen == SCR_APP_COUNTER && counterKey0Start > 0 && !counterKey0Reset) {
    if (millis() - counterKey0Start > COUNTER_LONG_PRESS_MS) {
      counterReset();
      counterKey0Reset = true;
      counterKey0Start = 0;
      okSound();
      screenDirty = true;
    }
  }

  char k = scanKeypad();

  // Отпускание клавиши — обработка
  if (!k && lastKey) {
    handleKeyRelease(lastKey);
    lastKey = 0;
  }

  // Новое нажатие
  if (k && k != lastKey) {
    handleKey(k);
    lastKey = k;
  }

  bool needRedraw = screenDirty;

  if (currentScreen == SCR_APP_TIMER && timerRunning) {
    static unsigned long lastTimerDraw = 0;
    if (millis() - lastTimerDraw > 500) {
      needRedraw = true;
      lastTimerDraw = millis();
    }
  }
  if (currentScreen == SCR_APP_INFO) needRedraw = true;

  if (needRedraw) {
    screenDirty = false;
    switch (currentScreen) {
      case SCR_DESKTOP:     drawDesktop(); break;
      case SCR_MENU:        drawMenu();    break;
      case SCR_APP_NOTES:   drawNotes();   break;
      case SCR_APP_CALC:    drawCalc();    break;
      case SCR_APP_PAINT:   drawPaint();   break;
      case SCR_APP_TIMER:   drawTimer();   break;
      case SCR_APP_COUNTER: drawCounter(); break;
      case SCR_APP_INFO:    drawInfo();    break;
      case SCR_SLEEP:       drawSleep();   break;
      default: break;
    }
  }
}
