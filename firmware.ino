#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
 
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";
 
const char* MQTT_BROKER    = "broker.hivemq.com";
const int   MQTT_PORT      = 1883;
const char* MQTT_USER      = "";
const char* MQTT_PASSWORD  = "";
const char* MQTT_CLIENT_ID = "esp32_vinicola_cp1";
 
const char* TOPIC_TELEMETRIA = "vinicola/cp1/telemetria";
const char* TOPIC_STATUS     = "vinicola/cp1/status";
 
#define DHT_PIN   4
#define DHT_TYPE  DHT22
#define LDR_PIN   34
 
const unsigned long INTERVALO_LEITURA   = 10000;
const unsigned long INTERVALO_RECONEXAO = 5000;
 
const float TEMP_MIN = 10.0;
const float TEMP_MAX = 20.0;
const float UMID_MIN = 55.0;
const float UMID_MAX = 75.0;
const int   LUZ_MAX  = 500;
 
// ✅ CORREÇÃO: struct declarado antes de ser usado
struct LeituraSensores {
  float temperatura;
  float umidade;
  int   luminosidade;
  bool  valido;
};
 
WiFiClient   espClient;
PubSubClient mqttClient(espClient);
DHT          dht(DHT_PIN, DHT_TYPE);
 
unsigned long ultimaLeitura   = 0;
unsigned long ultimaReconexao = 0;
unsigned long contadorMsg     = 0;
 
void setupWifi() {
  Serial.print("[WiFi] Conectando a ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
 
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 30) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }
 
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[WiFi] Conectado! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("[WiFi] Falha na conexao.");
  }
}
 
bool conectarMqtt() {
  if (mqttClient.connected()) return true;
 
  Serial.print("[MQTT] Conectando ao broker... ");
  bool ok = mqttClient.connect(
    MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD,
    TOPIC_STATUS, 1, true, "{\"status\":\"offline\"}"
  );
 
  if (ok) {
    Serial.println("OK!");
    mqttClient.publish(TOPIC_STATUS, "{\"status\":\"online\"}", true);
  } else {
    Serial.print("FALHA rc=");
    Serial.println(mqttClient.state());
  }
  return ok;
}
 
LeituraSensores lerSensores() {
  LeituraSensores dados;
  dados.temperatura  = dht.readTemperature();
  dados.umidade      = dht.readHumidity();
  dados.luminosidade = analogRead(LDR_PIN);
  dados.valido = !isnan(dados.temperatura) && !isnan(dados.umidade);
  if (!dados.valido) Serial.println("[SENSOR] Falha na leitura do DHT22!");
  return dados;
}
 
String classificarAlerta(const LeituraSensores& dados) {
  String alerta = "";
  if (dados.temperatura < TEMP_MIN)      alerta += "temp_baixa ";
  else if (dados.temperatura > TEMP_MAX) alerta += "temp_alta ";
  if (dados.umidade < UMID_MIN)          alerta += "umidade_baixa ";
  else if (dados.umidade > UMID_MAX)     alerta += "umidade_alta ";
  if (dados.luminosidade > LUZ_MAX)      alerta += "luz_excessiva ";
  if (alerta.length() == 0) alerta = "ok";
  alerta.trim();
  return alerta;
}
 
void publicarTelemetria(const LeituraSensores& dados) {
  StaticJsonDocument<256> doc;
  doc["dispositivo"]  = MQTT_CLIENT_ID;
  doc["temperatura"]  = round(dados.temperatura * 10.0) / 10.0;
  doc["umidade"]      = round(dados.umidade * 10.0) / 10.0;
  doc["luminosidade"] = dados.luminosidade;
  doc["alerta"]       = classificarAlerta(dados);
  doc["msg_id"]       = ++contadorMsg;
  doc["uptime_s"]     = millis() / 1000;
 
  char buffer[256];
  size_t n = serializeJson(doc, buffer);
 
  if (mqttClient.publish(TOPIC_TELEMETRIA, buffer, false)) {
    Serial.print("[MQTT] Publicado: ");
    Serial.println(buffer);
  } else {
    Serial.println("[MQTT] FALHA ao publicar!");
  }
}
 
void setup() {
  Serial.begin(115200);
  Serial.println("========================================");
  Serial.println(" CHECKPOINT IoT - Vinicola Monitor v1.0");
  Serial.println("========================================");
  dht.begin();
  pinMode(LDR_PIN, INPUT);
  setupWifi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  Serial.println("[SETUP] Inicializacao concluida.");
}
 
void loop() {
  unsigned long agora = millis();
 
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      if (agora - ultimaReconexao >= INTERVALO_RECONEXAO) {
        ultimaReconexao = agora;
        conectarMqtt();
      }
    }
    mqttClient.loop();
  }
 
  if (agora - ultimaLeitura >= INTERVALO_LEITURA) {
    ultimaLeitura = agora;
    LeituraSensores dados = lerSensores();
 
    if (dados.valido) {
      Serial.print("[SENSOR] Temp=");
      Serial.print(dados.temperatura);
      Serial.print("C | Umid=");
      Serial.print(dados.umidade);
      Serial.print("% | Luz=");
      Serial.println(dados.luminosidade);
 
      if (mqttClient.connected()) {
        publicarTelemetria(dados);
      }
    }
  }
}