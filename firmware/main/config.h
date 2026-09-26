// ============================================================
// config.h — Water Quality Monitor Configuration
// ============================================================
// All tunable parameters in one place. Edit this file to match
// your hardware wiring, SIM card provider, and AWS server.
// ============================================================

#ifndef CONFIG_H
#define CONFIG_H

// =============================================================
// 1. DEVICE IDENTITY
// =============================================================
#define DEVICE_ID           "WQM-001"
#define FIRMWARE_VERSION    "2.0.0"

// =============================================================
// 2. SIMA7670C 4G/LTE MODULE (UART + Control)
// =============================================================
// --- SIMA7670C 4G Module Pins ---
// Connect SIMA7670C TXD → ESP32 GPIO18 (RX2)
// Connect SIMA7670C RXD → ESP32 GPIO19 (TX2)
// No PWRKEY pin needed on this board (Auto-power on with 5V)
#define SIM_RX_PIN          18    // ESP32 RX2 ← SIM7670 TXD
#define SIM_TX_PIN          19    // ESP32 TX2 → SIM7670 RXD
#define SIM_PWRKEY_PIN      -1    // -1 = Auto power-on (no PWRKEY pin)
#define SIM_BAUD_RATE       115200

// --- SIM Card / APN Settings ---
// Change these to match your SIM card provider:
//   Jio     → "jionet"
//   Airtel  → "airtelgprs.com"
//   Vi      → "vi.net"
//   BSNL    → "bsnlnet"
#define APN_NAME            "airtelgprs.com"
#define APN_USER            ""      // Usually empty for Indian carriers
#define APN_PASS            ""

// =============================================================
// 3. MQTT BROKER (Mosquitto on AWS EC2)
// =============================================================
#define MQTT_BROKER         "YOUR_AWS_EC2_PUBLIC_IP"   // Replace with your AWS EC2 Public IP / Elastic IP
#define MQTT_PORT           1883
#define MQTT_CLIENT_ID      DEVICE_ID
#define MQTT_USERNAME       ""      // Leave empty if no auth
#define MQTT_PASSWORD       ""      // Leave empty if no auth
#define MQTT_KEEPALIVE      15      // seconds (broker detects disconnect in ~22s)

// --- MQTT Topics ---
#define MQTT_TOPIC_SENSORS  "waterquality/" DEVICE_ID "/sensors/live"
#define MQTT_TOPIC_STATUS   "waterquality/" DEVICE_ID "/status"
#define MQTT_TOPIC_COMMAND  "waterquality/" DEVICE_ID "/command"

// =============================================================
// 4. SENSOR PIN DEFINITIONS
// =============================================================
#define TDS_SENSOR_PIN      32    // Analog — TDS probe
#define PH_SENSOR_PIN       34    // Analog — pH probe
#define TURBIDITY_PIN       33    // Analog — Turbidity sensor
#define TDS_CONTROL_PIN     26    // Digital — TDS power relay (old wiring)
#define ONE_WIRE_BUS        4     // Digital — DS18B20 temperature (old wiring)

// =============================================================
// 5. SENSOR CALIBRATION COEFFICIENTS
// =============================================================

// --- pH Calibration (quadratic: pH = a*V² + b*V + c) ---
// Calibrated with lab reference (pH 7.97)
#define PH_COEFF_A         -3.0951
#define PH_COEFF_B          5.6410
#define PH_COEFF_C          6.3216

// --- TDS Calibration (cubic: TDS = a*V³ + b*V² + c*V, scaled by 1.426) ---
// Calibrated with lab reference (TDS 77.0 mg/L)
#define TDS_COEFF_A         133.42
#define TDS_COEFF_B        -255.86
#define TDS_COEFF_C         857.39
#define TDS_SCALE           1.426

// --- Turbidity Calibration (quadratic: NTU = a*V² + b*V + c) ---
// Calibrated with lab reference (Turbidity 0.4 NTU in clear water)
#define TURB_COEFF_A       -1120.4
#define TURB_COEFF_B        5742.3
#define TURB_COEFF_C       -6748.5

// --- Dissolved Oxygen (empirical: DO = a - b*T - c*TDS - d*Turb + e*pH) ---
// Calibrated with lab reference (DO 7.2 mg/L at 27.4°C)
#define DO_CONST_A          18.2573
#define DO_CONST_B          0.41
#define DO_CONST_C          0.0008
#define DO_CONST_D          0.002
#define DO_CONST_E          0.03

// =============================================================
// 6. ADC / FILTERING
// =============================================================
#define VREF                3.3
#define ADC_RESOLUTION      4095.0
#define MEDIAN_SAMPLE_COUNT 30     // Samples for median filter

// =============================================================
// 7. TIMING (milliseconds)
// =============================================================
#define SENSOR_READ_INTERVAL    2000    // Read sensors every 2s
#define MQTT_PUBLISH_INTERVAL   5000    // Publish data every 5s
#define STATUS_PUBLISH_INTERVAL 60000   // Status heartbeat every 60s
#define PH_PHASE_DURATION       5000    // pH reading phase
#define BUFFER_PHASE_DURATION   2000    // Buffer between pH and TDS
#define TDS_PHASE_DURATION      5000    // TDS reading phase

// --- Reconnection ---
#define MQTT_RECONNECT_BASE_MS  2000    // Initial retry delay
#define MQTT_RECONNECT_MAX_MS   3000    // Max retry delay (3s for fast testing)

// =============================================================
// 8. DEBUG
// =============================================================
#define DEBUG_SERIAL        Serial
#define DEBUG_BAUD          115200
#define ENABLE_AT_DEBUG     true  // Print AT commands to Serial Monitor

#endif // CONFIG_H
