/*
 * ============================================================================
 *  CHECKPOINT IoT — Monitoramento Inteligente de Vinícola
 * ============================================================================
 *  Hardware:  ESP32 DevKit v1 + DHT22 + LDR
 *  Protocolo: MQTT (HiveMQ Cloud / Mosquitto)
 *  Libs:      WiFi, PubSubClient, ArduinoJson, DHT
 *
 *  Arquitetura: Loop non-blocking com millis(). Sem delay().
 *  Tópico MQTT: vinicola/cp1/telemetria
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// =============================================================================
// CONFIGURAÇÃO — Edite estes valores conforme seu ambiente
// =============================================================================

// Wi-Fi
const char* WIFI_SSID     = "SEU_SSID";
const char* WIFI_PASSWORD  = "SUA_SENHA";

// MQTT Broker (HiveMQ Cloud usa TLS na porta 8883, Mosquitto local usa 1883)
const char* MQTT_BROKER    = "broker.hivemq.com";  // ou IP do seu Mosquitto
const int   MQTT_PORT      = 1883;
const char* MQTT_USER      = "";                    // preencha se usar auth
const char* MQTT_PASSWORD  = "";
const char* MQTT_CLIENT_ID = "esp32_vinicola_cp1";

// Tópicos MQTT
const char* TOPIC_TELEMETRIA = "vinicola/cp1/telemetria";
const char* TOPIC_STATUS     = "vinicola/cp1/status";

// Pins
#define DHT_PIN    4        // GPIO4 — DATA do DHT22
#define DHT_TYPE   DHT22
#define LDR_PIN    34       // GPIO34 — Leitura analógica do LDR

// Intervalos (ms)
const unsigned long INTERVALO_LEITURA      = 10000;  // 10s entre leituras
const unsigned long INTERVALO_RECONEXAO    = 5000;   // 5s entre tentativas
const unsigned long INTERVALO_WIFI_CHECK   = 30000;  // 30s check Wi-Fi

// Limites de alerta para vinícola (referência, ajuste conforme necessidade)
const float TEMP_MIN     = 10.0;   // °C
const float TEMP_MAX     = 20.0;   // °C
const float UMID_MIN     = 55.0;   // %
const float UMID_MAX     = 75.0;   // %
const int   LUZ_MAX      = 500;    // valor analógico (0-4095), quanto maior = mais luz

// =============================================================================
// OBJETOS GLOBAIS
// =============================================================================

WiFiClient   espClient;
PubSubClient mqttClient(espClient);
DHT          dht(DHT_PIN, DHT_TYPE);

// Controle de tempo (non-blocking)
unsigned long ultimaLeitura    = 0;
unsigned long ultimaReconexao  = 0;
unsigned long ultimoWifiCheck  = 0;

// Contador de mensagens (útil para debug e rastreamento)
unsigned long contadorMsg = 0;

// =============================================================================
// SETUP WI-FI
// =============================================================================

void setupWifi() {
  Serial.print("[WiFi] Conectando a ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);  // Station mode — não cria AP
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Espera inicial com timeout de 15s
  // (único ponto com blocking, só roda no boot)
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
    Serial.println("[WiFi] FALHA na conexão inicial. Reconexão via loop.");
  }
}

// =============================================================================
// RECONEXÃO WI-FI (non-blocking, chamada no loop)
// =============================================================================

void checkWifi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Desconectado. Tentando reconectar...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// =============================================================================
// CONEXÃO MQTT (non-blocking)
// =============================================================================

/*
 * Tenta conectar ao broker MQTT.
 * Retorna true se conectou, false caso contrário.
 * NÃO bloqueia — é chamada periodicamente pelo loop.
 */
bool conectarMqtt() {
  if (mqttClient.connected()) return true;

  Serial.print("[MQTT] Tentando conexão ao broker... ");

  // LWT (Last Will and Testament) — publica "offline" se o ESP32 cair
  bool conectado = mqttClient.connect(
    MQTT_CLIENT_ID,
    MQTT_USER,
    MQTT_PASSWORD,
    TOPIC_STATUS,    // tópico do LWT
    1,               // QoS 1
    true,            // retain
    "{\"status\":\"offline\"}"
  );

  if (conectado) {
    Serial.println("OK!");
    // Publica status online (retained)
    mqttClient.publish(TOPIC_STATUS, "{\"status\":\"online\"}", true);
  } else {
    Serial.print("FALHA, rc=");
    Serial.println(mqttClient.state());
    /*
     * Códigos de erro PubSubClient:
     * -4 = MQTT_CONNECTION_TIMEOUT
     * -3 = MQTT_CONNECTION_LOST
     * -2 = MQTT_CONNECT_FAILED
     * -1 = MQTT_DISCONNECTED
     *  1 = MQTT_CONNECT_BAD_PROTOCOL
     *  2 = MQTT_CONNECT_BAD_CLIENT_ID
     *  3 = MQTT_CONNECT_UNAVAILABLE
     *  4 = MQTT_CONNECT_BAD_CREDENTIALS
     *  5 = MQTT_CONNECT_UNAUTHORIZED
     */
  }

  return conectado;
}

// =============================================================================
// LEITURA DOS SENSORES
// =============================================================================

/*
 * Estrutura para encapsular leitura dos sensores.
 * 'valido' indica se a leitura do DHT22 foi bem-sucedida.
 */
struct LeituraSensores {
  float temperatura;
  float umidade;
  int   luminosidade;   // 0 (escuro) a 4095 (muita luz)
  bool  valido;
};

LeituraSensores lerSensores() {
  LeituraSensores dados;

  dados.temperatura  = dht.readTemperature();   // °C
  dados.umidade      = dht.readHumidity();       // %
  dados.luminosidade = analogRead(LDR_PIN);      // 0-4095 (ADC 12-bit)

  // DHT22 retorna NaN se falhar
  dados.valido = !isnan(dados.temperatura) && !isnan(dados.umidade);

  if (!dados.valido) {
    Serial.println("[SENSOR] Falha na leitura do DHT22!");
  }

  return dados;
}

// =============================================================================
// CLASSIFICAÇÃO DE ALERTAS
// =============================================================================

/*
 * Retorna uma string de alerta baseada nos limites da vinícola.
 * "ok" se tudo normal, ou descrição do problema.
 */
String classificarAlerta(const LeituraSensores& dados) {
  String alerta = "";

  if (dados.temperatura < TEMP_MIN)
    alerta += "temp_baixa ";
  else if (dados.temperatura > TEMP_MAX)
    alerta += "temp_alta ";

  if (dados.umidade < UMID_MIN)
    alerta += "umidade_baixa ";
  else if (dados.umidade > UMID_MAX)
    alerta += "umidade_alta ";

  if (dados.luminosidade > LUZ_MAX)
    alerta += "luz_excessiva ";

  if (alerta.length() == 0) alerta = "ok";
  alerta.trim();
  return alerta;
}

// =============================================================================
// PUBLICAÇÃO MQTT (JSON)
// =============================================================================

/*
 * Monta o payload JSON e publica no tópico de telemetria.
 *
 * Formato do JSON:
 * {
 *   "dispositivo": "esp32_vinicola_cp1",
 *   "temperatura": 15.2,
 *   "umidade": 62.5,
 *   "luminosidade": 120,
 *   "alerta": "ok",
 *   "msg_id": 42,
 *   "uptime_s": 3600
 * }
 */
void publicarTelemetria(const LeituraSensores& dados) {
  // StaticJsonDocument aloca na stack — mais rápido que Dynamic para payloads pequenos
  StaticJsonDocument<256> doc;

  doc["dispositivo"]   = MQTT_CLIENT_ID;
  doc["temperatura"]   = round(dados.temperatura * 10.0) / 10.0;  // 1 casa decimal
  doc["umidade"]       = round(dados.umidade * 10.0) / 10.0;
  doc["luminosidade"]  = dados.luminosidade;
  doc["alerta"]        = classificarAlerta(dados);
  doc["msg_id"]        = ++contadorMsg;
  doc["uptime_s"]      = millis() / 1000;

  char buffer[256];
  size_t n = serializeJson(doc, buffer);

  if (mqttClient.publish(TOPIC_TELEMETRIA, buffer, false)) {
    Serial.print("[MQTT] Publicado (");
    Serial.print(n);
    Serial.print(" bytes): ");
    Serial.println(buffer);
  } else {
    Serial.println("[MQTT] FALHA ao publicar!");
  }
}

// =============================================================================
// SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("========================================");
  Serial.println(" CHECKPOINT IoT — Vinícola Monitor v1.0");
  Serial.println("========================================");

  // Inicializa sensor DHT22
  dht.begin();

  // Configura pino do LDR como entrada
  // GPIO34 é input-only no ESP32, não precisa de pinMode,
  // mas deixamos explícito por clareza
  pinMode(LDR_PIN, INPUT);

  // Conecta Wi-Fi (blocking apenas no boot)
  setupWifi();

  // Configura broker MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  // Sem callback de subscribe neste firmware (apenas publica)

  Serial.println("[SETUP] Inicialização concluída.");
}

// =============================================================================
// LOOP PRINCIPAL (100% non-blocking)
// =============================================================================

void loop() {
  unsigned long agora = millis();

  // --- 1. Verifica Wi-Fi periodicamente ---
  if (agora - ultimoWifiCheck >= INTERVALO_WIFI_CHECK) {
    ultimoWifiCheck = agora;
    checkWifi();
  }

  // --- 2. Se Wi-Fi OK, mantém MQTT vivo ---
  if (WiFi.status() == WL_CONNECTED) {

    // Tenta reconectar MQTT se necessário (non-blocking)
    if (!mqttClient.connected()) {
      if (agora - ultimaReconexao >= INTERVALO_RECONEXAO) {
        ultimaReconexao = agora;
        conectarMqtt();
      }
    }

    // mqttClient.loop() DEVE ser chamado frequentemente
    // para processar pacotes MQTT (keep-alive, etc.)
    mqttClient.loop();
  }

  // --- 3. Leitura e publicação periódica ---
  if (agora - ultimaLeitura >= INTERVALO_LEITURA) {
    ultimaLeitura = agora;

    LeituraSensores dados = lerSensores();

    if (dados.valido && mqttClient.connected()) {
      publicarTelemetria(dados);
    } else if (dados.valido) {
      // Dados válidos mas sem MQTT — loga no Serial para não perder
      Serial.print("[LOCAL] Temp=");
      Serial.print(dados.temperatura);
      Serial.print("°C | Umid=");
      Serial.print(dados.umidade);
      Serial.print("% | Luz=");
      Serial.println(dados.luminosidade);
    }
  }
}
