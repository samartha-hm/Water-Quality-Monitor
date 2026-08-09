// ============================================================
// main.ino — Water Quality Monitor v2.0
// ============================================================
// ESP32 + SIMA7670C 4G/LTE + Mosquitto MQTT
// Reads: pH, TDS, Temperature, Turbidity, Dissolved Oxygen
// Publishes structured JSON to AWS MQTT broker over cellular
// ============================================================

#include <Arduino.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include "config.h"

// =============================================================
// 1. GLOBAL OBJECTS & RTC STATE
// =============================================================

// NVS Storage
Preferences preferences;

// RTC Memory — persists across software resets and deep sleep cycles
RTC_DATA_ATTR int consecutiveBootFailures = 0;

// UART to SIMA7670C (UART2 on GPIO18/19)
HardwareSerial SerialAT(2);

// DS18B20 Temperature
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

// --- Sensor Phase Multiplexing ---
// pH and TDS share analog ground; read them in alternating phases
// to prevent electrical interference
enum SensorPhase { PHASE_PH, PHASE_BUFFER, PHASE_TDS };
SensorPhase currentPhase = PHASE_PH;
unsigned long lastPhaseChange = 0;

// --- Sensor Values ---
struct SensorData {
    float ph           = 7.0;
    float tds          = 0.0;
    float temperature  = 25.0;
    float turbidity    = 0.0;
    float dissolvedO2  = 0.0;
};

SensorData current;
SensorData previous;  // Holds last valid values for hold-over during phase switching

// --- Median Filter Buffer ---
int analogBuffer[MEDIAN_SAMPLE_COUNT];

// --- Timing ---
unsigned long lastSensorRead    = 0;
unsigned long lastMqttPublish   = 0;
unsigned long lastStatusPublish = 0;
unsigned long deviceStartTime   = 0;

// --- MQTT State ---
bool mqttConnected    = false;
bool networkConnected = false;
int  signalQuality    = 0;
unsigned long reconnectDelay = MQTT_RECONNECT_BASE_MS;

// =============================================================
// 1b. RUNTIME-CONFIGURABLE PARAMETERS (NVS PERSISTENT)
// =============================================================
unsigned long cfgSensorReadInterval    = SENSOR_READ_INTERVAL;
unsigned long cfgMqttPublishInterval   = MQTT_PUBLISH_INTERVAL;
unsigned long cfgStatusPublishInterval = STATUS_PUBLISH_INTERVAL;
unsigned long cfgPhPhaseDuration       = PH_PHASE_DURATION;
unsigned long cfgBufferPhaseDuration   = BUFFER_PHASE_DURATION;
unsigned long cfgTdsPhaseDuration      = TDS_PHASE_DURATION;
bool          cfgDeepSleepEnabled      = false;
unsigned long cfgDeepSleepDurationSec  = 0;       // 0 = disabled
unsigned long cfgDashboardPollMs       = 3000;     // Sent to dashboard via status

void loadConfigFromNVS() {
    preferences.begin("wqm_cfg", true); // Read-only mode
    cfgSensorReadInterval    = preferences.getULong("read_int", SENSOR_READ_INTERVAL);
    cfgMqttPublishInterval   = preferences.getULong("pub_int", MQTT_PUBLISH_INTERVAL);
    cfgStatusPublishInterval = preferences.getULong("stat_int", STATUS_PUBLISH_INTERVAL);
    cfgPhPhaseDuration       = preferences.getULong("ph_dur", PH_PHASE_DURATION);
    cfgBufferPhaseDuration   = preferences.getULong("buf_dur", BUFFER_PHASE_DURATION);
    cfgTdsPhaseDuration      = preferences.getULong("tds_dur", TDS_PHASE_DURATION);
    cfgDeepSleepEnabled      = preferences.getBool("ds_en", false);
    cfgDeepSleepDurationSec  = preferences.getULong("ds_dur", 0);
    cfgDashboardPollMs       = preferences.getULong("dash_poll", 3000);
    preferences.end();

    DEBUG_SERIAL.printf("[NVS] Loaded config: read=%lu ms, pub=%lu ms, sleep=%s(%lus)\n",
        cfgSensorReadInterval, cfgMqttPublishInterval,
        cfgDeepSleepEnabled ? "ON" : "OFF", cfgDeepSleepDurationSec);
}

void saveConfigToNVS() {
    preferences.begin("wqm_cfg", false); // Read-write mode
    preferences.putULong("read_int", cfgSensorReadInterval);
    preferences.putULong("pub_int", cfgMqttPublishInterval);
    preferences.putULong("stat_int", cfgStatusPublishInterval);
    preferences.putULong("ph_dur", cfgPhPhaseDuration);
    preferences.putULong("buf_dur", cfgBufferPhaseDuration);
    preferences.putULong("tds_dur", cfgTdsPhaseDuration);
    preferences.putBool("ds_en", cfgDeepSleepEnabled);
    preferences.putULong("ds_dur", cfgDeepSleepDurationSec);
    preferences.putULong("dash_poll", cfgDashboardPollMs);
    preferences.end();

    DEBUG_SERIAL.println("[NVS] Saved configuration parameters to Flash.");
}

// =============================================================
// 2. AT COMMAND HELPERS
// =============================================================

// Send an AT command and wait for expected response
String sendAT(const char* cmd, const char* expected, unsigned long timeout = 3000) {
    // Flush any old data
    while (SerialAT.available()) SerialAT.read();

    if (ENABLE_AT_DEBUG) {
        DEBUG_SERIAL.print("[AT TX] ");
        DEBUG_SERIAL.println(cmd);
    }

    SerialAT.println(cmd);

    String response = "";
    unsigned long start = millis();

    while (millis() - start < timeout) {
        while (SerialAT.available()) {
            char c = SerialAT.read();
            response += c;
        }
        if (response.indexOf(expected) >= 0) {
            break;
        }
        delay(10);
    }

    if (ENABLE_AT_DEBUG) {
        DEBUG_SERIAL.print("[AT RX] ");
        DEBUG_SERIAL.println(response);
    }

    return response;
}

// Send AT command that expects raw data input after a ">" prompt
bool sendATWithData(const char* cmd, const String& data, unsigned long timeout = 5000) {
    while (SerialAT.available()) SerialAT.read();

    if (ENABLE_AT_DEBUG) {
        DEBUG_SERIAL.print("[AT TX] ");
        DEBUG_SERIAL.println(cmd);
    }

    SerialAT.println(cmd);

    // Wait for ">" prompt
    String response = "";
    unsigned long start = millis();
    bool gotPrompt = false;

    while (millis() - start < timeout) {
        while (SerialAT.available()) {
            char c = SerialAT.read();
            response += c;
        }
        if (response.indexOf(">") >= 0) {
            gotPrompt = true;
            break;
        }
        if (response.indexOf("ERROR") >= 0) {
            return false;
        }
        delay(10);
    }

    if (!gotPrompt) return false;

    // Send the data
    SerialAT.print(data);
    delay(100);

    // Wait for OK
    response = "";
    start = millis();
    while (millis() - start < timeout) {
        while (SerialAT.available()) {
            char c = SerialAT.read();
            response += c;
        }
        if (response.indexOf("OK") >= 0) return true;
        if (response.indexOf("ERROR") >= 0) return false;
        delay(10);
    }

    return false;
}

// =============================================================
// 3. SIMA7670C MODULE MANAGEMENT
// =============================================================

void powerOnModule() {
    if (SIM_PWRKEY_PIN >= 0) {
        DEBUG_SERIAL.println("[SIM] Toggling PWRKEY to power on SIMA7670C...");
        pinMode(SIM_PWRKEY_PIN, OUTPUT);
        digitalWrite(SIM_PWRKEY_PIN, LOW);
        delay(1500);
        digitalWrite(SIM_PWRKEY_PIN, HIGH);
        delay(5000);  // Wait for module boot
    } else {
        DEBUG_SERIAL.println("[SIM] Auto power-on mode. Waiting 6s for modem power stabilization...");
        delay(6000);  // 6s delay for SIMA7670C cold power-on
    }
    DEBUG_SERIAL.println("[SIM] Power-on check complete.");
}

bool initModule() {
    DEBUG_SERIAL.println("[SIM] Probing modem UART readiness...");

    // 1. Primary probe on standard pins (GPIO18=RX, GPIO19=TX) at 115200 baud
    SerialAT.end();
    SerialAT.begin(SIM_BAUD_RATE, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
    delay(200);

    bool connected = false;
    int activeRx = SIM_RX_PIN;
    int activeTx = SIM_TX_PIN;
    uint32_t activeBaud = SIM_BAUD_RATE;

    // Probe loop (gives modem up to 10 seconds to finish cold boot UART init)
    for (int probe = 0; probe < 15; probe++) {
        while (SerialAT.available()) SerialAT.read();
        SerialAT.println("AT");
        delay(350);
        String resp = "";
        while (SerialAT.available()) resp += (char)SerialAT.read();
        if (resp.indexOf("OK") >= 0) {
            connected = true;
            DEBUG_SERIAL.printf("[SIM] SUCCESS! Modem responsive at %d baud (RX: GPIO%d, TX: GPIO%d)\n",
                                  SIM_BAUD_RATE, SIM_RX_PIN, SIM_TX_PIN);
            break;
        }
        delay(300);
    }

    // 2. Fallback multi-baud and pin-swap scanner if primary probe didn't respond
    if (!connected) {
        DEBUG_SERIAL.println("[SIM] Primary probe silent. Starting multi-baud / pin-swap scanner...");
        uint32_t testBauds[] = {115200, 9600, 57600, 38400, 19200};
        int numBauds = sizeof(testBauds) / sizeof(testBauds[0]);
        int rxPins[] = {SIM_RX_PIN, SIM_TX_PIN};
        int txPins[] = {SIM_TX_PIN, SIM_RX_PIN};

        for (int p = 0; p < 2; p++) {
            for (int b = 0; b < numBauds; b++) {
                int currentRx = rxPins[p];
                int currentTx = txPins[p];
                uint32_t currentBaud = testBauds[b];

                SerialAT.end();
                SerialAT.begin(currentBaud, SERIAL_8N1, currentRx, currentTx);
                delay(150);

                while (SerialAT.available()) SerialAT.read();

                for (int i = 0; i < 3; i++) {
                    SerialAT.println("AT");
                    delay(300);
                    String resp = "";
                    while (SerialAT.available()) resp += (char)SerialAT.read();
                    if (resp.indexOf("OK") >= 0) {
                        connected = true;
                        activeRx = currentRx;
                        activeTx = currentTx;
                        activeBaud = currentBaud;
                        DEBUG_SERIAL.printf("[SIM] SUCCESS! Module found at %lu baud (RX: GPIO%d, TX: GPIO%d)\n",
                                              activeBaud, activeRx, activeTx);
                        break;
                    }
                }
                if (connected) break;
            }
            if (connected) break;
        }
    }

    if (!connected) {
        DEBUG_SERIAL.println("[SIM] ERROR: No response on any baud rate or pin orientation!");
        DEBUG_SERIAL.println("[SIM] CHECK: 1. Is the PWR/NET LED on the SIM7670C glowing/blinking?");
        DEBUG_SERIAL.println("[SIM]        2. Is external 5V/2A power connected?");
        DEBUG_SERIAL.println("[SIM]        3. Is GND connected between ESP32 and SIM module?");
        return false;
    }

    // Set baud rate to 115200 if it was at a different baud
    if (activeBaud != 115200) {
        sendAT("AT+IPR=115200", "OK", 1000);
        SerialAT.end();
        SerialAT.begin(115200, SERIAL_8N1, activeRx, activeTx);
        delay(200);
    }

    // Disable echo
    sendAT("ATE0", "OK");

    // Check SIM card
    String simResp = sendAT("AT+CPIN?", "READY", 5000);
    if (simResp.indexOf("READY") < 0) {
        DEBUG_SERIAL.println("[SIM] ERROR: SIM card not detected!");
        return false;
    }
    DEBUG_SERIAL.println("[SIM] SIM card detected.");

    // Set APN
    char apnCmd[128];
    snprintf(apnCmd, sizeof(apnCmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN_NAME);
    sendAT(apnCmd, "OK", 3000);

    // Wait for network registration (4G/LTE)
    DEBUG_SERIAL.println("[SIM] Waiting for network registration...");
    for (int i = 0; i < 30; i++) {
        String regResp = sendAT("AT+CEREG?", "+CEREG:", 3000);
        // +CEREG: 0,1 = registered home, +CEREG: 0,5 = registered roaming
        if (regResp.indexOf(",1") >= 0 || regResp.indexOf(",5") >= 0) {
            DEBUG_SERIAL.println("[SIM] Network registered!");
            networkConnected = true;

            // Activate PDP context & open network
            DEBUG_SERIAL.println("[SIM] Activating 4G PDP context...");
            sendAT("AT+CGACT=1,1", "OK", 5000);
            sendAT("AT+NETOPEN", "OK", 5000);
            delay(1000);

            // Get signal quality
            updateSignalQuality();
            return true;
        }
        DEBUG_SERIAL.printf("[SIM] Waiting for registration... (%d/30)\n", i + 1);
        delay(2000);
    }

    DEBUG_SERIAL.println("[SIM] ERROR: Network registration timeout!");
    return false;
}

void updateSignalQuality() {
    String resp = sendAT("AT+CSQ", "+CSQ:", 2000);
    int idx = resp.indexOf("+CSQ:");
    if (idx >= 0) {
        int commaIdx = resp.indexOf(",", idx);
        String qualStr = resp.substring(idx + 6, commaIdx);
        signalQuality = qualStr.toInt();
        DEBUG_SERIAL.printf("[SIM] Signal quality: %d/31\n", signalQuality);
    }
}

// =============================================================
// 4. MQTT VIA AT COMMANDS
// =============================================================

bool mqttStart() {
    DEBUG_SERIAL.println("[MQTT] Resetting network & MQTT stack...");

    // Teardown any previous or stuck session
    sendAT("AT+CMQTTDISC=0,120", "OK", 1000);
    sendAT("AT+CMQTTREL=0", "OK", 1000);
    sendAT("AT+CMQTTSTOP", "OK", 1000);
    delay(500);

    // Refresh 4G IP network socket
    sendAT("AT+NETCLOSE", "OK", 2000);
    delay(500);
    sendAT("AT+CGACT=1,1", "OK", 5000);
    sendAT("AT+NETOPEN", "OK", 5000);
    delay(1500);

    // Start fresh MQTT stack
    String resp = sendAT("AT+CMQTTSTART", "OK", 5000);
    if (resp.indexOf("OK") < 0 && resp.indexOf("+CMQTTSTART: 0") < 0 && resp.indexOf("23") < 0 && resp.indexOf("already") < 0) {
        DEBUG_SERIAL.println("[MQTT] ERROR: Failed to start MQTT service!");
        return false;
    }

    // Acquire client 0
    char clientCmd[128];
    snprintf(clientCmd, sizeof(clientCmd), "AT+CMQTTACCQ=0,\"%s\",0", MQTT_CLIENT_ID);
    resp = sendAT(clientCmd, "OK", 3000);
    if (resp.indexOf("OK") < 0 && resp.indexOf("already") < 0) {
        DEBUG_SERIAL.println("[MQTT] ERROR: Failed to acquire client!");
        return false;
    }

    // Connect to broker
    char connectCmd[256];
    if (strlen(MQTT_USERNAME) > 0) {
        snprintf(connectCmd, sizeof(connectCmd),
            "AT+CMQTTCONNECT=0,\"tcp://%s:%d\",%d,1,\"%s\",\"%s\"",
            MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE, MQTT_USERNAME, MQTT_PASSWORD);
    } else {
        snprintf(connectCmd, sizeof(connectCmd),
            "AT+CMQTTCONNECT=0,\"tcp://%s:%d\",%d,1",
            MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE);
    }

    resp = sendAT(connectCmd, "CMQTTCONNECT: 0,0", 15000);
    if (resp.indexOf("CMQTTCONNECT: 0,0") >= 0) {
        DEBUG_SERIAL.println("[MQTT] Connected to broker!");
        mqttConnected = true;
        reconnectDelay = MQTT_RECONNECT_BASE_MS;  // Reset backoff

        // Subscribe to command topic for remote config
        mqttSubscribeCommand();
        return true;
    }

    DEBUG_SERIAL.println("[MQTT] ERROR: Failed to connect to broker!");
    return false;
}

void mqttSubscribeCommand() {
    const char* topic = MQTT_TOPIC_COMMAND;
    char subCmd[128];
    snprintf(subCmd, sizeof(subCmd), "AT+CMQTTSUBTOPIC=0,%d,1", strlen(topic));
    if (sendATWithData(subCmd, topic)) {
        sendAT("AT+CMQTTSUB=0", "OK", 5000);
        DEBUG_SERIAL.printf("[MQTT] Subscribed to %s\n", topic);
    } else {
        DEBUG_SERIAL.println("[MQTT] WARNING: Failed to subscribe to command topic.");
    }
}

bool mqttPublish(const char* topic, const String& payload) {
    if (!mqttConnected) return false;

    // Set topic
    char topicCmd[128];
    snprintf(topicCmd, sizeof(topicCmd), "AT+CMQTTTOPIC=0,%d", strlen(topic));
    if (!sendATWithData(topicCmd, topic)) {
        DEBUG_SERIAL.println("[MQTT] ERROR: Failed to set topic!");
        mqttConnected = false;
        return false;
    }

    // Set payload
    char payloadCmd[64];
    snprintf(payloadCmd, sizeof(payloadCmd), "AT+CMQTTPAYLOAD=0,%d", payload.length());
    if (!sendATWithData(payloadCmd, payload)) {
        DEBUG_SERIAL.println("[MQTT] ERROR: Failed to set payload!");
        mqttConnected = false;
        return false;
    }

    // Publish (QoS 1, retain = 0)
    String resp = sendAT("AT+CMQTTPUB=0,1,60", "CMQTTPUB: 0,0", 10000);
    if (resp.indexOf("CMQTTPUB: 0,0") >= 0) {
        DEBUG_SERIAL.printf("[MQTT] Published to %s (%d bytes)\n", topic, payload.length());
        return true;
    }

    DEBUG_SERIAL.println("[MQTT] ERROR: Publish failed!");
    mqttConnected = false;
    return false;
}

int mqttFailCount = 0;

void mqttReconnect() {
    DEBUG_SERIAL.printf("[MQTT] Reconnecting in %lu ms...\n", reconnectDelay);
    delay(reconnectDelay);

    mqttFailCount++;
    if (mqttFailCount >= 3) {
        DEBUG_SERIAL.println("[SIM] 3 consecutive MQTT failures. Performing modem soft reset (AT+CFUN=1,1)...");
        sendAT("AT+CFUN=1,1", "OK", 3000);
        delay(6000); // Wait for modem restart
        mqttFailCount = 0;
        networkConnected = false;
        initModule();
    }

    if (!networkConnected) {
        initModule();
    }

    if (networkConnected) {
        if (mqttStart()) {
            mqttFailCount = 0;
        }
    }

    // Exponential backoff
    reconnectDelay = min(reconnectDelay * 2, (unsigned long)MQTT_RECONNECT_MAX_MS);
}

// =============================================================
// 4b. MQTT COMMAND PARSER
// =============================================================

void checkForMqttCommands() {
    // Poll UART for unsolicited +CMQTTRXSTART messages
    String urc = "";
    unsigned long start = millis();
    while (millis() - start < 50) {
        while (SerialAT.available()) {
            urc += (char)SerialAT.read();
        }
        if (urc.length() > 0) delay(5);
        else break;
    }

    if (urc.length() == 0) return;

    // Look for +CMQTTRXPAYLOAD which contains the JSON command
    int payloadStart = urc.indexOf('{');
    int payloadEnd = urc.lastIndexOf('}');
    if (payloadStart < 0 || payloadEnd < 0 || payloadEnd <= payloadStart) return;

    String jsonStr = urc.substring(payloadStart, payloadEnd + 1);
    DEBUG_SERIAL.printf("[CMD] Received: %s\n", jsonStr.c_str());

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, jsonStr);
    if (err) {
        DEBUG_SERIAL.printf("[CMD] JSON parse error: %s\n", err.c_str());
        return;
    }

    const char* command = doc["command"] | "";

    if (strcmp(command, "restart") == 0) {
        DEBUG_SERIAL.println("[CMD] Restart command received. Rebooting in 2 seconds...");
        delay(2000);
        ESP.restart();
    }

    if (strcmp(command, "set_config") == 0) {
        if (doc.containsKey("sensor_read_interval"))
            cfgSensorReadInterval = doc["sensor_read_interval"].as<unsigned long>();
        if (doc.containsKey("mqtt_publish_interval"))
            cfgMqttPublishInterval = doc["mqtt_publish_interval"].as<unsigned long>();
        if (doc.containsKey("status_publish_interval"))
            cfgStatusPublishInterval = doc["status_publish_interval"].as<unsigned long>();
        if (doc.containsKey("ph_phase_duration"))
            cfgPhPhaseDuration = doc["ph_phase_duration"].as<unsigned long>();
        if (doc.containsKey("buffer_phase_duration"))
            cfgBufferPhaseDuration = doc["buffer_phase_duration"].as<unsigned long>();
        if (doc.containsKey("tds_phase_duration"))
            cfgTdsPhaseDuration = doc["tds_phase_duration"].as<unsigned long>();
        if (doc.containsKey("deep_sleep_enabled"))
            cfgDeepSleepEnabled = doc["deep_sleep_enabled"].as<bool>();
        if (doc.containsKey("deep_sleep_duration_sec"))
            cfgDeepSleepDurationSec = doc["deep_sleep_duration_sec"].as<unsigned long>();
        if (doc.containsKey("dashboard_poll_ms"))
            cfgDashboardPollMs = doc["dashboard_poll_ms"].as<unsigned long>();

        DEBUG_SERIAL.printf("[CMD] Config updated: read=%lu pub=%lu status=%lu sleep=%s(%lus)\n",
            cfgSensorReadInterval, cfgMqttPublishInterval, cfgStatusPublishInterval,
            cfgDeepSleepEnabled ? "ON" : "OFF", cfgDeepSleepDurationSec);

        // Save updated settings to Flash memory (NVS)
        saveConfigToNVS();

        // ACK back to server
        String ack = "{\"command\":\"config_ack\",\"device_id\":\"" + String(DEVICE_ID) + "\"}";
        mqttPublish(MQTT_TOPIC_STATUS, ack);
    }

    if (strcmp(command, "get_config") == 0) {
        // Respond with current config
        String statusPayload = buildStatusPayload();
        mqttPublish(MQTT_TOPIC_STATUS, statusPayload);
    }
}

// =============================================================
// 5. SENSOR READING FUNCTIONS
// =============================================================

// Median filter — removes noise spikes from analog readings
int getMedianNum(int bArray[], int iFilterLen) {
    int bTab[iFilterLen];
    for (int i = 0; i < iFilterLen; i++) bTab[i] = bArray[i];

    // Bubble sort
    for (int j = 0; j < iFilterLen - 1; j++) {
        for (int i = 0; i < iFilterLen - j - 1; i++) {
            if (bTab[i] > bTab[i + 1]) {
                int temp = bTab[i];
                bTab[i] = bTab[i + 1];
                bTab[i + 1] = temp;
            }
        }
    }

    if (iFilterLen & 1) {
        return bTab[(iFilterLen - 1) / 2];
    } else {
        return (bTab[iFilterLen / 2] + bTab[iFilterLen / 2 - 1]) / 2;
    }
}

void readTemperature() {
    tempSensor.requestTemperatures();
    float tempC = tempSensor.getTempCByIndex(0);
    if (tempC != DEVICE_DISCONNECTED_C && tempC > -10 && tempC < 80) {
        current.temperature = tempC;
    }
    // If disconnected, keep previous value
}

void readPH() {
    int phRaw = analogRead(PH_SENSOR_PIN);
    if (phRaw > 100) {
        float phVoltage = phRaw * (VREF / ADC_RESOLUTION);
        float phCalc = PH_COEFF_A * sq(phVoltage) + PH_COEFF_B * phVoltage + PH_COEFF_C;
        if (!isnan(phCalc) && phCalc >= 0.0 && phCalc <= 14.0) {
            current.ph = phCalc;
            previous.ph = phCalc;
        }
    }
}

void readTDS() {
    for (int i = 0; i < MEDIAN_SAMPLE_COUNT; i++) {
        analogBuffer[i] = analogRead(TDS_SENSOR_PIN);
    }
    float avgVoltage = getMedianNum(analogBuffer, MEDIAN_SAMPLE_COUNT) * VREF / (ADC_RESOLUTION + 1);
    float tdsCalc = (TDS_COEFF_A * pow(avgVoltage, 3) + TDS_COEFF_B * pow(avgVoltage, 2) + TDS_COEFF_C * avgVoltage) * TDS_SCALE;

    if (!isnan(tdsCalc) && tdsCalc >= 0) {
        current.tds = tdsCalc;
        previous.tds = tdsCalc;
    } else {
        current.tds = previous.tds;
    }
}

void readTurbidity() {
    int sensorValue = analogRead(TURBIDITY_PIN);
    float voltage = sensorValue * (VREF / ADC_RESOLUTION);
    float turbCalc = TURB_COEFF_A * voltage * voltage + TURB_COEFF_B * voltage + TURB_COEFF_C;
    current.turbidity = turbCalc;
}

void calculateDissolvedOxygen() {
    // Empirical formula based on temperature, TDS, turbidity, and pH
    current.dissolvedO2 = DO_CONST_A
                        - (DO_CONST_B * current.temperature)
                        - (DO_CONST_C * current.tds)
                        - (DO_CONST_D * current.turbidity)
                        + (DO_CONST_E * current.ph);
}

void readAllSensors() {
    // Temperature is always readable (digital, no interference)
    readTemperature();

    // pH and TDS are phase-multiplexed to avoid crosstalk
    if (currentPhase == PHASE_PH) {
        readPH();
        current.tds = previous.tds;  // Hold previous TDS
    } else if (currentPhase == PHASE_TDS) {
        readTDS();
        current.ph = previous.ph;    // Hold previous pH
    } else {
        // BUFFER phase — hold both
        current.ph = previous.ph;
        current.tds = previous.tds;
    }

    // Turbidity is independent (different pin)
    readTurbidity();

    // DO is calculated from other sensors
    calculateDissolvedOxygen();
}

// =============================================================
// 6. JSON PAYLOAD BUILDERS
// =============================================================

String buildSensorPayload() {
    StaticJsonDocument<512> doc;

    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = (unsigned long)(millis() / 1000);  // Uptime-based

    JsonObject sensors = doc.createNestedObject("sensors");

    JsonObject ph = sensors.createNestedObject("ph");
    ph["value"] = round(current.ph * 100.0) / 100.0;
    ph["unit"] = "pH";

    JsonObject tds = sensors.createNestedObject("tds");
    tds["value"] = round(current.tds * 10.0) / 10.0;
    tds["unit"] = "ppm";

    JsonObject temp = sensors.createNestedObject("temperature");
    temp["value"] = round(current.temperature * 10.0) / 10.0;
    temp["unit"] = "°C";

    JsonObject turb = sensors.createNestedObject("turbidity");
    turb["value"] = round(current.turbidity * 10.0) / 10.0;
    turb["unit"] = "NTU";

    JsonObject dox = sensors.createNestedObject("dissolved_oxygen");
    dox["value"] = round(current.dissolvedO2 * 100.0) / 100.0;
    dox["unit"] = "mg/L";

    JsonObject meta = doc.createNestedObject("metadata");
    meta["signal_quality"] = signalQuality;
    meta["firmware_version"] = FIRMWARE_VERSION;
    meta["uptime_seconds"] = (unsigned long)(millis() / 1000);

    String payload;
    serializeJson(doc, payload);
    return payload;
}

String buildStatusPayload() {
    StaticJsonDocument<768> doc;

    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = (unsigned long)(millis() / 1000);
    doc["status"] = mqttConnected ? "online" : "reconnecting";
    doc["signal_quality"] = signalQuality;
    doc["uptime_seconds"] = (unsigned long)(millis() / 1000);
    doc["free_heap"] = ESP.getFreeHeap();
    doc["firmware_version"] = FIRMWARE_VERSION;

    // Include current runtime config so the dashboard can display it
    JsonObject cfg = doc.createNestedObject("config");
    cfg["sensor_read_interval"] = cfgSensorReadInterval;
    cfg["mqtt_publish_interval"] = cfgMqttPublishInterval;
    cfg["status_publish_interval"] = cfgStatusPublishInterval;
    cfg["ph_phase_duration"] = cfgPhPhaseDuration;
    cfg["buffer_phase_duration"] = cfgBufferPhaseDuration;
    cfg["tds_phase_duration"] = cfgTdsPhaseDuration;
    cfg["deep_sleep_enabled"] = cfgDeepSleepEnabled;
    cfg["deep_sleep_duration_sec"] = cfgDeepSleepDurationSec;
    cfg["dashboard_poll_ms"] = cfgDashboardPollMs;

    String payload;
    serializeJson(doc, payload);
    return payload;
}

// =============================================================
// 7. PHASE MANAGEMENT
// =============================================================

void updatePhase() {
    unsigned long now = millis();

    switch (currentPhase) {
        case PHASE_PH:
            if (now - lastPhaseChange >= cfgPhPhaseDuration) {
                currentPhase = PHASE_BUFFER;
                lastPhaseChange = now;
                digitalWrite(TDS_CONTROL_PIN, HIGH);  // Prepare TDS
            }
            break;

        case PHASE_BUFFER:
            if (now - lastPhaseChange >= cfgBufferPhaseDuration) {
                currentPhase = PHASE_TDS;
                lastPhaseChange = now;
                digitalWrite(TDS_CONTROL_PIN, LOW);   // Enable TDS reading
            }
            break;

        case PHASE_TDS:
            if (now - lastPhaseChange >= cfgTdsPhaseDuration) {
                currentPhase = PHASE_PH;
                lastPhaseChange = now;
                digitalWrite(TDS_CONTROL_PIN, HIGH);  // Disable TDS
            }
            break;
    }
}

// =============================================================
// 8. SETUP
// =============================================================

void setup() {
    // Debug serial on USB (115200 baud)
    DEBUG_SERIAL.begin(DEBUG_BAUD);
    DEBUG_SERIAL.println();
    DEBUG_SERIAL.println("=============================================");
    DEBUG_SERIAL.println(" Water Quality Monitor v" FIRMWARE_VERSION);
    DEBUG_SERIAL.println(" Device: " DEVICE_ID);
    DEBUG_SERIAL.println("=============================================");

    // Load persistent parameters from NVS Flash (if previously saved)
    loadConfigFromNVS();

    // Sensor pins
    pinMode(TDS_SENSOR_PIN, INPUT);
    pinMode(PH_SENSOR_PIN, INPUT);
    pinMode(TURBIDITY_PIN, INPUT);
    pinMode(TDS_CONTROL_PIN, OUTPUT);
    digitalWrite(TDS_CONTROL_PIN, HIGH);

    // DS18B20
    tempSensor.begin();

    // SIMA7670C UART
    SerialAT.begin(SIM_BAUD_RATE, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);

    // Power on the 4G module
    powerOnModule();

    // Initialize module (SIM check, APN, network registration) with cold-boot retries
    int bootAttempts = 0;
    while (!networkConnected && bootAttempts < 3) {
        bootAttempts++;
        DEBUG_SERIAL.printf("[INIT] 4G initialization attempt %d/3...\n", bootAttempts);
        if (initModule()) {
            DEBUG_SERIAL.println("[INIT] 4G module ready.");
            break;
        } else {
            DEBUG_SERIAL.println("[INIT] 4G module not ready yet. Waiting 3 seconds before retry...");
            delay(3000);
        }
    }

    // Auto-restart or Deep Sleep protection guard on cold boot failure
    if (!networkConnected) {
        consecutiveBootFailures++;
        if (consecutiveBootFailures >= 3) {
            DEBUG_SERIAL.printf("[GUARD] 3 consecutive network boot cycles failed (%d). Entering 5-min low-power sleep guard...\n", consecutiveBootFailures);
            consecutiveBootFailures = 0; // Reset for next attempt
            esp_sleep_enable_timer_wakeup(300ULL * 1000000ULL); // 5 min sleep
            esp_deep_sleep_start();
        } else {
            DEBUG_SERIAL.printf("[INIT] Attempt failed (cycle %d/3). Rebooting ESP32 in 5 seconds...\n", consecutiveBootFailures);
            delay(5000);
            ESP.restart();
        }
    } else {
        consecutiveBootFailures = 0; // Connection success — reset failure counter!
    }

    // Connect to MQTT broker
    if (networkConnected) {
        mqttStart();
    }

    deviceStartTime = millis();
    lastPhaseChange = millis();

    DEBUG_SERIAL.println("[INIT] Setup complete. Starting main loop.");
    DEBUG_SERIAL.println();
}

// =============================================================
// 9. MAIN LOOP
// =============================================================

void loop() {
    unsigned long now = millis();

    // --- Phase Management ---
    updatePhase();

    // --- Check for incoming MQTT commands ---
    if (mqttConnected) {
        checkForMqttCommands();
    }

    // --- Read Sensors ---
    if (now - lastSensorRead >= cfgSensorReadInterval) {
        lastSensorRead = now;
        readAllSensors();

        // Print to debug serial
        DEBUG_SERIAL.printf("[DATA] pH:%.2f | TDS:%.0f | Temp:%.1f°C | Turb:%.1f NTU | DO:%.2f mg/L | Phase:%s\n",
            current.ph, current.tds, current.temperature,
            current.turbidity, current.dissolvedO2,
            currentPhase == PHASE_PH ? "pH" : (currentPhase == PHASE_TDS ? "TDS" : "BUF"));
    }

    // --- Publish Sensor Data ---
    if (now - lastMqttPublish >= cfgMqttPublishInterval) {
        lastMqttPublish = now;

        if (mqttConnected) {
            String payload = buildSensorPayload();
            if (!mqttPublish(MQTT_TOPIC_SENSORS, payload)) {
                DEBUG_SERIAL.println("[LOOP] Publish failed, will reconnect.");
            }
        } else {
            mqttReconnect();
        }
    }

    // --- Publish Status Heartbeat ---
    if (now - lastStatusPublish >= cfgStatusPublishInterval) {
        lastStatusPublish = now;

        // Refresh signal quality
        updateSignalQuality();

        if (mqttConnected) {
            String statusPayload = buildStatusPayload();
            mqttPublish(MQTT_TOPIC_STATUS, statusPayload);
        }
    }

    // --- Deep Sleep (if enabled) ---
    if (cfgDeepSleepEnabled && cfgDeepSleepDurationSec > 0) {
        DEBUG_SERIAL.printf("[SLEEP] Entering deep sleep for %lu seconds...\n", cfgDeepSleepDurationSec);
        // Gracefully disconnect MQTT before sleep
        sendAT("AT+CMQTTDISC=0,10", "OK", 1000);
        sendAT("AT+CMQTTREL=0", "OK", 1000);
        sendAT("AT+CMQTTSTOP", "OK", 1000);
        delay(500);
        esp_sleep_enable_timer_wakeup(cfgDeepSleepDurationSec * 1000000ULL);
        esp_deep_sleep_start();
    }

    // Small yield to prevent watchdog reset
    delay(10);
}
