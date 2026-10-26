/*
  Temperatura inteligente - Ender 3 Pro, version 3.5 + MQTT Hito 4.
  PROGRAMA COMPLETO EN UN SOLO ARCHIVO .ino.

  Incluye servo, DS18B20, OLED, luz, pagina web y Google Sheets.
  Tras llegar al angulo de apagado espera 5 segundos y vuelve a reposo.
  Compatible con Servidor_Temperatura.gs v3 y ArduinoJson 7.

  Abre este archivo en una carpeta nueva. No agregues el Temperatura.cpp
  de la version anterior: todo el programa ya esta incluido aqui.
  Selecciona tu placa ESP32 Dev Module y el puerto de la ESP32.

  Correcciones de compilacion: tipos y declaraciones explicitas al inicio;
  HTML/JavaScript y certificados como cadenas C++ entre comillas normales.
  Correccion de registro: una sola referencia de tiempo por ciclo evita
  repetir consultas de configuracion y retrasar las muestras de temperatura.
  Al apagar se pausa el registro en Google y se guarda la pausa en memoria.
  Tras encender a mano, pulsa Reanudar registro o crea una nueva impresion.
  Reanudar usa la impresion ya conocida sin esperar otra consulta a Google.
  La web distingue Registro activo de una espera real y limita cada peticion.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "emqx_ca.h"
#include "local_secrets.h"
#include <WebServer.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <esp_system.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Tipos disponibles antes de cualquier funcion o declaracion generada por Arduino.
const uint8_t NUBE_CONFIG = 0, NUBE_MUESTRA = 1, NUBE_USUARIO = 2;
const size_t MAX_RESPUESTA_NUBE = 8192;
struct SolicitudNube { uint8_t tipo; uint32_t numero; char url[320]; char cuerpo[1500]; };
struct RespuestaNube { uint8_t tipo; uint32_t numero; char cuerpo[MAX_RESPUESTA_NUBE]; };
struct MuestraNube {
  float temp; uint32_t epoch; uint32_t muestras;
  char ip[16]; char impresion[101]; char id[70];
};

// Declaraciones explicitas: Arduino no necesita deducir estos prototipos.
bool enteroValido(String texto, long minimo, long maximo, long& resultado);
bool realValido(String texto, float& resultado);
bool urlGestorValida(const String& url);
bool relojValido();
void errorTransporte(char* destino, const String& mensaje);
void tareaGoogle(void*);
bool prepararSolicitud(JsonDocument& doc, uint8_t tipo, uint32_t numero, SolicitudNube& req);
bool enviarSolicitud(JsonDocument& doc, uint8_t tipo);
void quitarPrimeraMuestra();
void descartarImpresion(const String& id);
bool guardarPausaRegistro(bool pausa);
void pausarRegistro();
bool reanudarRegistro();
void aplicarConfigNube(JsonDocument& doc, uint32_t ahora);
void capturarMuestraNube(uint32_t ahora);
void procesarNube(uint32_t ahora);
String estadoNubeJSON();
void handleAccionSheets();
void handleResultadoSheets();
void iniciarNube();
void cargarConfig();
bool guardarConfig();
String estadoControl();
String estadoTermico();
void actualizarServo(uint32_t ahora);
void registrarSample(uint32_t ahora);
void actualizarTemperatura(uint32_t ahora);
void actualizarOLED(uint32_t ahora);
void actualizarWiFi(uint32_t ahora);
void conectarMQTT(uint32_t ahora);
void publicarTemperaturaMQTT(uint32_t ahora);
void actualizarMQTT(uint32_t ahora);
String jsonString(const String& texto);
void enviarError(int codigo, const String& mensaje);
bool autorizarCambio(bool requiereReposo);
void redirigir();
String generarHTML();
void handleStatus();
void handleHistory();
void handleCSV();
void handleConfigServo();
void handleConfigAuto();
void handleConfigSheets();
void iniciarServidor();
void setup();
void loop();

// WIFI y secretos privados se cargan desde local_secrets.h.
// Ese archivo es local y no debe subirse a GitHub.

// ===== MQTT - Tarea 2 IoT =====
// Broker EMQX Cloud Serverless. MQTT cifrado por TLS.
const char* MQTT_HOST = "ada6f11f.ala.us-east-1.emqxsl.com";
const uint16_t MQTT_PORT = 8883;
const char* MQTT_USER = "esp32-ender";

// Debe coincidir con el dispositivo creado en Django.
const char* MQTT_DEVICE = "ender-esp32";

const uint32_t INTERVALO_RECONEXION_MQTT = 5000UL;
const uint32_t INTERVALO_PUBLICACION_MQTT = 3000UL;
// Raíces públicas Google Trust Services R1, R3 y R4 del almacén CA del sistema.
const char GOOGLE_ROOT_CA[] PROGMEM = 
"-----BEGIN CERTIFICATE-----\n"
"MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
"CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
"MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
"MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
"Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
"A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
"27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
"Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
"TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
"qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
"szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
"Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
"MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
"wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
"aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
"VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
"AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
"FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
"C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
"QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
"h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
"7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
"ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
"MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
"Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n"
"6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
"0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
"2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
"bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
"-----END CERTIFICATE-----\n"
"-----BEGIN CERTIFICATE-----\n"
"MIICCTCCAY6gAwIBAgINAgPluILrIPglJ209ZjAKBggqhkjOPQQDAzBHMQswCQYD\n"
"VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
"A1UEAxMLR1RTIFJvb3QgUjMwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
"WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
"IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjMwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
"AAQfTzOHMymKoYTey8chWEGJ6ladK0uFxh1MJ7x/JlFyb+Kf1qPKzEUURout736G\n"
"jOyxfi//qXGdGIRFBEFVbivqJn+7kAHjSxm65FSWRQmx1WyRRK2EE46ajA2ADDL2\n"
"4CejQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
"BBTB8Sa6oC2uhYHP0/EqEr24Cmf9vDAKBggqhkjOPQQDAwNpADBmAjEA9uEglRR7\n"
"VKOQFhG/hMjqb2sXnh5GmCCbn9MN2azTL818+FsuVbu/3ZL3pAzcMeGiAjEA/Jdm\n"
"ZuVDFhOD3cffL74UOO0BzrEXGhF16b0DjyZ+hOXJYKaV11RZt+cRLInUue4X\n"
"-----END CERTIFICATE-----\n"
"-----BEGIN CERTIFICATE-----\n"
"MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
"VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
"A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
"WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
"IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
"AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
"QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
"HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
"BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
"9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
"p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
"-----END CERTIFICATE-----\n";

#define DS18B20_PIN 4
#define SDA_PIN 21
#define SCL_PIN 22
#define SERVO_PIN 25
#define LED_PIN 23

// BEGIN_CONTROL_CORE
// Núcleo independiente de Arduino: se prueba también con reloj simulado.
class ControlImpresora {
public:
  enum Fase { REPOSO, APAGANDO, ESPERANDO, RETORNANDO };
  int reposo = 140, apagado = 40, msPorGrado = 8;
  float tempArmar = 27.0f, tempApagar = 21.0f;
  uint32_t msArmar = 300000UL, msFrio = 180000UL;
  static constexpr uint32_t ESPERA_MS = 5000UL;

  Fase fase = REPOSO;
  int posicion = 140, destino = 140;
  bool armado = false, esperandoBajada = false;
  bool contandoCalor = false, contandoFrio = false;
  bool huboApagado = false, ultimoFueAutomatico = false;
  uint32_t inicioCalor = 0, inicioFrio = 0, inicioEspera = 0;
  uint32_t ultimoPaso = 0, ultimaLectura = 0;
  bool hayLecturaPrevia = false;

  static uint32_t transcurrido(uint32_t ahora, uint32_t antes) {
    return uint32_t(ahora - antes); // Incluye desbordamiento de millis().
  }
  void borrarTiempos() {
    contandoCalor = false;
    contandoFrio = false;
  }
  bool ocupado() const { return fase != REPOSO; }
  void iniciar(uint32_t ahora) {
    fase = REPOSO;
    posicion = destino = reposo;
    ultimoPaso = ahora;
    armado = esperandoBajada = huboApagado = false;
    ultimoFueAutomatico = false;
    hayLecturaPrevia = false;
    borrarTiempos();
  }
  bool reiniciarAutomatico() {
    if (ocupado()) return false;
    armado = esperandoBajada = false;
    borrarTiempos();
    return true;
  }
  bool pedirApagado(uint32_t ahora, bool automatico) {
    return pedirMovimiento(ahora, apagado, automatico);
  }
  bool pedirMovimiento(uint32_t ahora, int objetivo, bool automatico) {
    if (ocupado() || objetivo < 0 || objetivo > 180) return false;
    armado = false;
    esperandoBajada = true;
    borrarTiempos();
    huboApagado = true;
    ultimoFueAutomatico = automatico;
    destino = objetivo;
    ultimoPaso = ahora;
    fase = APAGANDO;
    if (posicion == destino) {
      fase = ESPERANDO;
      inicioEspera = ahora;
    }
    return true;
  }
  bool avanzarServo(uint32_t ahora) {
    // Devuelve true únicamente si hay un nuevo ángulo que enviar al servo.
    if (fase == ESPERANDO) {
      if (transcurrido(ahora, inicioEspera) < ESPERA_MS) return false;
      destino = reposo;
      fase = RETORNANDO;
      ultimoPaso = ahora - uint32_t(msPorGrado);
    }
    if (fase != APAGANDO && fase != RETORNANDO) return false;
    if (transcurrido(ahora, ultimoPaso) < uint32_t(msPorGrado)) return false;
    ultimoPaso = ahora;
    const int anterior = posicion;
    if (posicion < destino) ++posicion;
    else if (posicion > destino) --posicion;
    if (posicion == destino) {
      if (fase == APAGANDO) {
        fase = ESPERANDO;
        inicioEspera = ahora;
      } else {
        fase = REPOSO;
      }
    }
    return posicion != anterior;
  }
  void evaluarTemperatura(uint32_t ahora, bool valida, float valor) {
    // No sumar tiempo desconectado ni huecos largos entre mediciones.
    if (hayLecturaPrevia && transcurrido(ahora, ultimaLectura) > 2500UL)
      borrarTiempos();
    ultimaLectura = ahora;
    hayLecturaPrevia = true;
    if (!valida) {
      borrarTiempos();
      return;
    }
    // Tras apagar, exigir bajar de T_armar antes de otro ciclo caliente.
    // Evita volver a armarse solo por el calor residual de la impresión anterior.
    if (esperandoBajada && valor < tempArmar) esperandoBajada = false;
    if (ocupado() || esperandoBajada) {
      borrarTiempos();
      return;
    }
    if (!armado) {
      contandoFrio = false;
      if (valor < tempArmar) {
        contandoCalor = false;
      } else {
        if (!contandoCalor) {
          contandoCalor = true;
          inicioCalor = ahora;
        }
        if (transcurrido(ahora, inicioCalor) >= msArmar) {
          armado = true;
          contandoCalor = false;
        }
      }
      return;
    }
    if (valor > tempApagar) {
      contandoFrio = false;
    } else {
      if (!contandoFrio) {
        contandoFrio = true;
        inicioFrio = ahora;
      }
      if (transcurrido(ahora, inicioFrio) >= msFrio)
        pedirApagado(ahora, true);
    }
  }
};
// END_CONTROL_CORE

ControlImpresora control;
WebServer server(80);
OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
Servo servoPower;
Preferences prefs;

WiFiClientSecure mqttWiFiClient;
PubSubClient mqttClient(mqttWiFiClient);
uint32_t ultimoIntentoMQTT = 0;
uint32_t ultimaPublicacionMQTT = 0;

bool oledOK = false, servoOK = false, ledEncendido = false;
bool sensorOK = false, conversionPendiente = false;
float temperatura = NAN;
uint32_t inicioConversion = 0, ultimaPeticionSensor = 0;
uint32_t ultimoOLED = 0, ultimoWiFi = 0, ultimoSample = 0;
uint32_t anteriorMillis = 0;
uint64_t uptimeMs = 0;
bool wifiEstabaConectado = false;
String csrfToken;
String sheetsURL;
String ultimoMovimiento = "Reposo inicial";

const int MAX_SAMPLES = 720;
const uint32_t SAMPLE_INTERVAL_MS = 30000UL;
float tempSamples[MAX_SAMPLES];
uint32_t segundosSamples[MAX_SAMPLES];
int sampleCount = 0, sampleIndex = 0;
float tempMin = 0, tempMax = 0;
double tempSum = 0;
uint32_t tempCount = 0;

bool enteroValido(String texto, long minimo, long maximo, long& resultado) {
  texto.trim();
  if (texto.length() == 0 || texto.length() > 16) return false;
  char* fin;
  errno = 0;
  long v = strtol(texto.c_str(), &fin, 10);
  if (errno || *fin || fin == texto.c_str() || v < minimo || v > maximo) return false;
  resultado = v;
  return true;
}

bool realValido(String texto, float& resultado) {
  texto.trim();
  if (texto.length() == 0 || texto.length() > 16) return false;
  char* fin;
  errno = 0;
  float v = strtof(texto.c_str(), &fin);
  if (errno || *fin || fin == texto.c_str() || !isfinite(v) || v < -50 || v > 125)
    return false;
  resultado = v;
  return true;
}

bool urlGestorValida(const String& url) {
  if (url.length() == 0) return true; // Vacío desactiva el enlace.
  const String prefijo = "https://script.google.com/macros/s/";
  if (url.length() > 300 || !url.startsWith(prefijo) || !url.endsWith("/exec"))
    return false;
  int fin = int(url.length()) - 5;
  if (fin <= int(prefijo.length())) return false;
  for (int i = prefijo.length(); i < fin; ++i) {
    char c = url[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}


// Google Sheets v3. Solo loop() toca control, WebServer, NVS y cola de muestras.
// La tarea de red usa COPIAS por colas FreeRTOS; HTTPS no bloquea el servo.
QueueHandle_t colaSolicitudes = nullptr, colaRespuestas = nullptr;
bool tareaNubeOK = false, nubeOcupada = false;
SolicitudNube solicitudUsuario;
bool usuarioPendiente = false, usuarioEnCurso = false;
uint32_t contadorOperacion = 0, ultimaOperacion = 0;
String respuestaUsuario = "{}", bootID, accionUsuario;
bool configNubeConocida = false, registroActivo = false;
// La validez de la sesion en Google y la pausa local son estados distintos.
// En la primera carga, esperar a que el usuario confirme el inicio del registro.
bool registroPausado = true, memoriaRegistroOK = true;
bool transicionRegistroPendiente = false;
uint32_t revisionRegistro = 0, revisionSolicitudUsuario = 0;
String idImpresion, nombreImpresion, mensajeNube = "Falta configurar URL de Apps Script";
String estadoEstabilidad = "Sin datos";
const int MAX_PENDIENTES = 30;
MuestraNube pendientes[MAX_PENDIENTES];
int cabezaPendientes = 0, cantidadPendientes = 0;
uint32_t ultimoRegistroNube = 0, ultimoConfigNube = 0, ultimoIntentoNube = 0;
uint32_t muestrasGeneradas = 0, muestrasConfirmadas = 0, muestrasPerdidas = 0;
uint32_t ultimaMuestraEnviada = 0;
uint32_t esperaReintento = 0;
bool primeraMuestra = true;

class SalidaAcotada : public Stream {
  char* datos; size_t capacidad; size_t usados = 0;
public:
  bool excedido = false;
  SalidaAcotada(char* d, size_t n) : datos(d), capacidad(n) { datos[0] = '\0'; }
  size_t write(uint8_t c) override {
    if (usados + 1 >= capacidad) { excedido = true; return 0; }
    datos[usados++] = char(c); datos[usados] = '\0'; return 1;
  }
  size_t write(const uint8_t* d, size_t n) override {
    size_t i = 0; while (i < n && write(d[i])) ++i; return i;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

bool relojValido() { return time(nullptr) >= 1704067200; }

void errorTransporte(char* destino, const String& mensaje) {
  JsonDocument d;
  d["api_version"] = 3; d["ok"] = false; d["codigo"] = "TRANSPORTE";
  d["error"] = mensaje; d["permanente"] = false;
  serializeJson(d, destino, MAX_RESPUESTA_NUBE);
}

void tareaGoogle(void*) {
  SolicitudNube req;
  static RespuestaNube res; // Fuera de la pila de la tarea.
  for (;;) {
    if (xQueueReceive(colaSolicitudes, &req, portMAX_DELAY) != pdTRUE) continue;
    res.tipo = req.tipo; res.numero = req.numero; res.cuerpo[0] = '\0';
    if (WiFi.status() != WL_CONNECTED || !relojValido()) {
      errorTransporte(res.cuerpo, "Sin Wi-Fi o sin hora sincronizada. El control local sigue activo.");
    } else {
      WiFiClientSecure cliente;
      cliente.setCACert(GOOGLE_ROOT_CA);
      cliente.setHandshakeTimeout(10);
      HTTPClient http;
      http.setConnectTimeout(8000); http.setTimeout(15000);
      http.setReuse(false); http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
      const char* cabeceras[] = {"Location"};
      if (!http.begin(cliente, String(req.url))) {
        errorTransporte(res.cuerpo, "No se pudo iniciar HTTPS.");
      } else {
        http.collectHeaders(cabeceras, 1);
        http.addHeader("Content-Type", "application/json");
        int codigo = http.POST(String(req.cuerpo));
        if (codigo == 302 || codigo == 303) {
          String redireccion = http.header("Location");
          http.end();
          // ContentService entrega JSON por GET en una URL temporal de Google.
          // No reenviar la clave ni el POST a la redirección.
          if (!redireccion.startsWith("https://script.googleusercontent.com/")) {
            errorTransporte(res.cuerpo, "Google pide iniciar sesion: revisa acceso Cualquier persona y URL /exec.");
            codigo = -1000;
          } else if (http.begin(cliente, redireccion)) {
            http.setConnectTimeout(8000); http.setTimeout(15000); http.setReuse(false);
            codigo = http.GET();
          } else codigo = -1001;
        }
        if (codigo == 200) {
          SalidaAcotada salida(res.cuerpo, MAX_RESPUESTA_NUBE);
          if (http.getSize() >= int(MAX_RESPUESTA_NUBE)) {
            errorTransporte(res.cuerpo, "Respuesta de Google demasiado grande.");
          } else {
            int recibidos = http.writeToStream(&salida);
            if (salida.excedido || recibidos < 0 || !res.cuerpo[0])
              errorTransporte(res.cuerpo, "Respuesta incompleta de Google. Se reintentara el registro sin duplicarlo.");
          }
        } else if (!res.cuerpo[0]) {
          errorTransporte(res.cuerpo, "Error HTTPS " + String(codigo) + ". Revisa Internet, URL /exec, permisos y certificados.");
        }
      }
      http.end();
    }
    xQueueSend(colaRespuestas, &res, portMAX_DELAY);
  }
}

bool prepararSolicitud(JsonDocument& doc, uint8_t tipo, uint32_t numero, SolicitudNube& req) {
  doc["key"] = SHEETS_SECRET_KEY;
  if (measureJson(doc) >= sizeof(req.cuerpo) || sheetsURL.length() >= sizeof(req.url)) return false;
  req.tipo = tipo; req.numero = numero;
  snprintf(req.url, sizeof(req.url), "%s", sheetsURL.c_str());
  serializeJson(doc, req.cuerpo, sizeof(req.cuerpo));
  return true;
}

bool enviarSolicitud(JsonDocument& doc, uint8_t tipo) {
  if (nubeOcupada || !tareaNubeOK) return false;
  SolicitudNube req;
  if (!prepararSolicitud(doc, tipo, 0, req)) return false;
  if (xQueueSend(colaSolicitudes, &req, 0) != pdTRUE) return false;
  nubeOcupada = true; return true;
}

void quitarPrimeraMuestra() {
  if (cantidadPendientes) {
    cabezaPendientes = (cabezaPendientes + 1) % MAX_PENDIENTES;
    --cantidadPendientes;
  }
}

void descartarImpresion(const String& id) {
  const int cantidad = cantidadPendientes;
  for (int i = 0; i < cantidad; ++i) {
    MuestraNube m = pendientes[cabezaPendientes];
    quitarPrimeraMuestra();
    if (id != String(m.impresion)) {
      pendientes[(cabezaPendientes + cantidadPendientes) % MAX_PENDIENTES] = m;
      ++cantidadPendientes;
    }
  }
}

bool guardarPausaRegistro(bool pausa) {
  if (!prefs.begin("config", false)) { memoriaRegistroOK = false; return false; }
  const int valor = pausa ? 1 : 0;
  if (prefs.getInt("reg_pause", -1) != valor) prefs.putInt("reg_pause", valor);
  memoriaRegistroOK = prefs.getInt("reg_pause", -1) == valor;
  prefs.end();
  return memoriaRegistroOK;
}

void pausarRegistro() {
  registroPausado = true;
  ++revisionRegistro; // Un apagado posterior tiene prioridad sobre una respuesta tardia.
  guardarPausaRegistro(true);
}

bool reanudarRegistro() {
  if (!registroPausado) return true; // Repetir el clic no genera otra muestra inmediata.
  if (!guardarPausaRegistro(false)) { registroPausado = true; return false; }
  registroPausado = false;
  primeraMuestra = true;
  esperaReintento = 0;
  return true;
}

void aplicarConfigNube(JsonDocument& doc, uint32_t ahora) {
  if (!doc["config"].is<JsonObject>()) return;
  String id = doc["config"]["id_impresion"].as<String>();
  String nombre = doc["config"]["nombre_impresion"].as<String>();
  if (id.length() == 0 || id.length() > 100 || nombre.length() > 320) return;
  if (id != idImpresion) { primeraMuestra = true; estadoEstabilidad = "Sin datos"; }
  idImpresion = id; nombreImpresion = nombre;
  registroActivo = doc["config"]["activa"] | false;
  if (!registroActivo) estadoEstabilidad = "Registro detenido";
  configNubeConocida = true;
  // Usar el mismo instante que procesarNube. Un millis() posterior puede
  // hacer que ahora - ultimoConfigNube desborde y repita la consulta.
  ultimoConfigNube = ahora;
}

void capturarMuestraNube(uint32_t ahora) {
  if (registroPausado || transicionRegistroPendiente) return;
  if (!sheetsURL.length() || !configNubeConocida || !registroActivo || !sensorOK || !relojValido()) return;
  if (!primeraMuestra && uint32_t(ahora - ultimoRegistroNube) < 60000UL) return;
  primeraMuestra = false; ultimoRegistroNube = ahora;
  if (cantidadPendientes == MAX_PENDIENTES) { ++muestrasPerdidas; return; }
  MuestraNube& m = pendientes[(cabezaPendientes + cantidadPendientes) % MAX_PENDIENTES];
  m.temp = temperatura; m.epoch = uint32_t(time(nullptr)); m.muestras = tempCount;
  snprintf(m.impresion, sizeof(m.impresion), "%s", idImpresion.c_str());
  snprintf(m.ip, sizeof(m.ip), "%s", WiFi.localIP().toString().c_str());
  String id = bootID + "_" + String(++muestrasGeneradas);
  snprintf(m.id, sizeof(m.id), "%s", id.c_str());
  ++cantidadPendientes;
}

void procesarNube(uint32_t ahora) {
  if (!tareaNubeOK) return;
  static RespuestaNube res;
  if (xQueueReceive(colaRespuestas, &res, 0) == pdTRUE) {
    nubeOcupada = false;
    JsonDocument d;
    if (deserializeJson(d, res.cuerpo) || (d["api_version"] | 0) != 3) {
      d.clear(); d["ok"] = false; d["codigo"] = "VERSION";
      d["error"] = "La respuesta no corresponde al Apps Script v3. Actualiza la implementacion y revisa URL /exec.";
    }
    bool ok = d["ok"] | false;
    if (ok) {
      aplicarConfigNube(d, ahora); esperaReintento = 0;
      mensajeNube = "Conectado a Google Sheets";
    } else {
      mensajeNube = d["error"] | "Error de Google Sheets";
      esperaReintento = esperaReintento ? min(esperaReintento * 2, uint32_t(300000)) : 15000UL;
      ultimoIntentoNube = ahora;
    }
    if (res.tipo == NUBE_MUESTRA && cantidadPendientes) {
      if (ok && String(pendientes[cabezaPendientes].id) == d["id_muestra"].as<String>()) {
        ultimaMuestraEnviada = pendientes[cabezaPendientes].epoch;
        ++muestrasConfirmadas; quitarPrimeraMuestra();
        if (d["id_impresion"].as<String>() == idImpresion && registroActivo)
          estadoEstabilidad = d["estado"] | "Dato guardado";
        if (d["aviso_correo"].is<const char*>()) mensajeNube = d["aviso_correo"].as<String>();
      } else if (!ok && (d["permanente"] | false)) {
        String codigo = d["codigo"] | "";
        // Credenciales/configuración corregibles: conservar la cola para reintentar.
        if (codigo == "IMPRESION_BORRADA") {
          String eliminada = pendientes[cabezaPendientes].impresion;
          descartarImpresion(eliminada);
          if (idImpresion == eliminada) { registroActivo = false; configNubeConocida = false; }
        } else if (codigo == "DATO_INVALIDO" || codigo == "CONFLICTO" || codigo == "SESION_DESCONOCIDA") {
          ++muestrasPerdidas; quitarPrimeraMuestra();
          if (codigo == "SESION_DESCONOCIDA") configNubeConocida = false;
        }
      } else if (ok) {
        mensajeNube = "Google no confirmo el ID de la muestra; se conserva para reintentar.";
        esperaReintento = 15000; ultimoIntentoNube = ahora;
      }
    }
    if (res.tipo == NUBE_USUARIO) {
      usuarioEnCurso = false; ultimaOperacion = res.numero;
      const bool iniciaRegistro = accionUsuario == "nueva" || accionUsuario == "reanudar";
      transicionRegistroPendiente = false;
      if (!ok && (accionUsuario == "nueva" || accionUsuario == "borrar")) {
        // Una respuesta perdida no demuestra que Google no aplicó el cambio.
        configNubeConocida = false; registroActivo = false;
      }
      if (ok && d["filas_borradas"].is<int>()) descartarImpresion(d["id"].as<String>());
      if (ok && iniciaRegistro) {
        if (!d["config"].is<JsonObject>() || !configNubeConocida || !registroActivo) {
          d["ok"] = false;
          d["error"] = "No hay una impresion disponible para reanudar. Crea una nueva impresion.";
        } else if (revisionSolicitudUsuario != revisionRegistro) {
          // Google puede haber creado la sesion, pero el apagado mantiene la pausa.
          d["aviso_registro"] = "Impresion seleccionada; registro en pausa porque se ordeno un apagado. Enciende la Ender y pulsa Reanudar registro.";
        } else if (!reanudarRegistro()) {
          d["ok"] = false;
          d["error"] = "La impresion esta seleccionada, pero no se pudo guardar el estado del registro. Vuelve a pulsar Reanudar registro.";
        }
      }
      d["registro_pausado"] = registroPausado;
      respuestaUsuario = ""; serializeJson(d, respuestaUsuario);
    }
  }
  capturarMuestraNube(ahora);
  if (nubeOcupada || WiFi.status() != WL_CONNECTED || !relojValido() || !sheetsURL.length()) return;
  if (usuarioPendiente) {
    if (xQueueSend(colaSolicitudes, &solicitudUsuario, 0) == pdTRUE) {
      usuarioPendiente = false; usuarioEnCurso = true; nubeOcupada = true;
    }
    return;
  }
  if (esperaReintento && uint32_t(ahora - ultimoIntentoNube) < esperaReintento) return;
  if (!configNubeConocida || uint32_t(ahora - ultimoConfigNube) >= 60000UL) {
    JsonDocument d; d["accion"] = "config";
    enviarSolicitud(d, NUBE_CONFIG); return;
  }
  // Conservar muestras previas sin enviar mas temperaturas durante la pausa.
  // Una solicitud ya entregada a la tarea de red puede terminar y confirmarse.
  if (registroPausado || transicionRegistroPendiente) return;
  if (cantidadPendientes) {
    const MuestraNube& m = pendientes[cabezaPendientes];
    JsonDocument d; d["accion"] = "registrar";
    d["id_muestra"] = m.id; d["id_impresion"] = m.impresion;
    d["temperatura_c"] = m.temp; d["epoch_s"] = m.epoch;
    d["muestras_locales"] = m.muestras; d["ip_esp32"] = m.ip;
    enviarSolicitud(d, NUBE_MUESTRA);
  }
}

String estadoNubeJSON() {
  JsonDocument d;
  d["configurada"] = sheetsURL.length() > 0;
  d["hora_ok"] = relojValido(); d["wifi"] = WiFi.status() == WL_CONNECTED;
  d["tarea_ok"] = tareaNubeOK;
  d["sensor_ok"] = sensorOK;
  d["activa"] = registroActivo; d["conocida"] = configNubeConocida;
  d["pausado"] = registroPausado;
  d["cambiando"] = transicionRegistroPendiente;
  d["memoria_ok"] = memoriaRegistroOK;
  d["servo_ocupado"] = control.ocupado();
  d["id"] = idImpresion; d["nombre"] = nombreImpresion;
  d["pendientes"] = cantidadPendientes; d["perdidas"] = muestrasPerdidas;
  d["enviadas"] = muestrasConfirmadas; d["ultima"] = ultimaMuestraEnviada;
  d["estado"] = mensajeNube; d["estabilidad"] = estadoEstabilidad;
  String s; serializeJson(d, s); return s;
}

void handleAccionSheets() {
  if (!autorizarCambio(false)) return;
  if (!sheetsURL.length() || !tareaNubeOK) {
    enviarError(503, "Configura la URL de Apps Script y comprueba el estado de Google Sheets."); return;
  }
  if (usuarioPendiente || usuarioEnCurso) { enviarError(409, "Hay otra consulta a Google en curso."); return; }
  String accion = server.arg("accion");
  JsonDocument d; d["accion"] = accion;
  const bool iniciaRegistro = accion == "nueva" || accion == "reanudar";
  if (iniciaRegistro && control.ocupado()) {
    enviarError(409, "Espera a que termine el movimiento del servo antes de iniciar el registro."); return;
  }
  if (accion == "reanudar" && configNubeConocida) {
    // La pausa pertenece a la ESP32. No hace falta esperar a Google otra vez.
    // Solo se reutiliza una sesion confirmada; las nuevas se siguen creando en Google.
    if (!registroActivo) {
      enviarError(409, "La impresion actual fue borrada. Crea e inicia una nueva impresion."); return;
    }
    if (!reanudarRegistro()) {
      enviarError(500, "No se pudo guardar el estado del registro. El registro sigue en pausa."); return;
    }
    d.clear(); d["ok"] = true; d["registro_pausado"] = registroPausado;
    d["config"]["id_impresion"] = idImpresion;
    d["config"]["nombre_impresion"] = nombreImpresion;
    d["config"]["activa"] = registroActivo;
    String resultado; serializeJson(d, resultado);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", resultado);
    return;
  }
  if (WiFi.status() != WL_CONNECTED || !relojValido()) {
    enviarError(503, "Espera conexion Wi-Fi y hora NTP para consultar la impresion en Google."); return;
  }
  if (accion == "listar") {
    long pagina;
    if (!enteroValido(server.arg("pagina"), 0, 100000, pagina)) { enviarError(400, "Pagina invalida."); return; }
    d["pagina"] = pagina;
  } else if (accion == "preparar_borrado" || accion == "borrar") {
    if (server.arg("id").length() > 100 || server.arg("token").length() > 100) { enviarError(400, "ID invalido."); return; }
    d["id"] = server.arg("id");
    if (accion == "borrar") d["token"] = server.arg("token");
  } else if (accion == "nueva") {
    String nombre = server.arg("nombre"); nombre.trim();
    if (!nombre.length() || nombre.length() > 320) { enviarError(400, "Nombre invalido."); return; }
    d["nombre"] = nombre;
  } else if (accion == "reanudar") {
    // La sesion vigente se obtiene de Google, sin crear otro ID ni mover el servo.
    d["accion"] = "config";
  } else if (accion != "config") { enviarError(400, "Accion no disponible."); return; }
  ++contadorOperacion;
  if (contadorOperacion > 2147483647UL) contadorOperacion = 1;
  d["operacion_id"] = "WEB_" + bootID + "_" + String(contadorOperacion);
  if (!prepararSolicitud(d, NUBE_USUARIO, contadorOperacion, solicitudUsuario)) {
    enviarError(400, "Solicitud demasiado grande."); return;
  }
  accionUsuario = accion;
  revisionSolicitudUsuario = revisionRegistro;
  transicionRegistroPendiente = iniciaRegistro;
  if (accion == "nueva") {
    // Desde este clic no capturar mas temperaturas con el nombre anterior.
    configNubeConocida = false;
    registroActivo = false;
  }
  usuarioPendiente = true;
  server.send(202, "application/json", "{\"job\":" + String(contadorOperacion) + "}");
}

void handleResultadoSheets() {
  long numero;
  if (!enteroValido(server.arg("id"), 1, 2147483647L, numero)) { enviarError(400, "Operacion invalida."); return; }
  server.sendHeader("Cache-Control", "no-store");
  if (uint32_t(numero) == ultimaOperacion) {
    server.send(200, "application/json; charset=utf-8", "{\"pendiente\":false,\"resultado\":" + respuestaUsuario + "}");
  } else if (uint32_t(numero) == contadorOperacion && (usuarioPendiente || usuarioEnCurso)) {
    server.send(200, "application/json", "{\"pendiente\":true}");
  } else enviarError(404, "Consulta caducada. Actualiza la lista antes de repetir una operacion.");
}

void iniciarNube() {
  bootID = String(esp_random(), HEX) + String(esp_random(), HEX) + String(esp_random(), HEX) + String(esp_random(), HEX);
  configTime(0, 0, "time.google.com", "pool.ntp.org");
  colaSolicitudes = xQueueCreate(1, sizeof(SolicitudNube));
  colaRespuestas = xQueueCreate(1, sizeof(RespuestaNube));
  if (colaSolicitudes && colaRespuestas) {
    tareaNubeOK = xTaskCreate(tareaGoogle, "GoogleSheets", 16384, nullptr, 1, nullptr) == pdPASS;
  }
  if (!tareaNubeOK) mensajeNube = "No se pudo iniciar la tarea de registro. El control local sigue activo.";
  else if (sheetsURL.length()) mensajeNube = "Esperando Wi-Fi, hora NTP y configuracion de Google";
}

void cargarConfig() {
  if (prefs.begin("config", true)) {
    registroPausado = prefs.getInt("reg_pause", 1) != 0;
    control.reposo = prefs.getInt("a_on", 140); // Clave original; ya no enciende.
    control.apagado = prefs.getInt("a_off", 40);
    control.msPorGrado = prefs.getInt("vel", 8);
    control.tempArmar = prefs.getFloat("t_armar", 27.0);
    control.tempApagar = prefs.getFloat("t_apagar", 21.0);
    control.msArmar = prefs.getULong("ms_armar", 300000UL);
    control.msFrio = prefs.getULong("ms_frio", 180000UL);
    sheetsURL = prefs.getString("sheets_exec", "");
    prefs.end();
  } else memoriaRegistroOK = false;
  if (control.reposo < 0 || control.reposo > 180 || control.apagado < 0 ||
      control.apagado > 180 || control.reposo == control.apagado) {
    control.reposo = 140;
    control.apagado = 40;
    Serial.println("Angulos invalidos en memoria: usando 140/40.");
  }
  if (control.msPorGrado < 1 || control.msPorGrado > 50) control.msPorGrado = 8;
  if (!isfinite(control.tempArmar) || !isfinite(control.tempApagar) ||
      control.tempApagar < -50 || control.tempArmar > 125 ||
      control.tempApagar >= control.tempArmar) {
    control.tempArmar = 27;
    control.tempApagar = 21;
  }
  if (control.msArmar < 60000UL || control.msArmar > 7200000UL) control.msArmar = 300000UL;
  if (control.msFrio < 60000UL || control.msFrio > 7200000UL) control.msFrio = 180000UL;
  if (!urlGestorValida(sheetsURL)) sheetsURL = "";
  // No usar last_ang: guardar el último comando no mide la posición física.
}

bool guardarConfig() {
  if (!prefs.begin("config", false)) return false;
  bool ok = true;
  ok = (prefs.putInt("a_on", control.reposo) > 0) && ok;
  ok = (prefs.putInt("a_off", control.apagado) > 0) && ok;
  ok = (prefs.putInt("vel", control.msPorGrado) > 0) && ok;
  ok = (prefs.putFloat("t_armar", control.tempArmar) > 0) && ok;
  ok = (prefs.putFloat("t_apagar", control.tempApagar) > 0) && ok;
  ok = (prefs.putULong("ms_armar", control.msArmar) > 0) && ok;
  ok = (prefs.putULong("ms_frio", control.msFrio) > 0) && ok;
  prefs.end();
  return ok;
}

String estadoControl() {
  if (!servoOK) return "Error de servo: control no disponible";
  if (control.fase == ControlImpresora::APAGANDO) return "Moviendo a apagado";
  if (control.fase == ControlImpresora::ESPERANDO) return "Esperando 5 s para retirar servo";
  if (control.fase == ControlImpresora::RETORNANDO) return "Volviendo a reposo";
  if (!sensorOK) return "Error de sensor: automatico en pausa";
  if (control.esperandoBajada) return "Esperando nueva bajada y calentamiento";
  if (control.contandoFrio) return "Contando enfriamiento";
  if (control.armado) return "Auto armado: vigilando temperatura";
  if (control.contandoCalor) return "Contando calentamiento";
  return "Esperando calentamiento";
}

String estadoTermico() {
  if (!sensorOK) return "Sin lectura valida";
  if (temperatura <= control.tempApagar) return "Fria";
  if (temperatura < control.tempArmar) return "Intermedia";
  return "Caliente";
}

void actualizarServo(uint32_t ahora) {
  if (!servoOK) return;
  ControlImpresora::Fase antes = control.fase;
  if (control.avanzarServo(ahora)) servoPower.write(control.posicion);
  if (antes != control.fase && control.fase == ControlImpresora::REPOSO)
    ultimoMovimiento = "Servo retirado: reposo";
}

void registrarSample(uint32_t ahora) {
  if (!sensorOK || uint32_t(ahora - ultimoSample) < SAMPLE_INTERVAL_MS) return;
  ultimoSample = ahora;
  tempSamples[sampleIndex] = temperatura;
  segundosSamples[sampleIndex] = uint32_t(uptimeMs / 1000ULL);
  sampleIndex = (sampleIndex + 1) % MAX_SAMPLES;
  if (sampleCount < MAX_SAMPLES) ++sampleCount;
  if (tempCount == 0) tempMin = tempMax = temperatura;
  if (temperatura < tempMin) tempMin = temperatura;
  if (temperatura > tempMax) tempMax = temperatura;
  tempSum += temperatura;
  ++tempCount;
}

void actualizarTemperatura(uint32_t ahora) {
  if (conversionPendiente && uint32_t(ahora - inicioConversion) >= 800UL) {
    temperatura = sensors.getTempCByIndex(0);
    conversionPendiente = false;
    sensorOK = isfinite(temperatura) && temperatura != DEVICE_DISCONNECTED_C &&
               temperatura >= -50 && temperatura <= 125 && temperatura != 85.0f;
    // 85 C exactos se rechazan conservadoramente: valor de arranque del DS18B20.
    bool estabaOcupado = control.ocupado();
    if (servoOK) control.evaluarTemperatura(ahora, sensorOK, temperatura);
    if (!estabaOcupado && control.ocupado()) {
      pausarRegistro();
      ultimoMovimiento = "Apagado automatico solicitado";
    }
    registrarSample(ahora);
  }
  if (!conversionPendiente && uint32_t(ahora - ultimaPeticionSensor) >= 1000UL) {
    sensors.requestTemperatures();
    ultimaPeticionSensor = inicioConversion = ahora;
    conversionPendiente = true;
  }
}

void actualizarOLED(uint32_t ahora) {
  if (!oledOK || uint32_t(ahora - ultimoOLED) < 1000UL) return;
  ultimoOLED = ahora;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Ender temp monitor");
  display.setTextSize(2);
  display.setCursor(0, 16);
  if (sensorOK) { display.print(temperatura, 1); display.write(247); display.print("C"); }
  else display.print("ERROR");
  display.setTextSize(1);
  display.setCursor(0, 42);
  display.println("Estado:");
  display.setCursor(0, 54);
  if (control.ocupado()) {
    if (control.fase == ControlImpresora::APAGANDO) display.println("Apagando");
    else if (control.fase == ControlImpresora::ESPERANDO) display.println("Retorno en 5 s");
    else display.println("Retirando servo");
  } else if (!servoOK) display.println("Error servo");
  else if (!sensorOK) display.println("Auto: sin sensor");
  else if (control.esperandoBajada) display.println("Esperando nuevo ciclo");
  else if (control.contandoFrio) display.println("Enfriando");
  else if (control.armado) display.println("Automatico armado");
  else if (control.contandoCalor) display.println("Armando automatico");
  else display.println("Esperando calor");
  display.display();
}

void actualizarWiFi(uint32_t ahora) {
  bool conectado = WiFi.status() == WL_CONNECTED;
  if (conectado && !wifiEstabaConectado) {
    Serial.print("Web: http://");
    Serial.println(WiFi.localIP());
  }
  wifiEstabaConectado = conectado;
  if (!conectado && uint32_t(ahora - ultimoWiFi) >= 30000UL) {
    ultimoWiFi = ahora;
    WiFi.reconnect();
  }
}

void conectarMQTT(uint32_t ahora) {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;
  if (uint32_t(ahora - ultimoIntentoMQTT) < INTERVALO_RECONEXION_MQTT) return;

  ultimoIntentoMQTT = ahora;

  String clientId = "ender-esp32-";
  clientId += String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFFULL), HEX);

  Serial.print("MQTT: conectando a ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
    Serial.println("MQTT: conectado.");
  } else {
    Serial.print("MQTT: fallo rc=");
    Serial.println(mqttClient.state());
  }
}

void publicarTemperaturaMQTT(uint32_t ahora) {
  if (!mqttClient.connected() || !sensorOK) return;
  if (uint32_t(ahora - ultimaPublicacionMQTT) < INTERVALO_PUBLICACION_MQTT) return;

  ultimaPublicacionMQTT = ahora;

  char topic[96];
  snprintf(topic, sizeof(topic), "ecs/%s/temperatura_carpa", MQTT_DEVICE);

  JsonDocument d;
  d["value"] = temperatura;
  d["token"] = MQTT_TOKEN;

  String payload;
  serializeJson(d, payload);

  bool ok = mqttClient.publish(topic, payload.c_str(), false);

  Serial.print("MQTT temperatura: ");
  Serial.print(temperatura, 2);
  Serial.print(" C -> ");
  Serial.println(ok ? "OK" : "ERROR");
}

void actualizarMQTT(uint32_t ahora) {
  if (WiFi.status() != WL_CONNECTED) return;

  conectarMQTT(ahora);
  if (!mqttClient.connected()) return;

  mqttClient.loop();
  publicarTemperaturaMQTT(ahora);
}

String jsonString(const String& texto) {
  String salida = "\"";
  for (unsigned int i = 0; i < texto.length(); ++i) {
    unsigned char c = texto[i];
    if (c == '"' || c == '\\') { salida += '\\'; salida += char(c); }
    else if (c >= 32) salida += char(c);
  }
  return salida + "\"";
}

void enviarError(int codigo, const String& mensaje) {
  server.send(codigo, "text/plain; charset=utf-8", mensaje);
}

bool autorizarCambio(bool requiereReposo) {
  if (!server.hasArg("csrf") || server.arg("csrf") != csrfToken) {
    enviarError(403, "Pagina desactualizada o solicitud no valida. Recarga la pagina.");
    return false;
  }
  if (requiereReposo && control.ocupado()) {
    enviarError(409, "Espera a que el servo termine de volver a reposo.");
    return false;
  }
  return true;
}

void redirigir() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

const char PAGINA[] PROGMEM =
"\n"
"<!doctype html><html lang=\"es\"><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>Temperatura · Ender 3 Pro</title><style>\n"
"*{box-sizing:border-box}body{margin:0;background:#0b1220;color:#e7edf8;font:16px Arial,sans-serif}\n"
"main{max-width:1120px;margin:auto;padding:20px}h1{font-size:25px}h2{font-size:19px;margin-top:0}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}\n"
".card{background:#1c2738;border:1px solid #30415b;border-radius:12px;padding:18px;margin-bottom:16px}\n"
".label,.small{color:#bdcde0;font-size:14px;line-height:1.55}.value{font-size:25px;margin-top:8px}\n"
"button,.button{display:inline-block;border:0;border-radius:8px;padding:12px 16px;margin:5px 5px 5px 0;background:#2563eb;color:white;font-weight:bold;font-size:15px;cursor:pointer;text-decoration:none}\n"
".red{background:#b92332}.gray{background:#43536b}button:disabled{opacity:.45;cursor:default}button[aria-busy=\"true\"]{cursor:progress}\n"
"input,select{width:100%;background:#0b1220;color:#fff;border:1px solid #687b97;border-radius:6px;padding:10px;font-size:16px;margin:6px 0 10px}\n"
"label{display:block;color:#d2def0;font-size:14px}form.inline{display:inline-block}canvas{width:100%;height:280px;background:#0b1220;border-radius:8px}\n"
"#mensaje,#mensajeSheets{min-height:24px;color:#ffd79c}[hidden]{display:none!important}summary{cursor:pointer;padding:4px 0 14px}a{color:#91bbff}\n"
"</style></head><body><main>\n"
"<h1>Temperatura · Ender 3 Pro</h1><p class=\"small\">Versión 3.5</p>\n"
"<p class=\"small\">El encendido se hace a mano. El servo solo acciona el apagado y se retira 5 segundos después.</p>\n"
"<div class=\"grid\">\n"
"<div class=\"card\"><div class=\"label\">Temperatura de la carpa</div><div class=\"value\" id=\"temp\">--</div><p id=\"termico\" class=\"small\"></p></div>\n"
"<div class=\"card\"><div class=\"label\">Detección automática</div><div class=\"value\" id=\"estado\" style=\"font-size:19px\">Cargando…</div></div>\n"
"<div class=\"card\"><div class=\"label\">Servo · ángulo ordenado</div><div class=\"value\" id=\"angulo\">--</div><p id=\"movimiento\" class=\"small\"></p></div>\n"
"<div class=\"card\"><div class=\"label\">Mín. / máx. / promedio desde arranque</div><div id=\"stats\" class=\"value\" style=\"font-size:18px\">--</div><p class=\"small\">Estadísticas de las muestras locales guardadas.</p></div>\n"
"</div>\n"
"<div class=\"card\"><h2>Apagar impresora</h2>\n"
"<form class=\"inline accion\" action=\"/printer/off\" method=\"post\" data-confirm=\"¿Accionar ahora el interruptor de apagado?\">\n"
"<input type=\"hidden\" name=\"csrf\" value=\"@CSRF@\"><button class=\"red servo\" type=\"submit\">Apagar impresora</button></form>\n"
"<p class=\"small\">Reposo: @REPOSO@°. Apagado: @APAGADO@°. Al volver a reposo, el interruptor queda libre para encender manualmente.</p>\n"
"<p class=\"small\">El estado se deduce de la temperatura. No hay una señal que confirme si la impresora está encendida o terminó el G-code.</p>\n"
"</div>\n"
"<div class=\"card\"><h2>Luz interior · <span id=\"led\">--</span></h2>\n"
"<form class=\"inline accion\" action=\"/led/on\" method=\"post\"><input type=\"hidden\" name=\"csrf\" value=\"@CSRF@\"><button>Encender luz</button></form>\n"
"<form class=\"inline accion\" action=\"/led/off\" method=\"post\"><input type=\"hidden\" name=\"csrf\" value=\"@CSRF@\"><button class=\"gray\">Apagar luz</button></form></div>\n"
"<div class=\"card\"><h2>Google Sheets · registro por impresión</h2>\n"
"<p id=\"registroEstado\" role=\"status\">Consultando registro…</p>\n"
"<button id=\"reanudarRegistro\" class=\"nubeBtn\" type=\"button\" disabled>Reanudar registro</button>\n"
"<p id=\"mensajeSheets\" role=\"status\" aria-live=\"polite\"></p>\n"
"<p class=\"small\">Después de encender la Ender a mano, pulsa Reanudar registro para continuar la misma impresión. El apagado manual desde esta web y el automático pausan el registro.</p>\n"
"<p id=\"nubeEstado\">Consultando estado…</p>\n"
"<p id=\"nubeImpresion\" class=\"small\"></p><p id=\"nubeContadores\" class=\"small\"></p>\n"
"<details><summary>Conectar con mi hoja de Google</summary>\n"
"<form action=\"/config-sheets\" method=\"post\"><input type=\"hidden\" name=\"csrf\" value=\"@CSRF@\">\n"
"<label>URL del Apps Script v3, terminada en /exec<input name=\"url\" type=\"url\" maxlength=\"300\" value=\"@SHEETS_URL@\" placeholder=\"https://script.google.com/macros/s/…/exec\"></label>\n"
"<button>Guardar conexión</button></form>\n"
"<p class=\"small\">Usa la URL de implementación del Servidor_Temperatura.gs incluido. El registro se envía cada 60 segundos.</p></details>\n"
"<details open><summary>Comenzar el registro de otra impresión</summary>\n"
"<form id=\"formNueva\"><label>Nombre de la impresión<input id=\"nombreNueva\" required maxlength=\"80\" placeholder=\"Ejemplo: soporte ABS\"></label>\n"
"<button id=\"crearNueva\" class=\"nubeBtn\">Crear e iniciar nueva impresión</button></form>\n"
"<p class=\"small\">Con la Ender encendida, escribe el nombre y pulsa Crear e iniciar nueva impresión. Empezará un registro nuevo y conservará los anteriores. Reanudar mantiene el nombre y el identificador existentes. Estos botones no mueven el servo.</p>\n"
"<p class=\"small\">La pausa se guarda aunque reinicies la ESP32. Las muestras anteriores pendientes se enviarán al reanudar, con su impresión original. Una solicitud que ya estaba en curso al apagar puede terminar de guardarse.</p></details>\n"
"<hr style=\"border:0;border-top:1px solid #43536b;margin:20px 0\">\n"
"<h2>Borrar datos de una impresión</h2>\n"
"<button id=\"cargarImpresiones\" class=\"gray nubeBtn\">Cargar impresiones</button>\n"
"<label for=\"listaImpresiones\">Impresión que quieres limpiar</label>\n"
"<select id=\"listaImpresiones\"><option>Carga la lista para elegir una impresión</option></select>\n"
"<div><button id=\"pagAnterior\" class=\"gray nubeBtn\" disabled>Anterior</button><span id=\"paginaLista\" class=\"small\"></span><button id=\"pagSiguiente\" class=\"gray nubeBtn\" disabled>Siguiente</button></div>\n"
"<p id=\"infoImpresion\" class=\"small\"></p><button id=\"revisarBorrado\" class=\"red nubeBtn\" disabled>Revisar borrado</button>\n"
"<div id=\"confirmarPanel\" hidden><p id=\"resumenBorrado\"></p><button id=\"confirmarBorrado\" class=\"red nubeBtn\">Confirmar borrado de esos registros</button><button id=\"cancelarBorrado\" class=\"gray nubeBtn\">Cancelar</button></div>\n"
"<p class=\"small\">Se conserva la fila 1. Si borras la impresión actual, su registro en Google se detendrá hasta crear una nueva. El monitoreo local y el control del servo continúan.</p>\n"
"</div>\n"
"<div class=\"card\"><details><summary>Ajustar servo</summary>\n"
"<form action=\"/config-servo\" method=\"post\"><input type=\"hidden\" name=\"csrf\" value=\"@CSRF@\"><div class=\"grid\">\n"
"<label>Ángulo de reposo (antes «encender»)<input required name=\"a_on\" type=\"number\" min=\"0\" max=\"180\" value=\"@REPOSO@\"></label>\n"
"<label>Ángulo de apagado<input required name=\"a_off\" type=\"number\" min=\"0\" max=\"180\" value=\"@APAGADO@\"></label>\n"
"<label>Milisegundos por grado<input required name=\"vel\" type=\"number\" min=\"1\" max=\"50\" value=\"@VELOCIDAD@\"></label></div>\n"
"<button class=\"servo\">Guardar y colocar en reposo</button></form>\n"
"<form class=\"accion\" action=\"/move-servo\" method=\"post\" data-confirm=\"Esta prueba puede tocar el interruptor. ¿Mover el servo y volver a reposo después de 5 s?\">\n"
"<input type=\"hidden\" name=\"csrf\" value=\"@CSRF@\"><label>Probar un ángulo (regresa a reposo en 5 s)<input name=\"angle\" required type=\"number\" min=\"0\" max=\"180\" value=\"@APAGADO@\"></label>\n"
"<button class=\"gray servo\">Probar movimiento</button></form></details></div>\n"
"<div class=\"card\"><details><summary>Ajustar detección automática</summary>\n"
"<form action=\"/config-auto\" method=\"post\"><input type=\"hidden\" name=\"csrf\" value=\"@CSRF@\"><div class=\"grid\">\n"
"<label>Temperatura para armar (°C)<input required name=\"t_armar\" type=\"number\" step=\"0.1\" min=\"-50\" max=\"125\" value=\"@TARMAR@\"></label>\n"
"<label>Temperatura para apagar (°C)<input required name=\"t_apagar\" type=\"number\" step=\"0.1\" min=\"-50\" max=\"125\" value=\"@TAPAGAR@\"></label>\n"
"<label>Minutos continuos para armar<input required name=\"min_armar\" type=\"number\" min=\"1\" max=\"120\" value=\"@MARMAR@\"></label>\n"
"<label>Minutos continuos en frío<input required name=\"min_frio\" type=\"number\" min=\"1\" max=\"120\" value=\"@MFRIO@\"></label></div>\n"
"<button class=\"servo\">Guardar y reiniciar detección</button></form>\n"
"<form class=\"accion\" action=\"/reset-auto\" method=\"post\" data-confirm=\"¿Reiniciar los tiempos y permitir un nuevo ciclo con la temperatura actual?\">\n"
"<input type=\"hidden\" name=\"csrf\" value=\"@CSRF@\"><button class=\"gray servo\">Reiniciar detección automática</button></form>\n"
"<p class=\"small\">Después de apagar, se exige una lectura menor que la temperatura de armado y un nuevo calentamiento completo. Si comenzaste otra impresión todavía caliente, puedes reiniciar la detección. Esto no mueve el servo.</p></details></div>\n"
"<div class=\"card\"><h2>Temperatura local</h2><canvas id=\"chart\" width=\"1000\" height=\"300\"></canvas>\n"
"<p class=\"small\">Hasta 720 muestras, una cada 30 s: aproximadamente 6 horas. Eje horizontal: horas desde el arranque. Los datos locales se pierden al reiniciar la ESP32.</p>\n"
"<a class=\"button\" href=\"/csv\">Descargar CSV local</a></div>\n"
"<p id=\"mensaje\" role=\"status\"></p>\n"
"<script>\n"
"const byId=id=>document.getElementById(id);\n"
"let busy=false;\n"
"async function pedirWeb(url,opciones={},json=true,esperaMs=8000){\n"
" const controller=new AbortController();\n"
" const timer=setTimeout(()=>controller.abort(),esperaMs);\n"
" try{\n"
"  const r=await fetch(url,{...opciones,cache:'no-store',signal:controller.signal});\n"
"  if(!r.ok)throw Error((await r.text())||('Error HTTP '+r.status));\n"
"  return json?await r.json():await r.text();\n"
" }catch(e){\n"
"  if(e.name==='AbortError')throw Error('La ESP32 no respondió en 8 segundos. Revisa el estado del registro antes de repetir la acción.');\n"
"  throw e;\n"
" }finally{clearTimeout(timer);}\n"
"}\n"
"async function estado(){\n"
" try{const d=await pedirWeb('/api/status');\n"
" byId('temp').textContent=d.temp===null?'ERROR':d.temp.toFixed(1)+' °C';\n"
" byId('termico').textContent=d.termico;byId('estado').textContent=d.estado;\n"
" byId('angulo').textContent=d.angulo+'°';byId('movimiento').textContent=d.movimiento;\n"
" byId('led').textContent=d.led?'Encendida':'Apagada';\n"
" byId('stats').textContent=d.min===null?'Sin muestras':d.min.toFixed(1)+' / '+d.max.toFixed(1)+' / '+d.prom.toFixed(1)+' °C';\n"
" document.querySelectorAll('button.servo').forEach(b=>b.disabled=d.ocupado||busy);\n"
" }catch(e){byId('mensaje').textContent='No hay respuesta de la ESP32. Comprueba Wi-Fi o su alimentación.';}\n"
"}\n"
"document.querySelectorAll('form.accion').forEach(f=>f.addEventListener('submit',async e=>{\n"
" e.preventDefault();if(busy)return;if(f.dataset.confirm&&!confirm(f.dataset.confirm))return;\n"
" busy=true;f.querySelectorAll('button').forEach(b=>b.disabled=true);\n"
" try{byId('mensaje').textContent=await pedirWeb(f.action,{method:'POST',body:new URLSearchParams(new FormData(f))},false);}\n"
" catch(err){byId('mensaje').textContent=err.message;}\n"
" finally{busy=false;f.querySelectorAll('button').forEach(b=>b.disabled=false);await estado();}\n"
"}));\n"
"async function grafico(){\n"
" try{const d=await pedirWeb('/api/history');\n"
" const c=byId('chart'),ctx=c.getContext('2d');ctx.clearRect(0,0,c.width,c.height);ctx.font='14px Arial';ctx.fillStyle='#c6d6ed';\n"
" if(!d.length){ctx.fillText('Esperando primera muestra (30 s)…',20,35);return;}\n"
" let lo=Math.min(...d.map(p=>p[1])),hi=Math.max(...d.map(p=>p[1]));if(hi-lo<1){hi+=.5;lo-=.5;}\n"
" const start=d[0][0],span=Math.max(1,d[d.length-1][0]-start);\n"
" ctx.strokeStyle='#334155';for(let i=0;i<5;i++){let y=20+i*60;ctx.beginPath();ctx.moveTo(65,y);ctx.lineTo(980,y);ctx.stroke();ctx.fillText((hi-i*(hi-lo)/4).toFixed(1),5,y+4);}\n"
" ctx.strokeStyle='#69b6ff';ctx.lineWidth=2;ctx.beginPath();let prev=null;\n"
" d.forEach(p=>{const x=65+(p[0]-start)*915/span,y=20+(hi-p[1])*240/(hi-lo);\n"
" if(prev===null||p[0]-prev>45)ctx.moveTo(x,y);else ctx.lineTo(x,y);prev=p[0];});ctx.stroke();\n"
" if(d.length===1){ctx.beginPath();ctx.arc(65,20+(hi-d[0][1])*240/(hi-lo),3,0,Math.PI*2);ctx.fill();}\n"
" ctx.fillText((start/3600).toFixed(2)+' h',65,287);ctx.fillText((d[d.length-1][0]/3600).toFixed(2)+' h',900,287);\n"
" }catch(e){}\n"
"}\n"
"async function refrescar(){await estado();setTimeout(refrescar,1000);}\n"
"async function refrescarGrafico(){await grafico();setTimeout(refrescarGrafico,30000);}\n"
"const csrfCloud='@CSRF@';\n"
"let cloudBusyUI=false,cloudAction='',cloudRefreshVersion=0,printList=[],listPage=0,listMore=false,deletePlan=null,cloudStatus=null;\n"
"function nubeMensaje(t){byId('mensajeSheets').textContent=t;}\n"
"function botonesNube(){\n"
" const bloqueada=cloudBusyUI||!!(cloudStatus&&cloudStatus.cambiando);\n"
" document.querySelectorAll('.nubeBtn').forEach(b=>b.disabled=bloqueada);\n"
" byId('listaImpresiones').disabled=bloqueada||!printList.length;\n"
" byId('pagAnterior').disabled=bloqueada||listPage===0;\n"
" byId('pagSiguiente').disabled=bloqueada||!listMore;\n"
" byId('revisarBorrado').disabled=bloqueada||!printList.length;\n"
" const b=byId('reanudarRegistro');\n"
" const activa=!!(cloudStatus&&!cloudStatus.pausado&&cloudStatus.conocida&&cloudStatus.activa);\n"
" const reanudando=cloudBusyUI&&cloudAction==='reanudar';\n"
" b.disabled=bloqueada||!cloudStatus||!cloudStatus.pausado||cloudStatus.servo_ocupado;\n"
" b.textContent=reanudando?'Reanudando…':(activa?'Registro activo':'Reanudar registro');\n"
" b.setAttribute('aria-busy',String(reanudando));\n"
" byId('crearNueva').disabled=bloqueada||!!(cloudStatus&&cloudStatus.servo_ocupado);\n"
" byId('crearNueva').setAttribute('aria-busy',String(cloudBusyUI&&cloudAction==='nueva'));\n"
"}\n"
"function cancelarBorrado(){deletePlan=null;byId('confirmarPanel').hidden=true;}\n"
"function seleccionImpresion(){\n"
" cancelarBorrado();const p=printList.find(p=>p.id===byId('listaImpresiones').value);\n"
" byId('infoImpresion').textContent=p?p.filas+' filas · '+p.primera+' → '+p.ultima+(p.actual?' · Impresión actual':''):'';\n"
"}\n"
"function resultadoSheets(d){\n"
" if(!d||!d.ok)throw Error((d&&d.error)||'No se pudo confirmar la operación.');\n"
" return d;\n"
"}\n"
"async function solicitarSheets(accion,args={}){\n"
" const inicio=await pedirWeb('/api/sheets/action',{method:'POST',body:new URLSearchParams({csrf:csrfCloud,accion,...args})});\n"
" // Reanudar una impresión conocida se confirma en la ESP32, sin otra consulta a Google.\n"
" if(inicio.job===undefined)return resultadoSheets(inicio);\n"
" const job=inicio.job,limite=Date.now()+180000;\n"
" while(Date.now()<limite){\n"
"  await new Promise(resolve=>setTimeout(resolve,700));\n"
"  const d=await pedirWeb('/api/sheets/job?id='+job);\n"
"  if(!d.pendiente)return resultadoSheets(d.resultado);\n"
" }\n"
" throw Error('No llegó la confirmación de Google. Revisa el estado y la lista de impresiones antes de repetir la acción.');\n"
"}\n"
"async function operacionSheets(fn,accion=''){\n"
" if(cloudBusyUI)return;cloudBusyUI=true;cloudAction=accion;botonesNube();\n"
" nubeMensaje(accion==='reanudar'?'Reanudando registro…':'Consultando Google…');\n"
" try{await fn();}catch(e){cancelarBorrado();nubeMensaje(e.message);}\n"
" finally{cloudBusyUI=false;cloudAction='';botonesNube();}\n"
"}\n"
"function aplicarResultadoRegistro(d){\n"
" // Una lectura iniciada antes de este resultado no debe volver a mostrar la pausa anterior.\n"
" ++cloudRefreshVersion;\n"
" if(!cloudStatus)return;\n"
" cloudStatus={...cloudStatus,pausado:d.registro_pausado,cambiando:false,\n"
"  conocida:true,activa:d.config.activa,id:d.config.id_impresion,nombre:d.config.nombre_impresion};\n"
" mostrarEstadoNube(cloudStatus);botonesNube();\n"
"}\n"
"async function listarImpresiones(pagina){\n"
" cancelarBorrado();const d=await solicitarSheets('listar',{pagina:String(pagina)});\n"
" printList=d.impresiones;listPage=d.pagina;listMore=d.hay_mas;\n"
" const s=byId('listaImpresiones');s.replaceChildren();\n"
" if(!printList.length){const o=document.createElement('option');o.textContent='No hay datos borrables desde la fila 2';s.appendChild(o);}\n"
" printList.forEach(p=>{const o=document.createElement('option');o.value=p.id;o.textContent=p.nombre+' · '+p.id+' · '+p.filas+' filas';s.appendChild(o);});\n"
" byId('paginaLista').textContent='Página '+(listPage+1)+' · '+d.total+' impresiones';seleccionImpresion();botonesNube();\n"
"}\n"
"byId('cargarImpresiones').onclick=()=>operacionSheets(async()=>{await listarImpresiones(0);nubeMensaje('Lista actualizada.');});\n"
"byId('pagAnterior').onclick=()=>operacionSheets(async()=>{await listarImpresiones(listPage-1);nubeMensaje('Lista actualizada.');});\n"
"byId('pagSiguiente').onclick=()=>operacionSheets(async()=>{await listarImpresiones(listPage+1);nubeMensaje('Lista actualizada.');});\n"
"byId('listaImpresiones').onchange=seleccionImpresion;\n"
"byId('cancelarBorrado').onclick=cancelarBorrado;\n"
"byId('revisarBorrado').onclick=()=>operacionSheets(async()=>{\n"
" deletePlan=await solicitarSheets('preparar_borrado',{id:byId('listaImpresiones').value});\n"
" byId('resumenBorrado').textContent='Se limpiarán '+deletePlan.filas+' filas de «'+deletePlan.nombre+'» ('+deletePlan.id+'). La fila 1 se conserva.'+(deletePlan.actual?' Esta es la impresión actual: se detendrá su registro.':'');\n"
" byId('confirmarPanel').hidden=false;nubeMensaje('Revisa los datos antes de confirmar.');\n"
"});\n"
"byId('confirmarBorrado').onclick=()=>operacionSheets(async()=>{\n"
" if(!deletePlan)throw Error('Primero revisa la impresión que deseas borrar.');\n"
" const d=await solicitarSheets('borrar',{id:deletePlan.id,token:deletePlan.token});\n"
" cancelarBorrado();await listarImpresiones(0);nubeMensaje('Se borraron '+d.filas_borradas+' filas. La fila 1 se conservó.');\n"
"});\n"
"byId('formNueva').onsubmit=e=>{e.preventDefault();return operacionSheets(async()=>{\n"
" cancelarBorrado();const d=await solicitarSheets('nueva',{nombre:byId('nombreNueva').value});\n"
" aplicarResultadoRegistro(d);\n"
" nubeMensaje(d.aviso_registro||(d.registro_pausado?'Nueva impresión seleccionada; registro en pausa.':'Registro iniciado para «'+d.config.nombre_impresion+'».'));\n"
"},'nueva');};\n"
"byId('reanudarRegistro').onclick=()=>operacionSheets(async()=>{\n"
" const d=await solicitarSheets('reanudar');\n"
" aplicarResultadoRegistro(d);\n"
" nubeMensaje(d.aviso_registro||(d.registro_pausado?'Registro en pausa.':'Registro reanudado para «'+d.config.nombre_impresion+'».'));\n"
"},'reanudar');\n"
"function mostrarEstadoNube(d){\n"
" let registro=d.cambiando?'Confirmando la impresión con Google…':(d.pausado?'Registro en pausa. Enciende la Ender a mano y pulsa Reanudar registro.':(d.conocida&&d.activa?'Registro activo: una muestra cada 60 segundos.':'Esperando una impresión disponible.'));\n"
" if(!d.memoria_ok)registro+=' No se pudo guardar el estado en memoria; no desconectes la ESP32 hasta corregirlo.';\n"
" byId('registroEstado').textContent=registro;\n"
" let aviso=d.estado;\n"
" if(!d.configurada)aviso='Configura la URL de Apps Script para comenzar a registrar.';\n"
" else if(!d.tarea_ok)aviso='No se pudo iniciar el registro. Reinicia la ESP32.';\n"
" else if(!d.wifi)aviso='Sin Wi-Fi. Las muestras pendientes se conservan en RAM.';\n"
" else if(!d.hora_ok)aviso='Esperando sincronización de hora para conectar con Google.';\n"
" else if(d.conocida&&!d.activa)aviso='Esta impresión fue borrada. Crea otra para reanudar el registro.';\n"
" else if(d.sensor_ok===false)aviso='El sensor no entrega una temperatura válida. No se enviarán muestras hasta recuperar la lectura.';\n"
" byId('nubeEstado').textContent=aviso;\n"
" byId('nubeImpresion').textContent=d.conocida?d.nombre+' · '+d.id+' · '+d.estabilidad:'Esperando configuración de la impresión…';\n"
" byId('nubeContadores').textContent='Confirmadas desde el arranque: '+d.enviadas+' · Pendientes: '+d.pendientes+'/30 · No enviadas por error o cola llena: '+d.perdidas+(d.ultima?' · Última muestra: '+new Date(d.ultima*1000).toLocaleString():'');\n"
"}\n"
"async function actualizarNube(){\n"
" const version=++cloudRefreshVersion;\n"
" try{\n"
"  const d=await pedirWeb('/api/sheets/status');\n"
"  if(version!==cloudRefreshVersion)return;\n"
"  cloudStatus=d;mostrarEstadoNube(d);botonesNube();\n"
" }catch(e){\n"
"  if(version===cloudRefreshVersion)byId('nubeEstado').textContent='No se pudo actualizar el estado. '+e.message;\n"
" }\n"
"}\n"
"async function refrescarNube(){\n"
" await actualizarNube();setTimeout(refrescarNube,2000);\n"
"}\n"
"botonesNube();refrescarNube();\n"
"\n"
"refrescar();refrescarGrafico();\n"
"</script></main></body></html>\n";

String generarHTML() {
  String html = FPSTR(PAGINA);
  html.replace("@CSRF@", csrfToken);
  html.replace("@REPOSO@", String(control.reposo));
  html.replace("@APAGADO@", String(control.apagado));
  html.replace("@VELOCIDAD@", String(control.msPorGrado));
  html.replace("@TARMAR@", String(control.tempArmar, 1));
  html.replace("@TAPAGAR@", String(control.tempApagar, 1));
  html.replace("@MARMAR@", String(control.msArmar / 60000UL));
  html.replace("@MFRIO@", String(control.msFrio / 60000UL));
  html.replace("@SHEETS_URL@", sheetsURL);
  return html;
}

void handleStatus() {
  String s;
  s.reserve(650);
  s = "{\"temp\":" + (sensorOK ? String(temperatura, 2) : String("null"));
  s += ",\"termico\":" + jsonString(estadoTermico());
  s += ",\"estado\":" + jsonString(estadoControl());
  s += ",\"angulo\":" + String(control.posicion);
  s += ",\"movimiento\":" + jsonString(ultimoMovimiento);
  s += ",\"ocupado\":" + String(control.ocupado() ? "true" : "false");
  s += ",\"led\":" + String(ledEncendido ? "true" : "false");
  s += ",\"min\":" + (tempCount ? String(tempMin, 2) : String("null"));
  s += ",\"max\":" + (tempCount ? String(tempMax, 2) : String("null"));
  s += ",\"prom\":" + (tempCount ? String(tempSum / tempCount, 2) : String("null")) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", s);
}

void handleHistory() {
  String s;
  s.reserve(sampleCount * 24 + 2);
  s = "[";
  for (int i = 0; i < sampleCount; ++i) {
    int idx = (sampleIndex - sampleCount + i + MAX_SAMPLES) % MAX_SAMPLES;
    if (i) s += ",";
    s += "[" + String(segundosSamples[idx]) + "," + String(tempSamples[idx], 2) + "]";
  }
  s += "]";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

void handleCSV() {
  String csv;
  csv.reserve(sampleCount * 24 + 40);
  csv = "segundos_desde_arranque,temperatura_c\n";
  for (int i = 0; i < sampleCount; ++i) {
    int idx = (sampleIndex - sampleCount + i + MAX_SAMPLES) % MAX_SAMPLES;
    csv += String(segundosSamples[idx]) + "," + String(tempSamples[idx], 2) + "\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=registro_temperatura.csv");
  server.send(200, "text/csv; charset=utf-8", csv);
}

void handleConfigServo() {
  if (!autorizarCambio(true)) return;
  long reposo, apagado, velocidad;
  if (!enteroValido(server.arg("a_on"), 0, 180, reposo) ||
      !enteroValido(server.arg("a_off"), 0, 180, apagado) ||
      !enteroValido(server.arg("vel"), 1, 50, velocidad) || reposo == apagado) {
    enviarError(400, "Angulos diferentes entre 0 y 180; velocidad entera entre 1 y 50 ms/grado.");
    return;
  }
  control.reposo = int(reposo);
  control.apagado = int(apagado);
  control.msPorGrado = int(velocidad);
  bool guardado = guardarConfig();
  control.reiniciarAutomatico();
  // Se ordena directamente el nuevo reposo de calibración.
  control.posicion = control.destino = control.reposo;
  if (servoOK) servoPower.write(control.reposo);
  ultimoMovimiento = "Nuevo reposo de calibracion";
  if (!guardado) { enviarError(500, "Ajuste aplicado en RAM, pero no se pudo guardar en memoria."); return; }
  redirigir();
}

void handleConfigAuto() {
  if (!autorizarCambio(true)) return;
  float armar, apagar;
  long minutosArmar, minutosFrio;
  if (!realValido(server.arg("t_armar"), armar) || !realValido(server.arg("t_apagar"), apagar) ||
      apagar >= armar || !enteroValido(server.arg("min_armar"), 1, 120, minutosArmar) ||
      !enteroValido(server.arg("min_frio"), 1, 120, minutosFrio)) {
    enviarError(400, "T_apagar debe ser menor que T_armar; tiempos enteros de 1 a 120 minutos.");
    return;
  }
  control.tempArmar = armar;
  control.tempApagar = apagar;
  control.msArmar = uint32_t(minutosArmar) * 60000UL;
  control.msFrio = uint32_t(minutosFrio) * 60000UL;
  control.reiniciarAutomatico();
  if (!guardarConfig()) { enviarError(500, "Ajuste aplicado en RAM, pero no se pudo guardar en memoria."); return; }
  redirigir();
}

void handleConfigSheets() {
  if (!autorizarCambio(false)) return;
  if (nubeOcupada || usuarioPendiente || usuarioEnCurso) {
    enviarError(409, "Hay una consulta a Google en curso. Espera unos segundos y vuelve a guardar."); return;
  }
  String nueva = server.arg("url"); nueva.trim();
  if (!urlGestorValida(nueva)) { enviarError(400, "Usa https://script.google.com/macros/s/ID/exec, sin parametros."); return; }
  if (!prefs.begin("config", false)) { enviarError(500, "No se pudo abrir la memoria."); return; }
  prefs.putString("sheets_exec", nueva);
  bool ok = prefs.getString("sheets_exec", "__error__") == nueva; prefs.end();
  if (!ok) { enviarError(500, "No se pudo guardar el enlace."); return; }
  sheetsURL = nueva; configNubeConocida = false; registroActivo = false;
  esperaReintento = 0; ultimoConfigNube = 0;
  mensajeNube = "Conexion actualizada; esperando sincronizacion";
  redirigir();
}

void iniciarServidor() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", generarHTML());
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/csv", HTTP_GET, handleCSV);
  server.on("/api/sheets/status", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", estadoNubeJSON());
  });
  server.on("/api/sheets/action", HTTP_POST, handleAccionSheets);
  server.on("/api/sheets/job", HTTP_GET, handleResultadoSheets);
  server.on("/printer/off", HTTP_POST, []() {
    if (!autorizarCambio(true)) return;
    if (!servoOK) { enviarError(503, "Servo no disponible."); return; }
    if (!control.pedirApagado(millis(), false)) { enviarError(409, "El servo esta ocupado."); return; }
    pausarRegistro();
    ultimoMovimiento = "Apagado manual solicitado";
    server.send(202, "text/plain; charset=utf-8", "Apagado solicitado y registro en pausa. El servo regresara a reposo tras 5 s en apagado.");
  });
  // No existe /printer/on ni botones de encendido de la impresora.
  server.on("/move-servo", HTTP_POST, []() {
    if (!autorizarCambio(true)) return;
    if (!servoOK) { enviarError(503, "Servo no disponible."); return; }
    long angulo;
    if (!enteroValido(server.arg("angle"), 0, 180, angulo)) {
      enviarError(400, "El angulo debe ser un entero entre 0 y 180."); return;
    }
    control.pedirMovimiento(millis(), int(angulo), false);
    ultimoMovimiento = "Prueba de calibracion";
    server.send(202, "text/plain; charset=utf-8", "Prueba iniciada; regresara a reposo despues de 5 s.");
  });
  server.on("/reset-auto", HTTP_POST, []() {
    if (!autorizarCambio(true)) return;
    control.reiniciarAutomatico();
    server.send(200, "text/plain; charset=utf-8", "Deteccion reiniciada; el servo no se mueve.");
  });
  server.on("/led/on", HTTP_POST, []() {
    if (!autorizarCambio(false)) return;
    ledEncendido = true;
    digitalWrite(LED_PIN, HIGH);
    server.send(200, "text/plain; charset=utf-8", "Luz encendida.");
  });
  server.on("/led/off", HTTP_POST, []() {
    if (!autorizarCambio(false)) return;
    ledEncendido = false;
    digitalWrite(LED_PIN, LOW);
    server.send(200, "text/plain; charset=utf-8", "Luz apagada.");
  });
  server.on("/config-servo", HTTP_POST, handleConfigServo);
  server.on("/config-auto", HTTP_POST, handleConfigAuto);
  server.on("/config-sheets", HTTP_POST, handleConfigSheets);
  server.onNotFound([]() { enviarError(404, "Ruta no disponible. Abre la pagina principal."); });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  cargarConfig();
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Retirar el servo antes de depender de Wi-Fi, pantalla o sensor.
  control.iniciar(millis());
  ESP32PWM::allocateTimer(0);
  servoPower.setPeriodHertz(50);
  servoPower.attach(SERVO_PIN, 500, 2400);
  servoPower.write(control.reposo); // Sin pausa inicial a 90 grados.
  servoOK = servoPower.attached();
  if (!servoOK) Serial.println("ERROR: no se pudo adjuntar el servo.");

  Wire.begin(SDA_PIN, SCL_PIN);
  oledOK = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledOK) Serial.println("OLED no disponible; el control continua.");
  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();
  ultimaPeticionSensor = inicioConversion = millis();
  conversionPendiente = true;

  csrfToken = String(esp_random(), HEX) + String(esp_random(), HEX);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Validación TLS del broker EMQX Cloud. No usar setInsecure().
  mqttWiFiClient.setCACert(EMQX_ROOT_CA);
  mqttWiFiClient.setHandshakeTimeout(15);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setSocketTimeout(5);
  mqttClient.setBufferSize(512);

  iniciarNube();
  iniciarServidor();
  anteriorMillis = millis();
  ultimoWiFi = anteriorMillis;
  Serial.println("Control iniciado. Reposo = antiguo angulo encender. Esperando Wi-Fi sin bloquear.");
}

void loop() {
  uint32_t ahora = millis();
  uptimeMs += uint32_t(ahora - anteriorMillis);
  anteriorMillis = ahora;
  actualizarServo(ahora);
  actualizarTemperatura(ahora);
  actualizarServo(millis());
  server.handleClient();
  actualizarServo(millis());
  actualizarWiFi(millis());
  actualizarMQTT(millis());
  procesarNube(millis());
  actualizarOLED(millis());
  delay(1); // Ceder CPU; no hay esperas de segundos ni bucles de movimiento.
}