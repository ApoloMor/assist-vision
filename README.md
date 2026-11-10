# AssistVision

Dispositivo vestível que ajuda pessoas com deficiência visual a perceber obstáculos à frente. Um sensor a laser mede a distância, o ESP32 decide o nível do alerta e um servomotor aperta o pulso em três níveis. O ESP32 também funciona como **servidor HTTP na porta TCP 80**, e qualquer cliente da mesma rede Wi-Fi pode consultar os dados e enviar comandos.

Projeto da disciplina **Conectividade em Sistemas Ciberfísicos**, Engenharia de Software, PUCPR (2026).

## Arquitetura

![Arquitetura do AssistVision](arquitetura.png)

| Camada TCP/IP | O que o projeto usa |
|---|---|
| Aplicação | HTTP/1.1 (`GET /status`, `POST /config`), JSON, mDNS (`assistvision.local`), DHCP |
| Transporte | TCP porta 80 (servidor em escuta); UDP 5353 (mDNS) e 67/68 (DHCP) |
| Internet | IPv4, endereço recebido por DHCP do hotspot ou `192.168.4.1` no modo AP |
| Enlace/Física | Wi-Fi IEEE 802.11 b/g/n em 2,4 GHz |

**Cliente-servidor:** o ESP32 é o servidor (fica em escuta na porta 80 e responde). Os clientes são o navegador (a página consulta `/status` a cada 0,7 s) ou ferramentas como `curl`.

## Hardware

| Componente | Função | Ligação no ESP32 |
|---|---|---|
| ESP32 DevKit V1 | Microcontrolador e servidor HTTP | – |
| VL53L0X | Sensor de distância a laser (I2C) | SDA → GPIO33, SCL → GPIO32, VCC → 5 V, GND |
| Servo SG90 | Atuador tátil (aperta o pulso) | Sinal → GPIO13, VCC → 5 V, GND |
| Bateria 18650 + TP4056 + MT3608 | Alimentação (carregador e elevador para 5,08 V) | MT3608 VOUT → trilhas 5 V e GND |
| 2 resistores de 100 kΩ | Divisor para medir a bateria | Ponto médio → GPIO34 |

## Rotas do servidor (porta 80)

| Rota | Método | Função |
|---|---|---|
| `/` | GET | Página web (guardada no LittleFS) |
| `/status` | GET | JSON com distância, modo, ângulo do servo, bateria, rede, IP, MAC, sinal, erros e histórico |
| `/config` | POST | Comando: altera as distâncias dos modos (`distMedio`, `distForte`) e salva na flash |
| `/config/padrao` | POST | Comando: volta a calibração ao padrão |

Exemplos:
```
curl http://IP_DO_ESP32/status
curl -X POST -d "distMedio=50&distForte=25" http://IP_DO_ESP32/config
```

## Funcionamento

- **Modos:** repouso (servo 90°), médio (126°) e forte (139°), com limite de segurança de 140°.
- **Filtros:** descarte de leituras inválidas, mediana das 3 últimas, média exponencial e confirmação de 3 leituras antes de trocar de modo.
- **Histerese:** 5 cm, para o servo não ficar alternando na fronteira entre modos.
- **Wi-Fi:** conecta no hotspot (modo STA) e reconecta sozinho a cada 10 s se cair. Se não achar a rede, cria a própria (`AssistVisionESP`, `192.168.4.1`).

## Como gravar

1. Arduino IDE 1.8.19 com o pacote **esp32 (Espressif) 3.3.11**.
2. Bibliotecas: `Adafruit_VL53L0X`, `ESP32Servo`, `ArduinoJson` (v7).
3. Ajustar `CELULAR_REDE` e `CELULAR_SENHA` em `assistvision.ino`.
4. Gravar o código e enviar a pasta `data` com **ESP32 Sketch Data Upload** (LittleFS).
5. Abrir `http://assistvision.local` ou o IP mostrado no Monitor Serial (115200).

## Equipe

Alisson Rodrigo da Cunha, Apolo Moreira da Silva, Stéphanie Rocha dos Santos, Tainá Amaral Santos e João Artur Bochi Fernandes.
