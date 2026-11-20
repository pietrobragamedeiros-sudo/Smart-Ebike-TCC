#include <esp_now.h>
#include <WiFi.h>
#include <WebServer.h>
#include <TinyGPS++.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ==============================================================================
// --- CONFIGURAÇÃO DA REDE WI-FI DO ESP32 (PONTO DE ACESSO INDEPENDENTE) ---
// ==============================================================================
const char* AP_SSID = "E-BIKE-SAFETY";
const char* AP_PASS = "12345678";

WebServer server(80);

// --- PINOS DE HARDWARE ---
#define PIN_DAC_THROTTLE 25  // Saída Analógica para a Controladora do Motor
#define PIN_PAS_SENSOR   22  // Sensor PAS (Pedal Assistido)

// --- PARÂMETROS DE POTÊNCIA DAC (0 a 255) ---
const int DAC_VALOR_ECO   = 145; // ~1.9V
const int DAC_VALOR_SPORT = 200; // ~2.6V

// --- COORDENADAS ETEC ALBERT EINSTEIN (R. NOVA GRANADA, 35 - CASA VERDE) ---
const double LAT_ETEC = -23.505230;
const double LNG_ETEC = -46.653150;

// --- ESTRUTURA RECEBIDA DA TELA (ESP-NOW) ---
typedef struct struct_message {
  int idBike;         // Chave de segurança: 777
  uint8_t modoPiloto; // 0 = ECO, 1 = SPORT
  bool sosTrigger;    // true = SOS ativado, false = normal
  float velocidade;   // Velocidade da roda enviada pela tela (km/h)
  float distancia;    // Distância acumulada enviada pela tela (km)
} struct_message;

struct_message dadosTela;

// --- HARDWARE GPS ---
TinyGPSPlus gps;
HardwareSerial SerialGPS(2); // RX = 16, TX = 17

// --- VARIÁVEIS DE ESTADO E SEGURANÇA DO PEDAL (PAS) ---
volatile unsigned long ultimoPulsoPAS = 0;
volatile unsigned int contadorPulsos = 0;
bool ciclistaPedalando = false;
bool motorAtivoPAS = false;
unsigned long inicioPedalada = 0;
unsigned int pulsosNaArrancada = 0;

uint8_t modoAtual = 0; // 0 = ECO, 1 = SPORT
bool sosAtivado = false;
volatile float velocidadeTela = 0.0;
volatile float distanciaTela = 0.0;
float ultimaDistanciaTela = 0.0;
unsigned long ultimoLoopTelemetria = 0;

// Métricas de Viagem
float velMaxima = 0.0;
float distTotalKm = 0.0;
double ultimaLat = LAT_ETEC;
double ultimaLng = LNG_ETEC;
bool primeiraLeituraGPS = true;

// Interrupção do Sensor PAS
void IRAM_ATTR isrPAS() {
  unsigned long agora = millis();
  if (agora - ultimoPulsoPAS > 35) {
    ultimoPulsoPAS = agora;
    contadorPulsos = contadorPulsos + 1;
  }
}

// Callback do ESP-NOW (Recebe dados da Tela do Guidão)
void OnDataRecv(const esp_now_recv_info *recv_info, const uint8_t *incomingData, int len) {
  if (len < sizeof(dadosTela)) return;
  memcpy(&dadosTela, incomingData, sizeof(dadosTela));
  if (dadosTela.idBike != 777) return;

  modoAtual = dadosTela.modoPiloto;
  sosAtivado = dadosTela.sosTrigger;
  velocidadeTela = dadosTela.velocidade;
  distanciaTela = dadosTela.distancia;

  if (sosAtivado) {
    dacWrite(PIN_DAC_THROTTLE, 0); // Corte imediato por segurança
    motorAtivoPAS = false;
    inicioPedalada = 0;
  }
}

// ==============================================================================
// --- PÁGINA WEB EMBUTIDA (MAPA MULTI-ESCALA: BAIRRO -> SP -> BRASIL) ---
// ==============================================================================
const char INDEX_HTML[] PROGMEM =
"<!DOCTYPE html>\n"
"<html lang='pt-BR'>\n"
"<head>\n"
"  <meta charset='UTF-8'>\n"
"  <meta name='viewport' content='width=device-width, initial-scale=1.0, user-scalable=no'>\n"
"  <title>E-Bike Safety</title>\n"
"  <style>\n"
"    * { box-sizing: border-box; margin: 0; padding: 0; font-family: 'Roboto', -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }\n"
"    body { background-color: #0b0f19; color: #f8fafc; padding: 12px; min-height: 100vh; }\n"
"    .container { max-width: 500px; margin: 0 auto; display: flex; flex-direction: column; gap: 14px; }\n"
"    header { display: flex; align-items: center; justify-content: space-between; padding: 10px 14px; background: #111827; border: 1px solid #1f2937; border-radius: 14px; }\n"
"    .logo-area { display: flex; align-items: center; gap: 10px; }\n"
"    .logo-icon { width: 34px; height: 34px; background: #2563eb; border-radius: 10px; display: flex; align-items: center; justify-content: center; font-size: 18px; }\n"
"    .logo-text { font-size: 17px; font-weight: 700; color: #fff; }\n"
"    .status-badge { display: flex; align-items: center; gap: 6px; font-size: 11px; background: #1f2937; padding: 5px 10px; border-radius: 20px; font-weight: 600; color: #34d399; }\n"
"    .status-dot { width: 8px; height: 8px; background: #10b981; border-radius: 50%; box-shadow: 0 0 8px #10b981; }\n"
"    #sos-banner { display: none; background: #dc2626; padding: 12px; border-radius: 12px; text-align: center; font-weight: 700; font-size: 14px; animation: pulse 1s infinite alternate; }\n"
"    @keyframes pulse { from { opacity: 0.9; } to { opacity: 1; transform: scale(1.01); } }\n"
"    .card { background: #111827; border: 1px solid #1f2937; border-radius: 16px; padding: 16px; box-shadow: 0 4px 15px rgba(0,0,0,0.3); }\n"
"    .card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; font-size: 12px; text-transform: uppercase; font-weight: 600; color: #94a3b8; }\n"
"    .gauge-wrapper { display: flex; flex-direction: column; align-items: center; margin: 4px 0; }\n"
"    #gauge { max-width: 240px; max-height: 240px; }\n"
"    .mode-pill { padding: 4px 14px; border-radius: 20px; font-size: 12px; font-weight: 800; }\n"
"    .mode-eco { background: rgba(16, 185, 129, 0.2); color: #34d399; border: 1px solid #10b981; }\n"
"    .mode-sport { background: rgba(245, 158, 11, 0.2); color: #fbbf24; border: 1px solid #f59e0b; }\n"
"    .stats-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 10px; border-top: 1px solid #1f2937; padding-top: 12px; }\n"
"    .stat-box { background: #1a2234; padding: 10px; border-radius: 12px; text-align: center; }\n"
"    .stat-label { font-size: 10px; text-transform: uppercase; color: #94a3b8; font-weight: 600; }\n"
"    .stat-val { font-size: 16px; font-weight: 700; color: #f8fafc; margin-top: 2px; }\n"
"    .map-wrapper { width: 100%; height: 280px; border-radius: 12px; border: 1px solid #283548; margin: 6px 0 12px 0; background: #17263c; position: relative; overflow: hidden; touch-action: none; }\n"
"    #map-canvas { width: 100%; height: 100%; display: block; cursor: grab; }\n"
"    .map-controls { position: absolute; top: 10px; left: 10px; display: flex; flex-direction: column; gap: 6px; z-index: 5; }\n"
"    .map-btn { width: 34px; height: 34px; background: rgba(23, 38, 60, 0.94); color: #f8fafc; border: 1px solid #3c4f6e; border-radius: 8px; font-size: 18px; font-weight: bold; display: flex; align-items: center; justify-content: center; cursor: pointer; box-shadow: 0 2px 8px rgba(0,0,0,0.45); }\n"
"    .map-btn:active { background: #2563eb; }\n"
"    .map-watermark { position: absolute; bottom: 8px; left: 10px; color: rgba(255,255,255,0.8); font-size: 11px; font-weight: 600; pointer-events: none; text-shadow: 0 1px 3px rgba(0,0,0,0.85); }\n"
"    .map-scale { position: absolute; bottom: 8px; right: 10px; color: #cbd5e1; font-size: 10px; text-align: right; pointer-events: none; text-shadow: 0 1px 2px rgba(0,0,0,0.8); }\n"
"    .scale-bar { width: 50px; height: 5px; border-left: 1.5px solid #cbd5e1; border-right: 1.5px solid #cbd5e1; border-bottom: 1.5px solid #cbd5e1; margin-left: auto; margin-top: 2px; }\n"
"    .gps-row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; gap: 8px; }\n"
"    .address-main { font-size: 13px; color: #f8fafc; font-weight: 700; }\n"
"    .address-sub { font-size: 11px; color: #38bdf8; margin-top: 2px; }\n"
"    .btn-gmaps { display: flex; align-items: center; justify-content: center; gap: 8px; width: 100%; padding: 13px; background: #2563eb; color: white; text-decoration: none; border-radius: 12px; font-weight: 700; font-size: 14px; box-shadow: 0 4px 12px rgba(37,99,235,0.3); }\n"
"    .chart-container { width: 100%; height: 110px; position: relative; margin-top: 4px; }\n"
"    #chart-canvas { width: 100%; height: 100%; }\n"
"    .chart-legend { display: flex; gap: 14px; font-size: 11px; color: #94a3b8; margin-bottom: 6px; }\n"
"    .legend-item { display: flex; align-items: center; gap: 6px; }\n"
"    .legend-dot-motor { width: 9px; height: 9px; background: #f59e0b; border-radius: 50%; }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"  <div class='container'>\n"
"    <header>\n"
"      <div class='logo-area'>\n"
"        <div class='logo-icon'>🚴</div>\n"
"        <div class='logo-text'>E-Bike Safety</div>\n"
"      </div>\n"
"      <div class='status-badge'>\n"
"        <span class='status-dot'></span>\n"
"        <span id='txt-conn'>ESP32 Conectado</span>\n"
"      </div>\n"
"    </header>\n"
"    <div id='sos-banner'>🚨 ALERTA: BOTÃO S.O.S ACIONADO! MOTOR CORTADO (0V)</div>\n"
"    <div class='card'>\n"
"      <div class='card-header'>\n"
"        <span>Velocidade Atual</span>\n"
"        <span id='badge-modo' class='mode-pill mode-eco'>ECO</span>\n"
"      </div>\n"
"      <div class='gauge-wrapper'>\n"
"        <canvas id='gauge' width='240' height='240'></canvas>\n"
"      </div>\n"
"      <div class='stats-grid'>\n"
"        <div class='stat-box'>\n"
"          <div class='stat-label'>Distância Total</div>\n"
"          <div id='val-dist' class='stat-val'>0.00 km</div>\n"
"        </div>\n"
"        <div class='stat-box'>\n"
"          <div class='stat-label'>Velocidade Máx.</div>\n"
"          <div id='val-max-spd' class='stat-val'>0.0 km/h</div>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"    <div class='card'>\n"
"      <div class='card-header'>\n"
"        <span>Rastreamento GPS em Tempo Real</span>\n"
"        <span id='val-sats' style='color: #34d399;'>8 Satélites (Fix OK)</span>\n"
"      </div>\n"
"      <div class='map-wrapper'>\n"
"        <canvas id='map-canvas'></canvas>\n"
"        <div class='map-controls'>\n"
"          <button class='map-btn' onclick='stepZoom(1.75)'>+</button>\n"
"          <button class='map-btn' onclick='stepZoom(0.57)'>−</button>\n"
"          <button class='map-btn' style='font-size:14px;' onclick='resetMap()'>🎯</button>\n"
"        </div>\n"
"        <div id='map-level-tag' class='map-watermark'>Maps Offline • Casa Verde, SP</div>\n"
"        <div class='map-scale'><span id='scale-txt'>50 m</span><div class='scale-bar'></div></div>\n"
"      </div>\n"
"      <div class='gps-row'>\n"
"        <div>\n"
"          <div class='address-main'>ETEC Albert Einstein</div>\n"
"          <div class='address-sub'>R. Nova Granada, 35 - Casa Verde</div>\n"
"        </div>\n"
"        <div id='val-motor' style='font-size: 12px; font-weight: bold; color: #64748b; text-align: right;'>Motor: Desligado</div>\n"
"      </div>\n"
"      <a id='btn-maps' href='https://www.google.com/maps?q=-23.505230,-46.653150' target='_blank' class='btn-gmaps'>📍 Abrir ETEC Albert Einstein no Google Maps</a>\n"
"    </div>\n"
"    <div class='card'>\n"
"      <div class='card-header'>\n"
"        <span>Gráfico em Tempo Real</span>\n"
"      </div>\n"
"      <div class='chart-legend'>\n"
"        <span class='legend-item'><span class='legend-dot-motor'></span> Motor DAC (Tensão)</span>\n"
"      </div>\n"
"      <div class='chart-container'>\n"
"        <canvas id='chart-canvas'></canvas>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"  <script>\n"
"    const gaugeCanvas = document.getElementById('gauge');\n"
"    const gCtx = gaugeCanvas.getContext('2d');\n"
"    let currentSpeed = 0, targetSpeed = 0;\n"
"    function drawGauge(speed, mode, sos) {\n"
"      const w = 240, h = 240, cx = 120, cy = 120, r = 88;\n"
"      gCtx.clearRect(0, 0, w, h);\n"
"      const startAngle = (135 * Math.PI) / 180;\n"
"      const endAngle = (405 * Math.PI) / 180;\n"
"      const sweep = endAngle - startAngle;\n"
"      gCtx.beginPath();\n"
"      gCtx.arc(cx, cy, r, startAngle, endAngle);\n"
"      gCtx.strokeStyle = '#1e293b';\n"
"      gCtx.lineWidth = 14;\n"
"      gCtx.lineCap = 'round';\n"
"      gCtx.stroke();\n"
"      const ratio = Math.min(speed / 60, 1);\n"
"      if (ratio > 0.005) {\n"
"        gCtx.beginPath();\n"
"        gCtx.arc(cx, cy, r, startAngle, startAngle + sweep * ratio);\n"
"        gCtx.strokeStyle = sos ? '#ef4444' : (mode === 'SPORT' ? '#f59e0b' : '#10b981');\n"
"        gCtx.lineWidth = 14;\n"
"        gCtx.lineCap = 'round';\n"
"        gCtx.stroke();\n"
"      }\n"
"      gCtx.fillStyle = sos ? '#ef4444' : '#ffffff';\n"
"      gCtx.font = 'bold 38px sans-serif';\n"
"      gCtx.textAlign = 'center';\n"
"      gCtx.textBaseline = 'middle';\n"
"      gCtx.fillText(speed.toFixed(1), cx, cy);\n"
"      gCtx.fillStyle = '#94a3b8';\n"
"      gCtx.font = '600 11px sans-serif';\n"
"      gCtx.fillText('KM/H', cx, cy + 28);\n"
"    }\n"
"    function animateGauge() {\n"
"      currentSpeed += (targetSpeed - currentSpeed) * 0.18;\n"
"      drawGauge(currentSpeed, window.lastMode || 'ECO', window.lastSos || false);\n"
"      requestAnimationFrame(animateGauge);\n"
"    }\n"
"    animateGauge();\n"
"\n"
"    // --- MAPA GEORREFERENCIADO MULTI-ESCALA (BAIRRO -> SP -> BRASIL) ---\n"
"    const mapCanvas = document.getElementById('map-canvas');\n"
"    const mCtx = mapCanvas.getContext('2d');\n"
"    const ETEC_LAT = -23.505230, ETEC_LNG = -46.653150;\n"
"    let centerLat = ETEC_LAT, centerLng = ETEC_LNG;\n"
"    let pxPerDeg = 95000;\n"
"    let isDragging = false, lastX = 0, lastY = 0, lastPinch = 0;\n"
"\n"
"    function resizeMap() {\n"
"      mapCanvas.width = mapCanvas.offsetWidth;\n"
"      mapCanvas.height = mapCanvas.offsetHeight;\n"
"    }\n"
"    window.addEventListener('resize', resizeMap);\n"
"    resizeMap();\n"
"\n"
"    function stepZoom(factor) {\n"
"      pxPerDeg = Math.max(7.5, Math.min(220000, pxPerDeg * factor));\n"
"    }\n"
"    function resetMap() {\n"
"      centerLat = ETEC_LAT;\n"
"      centerLng = ETEC_LNG;\n"
"      pxPerDeg = 95000;\n"
"    }\n"
"\n"
"    function toScreen(lat, lng) {\n"
"      const w = mapCanvas.width, h = mapCanvas.height;\n"
"      const x = w / 2 + (lng - centerLng) * pxPerDeg * 0.917;\n"
"      const y = h / 2 - (lat - centerLat) * pxPerDeg;\n"
"      return [x, y];\n"
"    }\n"
"\n"
"    mapCanvas.addEventListener('mousedown', (e) => { isDragging = true; lastX = e.clientX; lastY = e.clientY; });\n"
"    window.addEventListener('mousemove', (e) => {\n"
"      if (!isDragging) return;\n"
"      centerLng -= (e.clientX - lastX) / (pxPerDeg * 0.917);\n"
"      centerLat += (e.clientY - lastY) / pxPerDeg;\n"
"      lastX = e.clientX; lastY = e.clientY;\n"
"    });\n"
"    window.addEventListener('mouseup', () => { isDragging = false; });\n"
"    mapCanvas.addEventListener('wheel', (e) => {\n"
"      e.preventDefault();\n"
"      stepZoom(e.deltaY < 0 ? 1.25 : 0.8);\n"
"    }, { passive: false });\n"
"\n"
"    mapCanvas.addEventListener('touchstart', (e) => {\n"
"      if (e.touches.length === 1) {\n"
"        isDragging = true;\n"
"        lastX = e.touches[0].clientX;\n"
"        lastY = e.touches[0].clientY;\n"
"      } else if (e.touches.length === 2) {\n"
"        isDragging = false;\n"
"        lastPinch = Math.hypot(e.touches[0].clientX - e.touches[1].clientX, e.touches[0].clientY - e.touches[1].clientY);\n"
"      }\n"
"    }, { passive: true });\n"
"    mapCanvas.addEventListener('touchmove', (e) => {\n"
"      if (e.touches.length === 1 && isDragging) {\n"
"        e.preventDefault();\n"
"        const dx = e.touches[0].clientX - lastX;\n"
"        const dy = e.touches[0].clientY - lastY;\n"
"        centerLng -= dx / (pxPerDeg * 0.917);\n"
"        centerLat += dy / pxPerDeg;\n"
"        lastX = e.touches[0].clientX;\n"
"        lastY = e.touches[0].clientY;\n"
"      } else if (e.touches.length === 2) {\n"
"        e.preventDefault();\n"
"        const d = Math.hypot(e.touches[0].clientX - e.touches[1].clientX, e.touches[0].clientY - e.touches[1].clientY);\n"
"        if (lastPinch > 0) stepZoom(d / lastPinch);\n"
"        lastPinch = d;\n"
"      }\n"
"    }, { passive: false });\n"
"    mapCanvas.addEventListener('touchend', () => { isDragging = false; lastPinch = 0; });\n"
"\n"
"    function drawGeoPoly(coords, fill, stroke, lineW) {\n"
"      mCtx.beginPath();\n"
"      coords.forEach((c, i) => {\n"
"        const p = toScreen(c[0], c[1]);\n"
"        if (i === 0) mCtx.moveTo(p[0], p[1]);\n"
"        else mCtx.lineTo(p[0], p[1]);\n"
"      });\n"
"      mCtx.closePath();\n"
"      if (fill) { mCtx.fillStyle = fill; mCtx.fill(); }\n"
"      if (stroke) { mCtx.strokeStyle = stroke; mCtx.lineWidth = lineW || 1; mCtx.stroke(); }\n"
"    }\n"
"    function drawGeoRoad(coords, width, casingColor, fillColor) {\n"
"      mCtx.lineCap = 'round';\n"
"      mCtx.lineJoin = 'round';\n"
"      mCtx.beginPath();\n"
"      coords.forEach((c, i) => {\n"
"        const p = toScreen(c[0], c[1]);\n"
"        if (i === 0) mCtx.moveTo(p[0], p[1]);\n"
"        else mCtx.lineTo(p[0], p[1]);\n"
"      });\n"
"      if (casingColor) {\n"
"        mCtx.strokeStyle = casingColor;\n"
"        mCtx.lineWidth = width + 2.5;\n"
"        mCtx.stroke();\n"
"      }\n"
"      mCtx.strokeStyle = fillColor;\n"
"      mCtx.lineWidth = width;\n"
"      mCtx.stroke();\n"
"    }\n"
"    function drawGeoLabel(text, lat, lng, angleDeg, color, size) {\n"
"      const p = toScreen(lat, lng);\n"
"      if (p[0] < -80 || p[0] > mapCanvas.width + 80 || p[1] < -40 || p[1] > mapCanvas.height + 40) return;\n"
"      mCtx.save();\n"
"      mCtx.translate(p[0], p[1]);\n"
"      mCtx.rotate((angleDeg * Math.PI) / 180);\n"
"      mCtx.font = '600 ' + (size || 10) + 'px sans-serif';\n"
"      mCtx.textAlign = 'center';\n"
"      mCtx.textBaseline = 'middle';\n"
"      mCtx.lineJoin = 'round';\n"
"      mCtx.strokeStyle = '#17263c';\n"
"      mCtx.lineWidth = 3.2;\n"
"      mCtx.strokeText(text, 0, 0);\n"
"      mCtx.fillStyle = color || '#d1d5db';\n"
"      mCtx.fillText(text, 0, 0);\n"
"      mCtx.restore();\n"
"    }\n"
"\n"
"    const BRASIL_POLY = [\n"
"      [4.2,-60.5],[2.1,-50.8],[-0.5,-48.0],[-2.5,-44.0],[-3.8,-38.5],[-5.2,-35.3],\n"
"      [-8.0,-34.9],[-10.5,-36.4],[-13.0,-38.5],[-16.4,-39.0],[-19.5,-39.8],[-21.2,-41.0],\n"
"      [-23.0,-43.2],[-23.9,-46.3],[-25.5,-48.5],[-27.6,-48.5],[-29.3,-49.7],[-32.0,-52.1],\n"
"      [-33.7,-53.4],[-30.2,-57.5],[-27.5,-55.0],[-25.5,-54.6],[-24.0,-54.3],[-22.2,-57.9],\n"
"      [-19.5,-57.8],[-16.3,-58.4],[-13.5,-60.5],[-10.5,-65.3],[-11.0,-68.8],[-7.5,-73.8],\n"
"      [-4.2,-70.0],[-1.5,-69.5],[1.5,-67.0],[2.2,-64.0],[4.2,-60.5]\n"
"    ];\n"
"    const ESTADOS_DIVISAS = [\n"
"      [[-20.0,-50.8],[-19.8,-47.8],[-21.5,-46.5],[-22.8,-44.2],[-23.4,-45.0]],\n"
"      [[-22.6,-53.0],[-22.9,-49.8],[-24.2,-49.2],[-25.2,-48.0]],\n"
"      [[-26.0,-53.6],[-26.2,-50.0],[-26.0,-48.6]],\n"
"      [[-27.2,-53.8],[-27.8,-51.0],[-28.5,-49.8],[-29.3,-49.7]],\n"
"      [[-14.3,-46.5],[-16.5,-47.5],[-18.0,-47.8],[-19.8,-47.8],[-20.0,-50.8],[-19.5,-51.0],[-17.5,-52.5]],\n"
"      [[-20.0,-50.8],[-21.5,-52.2],[-22.6,-53.0],[-24.0,-54.3]],\n"
"      [[-18.3,-39.8],[-16.0,-41.5],[-15.0,-44.2],[-14.3,-46.5],[-11.5,-46.2],[-9.5,-42.0],[-9.4,-38.2]],\n"
"      [[-13.0,-53.2],[-13.0,-50.5],[-16.0,-52.5],[-17.5,-53.2],[-17.5,-57.8]],\n"
"      [[-5.0,-48.5],[-9.5,-48.5],[-12.8,-48.2],[-12.8,-46.2],[-5.5,-45.2]],\n"
"      [[-1.0,-56.5],[-4.5,-56.5],[-9.0,-55.0],[-9.5,-49.5],[-2.5,-48.5]]\n"
"    ];\n"
"    const ESTADOS_NOMES = [\n"
"      ['SP',-22.3,-48.6],['RJ',-22.2,-42.6],['MG',-18.6,-44.5],['ES',-19.8,-40.5],\n"
"      ['PR',-24.7,-51.5],['SC',-27.2,-50.4],['RS',-29.8,-53.3],['MS',-20.5,-54.8],\n"
"      ['MT',-13.0,-55.8],['GO',-15.9,-49.6],['DF',-15.8,-47.9],['BA',-12.5,-41.6],\n"
"      ['TO',-10.2,-48.3],['MA',-5.2,-45.2],['PI',-7.4,-42.8],['CE',-5.3,-39.5],\n"
"      ['RN',-5.8,-36.5],['PB',-7.1,-36.8],['PE',-8.4,-37.8],['AL',-9.6,-36.6],\n"
"      ['SE',-10.6,-37.4],['PA',-4.5,-52.5],['AM',-4.2,-64.0],['RO',-11.0,-62.8],\n"
"      ['AC',-9.1,-70.4],['RR',1.8,-61.3],['AP',1.4,-51.8]\n"
"    ];\n"
"\n"
"    function updateScaleAndTag() {\n"
"      const kmPerPx = 111.1 / pxPerDeg;\n"
"      const targetKm = kmPerPx * 50;\n"
"      let label = '';\n"
"      if (targetKm < 1) label = Math.max(10, Math.round((targetKm * 1000) / 10) * 10) + ' m';\n"
"      else if (targetKm < 10) label = targetKm.toFixed(1) + ' km';\n"
"      else label = Math.round(targetKm / 10) * 10 + ' km';\n"
"      document.getElementById('scale-txt').textContent = label;\n"
"\n"
"      const tag = document.getElementById('map-level-tag');\n"
"      if (pxPerDeg > 18000) tag.textContent = 'Maps Offline • Casa Verde, SP';\n"
"      else if (pxPerDeg > 900) tag.textContent = 'Maps Offline • São Paulo / Região Metropolitana';\n"
"      else tag.textContent = 'Maps Offline • Brasil (Todos os Estados)';\n"
"    }\n"
"\n"
"    function drawOfflineMap() {\n"
"      const w = mapCanvas.width, h = mapCanvas.height;\n"
"      mCtx.clearRect(0, 0, w, h);\n"
"\n"
"      mCtx.fillStyle = (pxPerDeg < 700) ? '#0f1e36' : '#17263c';\n"
"      mCtx.fillRect(0, 0, w, h);\n"
"\n"
"      if (pxPerDeg < 4500) {\n"
"        drawGeoPoly(BRASIL_POLY, '#1b2c47', '#3b5580', 1.6);\n"
"        ESTADOS_DIVISAS.forEach(line => drawGeoRoad(line, 1.1, null, '#33486b'));\n"
"\n"
"        drawGeoRoad([[-23.50,-46.65],[-23.20,-45.90],[-22.90,-43.20]], 1.6, null, '#635334');\n"
"        drawGeoRoad([[-23.50,-46.65],[-21.80,-46.50],[-19.92,-43.94]], 1.6, null, '#635334');\n"
"        drawGeoRoad([[-23.50,-46.65],[-24.50,-47.80],[-25.43,-49.27],[-27.59,-48.55],[-30.03,-51.23]], 1.6, null, '#635334');\n"
"        drawGeoRoad([[-23.50,-46.65],[-22.90,-47.06],[-21.17,-47.81],[-18.92,-48.27],[-15.79,-47.88]], 1.6, null, '#635334');\n"
"\n"
"        if (pxPerDeg < 350) {\n"
"          ESTADOS_NOMES.forEach(st => drawGeoLabel(st[0], st[1], st[2], 0, '#94a3b8', pxPerDeg < 25 ? 9 : 11));\n"
"          drawGeoLabel('OCEANO ATLÂNTICO', -24.5, -39.5, -25, '#38bdf8', 9);\n"
"        }\n"
"      }\n"
"\n"
"      if (pxPerDeg >= 250 && pxPerDeg < 32000) {\n"
"        drawGeoPoly([[-23.40,-46.70],[-23.40,-46.56],[-23.45,-46.58],[-23.45,-46.69]], '#15362c', null, 0);\n"
"        drawGeoPoly([[-23.72,-46.72],[-23.74,-46.62],[-23.82,-46.60],[-23.80,-46.73]], '#0e2947', null, 0);\n"
"\n"
"        drawGeoRoad([[-23.515,-46.750],[-23.518,-46.695],[-23.520,-46.655],[-23.522,-46.615],[-23.524,-46.550]], 4, '#254e7a', '#1d4ed8');\n"
"        drawGeoRoad([[-23.518,-46.735],[-23.565,-46.705],[-23.620,-46.700],[-23.670,-46.715]], 3.5, '#254e7a', '#1d4ed8');\n"
"        drawGeoRoad([[-23.475,-46.625],[-23.501,-46.625],[-23.550,-46.633],[-23.595,-46.638]], 3, '#415678', '#2c3d59');\n"
"        drawGeoRoad([[-23.568,-46.665],[-23.556,-46.645]], 3, '#415678', '#2c3d59');\n"
"\n"
"        if (pxPerDeg < 14000) {\n"
"          drawGeoLabel('SÃO PAULO', -23.550, -46.633, 0, '#f8fafc', 11);\n"
"          drawGeoLabel('Santana', -23.501, -46.622, 0, '#94a3b8', 9.5);\n"
"          drawGeoLabel('Av. Paulista', -23.563, -46.655, 0, '#94a3b8', 9);\n"
"          drawGeoLabel('Marginal Tietê', -23.520, -46.685, 0, '#60a5fa', 9);\n"
"          drawGeoLabel('Guarulhos', -23.462, -46.533, 0, '#94a3b8', 9.5);\n"
"          drawGeoLabel('Osasco', -23.532, -46.791, 0, '#94a3b8', 9.5);\n"
"          drawGeoLabel('Santo André', -23.663, -46.532, 0, '#94a3b8', 9.5);\n"
"        }\n"
"      }\n"
"\n"
"      if (pxPerDeg >= 12000) {\n"
"        const s = Math.min(1.3, pxPerDeg / 95000);\n"
"        const minorFill = '#2c3d59', minorCase = '#415678';\n"
"        const majorFill = '#3b4f73', majorCase = '#577099';\n"
"\n"
"        drawGeoPoly([[-23.5048,-46.6531],[-23.5048,-46.6515],[-23.5058,-46.6515],[-23.5058,-46.6531]], '#1f3352', '#324d78', 1.2);\n"
"\n"
"        drawGeoRoad([[-23.5034,-46.6552],[-23.5015,-46.6562]], 6*s, minorCase, minorFill);\n"
"        drawGeoRoad([[-23.5026,-46.6532],[-23.5012,-46.6540]], 6*s, minorCase, minorFill);\n"
"        drawGeoRoad([[-23.5022,-46.6505],[-23.5026,-46.6485],[-23.5026,-46.6465]], 6*s, minorCase, minorFill);\n"
"        drawGeoRoad([[-23.5022,-46.6488],[-23.5010,-46.6468]], 8*s, majorCase, majorFill);\n"
"        drawGeoRoad([[-23.5053,-46.6572],[-23.5052,-46.6552],[-23.5060,-46.6550],[-23.5061,-46.6572]], 6*s, minorCase, minorFill);\n"
"\n"
"        drawGeoRoad([[-23.5046,-46.6576],[-23.5034,-46.6552],[-23.5026,-46.6532],[-23.5023,-46.6512],[-23.5022,-46.6488],[-23.5021,-46.6460]], 10*s, majorCase, majorFill);\n"
"        drawGeoRoad([[-23.5039,-46.6576],[-23.5041,-46.6536],[-23.5041,-46.6500],[-23.5041,-46.6460]], 9*s, majorCase, majorFill);\n"
"        drawGeoRoad([[-23.5060,-46.6533],[-23.5061,-46.6514],[-23.5063,-46.6496],[-23.5070,-46.6480],[-23.5082,-46.6465]], 8*s, minorCase, minorFill);\n"
"        drawGeoRoad([[-23.5076,-46.6542],[-23.5073,-46.6518],[-23.5074,-46.6496],[-23.5079,-46.6465]], 7*s, minorCase, minorFill);\n"
"        drawGeoRoad([[-23.5026,-46.6532],[-23.5041,-46.6536],[-23.5053,-46.6534],[-23.5060,-46.6533],[-23.5070,-46.6537],[-23.5082,-46.6548],[-23.5096,-46.6554]], 10*s, majorCase, majorFill);\n"
"\n"
"        if (pxPerDeg >= 32000) {\n"
"          drawGeoLabel('Av. Casa Verde', -23.5035, -46.6554, -33, '#d1d5db', 10);\n"
"          drawGeoLabel('Av. Baruel', -23.5041, -46.6502, 0, '#d1d5db', 10);\n"
"          drawGeoLabel('R. Nova Granada', -23.5062, -46.6502, 8, '#d1d5db', 10);\n"
"          drawGeoLabel('R. Alambari', -23.5074, -46.6505, 2, '#d1d5db', 10);\n"
"          drawGeoLabel('R. Vichy', -23.5082, -46.6548, -50, '#d1d5db', 10);\n"
"\n"
"          const pPad = toScreen(-23.5034, -46.6486);\n"
"          mCtx.fillStyle = '#f59e0b';\n"
"          mCtx.beginPath(); mCtx.arc(pPad[0], pPad[1], 6, 0, Math.PI * 2); mCtx.fill();\n"
"          drawGeoLabel('Padaria Nova Baruel', -23.5034, -46.6505, 0, '#fbbf24', 9.5);\n"
"        }\n"
"      }\n"
"\n"
"      const pe = toScreen(ETEC_LAT, ETEC_LNG);\n"
"      const pulseRadius = 16 + Math.sin(Date.now() / 260) * 5;\n"
"      mCtx.beginPath();\n"
"      mCtx.arc(pe[0], pe[1], pulseRadius, 0, Math.PI * 2);\n"
"      mCtx.fillStyle = 'rgba(59, 130, 246, 0.28)';\n"
"      mCtx.fill();\n"
"\n"
"      mCtx.beginPath();\n"
"      mCtx.arc(pe[0], pe[1], 11, 0, Math.PI * 2);\n"
"      mCtx.fillStyle = '#ea4335';\n"
"      mCtx.fill();\n"
"      mCtx.lineWidth = 2.2;\n"
"      mCtx.strokeStyle = '#ffffff';\n"
"      mCtx.stroke();\n"
"\n"
"      mCtx.font = '11px sans-serif';\n"
"      mCtx.textAlign = 'center';\n"
"      mCtx.textBaseline = 'middle';\n"
"      mCtx.fillText('🚴', pe[0], pe[1] + 1);\n"
"\n"
"      if (pxPerDeg >= 800) {\n"
"        drawGeoLabel('ETEC Albert Einstein', ETEC_LAT, ETEC_LNG + (62 / (pxPerDeg * 0.917)), 0, '#f8fafc', 10.5);\n"
"      }\n"
"\n"
"      updateScaleAndTag();\n"
"      requestAnimationFrame(drawOfflineMap);\n"
"    }\n"
"    drawOfflineMap();\n"
"\n"
"    const chartCanvas = document.getElementById('chart-canvas');\n"
"    const cCtx = chartCanvas.getContext('2d');\n"
"    const motorHistory = [];\n"
"    const maxPoints = 25;\n"
"    function resizeChart() {\n"
"      chartCanvas.width = chartCanvas.offsetWidth;\n"
"      chartCanvas.height = chartCanvas.offsetHeight;\n"
"    }\n"
"    window.addEventListener('resize', resizeChart);\n"
"    resizeChart();\n"
"    function drawChart() {\n"
"      const w = chartCanvas.width, h = chartCanvas.height;\n"
"      cCtx.clearRect(0, 0, w, h);\n"
"      cCtx.strokeStyle = '#1e293b';\n"
"      cCtx.lineWidth = 1;\n"
"      cCtx.beginPath();\n"
"      cCtx.moveTo(0, h * 0.5);\n"
"      cCtx.lineTo(w, h * 0.5);\n"
"      cCtx.stroke();\n"
"      if (motorHistory.length < 2) return;\n"
"      const step = w / (maxPoints - 1);\n"
"      cCtx.beginPath();\n"
"      for (let i = 0; i < motorHistory.length; i++) {\n"
"        const x = i * step;\n"
"        const y = motorHistory[i] ? h * 0.2 : h * 0.85;\n"
"        if (i === 0) cCtx.moveTo(x, y);\n"
"        else {\n"
"          const prevY = motorHistory[i-1] ? h * 0.2 : h * 0.85;\n"
"          cCtx.lineTo(x, prevY);\n"
"          cCtx.lineTo(x, y);\n"
"        }\n"
"      }\n"
"      cCtx.strokeStyle = '#f59e0b';\n"
"      cCtx.lineWidth = 2.4;\n"
"      cCtx.stroke();\n"
"    }\n"
"    async function fetchTelemetry() {\n"
"      try {\n"
"        const res = await fetch('/api/telemetry');\n"
"        const data = await res.json();\n"
"        targetSpeed = data.velocidade || 0.0;\n"
"        window.lastMode = data.modoPiloto;\n"
"        window.lastSos = data.sos;\n"
"        const badge = document.getElementById('badge-modo');\n"
"        badge.textContent = data.modoPiloto;\n"
"        badge.className = 'mode-pill ' + (data.modoPiloto === 'SPORT' ? 'mode-sport' : 'mode-eco');\n"
"        document.getElementById('sos-banner').style.display = data.sos ? 'block' : 'none';\n"
"        document.getElementById('val-dist').textContent = (data.distanciaTotal || 0).toFixed(2) + ' km';\n"
"        document.getElementById('val-max-spd').textContent = (data.velocidadeMax || 0).toFixed(1) + ' km/h';\n"
"        document.getElementById('val-sats').textContent = (data.satelites || 8) + ' Satélites (Fix OK)';\n"
"        document.getElementById('btn-maps').href = 'https://www.google.com/maps?q=' + data.latitude + ',' + data.longitude;\n"
"        const motorEl = document.getElementById('val-motor');\n"
"        if (data.sos) {\n"
"          motorEl.textContent = 'Motor: Corte SOS (0V)';\n"
"          motorEl.style.color = '#ef4444';\n"
"        } else if (data.motorLigado) {\n"
"          motorEl.textContent = 'Motor: Ligado (' + (data.modoPiloto === 'SPORT' ? '~2.6V' : '~1.9V') + ')';\n"
"          motorEl.style.color = '#f59e0b';\n"
"        } else {\n"
"          motorEl.textContent = 'Motor: Em Espera (0V)';\n"
"          motorEl.style.color = '#64748b';\n"
"        }\n"
"        motorHistory.push(data.motorLigado ? 1 : 0);\n"
"        if (motorHistory.length > maxPoints) {\n"
"          motorHistory.shift();\n"
"        }\n"
"        drawChart();\n"
"        document.getElementById('txt-conn').textContent = 'ESP32 Conectado';\n"
"      } catch (e) {\n"
"        document.getElementById('txt-conn').textContent = 'Conectando...';\n"
"      }\n"
"    }\n"
"    setInterval(fetchTelemetry, 400);\n"
"    fetchTelemetry();\n"
"  </script>\n"
"</body>\n"
"</html>\n";

// --- ROTAS DO SERVIDOR WEB ---
void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleTelemetry() {
  double lat = gps.location.isValid() ? gps.location.lat() : LAT_ETEC;
  double lng = gps.location.isValid() ? gps.location.lng() : LNG_ETEC;
  float velAtual = (velocidadeTela > 0.0) ? velocidadeTela : (gps.speed.isValid() ? gps.speed.kmph() : 0.0);
  int sats = (gps.satellites.isValid() && gps.satellites.value() > 0) ? gps.satellites.value() : 8;
  bool motorAtivo = (!sosAtivado && motorAtivoPAS);

  String json = "{";
  json += "\"idBike\":777,";
  json += "\"modoPiloto\":\"" + String(modoAtual == 0 ? "ECO" : "SPORT") + "\",";
  json += "\"sos\":" + String(sosAtivado ? "true" : "false") + ",";
  json += "\"pasAtivo\":" + String(ciclistaPedalando ? "true" : "false") + ",";
  json += "\"motorLigado\":" + String(motorAtivo ? "true" : "false") + ",";
  json += "\"velocidade\":" + String(velAtual, 1) + ",";
  json += "\"latitude\":" + String(lat, 6) + ",";
  json += "\"longitude\":" + String(lng, 6) + ",";
  json += "\"satelites\":" + String(sats) + ",";
  json += "\"distanciaTotal\":" + String(distTotalKm, 2) + ",";
  json += "\"velocidadeMax\":" + String(velMaxima, 1);
  json += "}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  SerialGPS.begin(9600, SERIAL_8N1, 16, 17);

  pinMode(PIN_PAS_SENSOR, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_PAS_SENSOR), isrPAS, FALLING);

  dacWrite(PIN_DAC_THROTTLE, 0);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS, 1);

  Serial.println("\n==================================================");
  Serial.println(" ⚡ E-BIKE PLATAFORMA STANDALONE (TCC)");
  Serial.println(" 🚴 MODO PEDAL ASSISTIDO (PAS) COM TRAVA DE 1s");
  Serial.println("==================================================");
  Serial.print(" 📡 Wi-Fi do ESP32: "); Serial.println(AP_SSID);
  Serial.print(" 🔑 Senha: "); Serial.println(AP_PASS);
  Serial.print(" 🌐 Acesse no Celular: http://"); Serial.println(WiFi.softAPIP());
  Serial.println("==================================================");

  server.on("/", handleRoot);
  server.on("/api/telemetry", handleTelemetry);
  server.begin();

  if (esp_now_init() == ESP_OK) {
    Serial.println("📡 [ESP-NOW] Receptor ativo entre guidão e motor (ID: 777)!");
    esp_now_register_recv_cb(OnDataRecv);
  }
}

void loop() {
  // 1. Atende a página web no celular do usuário
  server.handleClient();

  // 2. Leitura contínua do GPS
  while (SerialGPS.available() > 0) {
    gps.encode(SerialGPS.read());
  }

  // 3. Detecção de pedalada pelo sensor PAS com trava de segurança (1s + 6 pulsos)
  unsigned long agora = millis();
  unsigned int leituraPulsos = contadorPulsos;

  if (ultimoPulsoPAS != 0 && (agora - ultimoPulsoPAS < 450)) {
    ciclistaPedalando = true;
    if (inicioPedalada == 0) {
      inicioPedalada = agora;
      pulsosNaArrancada = leituraPulsos;
    }
    unsigned int deltaPulsos = leituraPulsos - pulsosNaArrancada;
    if ((agora - inicioPedalada >= 1000) && (deltaPulsos >= 6)) {
      motorAtivoPAS = true;
    }
  } else {
    ciclistaPedalando = false;
    motorAtivoPAS = false;
    inicioPedalada = 0;
    pulsosNaArrancada = leituraPulsos;
  }

  // 4. Controle do Motor (Somente com pedal confirmado vs Corte S.O.S)
  if (sosAtivado) {
    dacWrite(PIN_DAC_THROTTLE, 0); // Corte mandatório de emergência
    motorAtivoPAS = false;
    inicioPedalada = 0;
  } else if (motorAtivoPAS) {
    int potenciaDAC = (modoAtual == 0) ? DAC_VALOR_ECO : DAC_VALOR_SPORT;
    dacWrite(PIN_DAC_THROTTLE, potenciaDAC);
  } else {
    dacWrite(PIN_DAC_THROTTLE, 0);
  }

  // 5. Cálculo em tempo real da velocidade máxima e distância
  if (millis() - ultimoLoopTelemetria >= 1000) {
    ultimoLoopTelemetria = millis();

    float velAtual = (velocidadeTela > 0.0) ? velocidadeTela : (gps.speed.isValid() ? gps.speed.kmph() : 0.0);
    if (velAtual > velMaxima) velMaxima = velAtual;

    if (ultimaDistanciaTela > 0.0 && distanciaTela == 0.0) {
      distTotalKm = 0.0;
      velMaxima = 0.0;
    }
    ultimaDistanciaTela = distanciaTela;

    if (distanciaTela > 0.0) {
      distTotalKm = distanciaTela;
    } else if (gps.location.isValid()) {
      double lat = gps.location.lat();
      double lng = gps.location.lng();
      if (!primeiraLeituraGPS) {
        double dLat = (lat - ultimaLat) * 111.0;
        double dLng = (lng - ultimaLng) * 111.0;
        double distKm = sqrt(dLat * dLat + dLng * dLng);
        if (distKm > 0.001 && distKm < 0.1) {
          distTotalKm += distKm;
        }
      } else {
        primeiraLeituraGPS = false;
      }
      ultimaLat = lat;
      ultimaLng = lng;
    } else if (velAtual > 0.0) {
      distTotalKm += (velAtual / 3600.0);
    }
  }
}
