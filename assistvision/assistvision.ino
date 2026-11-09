#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <ESP32Servo.h>
#include <esp_system.h>

// Wi-Fi
// Roteador do celular
const char* CELULAR_REDE  = "AssistVision";
const char* CELULAR_SENHA = "senha1234";
const unsigned long TEMPO_CONEXAO_MS = 15000;

// Rede propria, se nao achar o celular
const char* PROPRIA_REDE  = "AssistVisionESP";
const char* PROPRIA_SENHA = "assist1234";       // minimo de 8 caracteres

const char* NOME_MDNS = "assistvision";         // endereco: http://assistvision.local

bool usandoCelular = false;
bool wifiConectado = false;

// Pinos
const int PINO_SDA     = 33;
const int PINO_SCL     = 32;
const int PINO_SERVO   = 13;
const int PINO_BATERIA = 34;                    // entrada analogica que funciona com Wi-Fi ligado

// Bateria
const bool  BATERIA_INSTALADA = true;           // false se os resistores nao estiverem ligados
const float FATOR_DIVISOR     = 2.0;            // dois resistores iguais dividem a tensao pela metade
const float AJUSTE_BATERIA    = 1.00;           // calibrar: tensao no multimetro / tensao na pagina
const float BATERIA_BAIXA     = 3.5;
const float BATERIA_CRITICA   = 3.35;
const float SUAVIZACAO_BAT    = 0.2;            // media lenta: a leitura nao pula quando o servo mexe

// Distancias (calibraveis pela pagina)
const float DIST_MEDIO_PADRAO   = 120.0;
const float DIST_FORTE_PADRAO   = 60.0;
const float DIST_MIN_PERMITIDA  = 10.0;
const float DIST_MAX_PERMITIDA  = 200.0;
const float HISTERESE           = 5.0;
const float DIST_FORA           = 999.0;

float distMedio = DIST_MEDIO_PADRAO;
float distForte = DIST_FORTE_PADRAO;

// Servo
const int ANGULO_REPOUSO = 90;
const int ANGULO_MEDIO   = 126;
const int ANGULO_FORTE   = 139;
const int ANGULO_LIMITE  = 140;
const int PASSO          = 4;

// Filtro e falhas
const float SUAVIZACAO                = 0.4;     // 0 a 1: menor = mais estavel, maior = reage mais rapido
const float ALCANCE_MAX_CM            = 200.0;   // acima disso o VL53L0X nao e confiavel
const int   INVALIDAS_PARA_FORA       = 5;       // leituras duvidosas seguidas ate considerar "nada na frente"
const int   FALHAS_PARA_ERRO          = 5;
const int   CONFIRMAR_LEITURAS        = 3;       // leituras seguidas pedindo o mesmo modo antes de trocar
const uint32_t TEMPO_MEDICAO_US       = 100000;  // 100 ms por medicao: mais alcance e menos ruido
const bool  MOSTRAR_LEITURAS          = false;   // true = mostra cada leitura crua no Monitor Serial
const unsigned long TENTAR_SENSOR_MS  = 5000;
const unsigned long LER_BATERIA_MS    = 2000;
const unsigned long SEM_CONTATO_MS    = 8000;
const unsigned long RECONECTAR_MS     = 10000;   // tenta reconectar o Wi-Fi a cada 10 s se cair

enum Modo { REPOUSO, MEDIO, FORTE };

Adafruit_VL53L0X sensor;
Servo servo;
WebServer server(80);
Preferences prefs;

Modo  modoAtual         = REPOUSO;
Modo  modoCandidato     = REPOUSO;
int   confirmacoes      = 0;
float distanciaFiltrada = DIST_FORA;
int   anguloAtual       = ANGULO_REPOUSO;

bool sensorOk      = false;
bool arquivosOk    = false;
int  falhasSeguidas = 0;
int  invalidasSeguidas = 0;

// As 3 ultimas leituras validas, para tirar a mediana (descarta picos isolados)
float ultimasLeituras[3];
int   totalLeituras = 0;
unsigned long ultimaTentativaSensor = 0;

float tensaoBateria = 0;
bool  bateriaLida   = false;
unsigned long ultimaLeituraBateria = 0;
unsigned long ultimaReconexao = 0;

unsigned long ultimoContatoPagina = 0;
bool paginaConectada = false;

// Registro de eventos
const int MAX_EVENTOS = 10;
unsigned long eventoTempo[MAX_EVENTOS];
String eventoTexto[MAX_EVENTOS];
int totalEventos = 0;

void registrarEvento(const String& texto) {
  for (int i = MAX_EVENTOS - 1; i > 0; i--) {
    eventoTempo[i] = eventoTempo[i - 1];
    eventoTexto[i] = eventoTexto[i - 1];
  }
  eventoTempo[0] = millis() / 1000;
  eventoTexto[0] = texto;
  if (totalEventos < MAX_EVENTOS) totalEventos++;
  Serial.println("[EVENTO] " + texto);
}

// Sensor
void tentarIniciarSensor() {
  ultimaTentativaSensor = millis();
  if (sensor.begin()) {
    sensor.configSensor(Adafruit_VL53L0X::VL53L0X_SENSE_LONG_RANGE);
    sensor.setMeasurementTimingBudgetMicroSeconds(TEMPO_MEDICAO_US);
    sensorOk = true;
    falhasSeguidas = 0;
    invalidasSeguidas = 0;
    totalLeituras = 0;
    registrarEvento("Sensor de distancia conectado");
  }
}

// Devolve false se o sensor nao respondeu (problema de fio/hardware).
// "valida" diz se a medida e confiavel. Medida duvidosa nao e usada.
bool lerDistanciaCm(float& distancia, bool& valida) {
  VL53L0X_RangingMeasurementData_t medida;
  VL53L0X_Error erro = sensor.rangingTest(&medida, false);

  if (erro != VL53L0X_ERROR_NONE) return false;

  distancia = medida.RangeMilliMeter / 10.0;
  valida = (medida.RangeStatus <= 2) && (distancia > 3) && (distancia <= ALCANCE_MAX_CM);

  if (MOSTRAR_LEITURAS) {
    Serial.print("Leitura crua: ");
    Serial.print(distancia, 1);
    Serial.print(" cm | status ");
    Serial.print(medida.RangeStatus);
    Serial.println(valida ? " (ok)" : " (descartada)");
  }
  return true;
}

// Guarda a leitura e devolve a mediana das 3 ultimas (o valor do meio)
float medianaDasUltimas(float nova) {
  if (totalLeituras < 3) {
    ultimasLeituras[totalLeituras++] = nova;
  } else {
    ultimasLeituras[0] = ultimasLeituras[1];
    ultimasLeituras[1] = ultimasLeituras[2];
    ultimasLeituras[2] = nova;
  }
  if (totalLeituras < 3) return nova;

  float a = ultimasLeituras[0], b = ultimasLeituras[1], c = ultimasLeituras[2];
  if ((a <= b && b <= c) || (c <= b && b <= a)) return b;
  if ((b <= a && a <= c) || (c <= a && a <= b)) return a;
  return c;
}

// Bateria
void lerBateria() {
  if (!BATERIA_INSTALADA) return;

  analogReadMilliVolts(PINO_BATERIA);            // primeira leitura descartada
  uint32_t soma = 0;
  for (int i = 0; i < 32; i++) {
    soma += analogReadMilliVolts(PINO_BATERIA);
    delayMicroseconds(200);
  }
  float agora = (soma / 32.0) / 1000.0 * FATOR_DIVISOR * AJUSTE_BATERIA;

  if (!bateriaLida) {
    tensaoBateria = agora;
    bateriaLida = true;
  } else {
    tensaoBateria = SUAVIZACAO_BAT * agora + (1 - SUAVIZACAO_BAT) * tensaoBateria;
  }
}

// A bateria de litio nao descarrega em linha reta: usa uma tabela tensao -> %
int percentualBateria() {
  const float tensoes[]   = {3.30, 3.40, 3.50, 3.60, 3.70, 3.80, 3.90, 4.00, 4.10, 4.20};
  const int   percentuais[] = {   0,    3,    8,   18,   32,   48,   63,   78,   90,  100};
  if (tensaoBateria <= tensoes[0]) return 0;
  if (tensaoBateria >= tensoes[9]) return 100;
  for (int i = 0; i < 9; i++) {
    if (tensaoBateria < tensoes[i + 1]) {
      float t = (tensaoBateria - tensoes[i]) / (tensoes[i + 1] - tensoes[i]);
      return percentuais[i] + t * (percentuais[i + 1] - percentuais[i]);
    }
  }
  return 100;
}

// Modos
Modo decidirModo(float d, Modo atual) {
  switch (atual) {
    case REPOUSO:
      if (d < distForte) return FORTE;
      if (d < distMedio) return MEDIO;
      return REPOUSO;
    case MEDIO:
      if (d < distForte)             return FORTE;
      if (d > distMedio + HISTERESE) return REPOUSO;
      return MEDIO;
    case FORTE:
      if (d > distMedio + HISTERESE) return REPOUSO;
      if (d > distForte + HISTERESE) return MEDIO;
      return FORTE;
  }
  return REPOUSO;
}

int anguloDoModo(Modo m) {
  if (m == MEDIO) return ANGULO_MEDIO;
  if (m == FORTE) return ANGULO_FORTE;
  return ANGULO_REPOUSO;
}

const char* nomeModo(Modo m) {
  if (m == MEDIO) return "MEDIO";
  if (m == FORTE) return "FORTE";
  return "REPOUSO";
}

void moverServo(int alvo) {
  alvo = constrain(alvo, ANGULO_REPOUSO, ANGULO_LIMITE);
  if (anguloAtual == alvo) return;
  if (anguloAtual < alvo) anguloAtual = min(anguloAtual + PASSO, alvo);
  else                    anguloAtual = max(anguloAtual - PASSO, alvo);
  servo.write(anguloAtual);
}

// Calibracao (Preferences)
void carregarCalibracao() {
  prefs.begin("assistvision", true);
  distMedio = prefs.getFloat("distMedio", DIST_MEDIO_PADRAO);
  distForte = prefs.getFloat("distForte", DIST_FORTE_PADRAO);
  prefs.end();
}

void salvarCalibracao() {
  prefs.begin("assistvision", false);
  prefs.putFloat("distMedio", distMedio);
  prefs.putFloat("distForte", distForte);
  prefs.end();
}

// Conexao Wi-Fi
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                          // sem economia de energia: responde sem atraso
  WiFi.setAutoReconnect(true);
  WiFi.begin(CELULAR_REDE, CELULAR_SENHA);

  Serial.print("Conectando no roteador do celular");
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < TEMPO_CONEXAO_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    usandoCelular = true;
    wifiConectado = true;
    registrarEvento(String("Conectado na rede ") + CELULAR_REDE);
    Serial.print("Abra no navegador: http://");
    Serial.println(WiFi.localIP());
  } else {
    usandoCelular = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    WiFi.softAP(PROPRIA_REDE, PROPRIA_SENHA);
    registrarEvento("Celular nao encontrado: rede propria criada");
    Serial.print("Rede propria: ");
    Serial.println(PROPRIA_REDE);
    Serial.print("Abra no navegador: http://");
    Serial.println(WiFi.softAPIP());
  }

  if (MDNS.begin(NOME_MDNS)) {
    Serial.print("Ou tambem: http://");
    Serial.print(NOME_MDNS);
    Serial.println(".local");
  }
}

void verificarWiFi() {
  if (!usandoCelular) return;
  bool agora = (WiFi.status() == WL_CONNECTED);

  if (!agora && millis() - ultimaReconexao > RECONECTAR_MS) {
    ultimaReconexao = millis();
    WiFi.reconnect();
  }

  if (agora == wifiConectado) return;
  wifiConectado = agora;
  if (agora) registrarEvento("Wi-Fi reconectado. IP: " + WiFi.localIP().toString());
  else       registrarEvento("Wi-Fi do celular caiu. Tentando reconectar...");
}

// Mostra no historico por que o ESP32 reiniciou (ajuda a achar queda de energia)
void registrarMotivoReinicio() {
  esp_reset_reason_t motivo = esp_reset_reason();
  if (motivo == ESP_RST_BROWNOUT)                         registrarEvento("Reiniciou por queda de energia (bateria/servo)");
  else if (motivo == ESP_RST_PANIC)                       registrarEvento("Reiniciou por erro no programa");
  else if (motivo == ESP_RST_TASK_WDT || motivo == ESP_RST_INT_WDT || motivo == ESP_RST_WDT)
                                                          registrarEvento("Reiniciou por travamento (watchdog)");
}

// Paginas
void enviarArquivo(const char* caminho, const char* tipo) {
  File arquivo = LittleFS.open(caminho, "r");
  if (!arquivo) {
    server.send(500, "text/plain; charset=utf-8",
                "Arquivo do site nao encontrado. Envie a pasta data com o ESP32 Sketch Data Upload.");
    return;
  }
  server.streamFile(arquivo, tipo);
  arquivo.close();
}

void responderJson(int codigo, JsonDocument& doc) {
  String saida;
  serializeJson(doc, saida);
  server.send(codigo, "application/json", saida);
}

void rotaStatus() {
  ultimoContatoPagina = millis();
  if (!paginaConectada) {
    paginaConectada = true;
    registrarEvento("Interface web conectada");
  }

  JsonDocument doc;
  if (sensorOk && distanciaFiltrada < DIST_FORA) doc["distancia"] = round(distanciaFiltrada * 10) / 10.0;
  else                                           doc["distancia"] = nullptr;
  doc["modo"]        = nomeModo(modoAtual);
  doc["angulo"]      = anguloAtual;
  doc["sensorOk"]    = sensorOk;
  doc["tempoLigado"] = millis() / 1000;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["modo"] = usandoCelular ? "celular" : "propria";
  wifi["rede"] = usandoCelular ? CELULAR_REDE : PROPRIA_REDE;
  wifi["ip"]   = usandoCelular ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  if (usandoCelular) wifi["sinal"] = WiFi.RSSI();

  JsonObject config = doc["config"].to<JsonObject>();
  config["distMedio"] = distMedio;
  config["distForte"] = distForte;

  JsonObject bateria = doc["bateria"].to<JsonObject>();
  bateria["disponivel"] = BATERIA_INSTALADA;
  if (BATERIA_INSTALADA) {
    bateria["tensao"]     = round(tensaoBateria * 100) / 100.0;
    bateria["leituraPino"] = round(tensaoBateria / FATOR_DIVISOR / AJUSTE_BATERIA * 100) / 100.0;
    bateria["percentual"] = percentualBateria();
  }

  JsonArray erros = doc["erros"].to<JsonArray>();
  if (!sensorOk) erros.add(falhasSeguidas >= FALHAS_PARA_ERRO ? "SENSOR_PAROU" : "SENSOR_AUSENTE");
  if (BATERIA_INSTALADA && tensaoBateria < BATERIA_CRITICA)     erros.add("BATERIA_CRITICA");
  else if (BATERIA_INSTALADA && tensaoBateria < BATERIA_BAIXA)  erros.add("BATERIA_BAIXA");
  if (!arquivosOk) erros.add("ARQUIVOS_AUSENTES");

  JsonArray eventos = doc["eventos"].to<JsonArray>();
  for (int i = 0; i < totalEventos; i++) {
    JsonObject e = eventos.add<JsonObject>();
    e["tempo"] = eventoTempo[i];
    e["texto"] = eventoTexto[i];
  }

  responderJson(200, doc);
}

void rotaSalvarConfig() {
  JsonDocument resposta;

  if (!server.hasArg("distMedio") || !server.hasArg("distForte")) {
    resposta["ok"] = false;
    resposta["mensagem"] = "Faltam valores.";
    responderJson(400, resposta);
    return;
  }

  float novoMedio = server.arg("distMedio").toFloat();
  float novoForte = server.arg("distForte").toFloat();

  if (novoMedio < DIST_MIN_PERMITIDA || novoMedio > DIST_MAX_PERMITIDA ||
      novoForte < DIST_MIN_PERMITIDA || novoForte > DIST_MAX_PERMITIDA) {
    resposta["ok"] = false;
    resposta["mensagem"] = "As distancias precisam ficar entre 10 e 200 cm.";
    responderJson(400, resposta);
    return;
  }

  if (novoForte > novoMedio - 2 * HISTERESE) {
    resposta["ok"] = false;
    resposta["mensagem"] = "A distancia do modo forte precisa ser pelo menos 10 cm menor que a do medio.";
    responderJson(400, resposta);
    return;
  }

  distMedio = novoMedio;
  distForte = novoForte;
  salvarCalibracao();
  registrarEvento("Calibracao salva: medio " + String(distMedio, 0) + " cm, forte " + String(distForte, 0) + " cm");

  resposta["ok"] = true;
  resposta["mensagem"] = "Calibracao salva.";
  responderJson(200, resposta);
}

void rotaConfigPadrao() {
  distMedio = DIST_MEDIO_PADRAO;
  distForte = DIST_FORTE_PADRAO;
  salvarCalibracao();
  registrarEvento("Calibracao voltou ao padrao");

  JsonDocument resposta;
  resposta["ok"] = true;
  resposta["mensagem"] = "Valores padrao restaurados.";
  responderJson(200, resposta);
}

// Setup
void setup() {
  Serial.begin(115200);
  registrarMotivoReinicio();

  servo.attach(PINO_SERVO, 500, 2400);
  servo.write(ANGULO_REPOUSO);

  arquivosOk = LittleFS.begin(true) && LittleFS.exists("/index.html");
  if (!arquivosOk) registrarEvento("Arquivos do site nao encontrados");

  carregarCalibracao();

  Wire.begin(PINO_SDA, PINO_SCL);
  tentarIniciarSensor();
  if (!sensorOk) registrarEvento("Sensor de distancia nao encontrado");

  analogSetPinAttenuation(PINO_BATERIA, ADC_11db);
  lerBateria();

  conectarWiFi();

  server.on("/",                   HTTP_GET,  []() { enviarArquivo("/index.html", "text/html; charset=utf-8"); });
  server.on("/style.css",          HTTP_GET,  []() { enviarArquivo("/style.css", "text/css"); });
  server.on("/script.js",          HTTP_GET,  []() { enviarArquivo("/script.js", "application/javascript"); });
  server.on("/status",             HTTP_GET,  rotaStatus);
  server.on("/config",             HTTP_POST, rotaSalvarConfig);
  server.on("/config/padrao",      HTTP_POST, rotaConfigPadrao);
  server.onNotFound([]() { server.send(404, "text/plain", "Pagina nao encontrada"); });
  server.begin();

  registrarEvento("AssistVision ligado");
}

// Loop
void loop() {
  server.handleClient();

  if (!sensorOk) {
    if (millis() - ultimaTentativaSensor > TENTAR_SENSOR_MS) tentarIniciarSensor();
    modoAtual = REPOUSO;
  } else {
    float d;
    bool valida;
    bool leituraNova = false;
    if (lerDistanciaCm(d, valida)) {
      falhasSeguidas = 0;

      if (valida) {
        invalidasSeguidas = 0;
        leituraNova = true;
        float m = medianaDasUltimas(d);
        if (distanciaFiltrada >= DIST_FORA) distanciaFiltrada = m;   // voltou a ver: comeca do valor real
        else distanciaFiltrada = SUAVIZACAO * m + (1 - SUAVIZACAO) * distanciaFiltrada;
      } else {
        // Leitura duvidosa: nao entra na media. Mantem o ultimo valor bom por um tempo.
        invalidasSeguidas++;
        if (invalidasSeguidas >= INVALIDAS_PARA_FORA) {
          distanciaFiltrada = DIST_FORA;   // muitas seguidas: nada na frente
          totalLeituras = 0;
          leituraNova = true;
        }
      }
    } else {
      falhasSeguidas++;
      if (falhasSeguidas >= FALHAS_PARA_ERRO) {
        sensorOk = false;
        distanciaFiltrada = DIST_FORA;
        registrarEvento("Sensor parou de responder");
      }
    }

    Modo novoModo = sensorOk ? decidirModo(distanciaFiltrada, modoAtual) : REPOUSO;

    if (!sensorOk || novoModo == modoAtual) {
      confirmacoes = 0;
      if (!sensorOk) modoAtual = REPOUSO;
    } else if (leituraNova) {
      // so troca de modo se varias leituras seguidas concordarem (ignora leitura falsa isolada)
      if (novoModo != modoCandidato) {
        modoCandidato = novoModo;
        confirmacoes = 0;
      }
      confirmacoes++;
      if (confirmacoes >= CONFIRMAR_LEITURAS) {
        Serial.print(">>> Mudou para: ");
        Serial.println(nomeModo(novoModo));
        modoAtual = novoModo;
        confirmacoes = 0;
      }
    }
  }

  moverServo(anguloDoModo(modoAtual));

  if (millis() - ultimaLeituraBateria > LER_BATERIA_MS) {
    ultimaLeituraBateria = millis();
    lerBateria();
  }

  verificarWiFi();

  if (paginaConectada && millis() - ultimoContatoPagina > SEM_CONTATO_MS) {
    paginaConectada = false;
    registrarEvento("Interface web sem comunicacao");
  }

  delay(10);
}
