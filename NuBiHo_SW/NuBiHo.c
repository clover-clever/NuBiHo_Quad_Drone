#include <Arduino.h>
#include <NeoSWSerial.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>

#define F_CPU       16000000UL
#define UART_BAUD   115200UL
#define PRESC_T2    32UL
#define RX_BUF_SZ   64

#define PPM_OUT_PIN 10
#define CHANNELS    5
#define FRAME_US    22500
#define PULSE_US    300
#define PIEZO_PIN   3
#define GPI_PIN     PD6

NeoSWSerial mySerial(A2, A3);

int rollPWM     = 1500;
int pitchPWM    = 1500;
int throttlePWM = 1000;
int yawPWM      = 1500;
int auxPWM      = 1000;

volatile uint16_t ppmValues[CHANNELS] = {1500, 1500, 1000, 1500, 1000};
volatile uint8_t  currentChannel      = 0;
volatile bool     isPulse             = false;
volatile uint16_t totalUs             = 0;

int pwmStep     = 5;
bool armState   = false;
bool lastBtnEState = false;

volatile uint8_t  gpiReq  = 0;
volatile uint8_t  uartReq = 0;
volatile uint8_t  alarmOn = 0;
volatile uint16_t freq    = 2000;
volatile int16_t  step    = 25;

uint8_t calcOCR2A(uint16_t f) {
  uint32_t t = 250000UL / f;
  return (t ? t - 1 : 0);
}

void startSiren() {
  freq = 2000;
  step = 25;
  uint8_t top = calcOCR2A(freq);
  OCR2A = top;
  OCR2B = (top + 1) >> 1;
  TCCR2A |= _BV(COM2B1);
  alarmOn = 1;
}

void stopSiren() {
  TCCR2A &= ~_BV(COM2B1);
  alarmOn = 0;
}

ISR(WDT_vect) {
  if (!alarmOn) return;
  freq += step;
  if (freq >= 3400 || freq <= 2000) step = -step;
  uint8_t top = calcOCR2A(freq);
  uint8_t cmp = (top + 1) >> 1;
  uint8_t s = SREG; cli();
  OCR2B = cmp;
  OCR2A = top;
  SREG = s;
}

void setupPWM_Timer2() {
  DDRD |= _BV(PIEZO_PIN);
  uint8_t top = calcOCR2A(freq);
  OCR2A = top; OCR2B = (top + 1) >> 1;
  TCCR2A = _BV(WGM21) | _BV(WGM20);
  TCCR2B = _BV(WGM22) | _BV(CS22) | _BV(CS20);
}

void setupWDT() {
  cli();
  MCUSR &= ~_BV(WDRF);
  WDTCSR = _BV(WDCE) | _BV(WDE);
  WDTCSR = _BV(WDIE) | _BV(WDP0);
  sei();
}

void setupPPMTimer() {
  cli();
  TCCR1A = 0;
  TCCR1B = (1 << WGM12) | (1 << CS11);
  OCR1A = (F_CPU / 8 / 1000000) * PULSE_US;
  TIMSK1 = (1 << OCIE1A);
  sei();
}

ISR(TIMER1_COMPA_vect) {
  if (isPulse) {
    digitalWrite(PPM_OUT_PIN, HIGH);
    isPulse = false;
    if (currentChannel < CHANNELS) {
      uint16_t dur = ppmValues[currentChannel] - PULSE_US;
      OCR1A = (F_CPU / 8 / 1000000) * dur;
      totalUs += ppmValues[currentChannel];
      currentChannel++;
    } else {
      uint16_t syncUs = FRAME_US - totalUs;
      OCR1A = (F_CPU / 8 / 1000000) * (syncUs - PULSE_US);
      totalUs = 0;
      currentChannel = 0;
    }
  } else {
    digitalWrite(PPM_OUT_PIN, LOW);
    OCR1A = (F_CPU / 8 / 1000000) * PULSE_US;
    isPulse = true;
  }
}

void setup() {
  Serial.begin(9600);
  mySerial.begin(9600);
  pinMode(A1, OUTPUT);
  digitalWrite(A1, HIGH);

  pinMode(PPM_OUT_PIN, OUTPUT);
  digitalWrite(PPM_OUT_PIN, HIGH);

  pinMode(GPI_PIN, INPUT);

  setupPPMTimer();
  setupPWM_Timer2();
  setupWDT();
  sei();

  Serial.println("[PPM + Siren] Ready");
}

void loop() {
  gpiReq = digitalRead(GPI_PIN);

  if (mySerial.available()) {
    String line = mySerial.readStringUntil('\n');
    line.trim();
    int arrow = line.indexOf(">>");
    if (arrow != -1) {
      line = line.substring(arrow + 2);
      line.trim();
    }
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    if (c1 > 0 && c2 > c1) {
      int rawX = line.substring(0, c1).toInt();
      int rawY = line.substring(c1 + 1, c2).toInt();
      String btn = line.substring(c2 + 1);
      btn.trim();
      if (btn.indexOf('A') != -1) throttlePWM += pwmStep;
      if (btn.indexOf('C') != -1) throttlePWM -= pwmStep;
      if (btn.indexOf('B') != -1) yawPWM = 1550;
      else if (btn.indexOf('D') != -1) yawPWM = 1450;
      else yawPWM = 1500;
      bool curE = btn.indexOf('E') != -1;
      if (curE && !lastBtnEState) {
        armState = !armState;
        Serial.print("[ARM TOGGLE] -> ");
        Serial.println(armState ? "ON" : "OFF");
      }
      lastBtnEState = curE;
      if (rawX < 333) rollPWM = map(rawX, 0, 333, 1000, 1500);
      else            rollPWM = map(rawX, 333, 675, 1500, 2000);
      if (rawY < 333) pitchPWM = map(rawY, 0, 333, 1000, 1500);
      else            pitchPWM = map(rawY, 333, 675, 1500, 2000);
      rollPWM     = constrain(rollPWM,     1400, 1600);
      pitchPWM    = constrain(pitchPWM,    1400, 1600);
      throttlePWM = constrain(throttlePWM, 1000, 2000);
      yawPWM      = constrain(yawPWM,      1400, 1600);
      auxPWM      = armState ? 1600 : 1000;
      noInterrupts();
      ppmValues[0] = rollPWM;
      ppmValues[1] = pitchPWM;
      ppmValues[2] = throttlePWM;
      ppmValues[3] = yawPWM;
      ppmValues[4] = auxPWM;
      interrupts();
      Serial.print("ROLL: "); Serial.print(rollPWM);
      Serial.print("  PITCH: "); Serial.print(pitchPWM);
      Serial.print("  THR: "); Serial.print(throttlePWM);
      Serial.print("  YAW: "); Serial.print(yawPWM);
      Serial.print("  AUX: "); Serial.println(auxPWM);
    }
  }

  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case '1': uartReq = 1; break;
      case '0': uartReq = 0; break;
      case 't': uartReq ^= 1; break;
      case 'h': case '?':
        Serial.println("1=ON 0=OFF t=toggle h=help");
        break;
    }
  }

  uint8_t need = (gpiReq | uartReq);
  if (need && !alarmOn) startSiren();
  else if (!need && alarmOn) stopSiren();
}