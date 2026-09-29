#include <Arduino.h>
#include <cstring>

// Zumo GP29 RX <- XIAO D2/GPIO3 TX; GP28 TX -> D3/GPIO4 RX.
HardwareSerial zumoUart(1);
char line[64];
size_t used = 0;
bool dropping = false;
uint32_t sequence = 0, lastPing = 0, replies = 0;
bool awaiting = false;

void receiveLine() {
  line[used] = '\0';
  Serial.printf("RX: %s\n", line);
  if (strncmp(line, "PING Z ", 7) == 0) {
    zumoUart.printf("PONG Z %s\n", line + 7);
    Serial.printf("TX: PONG Z %s\n", line + 7);
  } else {
    char expected[32];
    snprintf(expected, sizeof(expected), "PONG X %lu", static_cast<unsigned long>(sequence));
    if (awaiting && strcmp(line, expected) == 0) {
      awaiting = false;
      Serial.printf("PASS: XIAO round trip; replies=%lu\n", static_cast<unsigned long>(++replies));
    }
  }
}

void setup() {
  Serial.begin(115200);
  zumoUart.begin(115200, SERIAL_8N1, 4, 3); // RX, TX
  delay(1500);
  Serial.println("UART TEST: D2/GPIO3 TX, D3/GPIO4 RX, 115200 8N1");
  Serial.println("Temporary test firmware: run uart_ping_pong_test.py on Zumo.");
}

void loop() {
  while (zumoUart.available()) {
    char c = static_cast<char>(zumoUart.read());
    if (c == '\r') continue;
    if (c == '\n') {
      if (!dropping && used) receiveLine();
      used = 0;
      dropping = false;
    } else if (!dropping) {
      if (used < sizeof(line) - 1) line[used++] = c;
      else { dropping = true; Serial.println("RX: overlong line discarded"); }
    }
  }
  if (millis() - lastPing >= 2000) {
    if (awaiting) Serial.println("WAIT: no matching Zumo reply yet");
    lastPing = millis();
    ++sequence;
    awaiting = true;
    zumoUart.printf("PING X %lu\n", static_cast<unsigned long>(sequence));
    Serial.printf("TX: PING X %lu\n", static_cast<unsigned long>(sequence));
  }
  delay(1);
}
