/*
  SIMA7670C 4G Module Smart Diagnostic & Passthrough Tool
  --------------------------------------------------------
  Tests AT communication at 115200 and 9600 baud.
  Smart-toggles PWRKEY if module is off.
  
  Wiring:
    - ESP32 GPIO18 (RX2) <--> SIM7670 TXD
    - ESP32 GPIO19 (TX2) <--> SIM7670 RXD
    - ESP32 GPIO5        <--> SIM7670 PWRKEY
    - ESP32 GND          <--> SIM7670 GND
    - External 5V / 2A   <--> SIM7670 VCC
*/

#include <Arduino.h>

#define SIM_RX_PIN     18    // ESP32 RX2 <- SIM7670 TXD
#define SIM_TX_PIN     19    // ESP32 TX2 -> SIM7670 RXD
#define SIM_PWRKEY_PIN 5     // PWRKEY
HardwareSerial SerialSIM(2); // UART2

void pulsePWRKEY() {
    Serial.println("[POWER] Pulsing PWRKEY (GPIO5 LOW for 2s)...");
    pinMode(SIM_PWRKEY_PIN, OUTPUT);
    digitalWrite(SIM_PWRKEY_PIN, LOW);
    delay(2000);
    digitalWrite(SIM_PWRKEY_PIN, HIGH);
    pinMode(SIM_PWRKEY_PIN, INPUT_PULLUP);
    Serial.println("[POWER] Waiting 5 seconds for SIMA7670C to boot...");
    delay(5000);
}

bool testAT(uint32_t baud) {
    SerialSIM.begin(baud, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
    delay(200);
    
    // Clear buffer
    while (SerialSIM.available()) SerialSIM.read();
    
    // Send AT multiple times to help auto-baud lock on
    for (int i = 0; i < 5; i++) {
        SerialSIM.println("AT");
        delay(300);
        if (SerialSIM.available()) {
            String res = SerialSIM.readString();
            if (res.indexOf("OK") != -1) {
                Serial.printf("[SUCCESS] SIMA7670C responded OK at %lu baud!\n", baud);
                return true;
            }
        }
    }
    return false;
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n==================================================");
    Serial.println("   SIMA7670C 4G Smart Diagnostic & Tester        ");
    Serial.println("==================================================");

    // 1. Try 115200 first (default)
    Serial.println("\n[1] Checking if module is awake at 115200 baud...");
    if (!testAT(115200)) {
        // 2. Try 9600
        Serial.println("[2] Checking at 9600 baud...");
        if (!testAT(9600)) {
            // 3. Not responding -> Power ON module
            Serial.println("[3] Module not responding. Toggling PWRKEY...");
            pulsePWRKEY();

            // Retest 115200 and 9600 after power-on
            if (!testAT(115200) && !testAT(9600)) {
                Serial.println("\n❌ ERROR: No response from SIMA7670C.");
                Serial.println("Check: 1. Is 5V/2A power connected?");
                Serial.println("       2. Are TXD/RXD swapped? (Try swapping GPIO18/19)");
                Serial.println("       3. Is GND shared between ESP32 and SIM module?");
            }
        }
    }

    // Set baud rate to 115200
    SerialSIM.println("AT+IPR=115200");
    delay(200);
    SerialSIM.begin(115200, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);

    // Run diagnostic commands
    Serial.println("\n--- SIM & Network Diagnostics ---");
    
    SerialSIM.println("AT+CPIN?");
    delay(1000);
    while (SerialSIM.available()) Serial.write(SerialSIM.read());
    
    SerialSIM.println("AT+CSQ");
    delay(1000);
    while (SerialSIM.available()) Serial.write(SerialSIM.read());

    SerialSIM.println("AT+CEREG?");
    delay(1000);
    while (SerialSIM.available()) Serial.write(SerialSIM.read());

    SerialSIM.println("AT+COPS?");
    delay(2000);
    while (SerialSIM.available()) Serial.write(SerialSIM.read());

    Serial.println("\n==================================================");
    Serial.println("  PASSTHROUGH MODE READY");
    Serial.println("  Type 'AT' in the box above & press Enter to test");
    Serial.println("==================================================\n");
}

void loop() {
    // Forward typing from Serial Monitor to SIM Module
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input.length() > 0) {
            Serial.print(">> ");
            Serial.println(input);
            SerialSIM.println(input);
        }
    }

    // Print responses from SIM Module to Serial Monitor
    if (SerialSIM.available()) {
        while (SerialSIM.available()) {
            Serial.write(SerialSIM.read());
        }
    }
}
