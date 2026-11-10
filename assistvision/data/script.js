// ===================== CONFIGURACAO =====================
const INTERVALO_MS = 700;        // de quanto em quanto tempo pergunta ao ESP32
const TEMPO_LIMITE_MS = 3000;    // quanto espera cada resposta
const SINAL_PERDIDO_MS = 6000;   // sem resposta por esse tempo = sinal perdido

// Aberto direto do computador (file://) = modo demonstracao, sem ESP32
const MODO_DEMO = location.protocol === "file:";

const MENSAGENS_ERRO = {
  SENSOR_AUSENTE: "Sensor de distância não encontrado. Confira os fios VCC (3V3), GND, SDA (33) e SCL (32).",
  SENSOR_PAROU: "O sensor parou de responder. Tentando reconectar a cada 5 segundos.",
  BATERIA_BAIXA: "Bateria baixa. Recarregue pelo USB-C do carregador.",
  BATERIA_CRITICA: "Bateria crítica. O dispositivo pode desligar a qualquer momento.",
  ARQUIVOS_AUSENTES: "Arquivos do site não encontrados no ESP32."
};

let ultimaResposta = Date.now();
let sinalPerdido = false;
let ultimaDistancia = null;
let formularioPreenchido = false;

const $ = (id) => document.getElementById(id);

// ===================== MODO DEMONSTRACAO =====================
const demo = {
  inicio: Date.now(),
  distMedio: 120,
  distForte: 60,
  modo: "REPOUSO",
  angulo: 90,
  eventos: [{ tempo: 0, texto: "AssistVision ligado (simulado)" }]
};

function statusDemo() {
  const t = (Date.now() - demo.inicio) / 1000;
  const distancia = Math.round((95 + 70 * Math.sin(t / 4)) * 10) / 10;
  const alvo = distancia < demo.distForte ? "FORTE" : distancia < demo.distMedio ? "MEDIO" : "REPOUSO";
  demo.modo = alvo;
  demo.angulo = alvo === "FORTE" ? 139 : alvo === "MEDIO" ? 128 : 90;
  return {
    distancia,
    modo: demo.modo,
    angulo: demo.angulo,
    sensorOk: true,
    tempoLigado: Math.floor(t),
    wifi: { modo: "celular", rede: "Celular do grupo", ip: "192.168.43.25", mac: "AA:BB:CC:DD:EE:FF", sinal: -58 },
    config: { distMedio: demo.distMedio, distForte: demo.distForte },
    bateria: { disponivel: true, tensao: 3.86, percentual: 66 },
    erros: [],
    eventos: demo.eventos
  };
}

// ===================== BUSCAR DADOS =====================
async function buscarStatus() {
  if (MODO_DEMO) return statusDemo();

  const controle = new AbortController();
  const timer = setTimeout(() => controle.abort(), TEMPO_LIMITE_MS);
  try {
    const resposta = await fetch("/status", { signal: controle.signal, cache: "no-store" });
    if (!resposta.ok) throw new Error("HTTP " + resposta.status);
    return await resposta.json();
  } finally {
    clearTimeout(timer);
  }
}

async function atualizar() {
  try {
    const dados = await buscarStatus();
    ultimaResposta = Date.now();
    if (sinalPerdido) recuperarSinal();
    mostrar(dados);
  } catch (erro) {
    // sem resposta: quem decide se o sinal caiu e verificarSinal()
  }
}

// ===================== PERDA DE SINAL =====================
function verificarSinal() {
  const semResposta = Date.now() - ultimaResposta;
  if (semResposta < SINAL_PERDIDO_MS) return;

  $("tempo-sem-sinal").textContent = Math.floor(semResposta / 1000);
  if (!sinalPerdido) {
    sinalPerdido = true;
    $("faixa-sinal").hidden = false;
    $("conexao").textContent = "Sem sinal";
    $("conexao").className = "etiqueta etiqueta-erro";
    document.title = "⚠ Sinal perdido · AssistVision";
    if (navigator.vibrate) navigator.vibrate([300, 150, 300]);
    adicionarEventoLocal("Sinal perdido com o dispositivo");
  }
}

function recuperarSinal() {
  sinalPerdido = false;
  $("faixa-sinal").hidden = true;
  document.title = "AssistVision";
  adicionarEventoLocal("Sinal recuperado");
}

const eventosLocais = [];
function adicionarEventoLocal(texto) {
  const hora = new Date().toLocaleTimeString("pt-BR");
  eventosLocais.unshift({ hora, texto });
  if (eventosLocais.length > 5) eventosLocais.pop();
}

// ===================== MOSTRAR NA TELA =====================
function mostrar(d) {
  $("conexao").textContent = MODO_DEMO ? "Demonstração" : "Conectado";
  $("conexao").className = "etiqueta etiqueta-ok";

  ultimaDistancia = d.distancia;
  $("distancia").textContent = d.distancia === null ? "--" : d.distancia.toFixed(1);
  $("angulo").textContent = d.angulo;

  const modo = $("modo");
  modo.textContent = d.modo === "MEDIO" ? "MÉDIO" : d.modo;
  modo.className = "modo modo-" + d.modo.toLowerCase();

  $("sensor-estado").textContent = d.sensorOk ? "funcionando" : "com problema";
  $("tempo-ligado").textContent = formatarTempo(d.tempoLigado);
  mostrarWiFi(d.wifi);

  mostrarBateria(d.bateria);
  mostrarErros(d.erros);
  mostrarEventos(d.eventos);

  if (!formularioPreenchido) {
    $("dist-medio").value = d.config.distMedio;
    $("dist-forte").value = d.config.distForte;
    formularioPreenchido = true;
  }
}

function mostrarWiFi(w) {
  $("wifi-rede").textContent = w.modo === "celular" ? w.rede : w.rede + " (rede própria)";
  $("wifi-ip").textContent = w.ip;
  $("wifi-mac").textContent = w.mac || "--";
  if (w.sinal === undefined) {
    $("wifi-sinal").textContent = "--";
    return;
  }
  const qualidade = w.sinal >= -60 ? "ótimo" : w.sinal >= -70 ? "bom" : w.sinal >= -80 ? "fraco" : "muito fraco";
  $("wifi-sinal").textContent = qualidade + " (" + w.sinal + " dBm)";
}

function mostrarBateria(b) {
  const barra = $("bateria-barra");
  if (!b.disponivel) {
    $("bateria-percentual").textContent = "--";
    $("bateria-texto").textContent = "Medição de bateria ainda não instalada.";
    barra.style.width = "0";
    return;
  }
  $("bateria-percentual").textContent = b.percentual + "%";
  $("bateria-texto").textContent = b.tensao.toFixed(2) + " V";
  barra.style.width = b.percentual + "%";
  barra.className = "barra-preenchida" + (b.percentual <= 10 ? " critica" : b.percentual <= 25 ? " baixa" : "");
}

function mostrarErros(erros) {
  const lista = $("erros");
  lista.innerHTML = "";
  if (erros.length === 0) {
    lista.innerHTML = '<li class="sem-erro">Nenhum erro.</li>';
    return;
  }
  for (const codigo of erros) {
    const item = document.createElement("li");
    item.textContent = MENSAGENS_ERRO[codigo] || codigo;
    lista.appendChild(item);
  }
}

function mostrarEventos(eventos) {
  const lista = $("eventos");
  lista.innerHTML = "";
  for (const e of eventosLocais) {
    lista.appendChild(criarEvento(e.hora + " (celular)", e.texto));
  }
  for (const e of eventos) {
    lista.appendChild(criarEvento("+" + formatarTempo(e.tempo), e.texto));
  }
}

function criarEvento(hora, texto) {
  const item = document.createElement("li");
  const spanHora = document.createElement("span");
  spanHora.className = "hora";
  spanHora.textContent = hora;
  item.appendChild(spanHora);
  item.appendChild(document.createTextNode(texto));
  return item;
}

function formatarTempo(segundos) {
  const h = Math.floor(segundos / 3600);
  const m = Math.floor((segundos % 3600) / 60);
  const s = segundos % 60;
  if (h > 0) return h + "h " + m + "min";
  if (m > 0) return m + "min " + s + "s";
  return s + "s";
}

// ===================== CALIBRACAO =====================
function mensagemCalibracao(texto, ok) {
  const p = $("mensagem-calibracao");
  p.textContent = texto;
  p.className = "mensagem " + (ok ? "ok" : "erro");
}

document.querySelectorAll("[data-usar]").forEach((botao) => {
  botao.addEventListener("click", () => {
    if (ultimaDistancia === null) {
      mensagemCalibracao("Sem leitura do sensor agora. Aponte para um obstáculo.", false);
      return;
    }
    $(botao.dataset.usar).value = Math.round(ultimaDistancia);
  });
});

$("form-calibracao").addEventListener("submit", async (evento) => {
  evento.preventDefault();
  const distMedio = Number($("dist-medio").value);
  const distForte = Number($("dist-forte").value);

  if (distForte > distMedio - 10) {
    mensagemCalibracao("A distância do modo forte precisa ser pelo menos 10 cm menor que a do médio.", false);
    return;
  }

  if (MODO_DEMO) {
    demo.distMedio = distMedio;
    demo.distForte = distForte;
    demo.eventos.unshift({ tempo: 0, texto: "Calibração salva (simulado)" });
    mensagemCalibracao("Calibração salva (simulado).", true);
    return;
  }

  try {
    const resposta = await fetch("/config", {
      method: "POST",
      body: new URLSearchParams({ distMedio, distForte })
    });
    const dados = await resposta.json();
    mensagemCalibracao(dados.mensagem, dados.ok);
  } catch (erro) {
    mensagemCalibracao("Não foi possível falar com o dispositivo.", false);
  }
});

$("restaurar").addEventListener("click", async () => {
  if (MODO_DEMO) {
    demo.distMedio = 120;
    demo.distForte = 60;
  } else {
    try {
      const resposta = await fetch("/config/padrao", { method: "POST" });
      const dados = await resposta.json();
      mensagemCalibracao(dados.mensagem, dados.ok);
    } catch (erro) {
      mensagemCalibracao("Não foi possível falar com o dispositivo.", false);
      return;
    }
  }
  $("dist-medio").value = 120;
  $("dist-forte").value = 60;
  if (MODO_DEMO) mensagemCalibracao("Valores padrão restaurados (simulado).", true);
});

// ===================== INICIO =====================
if (MODO_DEMO) $("modo-demo").hidden = false;
// Pergunta de novo so depois da resposta anterior, para nao acumular pedidos no ESP32
async function ciclo() {
  await atualizar();
  setTimeout(ciclo, INTERVALO_MS);
}
ciclo();
setInterval(verificarSinal, 500);
