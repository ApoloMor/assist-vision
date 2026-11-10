#include <WiFi.h>            // conectar no Wi-Fi ou criar rede propria
#include <ESPmDNS.h>         // endereco assistvision.local, pra nao precisar do IP
#include <WebServer.h>       // servidor HTTP na porta 80
#include <LittleFS.h>        // "pendrive" interno onde ficam os arquivos da pagina
#include <Preferences.h>     // salva a calibracao na memoria (nao some ao desligar)
#include <ArduinoJson.h>     // monta as respostas em JSON
#include <Wire.h>            // comunicacao I2C com o sensor (SDA/SCL)
#include <Adafruit_VL53L0X.h> // biblioteca do sensor laser de distancia
#include <ESP32Servo.h>      // gera o sinal que controla o servo
#include <esp_system.h>      // pra saber por que o ESP32 reiniciou

// Wi-Fi
// rede do hotspot do celular (primeira opcao)
const char* CELULAR_REDE  = "AssistVision";
const char* CELULAR_SENHA = "senha1234";
const unsigned long TEMPO_CONEXAO_MS = 15000;   // espera ate 15 s pra conectar no celular

// se nao achar o celular, o ESP32 cria essa rede (abre em 192.168.4.1)
const char* PROPRIA_REDE  = "AssistVisionESP";
const char* PROPRIA_SENHA = "assist1234";       // minimo de 8 caracteres

const char* NOME_MDNS = "assistvision";         // endereco: http://assistvision.local

bool usandoCelular = false;   // true = conectado no hotspot, false = rede propria
bool wifiConectado = false;   // como estava a conexao na ultima checagem

// Pinos (tudo no lado esquerdo do ESP32)
const int PINO_SDA     = 33;   // dados do I2C (sensor)
const int PINO_SCL     = 32;   // clock do I2C (sensor)
const int PINO_SERVO   = 13;   // sinal do servo
const int PINO_BATERIA = 34;   // le o meio do divisor de tensao (funciona com Wi-Fi ligado)

// Bateria
const bool  BATERIA_INSTALADA = true;    // false se os resistores nao estiverem ligados
const float FATOR_DIVISOR     = 2.0;     // os 2 resistores iguais cortam a tensao pela metade, entao multiplica por 2
const float AJUSTE_BATERIA    = 1.00;    // ajuste fino pra bater com o multimetro
const float BATERIA_BAIXA     = 3.5;     // abaixo disso a pagina avisa bateria baixa
const float BATERIA_CRITICA   = 3.35;    // abaixo disso avisa bateria critica
const float SUAVIZACAO_BAT    = 0.2;     // media lenta pra % nao pular quando o servo mexe

// Distancias dos modos (padrao de fabrica, mudam pela pagina)
const float DIST_MEDIO_PADRAO   = 120.0;
const float DIST_FORTE_PADRAO   = 60.0;
const float DIST_MIN_PERMITIDA  = 10.0;   // menor valor aceito na calibracao
const float DIST_MAX_PERMITIDA  = 200.0;  // maior valor aceito na calibracao
const float HISTERESE           = 5.0;    // folga pra sair de um modo (evita vai e volta no limite)
const float DIST_FORA           = 999.0;  // codigo pra "nada na frente"

float distMedio = DIST_MEDIO_PADRAO;   // limite atual do modo medio (vem da memoria)
float distForte = DIST_FORTE_PADRAO;   // limite atual do modo forte (vem da memoria)

// Servo
const int ANGULO_REPOUSO = 90;    // solto
const int ANGULO_MEDIO   = 126;   // aperto medio
const int ANGULO_FORTE   = 139;   // aperto forte
const int ANGULO_LIMITE  = 140;   // trava de seguranca, nunca passa disso
const int PASSO          = 4;     // anda 4 graus por volta do loop, pra nao dar pico de corrente

// Filtros e falhas do sensor
const float SUAVIZACAO                = 0.4;     // peso da leitura nova na media (40% nova, 60% anterior)
const float ALCANCE_MAX_CM            = 200.0;   // acima disso o VL53L0X nao e confiavel
const int   INVALIDAS_PARA_FORA       = 5;       // 5 leituras ruins seguidas = nada na frente
const int   FALHAS_PARA_ERRO          = 5;       // 5 falhas de comunicacao seguidas = sensor travou
const int   CONFIRMAR_LEITURAS        = 3;       // votos seguidos pro mesmo modo antes de trocar
const uint32_t TEMPO_MEDICAO_US       = 100000;  // 100 ms por medida: mais precisa e mais alcance
const bool  MOSTRAR_LEITURAS          = false;   // true = mostra cada leitura crua no Monitor Serial
const unsigned long TENTAR_SENSOR_MS  = 5000;    // se o sensor caiu, tenta religar a cada 5 s
const unsigned long LER_BATERIA_MS    = 2000;    // le a bateria a cada 2 s
const unsigned long SEM_CONTATO_MS    = 8000;    // 8 s sem a pagina pedir nada = perdeu contato
const unsigned long RECONECTAR_MS     = 10000;   // tenta reconectar o Wi-Fi a cada 10 s se cair

enum Modo { REPOUSO, MEDIO, FORTE };   // os 3 modos possiveis

Adafruit_VL53L0X sensor;   // o sensor de distancia
Servo servo;               // o servo
WebServer server(80);      // servidor web na porta 80 (padrao do HTTP)
Preferences prefs;         // acesso a memoria permanente

Modo  modoAtual         = REPOUSO;     // modo valendo agora (e o que mexe o servo)
Modo  modoCandidato     = REPOUSO;     // modo que esta pedindo pra entrar (confirmacao)
int   confirmacoes      = 0;           // quantos votos seguidos o candidato ja teve
float distanciaFiltrada = DIST_FORA;   // distancia final, ja limpa pelos filtros
int   anguloAtual       = ANGULO_REPOUSO;

bool sensorOk      = false;    // o sensor esta respondendo?
bool arquivosOk    = false;    // os arquivos da pagina estao no LittleFS?
int  falhasSeguidas = 0;       // falhas de comunicacao seguidas com o sensor
int  invalidasSeguidas = 0;    // leituras ruins seguidas

// as 3 ultimas leituras validas, pra mediana (fila: a mais velha sai, a nova entra)
float ultimasLeituras[3];
int   totalLeituras = 0;
unsigned long ultimaTentativaSensor = 0;

float tensaoBateria = 0;
bool  bateriaLida   = false;
unsigned long ultimaLeituraBateria = 0;
unsigned long ultimaReconexao = 0;

unsigned long ultimoContatoPagina = 0;   // hora do ultimo pedido da pagina
bool paginaConectada = false;

// Historico: guarda os 10 ultimos eventos pra mostrar na pagina
const int MAX_EVENTOS = 10;
unsigned long eventoTempo[MAX_EVENTOS];
String eventoTexto[MAX_EVENTOS];
int totalEventos = 0;

// coloca um evento novo no topo do historico
void registrarEvento(const String& texto) {
  // empurra todo mundo uma casa pra baixo (o mais antigo sai)
  for (int i = MAX_EVENTOS - 1; i > 0; i--) {
    eventoTempo[i] = eventoTempo[i - 1];
    eventoTexto[i] = eventoTexto[i - 1];
  }
  eventoTempo[0] = millis() / 1000;   // segundos desde que ligou (o ESP32 nao sabe a hora real)
  eventoTexto[0] = texto;
  if (totalEventos < MAX_EVENTOS) totalEventos++;
  Serial.println("[EVENTO] " + texto);
}

// Sensor
// tenta ligar o sensor e configurar (roda no setup e quando o sensor cai)
void tentarIniciarSensor() {
  ultimaTentativaSensor = millis();
  if (sensor.begin()) {
    sensor.configSensor(Adafruit_VL53L0X::VL53L0X_SENSE_LONG_RANGE);   // modo longo alcance
    sensor.setMeasurementTimingBudgetMicroSeconds(TEMPO_MEDICAO_US);   // 100 ms por medida
    sensorOk = true;
    falhasSeguidas = 0;
    invalidasSeguidas = 0;
    totalLeituras = 0;   // zera a fila da mediana
    registrarEvento("Sensor de distancia conectado");
  }
}

// pede uma medida ao sensor
// devolve false se o sensor nao respondeu (fio solto ou travado)
// "valida" diz se a medida e confiavel (filtro 1: descarte)
bool lerDistanciaCm(float& distancia, bool& valida) {
  VL53L0X_RangingMeasurementData_t medida;   // "caixa" com a distancia e a nota do sensor
  VL53L0X_Error erro = sensor.rangingTest(&medida, false);   // aqui o ESP32 recebe a medida

  if (erro != VL53L0X_ERROR_NONE) return false;   // erro de comunicacao: nem usa o numero

  distancia = medida.RangeMilliMeter / 10.0;   // mm pra cm

  // descarte: aceita so se o sensor deu nota ate 2 e esta entre 3 e 200 cm
  valida = (medida.RangeStatus <= 2) && (distancia > 3) && (distancia <= ALCANCE_MAX_CM);

  // so pra teste: mostra cada leitura no Monitor Serial
  if (MOSTRAR_LEITURAS) {
    Serial.print("Leitura crua: ");
    Serial.print(distancia, 1);
    Serial.print(" cm | status ");
    Serial.print(medida.RangeStatus);
    Serial.println(valida ? " (ok)" : " (descartada)");
  }
  return true;   // o sensor respondeu
}

// filtro 2: mediana
// guarda a leitura nova na fila e devolve a do meio (em valor) das 3 ultimas
float medianaDasUltimas(float nova) {
  if (totalLeituras < 3) {
    ultimasLeituras[totalLeituras++] = nova;   // fila ainda nao encheu: so coloca
  } else {
    // fila cheia: todo mundo anda uma casa e a nova entra no fim
    ultimasLeituras[0] = ultimasLeituras[1];
    ultimasLeituras[1] = ultimasLeituras[2];
    ultimasLeituras[2] = nova;
  }
  if (totalLeituras < 3) return nova;   // com menos de 3 nao da pra tirar mediana

  // descobre qual dos 3 esta no meio, sem precisar ordenar
  float a = ultimasLeituras[0], b = ultimasLeituras[1], c = ultimasLeituras[2];
  if ((a <= b && b <= c) || (c <= b && b <= a)) return b;
  if ((b <= a && a <= c) || (c <= a && a <= b)) return a;
  return c;
}

// Bateria
// le a tensao da bateria pelo divisor
void lerBateria() {
  if (!BATERIA_INSTALADA) return;

  analogReadMilliVolts(PINO_BATERIA);   // primeira leitura costuma vir errada, descarta
  uint32_t soma = 0;
  for (int i = 0; i < 32; i++) {   // le 32 vezes e tira a media
    soma += analogReadMilliVolts(PINO_BATERIA);
    delayMicroseconds(200);
  }
  // media em mV -> volts -> x2 pra desfazer o divisor
  float agora = (soma / 32.0) / 1000.0 * FATOR_DIVISOR * AJUSTE_BATERIA;

  if (!bateriaLida) {
    tensaoBateria = agora;   // primeira vez: usa direto
    bateriaLida = true;
  } else {
    tensaoBateria = SUAVIZACAO_BAT * agora + (1 - SUAVIZACAO_BAT) * tensaoBateria;   // media lenta
  }
}

// transforma a tensao em %
// o litio nao descarrega em linha reta, entao usa uma tabela tensao -> %
int percentualBateria() {
  const float tensoes[]   = {3.30, 3.40, 3.50, 3.60, 3.70, 3.80, 3.90, 4.00, 4.10, 4.20};
  const int   percentuais[] = {   0,    3,    8,   18,   32,   48,   63,   78,   90,  100};
  if (tensaoBateria <= tensoes[0]) return 0;
  if (tensaoBateria >= tensoes[9]) return 100;
  // acha entre quais pontos da tabela a tensao esta e faz regra de tres entre eles
  for (int i = 0; i < 9; i++) {
    if (tensaoBateria < tensoes[i + 1]) {
      float t = (tensaoBateria - tensoes[i]) / (tensoes[i + 1] - tensoes[i]);
      return percentuais[i] + t * (percentuais[i + 1] - percentuais[i]);
    }
  }
  return 100;
}

// Modos
// sugere o modo pela distancia, com histerese
// a regra depende do modo atual: pra sair de um modo precisa passar 5 cm do limite
Modo decidirModo(float d, Modo atual) {
  switch (atual) {
    case REPOUSO:
      if (d < distForte) return FORTE;   // testa o forte primeiro
      if (d < distMedio) return MEDIO;
      return REPOUSO;
    case MEDIO:
      if (d < distForte)             return FORTE;
      if (d > distMedio + HISTERESE) return REPOUSO;   // so volta pro repouso acima de medio + 5
      return MEDIO;
    case FORTE:
      if (d > distMedio + HISTERESE) return REPOUSO;
      if (d > distForte + HISTERESE) return MEDIO;     // so volta pro medio acima de forte + 5
      return FORTE;
  }
  return REPOUSO;   // seguranca, na pratica nunca chega aqui
}

// traduz o modo pro angulo do servo
int anguloDoModo(Modo m) {
  if (m == MEDIO) return ANGULO_MEDIO;
  if (m == FORTE) return ANGULO_FORTE;
  return ANGULO_REPOUSO;
}

// traduz o modo pra texto (Monitor Serial e JSON)
const char* nomeModo(Modo m) {
  if (m == MEDIO) return "MEDIO";
  if (m == FORTE) return "FORTE";
  return "REPOUSO";
}

// leva o servo ate o angulo alvo aos poucos (4 graus por vez)
void moverServo(int alvo) {
  alvo = constrain(alvo, ANGULO_REPOUSO, ANGULO_LIMITE);   // trava entre 90 e 140
  if (anguloAtual == alvo) return;   // ja esta no lugar
  if (anguloAtual < alvo) anguloAtual = min(anguloAtual + PASSO, alvo);
  else                    anguloAtual = max(anguloAtual - PASSO, alvo);
  servo.write(anguloAtual);
}

// Calibracao (Preferences)
// le a calibracao salva; se nunca salvou, usa o padrao
void carregarCalibracao() {
  prefs.begin("assistvision", true);   // true = so leitura
  distMedio = prefs.getFloat("distMedio", DIST_MEDIO_PADRAO);
  distForte = prefs.getFloat("distForte", DIST_FORTE_PADRAO);
  prefs.end();
}

// grava a calibracao na memoria permanente
void salvarCalibracao() {
  prefs.begin("assistvision", false);   // false = leitura e escrita
  prefs.putFloat("distMedio", distMedio);
  prefs.putFloat("distForte", distForte);
  prefs.end();
}

// Conexao Wi-Fi
// tenta o hotspot do celular; se nao der em 15 s, cria a propria rede
void conectarWiFi() {
  WiFi.mode(WIFI_STA);             // modo estacao: entra numa rede que ja existe
  WiFi.setSleep(false);            // sem economia de energia: responde sem atraso
  WiFi.setAutoReconnect(true);     // reconexao automatica da propria biblioteca
  WiFi.begin(CELULAR_REDE, CELULAR_SENHA);

  // fica perguntando "ja conectou?" a cada meio segundo, por ate 15 s
  Serial.print("Conectando no roteador do celular");
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < TEMPO_CONEXAO_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    // conectou no celular: o IP veio do hotspot (DHCP)
    usandoCelular = true;
    wifiConectado = true;
    registrarEvento(String("Conectado na rede ") + CELULAR_REDE);
    Serial.print("Abra no navegador: http://");
    Serial.println(WiFi.localIP());
  } else {
    // nao achou o celular: vira roteador (ponto de acesso)
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

  // liga o endereco assistvision.local
  if (MDNS.begin(NOME_MDNS)) {
    Serial.print("Ou tambem: http://");
    Serial.print(NOME_MDNS);
    Serial.println(".local");
  }
}

// roda toda volta do loop: confere se o Wi-Fi caiu e tenta reconectar
void verificarWiFi() {
  if (!usandoCelular) return;   // na rede propria nao tem o que checar
  bool agora = (WiFi.status() == WL_CONNECTED);

  // caiu e ja passaram 10 s desde a ultima tentativa? tenta de novo
  if (!agora && millis() - ultimaReconexao > RECONECTAR_MS) {
    ultimaReconexao = millis();
    WiFi.reconnect();
  }

  // so escreve no historico quando muda (caiu ou voltou)
  if (agora == wifiConectado) return;
  wifiConectado = agora;
  if (agora) registrarEvento("Wi-Fi reconectado. IP: " + WiFi.localIP().toString());
  else       registrarEvento("Wi-Fi do celular caiu. Tentando reconectar...");
}

// mostra no historico por que o ESP32 reiniciou (ajuda a achar queda de energia)
void registrarMotivoReinicio() {
  esp_reset_reason_t motivo = esp_reset_reason();
  if (motivo == ESP_RST_BROWNOUT)                         registrarEvento("Reiniciou por queda de energia (bateria/servo)");
  else if (motivo == ESP_RST_PANIC)                       registrarEvento("Reiniciou por erro no programa");
  else if (motivo == ESP_RST_TASK_WDT || motivo == ESP_RST_INT_WDT || motivo == ESP_RST_WDT)
    registrarEvento("Reiniciou por travamento (watchdog)");
}

// Paginas
// pega um arquivo do LittleFS e manda pro navegador
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

// transforma o JSON em texto e envia com o codigo HTTP (200 ok, 400 erro)
void responderJson(int codigo, JsonDocument& doc) {
  String saida;
  serializeJson(doc, saida);
  server.send(codigo, "application/json", saida);
}

// GET /status: devolve o estado completo em JSON (a pagina pede a cada 0,7 s)
void rotaStatus() {
  // anota que a pagina falou com a gente
  ultimoContatoPagina = millis();
  if (!paginaConectada) {
    paginaConectada = true;
    registrarEvento("Interface web conectada");
  }

  JsonDocument doc;
  // distancia com 1 casa decimal, ou null se nao tem nada na frente / sensor fora
  if (sensorOk && distanciaFiltrada < DIST_FORA) doc["distancia"] = round(distanciaFiltrada * 10) / 10.0;
  else                                           doc["distancia"] = nullptr;
  doc["modo"]        = nomeModo(modoAtual);
  doc["angulo"]      = anguloAtual;
  doc["sensorOk"]    = sensorOk;
  doc["tempoLigado"] = millis() / 1000;

  // grupo "wifi" dentro do JSON
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["modo"] = usandoCelular ? "celular" : "propria";
  wifi["rede"] = usandoCelular ? CELULAR_REDE : PROPRIA_REDE;
  wifi["ip"]   = usandoCelular ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  wifi["mac"]  = usandoCelular ? WiFi.macAddress() : WiFi.softAPmacAddress();
  if (usandoCelular) wifi["sinal"] = WiFi.RSSI();   // forca do sinal em dBm

  // calibracao atual
  JsonObject config = doc["config"].to<JsonObject>();
  config["distMedio"] = distMedio;
  config["distForte"] = distForte;

  // bateria
  JsonObject bateria = doc["bateria"].to<JsonObject>();
  bateria["disponivel"] = BATERIA_INSTALADA;
  if (BATERIA_INSTALADA) {
    bateria["tensao"]     = round(tensaoBateria * 100) / 100.0;
    bateria["leituraPino"] = round(tensaoBateria / FATOR_DIVISOR / AJUSTE_BATERIA * 100) / 100.0;   // o que chega no pino (metade)
    bateria["percentual"] = percentualBateria();
  }

  // lista de erros (a pagina traduz cada codigo numa mensagem)
  JsonArray erros = doc["erros"].to<JsonArray>();
  if (!sensorOk) erros.add(falhasSeguidas >= FALHAS_PARA_ERRO ? "SENSOR_PAROU" : "SENSOR_AUSENTE");
  if (BATERIA_INSTALADA && tensaoBateria < BATERIA_CRITICA)     erros.add("BATERIA_CRITICA");
  else if (BATERIA_INSTALADA && tensaoBateria < BATERIA_BAIXA)  erros.add("BATERIA_BAIXA");
  if (!arquivosOk) erros.add("ARQUIVOS_AUSENTES");

  // historico
  JsonArray eventos = doc["eventos"].to<JsonArray>();
  for (int i = 0; i < totalEventos; i++) {
    JsonObject e = eventos.add<JsonObject>();
    e["tempo"] = eventoTempo[i];
    e["texto"] = eventoTexto[i];
  }

  responderJson(200, doc);
}

// POST /config: recebe a calibracao nova, valida e salva
void rotaSalvarConfig() {
  JsonDocument resposta;

  // faltou algum valor? erro 400
  if (!server.hasArg("distMedio") || !server.hasArg("distForte")) {
    resposta["ok"] = false;
    resposta["mensagem"] = "Faltam valores.";
    responderJson(400, resposta);
    return;
  }

  float novoMedio = server.arg("distMedio").toFloat();
  float novoForte = server.arg("distForte").toFloat();

  // fora de 10 a 200 cm? erro 400
  if (novoMedio < DIST_MIN_PERMITIDA || novoMedio > DIST_MAX_PERMITIDA ||
      novoForte < DIST_MIN_PERMITIDA || novoForte > DIST_MAX_PERMITIDA) {
    resposta["ok"] = false;
    resposta["mensagem"] = "As distancias precisam ficar entre 10 e 200 cm.";
    responderJson(400, resposta);
    return;
  }

  // forte precisa ser pelo menos 10 cm menor que o medio, senao as folgas da histerese se sobrepoem
  if (novoForte > novoMedio - 2 * HISTERESE) {
    resposta["ok"] = false;
    resposta["mensagem"] = "A distancia do modo forte precisa ser pelo menos 10 cm menor que a do medio.";
    responderJson(400, resposta);
    return;
  }

  // passou em tudo: aplica e salva na memoria
  distMedio = novoMedio;
  distForte = novoForte;
  salvarCalibracao();
  registrarEvento("Calibracao salva: medio " + String(distMedio, 0) + " cm, forte " + String(distForte, 0) + " cm");

  resposta["ok"] = true;
  resposta["mensagem"] = "Calibracao salva.";
  responderJson(200, resposta);
}

// POST /config/padrao: volta a calibracao pros valores de fabrica
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

// Setup: roda uma vez quando liga
void setup() {
  Serial.begin(115200);
  registrarMotivoReinicio();

  // liga o servo e deixa solto (90 graus)
  servo.attach(PINO_SERVO, 500, 2400);
  servo.write(ANGULO_REPOUSO);

  // liga o LittleFS e confere se a pagina esta la
  arquivosOk = LittleFS.begin(true) && LittleFS.exists("/index.html");
  if (!arquivosOk) registrarEvento("Arquivos do site nao encontrados");

  carregarCalibracao();

  // liga o I2C nos pinos 33/32 e tenta ligar o sensor
  Wire.begin(PINO_SDA, PINO_SCL);
  tentarIniciarSensor();
  if (!sensorOk) registrarEvento("Sensor de distancia nao encontrado");

  // ajusta o pino da bateria pra ler ate ~3,1 V e faz a primeira leitura
  analogSetPinAttenuation(PINO_BATERIA, ADC_11db);
  lerBateria();

  conectarWiFi();

  // rotas do servidor: endereco + metodo -> funcao que responde
  server.on("/",                   HTTP_GET,  []() { enviarArquivo("/index.html", "text/html; charset=utf-8"); });
  server.on("/style.css",          HTTP_GET,  []() { enviarArquivo("/style.css", "text/css"); });
  server.on("/script.js",          HTTP_GET,  []() { enviarArquivo("/script.js", "application/javascript"); });
  server.on("/status",             HTTP_GET,  rotaStatus);
  server.on("/config",             HTTP_POST, rotaSalvarConfig);
  server.on("/config/padrao",      HTTP_POST, rotaConfigPadrao);
  server.onNotFound([]() { server.send(404, "text/plain", "Pagina nao encontrada"); });
  server.begin();   // comeca a escutar na porta 80

  registrarEvento("AssistVision ligado");
}

// Loop: repete sem parar
void loop() {
  server.handleClient();   // atende os pedidos da pagina

  if (!sensorOk) {
    // sensor fora: tenta religar a cada 5 s e deixa o servo solto
    if (millis() - ultimaTentativaSensor > TENTAR_SENSOR_MS) tentarIniciarSensor();
    modoAtual = REPOUSO;
  } else {
    float d;
    bool valida;
    bool leituraNova = false;   // true se chegou dado novo nesse ciclo (so isso conta voto)
    if (lerDistanciaCm(d, valida)) {
      falhasSeguidas = 0;   // o sensor respondeu

      if (valida) {
        // leitura passou no descarte: vai pra mediana e pra media
        invalidasSeguidas = 0;
        leituraNova = true;
        float m = medianaDasUltimas(d);   // filtro 2: mediana
        if (distanciaFiltrada >= DIST_FORA) distanciaFiltrada = m;   // voltou a ver: comeca do valor real
        else distanciaFiltrada = SUAVIZACAO * m + (1 - SUAVIZACAO) * distanciaFiltrada;   // filtro 3: media (40% nova + 60% anterior)
      } else {
        // leitura ruim: nao entra na conta, mantem o ultimo valor bom
        invalidasSeguidas++;
        if (invalidasSeguidas >= INVALIDAS_PARA_FORA) {
          distanciaFiltrada = DIST_FORA;   // 5 ruins seguidas: nada na frente
          totalLeituras = 0;               // zera a fila da mediana
          leituraNova = true;
        }
      }
    } else {
      // sensor nao respondeu
      falhasSeguidas++;
      if (falhasSeguidas >= FALHAS_PARA_ERRO) {
        sensorOk = false;   // 5 falhas seguidas: considera que travou
        distanciaFiltrada = DIST_FORA;
        registrarEvento("Sensor parou de responder");
      }
    }

    // pergunta qual modo essa distancia pede (com histerese)
    Modo novoModo = sensorOk ? decidirModo(distanciaFiltrada, modoAtual) : REPOUSO;

    // filtro 4: confirmacao (so troca com 3 votos seguidos pro mesmo modo)
    if (!sensorOk || novoModo == modoAtual) {
      // pediu o mesmo modo (ou o sensor caiu): zera os votos
      confirmacoes = 0;
      if (!sensorOk) modoAtual = REPOUSO;   // sem sensor, solta na hora (seguranca)
    } else if (leituraNova) {
      // pediu um modo diferente: conta voto
      if (novoModo != modoCandidato) {
        modoCandidato = novoModo;   // mudou o candidato: comeca a contar do zero
        confirmacoes = 0;
      }
      confirmacoes++;
      if (confirmacoes >= CONFIRMAR_LEITURAS) {
        // 3 votos seguidos: agora troca de verdade
        Serial.print(">>> Mudou para: ");
        Serial.println(nomeModo(novoModo));
        modoAtual = novoModo;
        confirmacoes = 0;
      }
    }
  }

  moverServo(anguloDoModo(modoAtual));   // leva o servo pro angulo do modo atual

  // le a bateria a cada 2 s (millis em vez de delay pra nao travar o loop)
  if (millis() - ultimaLeituraBateria > LER_BATERIA_MS) {
    ultimaLeituraBateria = millis();
    lerBateria();
  }

  verificarWiFi();   // confere se o Wi-Fi caiu

  // 8 s sem a pagina pedir o /status: registra que perdeu contato
  if (paginaConectada && millis() - ultimoContatoPagina > SEM_CONTATO_MS) {
    paginaConectada = false;
    registrarEvento("Interface web sem comunicacao");
  }

  delay(10);   // pausinha curta
}
