// ===================================================================
// 3-BATTERY BALANCING SYSTEM with Firebase Realtime Database
// WITH DYNAMIC BALANCING & CYCLE MANAGEMENT
// - All batteries charge simultaneously with adjustable rates
// - Automatic charge/discharge cycles
// - Cycle counting with configurable max cycles
// - Matches the behavior of the Python seeder
// ===================================================================

#include <Wire.h>
#include <Adafruit_ADS1015.h>
#include <Adafruit_INA219.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_BME280.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ==================== WiFi CREDENTIALS ====================
// PALITAN ITO NG INYONG WIFI DETAILS
#define WIFI_SSID       "YOUR_WIFI_NAME"
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"

// ==================== Firebase CREDENTIALS ====================
#define API_KEY         "AIzaSyAb1tt5CDHtrGSkb9sFumjRo22BYV0ex9A"
#define DATABASE_URL    "https://smart-battery-b06ae-default-rtdb.asia-southeast1.firebasedatabase.app/"

// ==================== Firebase Objects ====================
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ==================== PIN ASSIGNMENTS ====================
#define RELAY_LOAD_BATT1  26
#define RELAY_LOAD_BATT2  27
#define RELAY_LOAD_BATT3  14
#define RELAY_SOLAR_BATT1 12
#define RELAY_SOLAR_BATT2 13
#define RELAY_SOLAR_BATT3 33

// PWM pins for MOSFET control (for variable charging current)
#define PWM_BATT1         4
#define PWM_BATT2         5
#define PWM_BATT3         6

#define ACS712_BATT1_PIN  35
#define ACS712_BATT2_PIN  36
#define ACS712_BATT3_PIN  39
#define LDR_PIN           34

#define ONE_WIRE_BUS      15
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensors(&oneWire);

#define STATUS_LED        2

Adafruit_ADS1115 ads;
Adafruit_INA219 ina219_1(0x40);
Adafruit_INA219 ina219_2(0x41);
Adafruit_INA219 ina219_3(0x42);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_BME280 bme;

// ==================== BATTERY PARAMETERS ====================
#define BATT1_VOLT_MIN    3.0
#define BATT1_VOLT_MAX    4.2
#define BATT1_TEMP_MAX    50.0
#define BATT1_CURRENT_MAX 2.0

#define BATT2_VOLT_MIN    5.4
#define BATT2_VOLT_MAX    6.8
#define BATT2_TEMP_MAX    50.0
#define BATT2_CURRENT_MAX 3.0

#define BATT3_VOLT_MIN    10.0
#define BATT3_VOLT_MAX    14.4
#define BATT3_TEMP_MAX    50.0
#define BATT3_CURRENT_MAX 5.0

#define IMBALANCE_THRESHOLD  5.0
#define DISCHARGE_CUTOFF     20.0
#define OVERCHARGE_CUTOFF    98.0

// ==================== DYNAMIC BALANCING PARAMETERS ====================
#define BALANCING_FACTOR_MIN  0.5
#define BALANCING_FACTOR_MAX  1.5
#define PWM_FREQ              5000
#define PWM_RESOLUTION        8

// ==================== CYCLE MANAGEMENT PARAMETERS ====================
#define MAX_CYCLES            3  // Number of full charge/discharge cycles
#define CHARGE_COMPLETE_THRESHOLD  99.5  // Consider fully charged above this %
#define DISCHARGE_EMPTY_THRESHOLD   0.5  // Consider fully discharged below this %

// ==================== GLOBAL VARIABLES ====================
float batt1_voltage = 0, batt2_voltage = 0, batt3_voltage = 0;
float batt1_percent = 0, batt2_percent = 0, batt3_percent = 0;
float batt1_current = 0, batt2_current = 0, batt3_current = 0;
float batt1_temp = 0, batt2_temp = 0, batt3_temp = 0;
float solar1_current = 0, solar2_current = 0, solar3_current = 0;
float ldr_value = 0;
float ambient_temp = 0, humidity = 0;
float avg_percent = 0;
float imbalance = 0;

// Balancing factors
float balancing_factor_b1 = 1.0;
float balancing_factor_b2 = 1.0;
float balancing_factor_b3 = 1.0;

// PWM duty cycles
int pwm_batt1 = 0;
int pwm_batt2 = 0;
int pwm_batt3 = 0;

// Relay states
bool load_relay1 = false, load_relay2 = false, load_relay3 = false;
bool solar_relay1 = false, solar_relay2 = false, solar_relay3 = false;
bool emergency_mode = false;
int system_status = 0;

// Cycle management
int cycle_count = 0;
int max_cycles = MAX_CYCLES;
String phase = "CHARGING";  // CHARGING, DISCHARGING, COMPLETED

// Timing
unsigned long lastReadTime = 0;
unsigned long lastSendTime = 0;
unsigned long lastHistoryTime = 0;
const unsigned long READ_INTERVAL = 500;
const unsigned long SEND_INTERVAL = 10000;
const unsigned long HISTORY_INTERVAL = 60000;

bool signupOK = false;

// ==================== FUNCTION DECLARATIONS ====================
void readAllSensors();
void updatePercentages();
void checkProtections();
void balancingLogic();
void updateDisplay();
void updateDynamicBalancing();
void updatePWMSignals();
float voltageToPercentage(float voltage, float vMin, float vMax);
float readACS712(int pin);
void setLoadRelay(int battery, bool state);
void setSolarRelay(int battery, bool state);
void emergencyStop(int battery);
void connectToWiFi();
void connectToFirebase();
void sendToFirebase();
void saveHistoricalData();

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n╔══════════════════════════════════════════════════════════════════╗");
  Serial.println("║   3-BATTERY BALANCING SYSTEM with DYNAMIC BALANCING              ║");
  Serial.println("║   WITH CHARGE/DISCHARGE CYCLE MANAGEMENT                          ║");
  Serial.println("║   ALL batteries charge simultaneously with adjustable rates      ║");
  Serial.println("╚══════════════════════════════════════════════════════════════════╝\n");
  
  // Initialize PWM pins for MOSFET control
  ledcSetup(0, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(1, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(2, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(PWM_BATT1, 0);
  ledcAttachPin(PWM_BATT2, 1);
  ledcAttachPin(PWM_BATT3, 2);
  
  // Initialize relay pins
  pinMode(RELAY_LOAD_BATT1, OUTPUT);
  pinMode(RELAY_LOAD_BATT2, OUTPUT);
  pinMode(RELAY_LOAD_BATT3, OUTPUT);
  pinMode(RELAY_SOLAR_BATT1, OUTPUT);
  pinMode(RELAY_SOLAR_BATT2, OUTPUT);
  pinMode(RELAY_SOLAR_BATT3, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(LDR_PIN, INPUT);
  
  // Start with all relays OFF
  digitalWrite(RELAY_LOAD_BATT1, LOW);
  digitalWrite(RELAY_LOAD_BATT2, LOW);
  digitalWrite(RELAY_LOAD_BATT3, LOW);
  digitalWrite(RELAY_SOLAR_BATT1, LOW);
  digitalWrite(RELAY_SOLAR_BATT2, LOW);
  digitalWrite(RELAY_SOLAR_BATT3, LOW);
  
  // Initialize ADS1115
  if (!ads.begin()) {
    Serial.println("ERROR: ADS1115 not found!");
  } else {
    ads.setGain(GAIN_ONE);
    Serial.println("ADS1115 initialized");
  }
  
  // Initialize INA219 sensors
  if (!ina219_1.begin()) Serial.println("ERROR: INA219 #1 not found!");
  else Serial.println("INA219 #1 (Solar 1) initialized");
  
  if (!ina219_2.begin()) Serial.println("ERROR: INA219 #2 not found!");
  else Serial.println("INA219 #2 (Solar 2) initialized");
  
  if (!ina219_3.begin()) Serial.println("ERROR: INA219 #3 not found!");
  else Serial.println("INA219 #3 (Solar 3) initialized");
  
  tempSensors.begin();
  int deviceCount = tempSensors.getDeviceCount();
  Serial.printf("DS18B20 sensors found: %d\n", deviceCount);
  
  if (!bme.begin(0x76)) {
    Serial.println("ERROR: BME280 not found!");
  } else {
    Serial.println("BME280 initialized");
  }
  
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Dynamic Balancing");
  lcd.setCursor(0, 1);
  lcd.print("System Ready");
  
  connectToWiFi();
  connectToFirebase();
  
  delay(2000);
  lcd.clear();
  
  Serial.println("\n▶ System Started. Dynamic Balancing ACTIVE!\n");
  Serial.println("   All 3 batteries will charge SIMULTANEOUSLY");
  Serial.println("   Charging rates adjust automatically to maintain balance");
  Serial.printf("   Max cycles: %d\n", max_cycles);
  Serial.println();
}

void connectToWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed!");
  }
}

void connectToFirebase() {
  Serial.print("Connecting to Firebase");
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;
  
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("\nFirebase sign-up OK");
    signupOK = true;
  } else {
    Serial.printf("\nFirebase sign-up failed: %s\n", config.signer.signupError.message.c_str());
    signupOK = false;
  }
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  if (signupOK) {
    Serial.println("Connected to Firebase Realtime Database!");
  }
}

// ==================== READ ALL SENSORS ====================
void readAllSensors() {
  int16_t adc0 = ads.readADC_SingleEnded(0);
  int16_t adc1 = ads.readADC_SingleEnded(1);
  int16_t adc2 = ads.readADC_SingleEnded(2);
  
  float adc_voltage0 = (adc0 * 4.096) / 32768.0;
  float adc_voltage1 = (adc1 * 4.096) / 32768.0;
  float adc_voltage2 = (adc2 * 4.096) / 32768.0;
  
  batt1_voltage = adc_voltage0 * 4.0;
  batt2_voltage = adc_voltage1 * 4.0;
  batt3_voltage = adc_voltage2 * 4.0;
  
  batt1_current = readACS712(ACS712_BATT1_PIN);
  batt2_current = readACS712(ACS712_BATT2_PIN);
  batt3_current = readACS712(ACS712_BATT3_PIN);
  
  solar1_current = ina219_1.getCurrent_mA() / 1000.0;
  solar2_current = ina219_2.getCurrent_mA() / 1000.0;
  solar3_current = ina219_3.getCurrent_mA() / 1000.0;
  
  tempSensors.requestTemperatures();
  batt1_temp = tempSensors.getTempCByIndex(0);
  batt2_temp = tempSensors.getTempCByIndex(1);
  batt3_temp = tempSensors.getTempCByIndex(2);
  
  if (isnan(batt1_temp) || batt1_temp == -127) batt1_temp = 25.0;
  if (isnan(batt2_temp) || batt2_temp == -127) batt2_temp = 25.0;
  if (isnan(batt3_temp) || batt3_temp == -127) batt3_temp = 25.0;
  
  ldr_value = analogRead(LDR_PIN);
  
  ambient_temp = bme.readTemperature();
  humidity = bme.readHumidity();
  if (isnan(ambient_temp)) ambient_temp = 25.0;
  if (isnan(humidity)) humidity = 50.0;
}

void updatePercentages() {
  batt1_percent = voltageToPercentage(batt1_voltage, BATT1_VOLT_MIN, BATT1_VOLT_MAX);
  batt2_percent = voltageToPercentage(batt2_voltage, BATT2_VOLT_MIN, BATT2_VOLT_MAX);
  batt3_percent = voltageToPercentage(batt3_voltage, BATT3_VOLT_MIN, BATT3_VOLT_MAX);
  
  batt1_percent = constrain(batt1_percent, 0, 100);
  batt2_percent = constrain(batt2_percent, 0, 100);
  batt3_percent = constrain(batt3_percent, 0, 100);
  
  avg_percent = (batt1_percent + batt2_percent + batt3_percent) / 3.0;
  imbalance = max(abs(batt1_percent - avg_percent),
                  max(abs(batt2_percent - avg_percent),
                      abs(batt3_percent - avg_percent)));
}

float voltageToPercentage(float voltage, float vMin, float vMax) {
  if (voltage <= vMin) return 0;
  if (voltage >= vMax) return 100;
  return (voltage - vMin) / (vMax - vMin) * 100.0;
}

float readACS712(int pin) {
  const float VREF = 3.3;
  const float SENSITIVITY = 0.185;
  const float ZERO_CURRENT_VOLTAGE = 2.5;
  
  float sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(pin);
    delayMicroseconds(100);
  }
  int raw = sum / 10;
  
  float voltage = (raw / 4095.0) * VREF;
  float current = (voltage - ZERO_CURRENT_VOLTAGE) / SENSITIVITY;
  
  if (abs(current) < 0.05) current = 0;
  return current;
}

// ==================== DYNAMIC BALANCING ====================
void updateDynamicBalancing() {
  float target = avg_percent;
  
  float diff1 = target - batt1_percent;
  float diff2 = target - batt2_percent;
  float diff3 = target - batt3_percent;
  
  float factor1 = 1.0 + (diff1 / 100.0) * 2.0;
  float factor2 = 1.0 + (diff2 / 100.0) * 2.0;
  float factor3 = 1.0 + (diff3 / 100.0) * 2.0;
  
  balancing_factor_b1 = constrain(factor1, BALANCING_FACTOR_MIN, BALANCING_FACTOR_MAX);
  balancing_factor_b2 = constrain(factor2, BALANCING_FACTOR_MIN, BALANCING_FACTOR_MAX);
  balancing_factor_b3 = constrain(factor3, BALANCING_FACTOR_MIN, BALANCING_FACTOR_MAX);
  
  pwm_batt1 = (int)(255 * (balancing_factor_b1 - 0.5) / 1.0);
  pwm_batt2 = (int)(255 * (balancing_factor_b2 - 0.5) / 1.0);
  pwm_batt3 = (int)(255 * (balancing_factor_b3 - 0.5) / 1.0);
  
  pwm_batt1 = constrain(pwm_batt1, 0, 255);
  pwm_batt2 = constrain(pwm_batt2, 0, 255);
  pwm_batt3 = constrain(pwm_batt3, 0, 255);
}

void updatePWMSignals() {
  ledcWrite(0, pwm_batt1);
  ledcWrite(1, pwm_batt2);
  ledcWrite(2, pwm_batt3);
}

void checkProtections() {
  emergency_mode = false;
  system_status = 0;
  
  if (batt1_temp > BATT1_TEMP_MAX) {
    Serial.printf("EMERGENCY: Battery 1 overheat! %.1f°C\n", batt1_temp);
    emergencyStop(1);
    emergency_mode = true;
    system_status = 3;
  }
  if (batt1_percent >= OVERCHARGE_CUTOFF && solar_relay1) setSolarRelay(1, false);
  if (batt1_percent <= DISCHARGE_CUTOFF && load_relay1) setLoadRelay(1, false);
  if (abs(batt1_current) > BATT1_CURRENT_MAX) {
    if (batt1_current > 0) setSolarRelay(1, false);
    else setLoadRelay(1, false);
  }
  
  if (batt2_temp > BATT2_TEMP_MAX) {
    Serial.printf("EMERGENCY: Battery 2 overheat! %.1f°C\n", batt2_temp);
    emergencyStop(2);
    emergency_mode = true;
    system_status = 3;
  }
  if (batt2_percent >= OVERCHARGE_CUTOFF && solar_relay2) setSolarRelay(2, false);
  if (batt2_percent <= DISCHARGE_CUTOFF && load_relay2) setLoadRelay(2, false);
  if (abs(batt2_current) > BATT2_CURRENT_MAX) {
    if (batt2_current > 0) setSolarRelay(2, false);
    else setLoadRelay(2, false);
  }
  
  if (batt3_temp > BATT3_TEMP_MAX) {
    Serial.printf("EMERGENCY: Battery 3 overheat! %.1f°C\n", batt3_temp);
    emergencyStop(3);
    emergency_mode = true;
    system_status = 3;
  }
  if (batt3_percent >= OVERCHARGE_CUTOFF && solar_relay3) setSolarRelay(3, false);
  if (batt3_percent <= DISCHARGE_CUTOFF && load_relay3) setLoadRelay(3, false);
  if (abs(batt3_current) > BATT3_CURRENT_MAX) {
    if (batt3_current > 0) setSolarRelay(3, false);
    else setLoadRelay(3, false);
  }
  
  if (!emergency_mode) {
    if (solar_relay1 || solar_relay2 || solar_relay3) system_status = 1;
    else if (load_relay1 || load_relay2 || load_relay3) system_status = 2;
    else system_status = 0;
  }
}

// ==================== MAIN BALANCING LOGIC WITH CYCLE MANAGEMENT ====================
void balancingLogic() {
  bool isSunny = (ldr_value > 2000);
  
  // Check if all batteries are fully charged
  bool all_charged = (batt1_percent >= CHARGE_COMPLETE_THRESHOLD && 
                      batt2_percent >= CHARGE_COMPLETE_THRESHOLD && 
                      batt3_percent >= CHARGE_COMPLETE_THRESHOLD);
  
  // Check if all batteries are fully discharged
  bool all_empty = (batt1_percent <= DISCHARGE_EMPTY_THRESHOLD && 
                    batt2_percent <= DISCHARGE_EMPTY_THRESHOLD && 
                    batt3_percent <= DISCHARGE_EMPTY_THRESHOLD);
  
  // ==================== CYCLE MANAGEMENT ====================
  if (phase == "CHARGING" && all_charged) {
    cycle_count++;
    Serial.println("\n╔════════════════════════════════════════════════════════════╗");
    Serial.printf("║  CYCLE %d COMPLETED!                                      ║\n", cycle_count);
    Serial.println("╚════════════════════════════════════════════════════════════╝");
    
    if (cycle_count >= max_cycles) {
      Serial.println("\n╔════════════════════════════════════════════════════════════╗");
      Serial.printf("║  ALL %d CYCLES COMPLETED! System finished.              ║\n", max_cycles);
      Serial.println("║  System will now enter IDLE mode.                          ║");
      Serial.println("╚════════════════════════════════════════════════════════════╝");
      phase = "COMPLETED";
    } else {
      Serial.printf("\nStarting DISCHARGE phase for Cycle %d...\n", cycle_count + 1);
      phase = "DISCHARGING";
    }
  }
  
  if (phase == "DISCHARGING" && all_empty) {
    Serial.printf("\nAll batteries discharged. Starting CHARGE phase for Cycle %d...\n", cycle_count + 1);
    phase = "CHARGING";
  }
  
  // ==================== DISCHARGE PHASE ====================
  if (phase == "DISCHARGING") {
    // Find battery with highest percentage to power the load
    float maxPercent = max(batt1_percent, max(batt2_percent, batt3_percent));
    int bestBattery = 1;
    if (batt2_percent == maxPercent) bestBattery = 2;
    if (batt3_percent == maxPercent) bestBattery = 3;
    
    if (maxPercent > DISCHARGE_CUTOFF && !emergency_mode) {
      setLoadRelay(1, (bestBattery == 1));
      setLoadRelay(2, (bestBattery == 2));
      setLoadRelay(3, (bestBattery == 3));
    } else {
      setLoadRelay(1, false);
      setLoadRelay(2, false);
      setLoadRelay(3, false);
    }
    
    // No charging during discharge phase
    setSolarRelay(1, false);
    setSolarRelay(2, false);
    setSolarRelay(3, false);
    pwm_batt1 = 0;
    pwm_batt2 = 0;
    pwm_batt3 = 0;
    updatePWMSignals();
    return;
  }
  
  // ==================== COMPLETED PHASE ====================
  if (phase == "COMPLETED") {
    setLoadRelay(1, false);
    setLoadRelay(2, false);
    setLoadRelay(3, false);
    setSolarRelay(1, false);
    setSolarRelay(2, false);
    setSolarRelay(3, false);
    pwm_batt1 = 0;
    pwm_batt2 = 0;
    pwm_batt3 = 0;
    updatePWMSignals();
    return;
  }
  
  // ==================== CHARGING PHASE ====================
  if (phase == "CHARGING") {
    // === DISCHARGE BALANCING (Load) - same as before ===
    float maxPercent = max(batt1_percent, max(batt2_percent, batt3_percent));
    int bestBattery = 1;
    if (batt2_percent == maxPercent) bestBattery = 2;
    if (batt3_percent == maxPercent) bestBattery = 3;
    
    if (maxPercent > DISCHARGE_CUTOFF && !emergency_mode) {
      setLoadRelay(1, (bestBattery == 1));
      setLoadRelay(2, (bestBattery == 2));
      setLoadRelay(3, (bestBattery == 3));
    } else if (!emergency_mode) {
      setLoadRelay(1, false);
      setLoadRelay(2, false);
      setLoadRelay(3, false);
    }
    
    // === CHARGE BALANCING - ALL BATTERIES CHARGE SIMULTANEOUSLY ===
    if (isSunny && !emergency_mode) {
      // Turn ON all solar relays
      setSolarRelay(1, true);
      setSolarRelay(2, true);
      setSolarRelay(3, true);
      
      // Update dynamic balancing factors
      updateDynamicBalancing();
      
      // Apply PWM signals
      updatePWMSignals();
      
      // Print balancing status when imbalance is significant
      if (imbalance > IMBALANCE_THRESHOLD) {
        Serial.println("DYNAMIC BALANCING ACTIVE (CHARGING PHASE):");
        Serial.printf("   Cycle: %d/%d | Phase: %s\n", cycle_count, max_cycles, phase.c_str());
        Serial.printf("   B1: %.1f%% | Factor: %.2f | PWM: %d\n", 
                      batt1_percent, balancing_factor_b1, pwm_batt1);
        Serial.printf("   B2: %.1f%% | Factor: %.2f | PWM: %d\n", 
                      batt2_percent, balancing_factor_b2, pwm_batt2);
        Serial.printf("   B3: %.1f%% | Factor: %.2f | PWM: %d\n", 
                      batt3_percent, balancing_factor_b3, pwm_batt3);
        Serial.printf("   Target: %.1f%% | Imbalance: %.1f%%\n", avg_percent, imbalance);
      }
    } else if (!emergency_mode) {
      setSolarRelay(1, false);
      setSolarRelay(2, false);
      setSolarRelay(3, false);
      pwm_batt1 = 0;
      pwm_batt2 = 0;
      pwm_batt3 = 0;
      updatePWMSignals();
    }
  }
}

void setLoadRelay(int battery, bool state) {
  int pin;
  bool *relayState;
  switch(battery) {
    case 1: pin = RELAY_LOAD_BATT1; relayState = &load_relay1; break;
    case 2: pin = RELAY_LOAD_BATT2; relayState = &load_relay2; break;
    case 3: pin = RELAY_LOAD_BATT3; relayState = &load_relay3; break;
    default: return;
  }
  if (*relayState != state) {
    digitalWrite(pin, state ? HIGH : LOW);
    *relayState = state;
  }
}

void setSolarRelay(int battery, bool state) {
  int pin;
  bool *relayState;
  switch(battery) {
    case 1: pin = RELAY_SOLAR_BATT1; relayState = &solar_relay1; break;
    case 2: pin = RELAY_SOLAR_BATT2; relayState = &solar_relay2; break;
    case 3: pin = RELAY_SOLAR_BATT3; relayState = &solar_relay3; break;
    default: return;
  }
  if (*relayState != state) {
    digitalWrite(pin, state ? HIGH : LOW);
    *relayState = state;
  }
}

void emergencyStop(int battery) {
  setLoadRelay(battery, false);
  setSolarRelay(battery, false);
  if (battery == 1) pwm_batt1 = 0;
  if (battery == 2) pwm_batt2 = 0;
  if (battery == 3) pwm_batt3 = 0;
  updatePWMSignals();
}

// ==================== SEND TO FIREBASE ====================
void sendToFirebase() {
  if (!Firebase.ready() || !signupOK) {
    Serial.println("⚠️ Firebase not ready");
    return;
  }
  
  Serial.println("\nSending Current Data to Firebase...");
  
  Firebase.RTDB.setFloat(&fbdo, "/current/battery1/voltage", batt1_voltage);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery1/percentage", batt1_percent);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery1/current", batt1_current);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery1/temperature", batt1_temp);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery1/balancing_factor", balancing_factor_b1);
  Firebase.RTDB.setInt(&fbdo, "/current/battery1/pwm_value", pwm_batt1);
  
  Firebase.RTDB.setFloat(&fbdo, "/current/battery2/voltage", batt2_voltage);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery2/percentage", batt2_percent);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery2/current", batt2_current);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery2/temperature", batt2_temp);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery2/balancing_factor", balancing_factor_b2);
  Firebase.RTDB.setInt(&fbdo, "/current/battery2/pwm_value", pwm_batt2);
  
  Firebase.RTDB.setFloat(&fbdo, "/current/battery3/voltage", batt3_voltage);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery3/percentage", batt3_percent);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery3/current", batt3_current);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery3/temperature", batt3_temp);
  Firebase.RTDB.setFloat(&fbdo, "/current/battery3/balancing_factor", balancing_factor_b3);
  Firebase.RTDB.setInt(&fbdo, "/current/battery3/pwm_value", pwm_batt3);
  
  Firebase.RTDB.setFloat(&fbdo, "/current/solar/panel1_current", solar1_current);
  Firebase.RTDB.setFloat(&fbdo, "/current/solar/panel2_current", solar2_current);
  Firebase.RTDB.setFloat(&fbdo, "/current/solar/panel3_current", solar3_current);
  
  Firebase.RTDB.setInt(&fbdo, "/current/system/status", system_status);
  Firebase.RTDB.setBool(&fbdo, "/current/system/emergency_mode", emergency_mode);
  Firebase.RTDB.setFloat(&fbdo, "/current/system/imbalance_percent", imbalance);
  Firebase.RTDB.setInt(&fbdo, "/current/system/last_update", millis());
  Firebase.RTDB.setFloat(&fbdo, "/current/system/avg_percent", avg_percent);
  Firebase.RTDB.setInt(&fbdo, "/current/system/cycle_count", cycle_count);
  Firebase.RTDB.setString(&fbdo, "/current/system/phase", phase);
  
  Firebase.RTDB.setFloat(&fbdo, "/current/environment/ambient_temperature", ambient_temp);
  Firebase.RTDB.setFloat(&fbdo, "/current/environment/humidity", humidity);
  Firebase.RTDB.setInt(&fbdo, "/current/environment/ldr_value", (int)ldr_value);
  
  Serial.println("Current data sent to Firebase (with balancing factors & cycle info)");
}

void saveHistoricalData() {
  if (!Firebase.ready() || !signupOK) {
    Serial.println("⚠️ Cannot save history - Firebase not ready");
    return;
  }
  
  unsigned long timestamp = millis();
  unsigned long currentDay = millis() / (86400000UL);
  String historyPath = "/history/day_" + String(currentDay) + "/" + String(timestamp);
  
  Serial.println("\nSaving Historical Data...");
  
  String b1Path = historyPath + "/battery1";
  Firebase.RTDB.setFloat(&fbdo, b1Path + "/voltage", batt1_voltage);
  Firebase.RTDB.setFloat(&fbdo, b1Path + "/percentage", batt1_percent);
  Firebase.RTDB.setFloat(&fbdo, b1Path + "/current", batt1_current);
  Firebase.RTDB.setFloat(&fbdo, b1Path + "/temperature", batt1_temp);
  
  String b2Path = historyPath + "/battery2";
  Firebase.RTDB.setFloat(&fbdo, b2Path + "/voltage", batt2_voltage);
  Firebase.RTDB.setFloat(&fbdo, b2Path + "/percentage", batt2_percent);
  Firebase.RTDB.setFloat(&fbdo, b2Path + "/current", batt2_current);
  Firebase.RTDB.setFloat(&fbdo, b2Path + "/temperature", batt2_temp);
  
  String b3Path = historyPath + "/battery3";
  Firebase.RTDB.setFloat(&fbdo, b3Path + "/voltage", batt3_voltage);
  Firebase.RTDB.setFloat(&fbdo, b3Path + "/percentage", batt3_percent);
  Firebase.RTDB.setFloat(&fbdo, b3Path + "/current", batt3_current);
  Firebase.RTDB.setFloat(&fbdo, b3Path + "/temperature", batt3_temp);
  
  String sysPath = historyPath + "/system";
  Firebase.RTDB.setInt(&fbdo, sysPath + "/status", system_status);
  Firebase.RTDB.setFloat(&fbdo, sysPath + "/imbalance", imbalance);
  Firebase.RTDB.setInt(&fbdo, sysPath + "/cycle_count", cycle_count);
  Firebase.RTDB.setString(&fbdo, sysPath + "/phase", phase);
  
  Serial.println("Historical data saved");
}

// ==================== DISPLAY UPDATE ====================
void updateDisplay() {
  Serial.println("\n┌─────────────────────────────────────────────────────────────────────────────────┐");
  Serial.println("│                         SYSTEM STATUS (DYNAMIC BALANCING)                       │");
  Serial.println("├─────────────────────────────────────────────────────────────────────────────────┤");
  Serial.printf("│ Cycle: %d/%d | Phase: %-10s | Avg: %.1f%% | Imbalance: %.1f%%                    │\n",
                cycle_count, max_cycles, phase.c_str(), avg_percent, imbalance);
  Serial.println("├─────────────────────────────────────────────────────────────────────────────────┤");
  Serial.printf("│ B1 (3.7V):  %.2fV  │ %5.1f%%  │ %+.2fA  │ %4.1f°C │ Factor: %.2f │ PWM: %3d   │\n",
                batt1_voltage, batt1_percent, batt1_current, batt1_temp, balancing_factor_b1, pwm_batt1);
  Serial.printf("│ B2 (6V):    %.2fV  │ %5.1f%%  │ %+.2fA  │ %4.1f°C │ Factor: %.2f │ PWM: %3d   │\n",
                batt2_voltage, batt2_percent, batt2_current, batt2_temp, balancing_factor_b2, pwm_batt2);
  Serial.printf("│ B3 (12V):   %.2fV  │ %5.1f%%  │ %+.2fA  │ %4.1f°C │ Factor: %.2f │ PWM: %3d   │\n",
                batt3_voltage, batt3_percent, batt3_current, batt3_temp, balancing_factor_b3, pwm_batt3);
  Serial.printf("│ Solar: %.2fA | %.2fA | %.2fA │ LDR: %.0f │ WiFi: %s │ Status: %d │ Ambient: %.1f°C │\n",
                solar1_current, solar2_current, solar3_current, ldr_value, 
                WiFi.isConnected() ? "OK" : "NO", system_status, ambient_temp);
  Serial.println("└─────────────────────────────────────────────────────────────────────────────────┘");
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.printf("C%d/%d %s", cycle_count, max_cycles, phase.substring(0, 4).c_str());
  lcd.setCursor(10, 0);
  lcd.printf("B1:%.0f%%", batt1_percent);
  lcd.setCursor(0, 1);
  lcd.printf("B2:%.0f%% B3:%.0f%%", batt2_percent, batt3_percent);
  
  if (emergency_mode) {
    lcd.setCursor(14, 1);
    lcd.print("E");
  }
}

// ==================== MAIN LOOP ====================
void loop() {
  unsigned long currentTime = millis();
  
  if (!signupOK) {
    connectToFirebase();
  }
  
  // Read sensors every 500ms
  if (currentTime - lastReadTime >= READ_INTERVAL) {
    lastReadTime = currentTime;
    
    readAllSensors();
    updatePercentages();
    checkProtections();
    
    if (!emergency_mode) {
      balancingLogic();
    }
    
    updateDisplay();
    
    if (emergency_mode) {
      digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
      delay(50);
      digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
      delay(50);
    } else {
      digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
    }
  }
  
  // Send current data every 10 seconds
  if (currentTime - lastSendTime >= SEND_INTERVAL) {
    lastSendTime = currentTime;
    if (signupOK) {
      sendToFirebase();
    }
  }
  
  // Save historical data every 60 seconds
  if (currentTime - lastHistoryTime >= HISTORY_INTERVAL) {
    lastHistoryTime = currentTime;
    if (signupOK) {
      saveHistoricalData();
    }
  }
}