/*
  Ender IIoT — ESP32 + DS18B20 (montaje final SIN servo)
  Telemetría exclusiva de temperatura de carpa -> EMQX Cloud por MQTT/TLS.
  Arduino IDE: placa ESP32, OneWire, DallasTemperature y PubSubClient.
  Copiar local_secrets.example.h a local_secrets.h y completar.
  Sketch de referencia reproducible: el prototipo histórico instalado en
  la ESP32 puede diferir y NO debe reemplazarse sin probar primero.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include <math.h>

#include "local_secrets.h"      // No subir este archivo a GitHub
#include "emqx_ca.h"            // Certificado público CA

constexpr int DS18B20_PIN = 4;
constexpr uint32_t SAMPLE_MS = 3000;
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t MQTT_RETRY_MS = 5000;
const char* MQTT_HOST = "ada6f11f.ala.us-east-1.emqxsl.com";
const uint16_t MQTT_PORT = 8883;
const char* MQTT_USER = "esp32-ender";
const char* MQTT_TOPIC = "ecs/ender-esp32/temperatura_carpa";

OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);
WiFiClientSecure tls;
PubSubClient mqtt(tls);

uint32_t lastPublish = 0;
uint32_t lastWifiTry = 0;
uint32_t lastMqttTry = 0;

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry < WIFI_RETRY_MS && lastWifiTry != 0) return;
  lastWifiTry = millis();
  Serial.println("WiFi: conectando...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

bool timeIsReady() {
  // El reloj UTC debe estar sincronizado para validar el certificado TLS.
  return time(nullptr) > 1760000000; // Fecha de referencia anterior a octubre 2026
}

void connectMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED || !timeIsReady()) return;
  if (millis() - lastMqttTry < MQTT_RETRY_MS && lastMqttTry != 0) return;
  lastMqttTry = millis();

  String clientId = String("ender-esp32-") + WiFi.macAddress();
  clientId.replace(":", "");
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
    Serial.println("EMQX: conectado mediante TLS");
  } else {
    Serial.printf("EMQX: fallo MQTT rc=%d\n", mqtt.state());
  }
}

void publishTemperature() {
  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);

  if (t == DEVICE_DISCONNECTED_C || !isfinite(t) || t < -55 || t > 125) {
    Serial.println("DS18B20: lectura no valida; no se publica");
    return;
  }

  char json[256];
  int n = snprintf(json, sizeof(json),
      "{\"value\":%.4f,\"token\":\"%s\"}", t, MQTT_TOKEN);

  if (n <= 0 || n >= int(sizeof(json))) {
    Serial.println("MQTT: payload demasiado largo");
    return;
  }

  // PubSubClient publica con QoS 0; el listener Django acepta la lectura.
  bool ok = mqtt.publish(MQTT_TOPIC, json, false);
  Serial.printf("Carpa: %.4f C | MQTT: %s\n", t, ok ? "OK" : "ERROR");
}

void setup() {
  Serial.begin(115200);
  sensors.begin();
  sensors.setResolution(12);

  tls.setCACert(EMQX_ROOT_CA);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);

  configTime(0, 0, "pool.ntp.org", "time.google.com");
  connectWifi();
  Serial.println("Ender IIoT — nodo de temperatura de carpa");
}

void loop() {
  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    if (!timeIsReady()) {
      // SNTP se sincroniza tras adquirir la red. Sin reloj válido no hay TLS.
    } else {
      connectMqtt();
      if (mqtt.connected()) {
        mqtt.loop();
        if (millis() - lastPublish >= SAMPLE_MS || lastPublish == 0) {
          lastPublish = millis();
          publishTemperature();
        }
      }
    }
  }
  delay(10);
}
