#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <esp_now.h>
#include <WiFi.h>

// --- ESTRUTURA DE DADOS ESP-NOW COM ID DE SEGURANÇA ---
typedef struct struct_message {
  int idBike;         // 🔑 SENHA / ID ÚNICO DA SUA BIKE (Ex: 777)
  uint8_t modoPiloto; // 0 = ECO, 1 = SPORT
  bool sosTrigger;    // true = SOS ativado, false = normal
  float velocidade;   // Velocidade da roda enviada para o site (km/h)
  float distancia;    // Distância acumulada enviada para o site (km)
} struct_message;

struct_message dadosEnviar;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- CONFIGURAÇÃO DA TELA E TOUCH ---
static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

SPIClass mySpi = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 10];

// --- PINOS E MATEMÁTICA DA RODA ---
#define PIN_HALL_INPUT 35  // Entrada do Reed Switch na roda
#define PERIMETRO_RODA_METROS 2.10

volatile unsigned long ultimoTempoPulsoRoda = 0;
volatile unsigned long intervaloVolta = 0;
volatile bool novaVoltaDetectada = false;

void IRAM_ATTR pulsoVelocimetro() {
  unsigned long agora = millis();
  if (agora - ultimoTempoPulsoRoda > 30) { 
    intervaloVolta = agora - ultimoTempoPulsoRoda;
    ultimoTempoPulsoRoda = agora;
    novaVoltaDetectada = true;
  }
}

// --- Telas do Sistema ---
lv_obj_t * scr_intro = nullptr;
lv_obj_t * scr_main  = nullptr;
lv_obj_t * scr_cfg   = nullptr;
lv_obj_t * scr_modos = nullptr;
lv_obj_t * scr_sos   = nullptr;

// --- Widgets ---
lv_obj_t * btn_exit = nullptr;
lv_obj_t * lbl_header = nullptr;
lv_obj_t * lbl_status_left;   // Bateria
lv_obj_t * lbl_status_center; // Horário
lv_obj_t * lbl_speed_val;
lv_obj_t * lbl_speed_unit;
lv_obj_t * lbl_pace_val;
lv_obj_t * lbl_dist_val;
lv_obj_t * lbl_time_val;
lv_obj_t * lbl_btn_start_text;
lv_obj_t * btn_modo;
lv_obj_t * lbl_btn_modo_sub; 
lv_obj_t * lbl_btn_sos;       
lv_obj_t * slider_bl;
lv_obj_t * lbl_time_cfg;
lv_obj_t * btn_unit_kmh;
lv_obj_t * btn_unit_mph;
lv_obj_t * btn_eco = nullptr;
lv_obj_t * btn_sport = nullptr;

// --- Variáveis Globais do S.O.S ---
unsigned long sosPressStartTime = 0;
bool sosIsPressing = false;
bool sosAtivoLocal = false;

// --- Variáveis de Controle e Estado ---
bool introAtiva = true;
bool corridaAtiva = false;
int nivelBrilho = 180;    
int modoPiloto = 0;       // 0 = ECO, 1 = SPORT
int modoPilotoTemp = 0;   
bool modoMetrico = true;  
bool temaBranco = true;   

// --- Relógio e Variáveis Físicas ---
int relogioHora = 12;     
int relogioMinuto = 00;
unsigned long ultimoSegundoMillis = 0;
float velocidadeReal = 0.0;
float distanciaAcumulada = 0.0;
int bateriaSimulada = 98; 
unsigned long tempoInicioCorrida = 0;
unsigned long tempoAcumuladoCorrida = 0;
unsigned long tempoExibicaoCorrida = 0;
unsigned long introMillis = 0;
unsigned long ultimoPrintSerial = 0;

// Prototipação
void go_intro(); void go_main(); void go_cfg(); void go_modos(); void go_sos();
void atualizarDashboardDinamico(); void atualizarConfiguracoesDinamico();
void atualizarBotoesUnidade(); void atualizarVisuaisBotoesModo();
int paginaAtualAtiva(); void aplicarCoresDoTema(); void transmitirDadosEspNow();

// ==========================================
// 🎨 PALETA DE CORES
// ==========================================
lv_color_t getBgColor()      { return temaBranco ? lv_color_make(245, 247, 250) : lv_color_make(15, 15, 18); }
lv_color_t getTextColor()    { return temaBranco ? lv_color_make(20, 20, 25)    : lv_color_make(255, 255, 255); }
lv_color_t getSubTextColor() { return temaBranco ? lv_color_make(110, 120, 135) : lv_color_make(160, 170, 185); }
lv_color_t getAccentBlue()   { return lv_color_make(0, 122, 255); }
lv_color_t getRedSOS()       { return lv_color_make(211, 47, 47); }
lv_color_t getGreenECO()     { return lv_color_make(46, 125, 50); }
lv_color_t getRedSport()     { return lv_color_make(198, 40, 40); }
lv_color_t getOrangeStop()   { return lv_color_make(216, 100, 0); }
lv_color_t getDarkBtnBg()    { return temaBranco ? lv_color_make(210, 215, 225) : lv_color_make(40, 45, 55); }

void aplicarCoresDoTema() {
  lv_color_t bg = getBgColor();
  lv_color_t txt = getTextColor();

  if (scr_main) lv_obj_set_style_bg_color(scr_main, bg, LV_PART_MAIN);
  if (scr_cfg) lv_obj_set_style_bg_color(scr_cfg, bg, LV_PART_MAIN);
  if (scr_modos) lv_obj_set_style_bg_color(scr_modos, bg, LV_PART_MAIN);

  if (btn_exit) {
    lv_obj_t * lbl = lv_obj_get_child(btn_exit, 0);
    if(lbl) lv_obj_set_style_text_color(lbl, txt, LV_PART_MAIN);
  }
  if (scr_main) {
    lv_obj_set_style_text_color(lbl_status_left, txt, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_status_center, txt, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_speed_val, txt, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_pace_val, txt, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_dist_val, txt, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_time_val, txt, LV_PART_MAIN);
  }
  if (scr_cfg) {
    lv_obj_set_style_text_color(lbl_time_cfg, txt, LV_PART_MAIN);
  }
  atualizarBotoesUnidade();
}

// --- TRANSMISSÃO ESP-NOW COM FILTRO ---
void transmitirDadosEspNow() {
  dadosEnviar.idBike = 777; // 👈 ASSINATURA DA SUA BIKE
  dadosEnviar.modoPiloto = (uint8_t)modoPiloto;
  dadosEnviar.sosTrigger = sosAtivoLocal;
  dadosEnviar.velocidade = velocidadeReal;
  dadosEnviar.distancia = distanciaAcumulada;
  esp_now_send(broadcastAddress, (uint8_t *) &dadosEnviar, sizeof(dadosEnviar));
}

void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp_drv);
}

void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data) {
    if (ts.touched()) {
        TS_Point p = ts.getPoint();
        data->point.x = map(p.x, 200, 3700, 0, screenWidth);
        data->point.y = map(p.y, 240, 3800, 0, screenHeight);
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

void ajustaBrilhoTela(int valor) {
    analogWrite(21, valor);
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_HALL_INPUT, INPUT);    
  attachInterrupt(digitalPinToInterrupt(PIN_HALL_INPUT), pulsoVelocimetro, FALLING);

  pinMode(21, OUTPUT);
  ajustaBrilhoTela(nivelBrilho);

  tft.begin();
  tft.setRotation(1);
  tft.invertDisplay(true);

  mySpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(mySpi);
  ts.setRotation(1);

  WiFi.mode(WIFI_STA);
  if (esp_now_init() == ESP_OK) {
    Serial.println("📡 [ESP-NOW GUIDAO] Inicializado com sucesso!");
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0;  
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  } else {
    Serial.println("❌ [ESP-NOW GUIDAO] Erro ao inicializar!");
  }

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  btn_exit = lv_obj_create(lv_layer_top());
  lv_obj_clear_flag(btn_exit, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(btn_exit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(btn_exit, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_exit, 0, LV_PART_MAIN);
  lv_obj_set_size(btn_exit, 40, 40);
  lv_obj_align(btn_exit, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_add_event_cb(btn_exit, [](lv_event_t * e) -> void { go_main(); }, LV_EVENT_CLICKED, NULL);

  lv_obj_t * lbl_exit_symbol = lv_label_create(btn_exit);
  lv_obj_set_style_text_font(lbl_exit_symbol, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl_exit_symbol, getTextColor(), LV_PART_MAIN);
  lv_label_set_text(lbl_exit_symbol, LV_SYMBOL_CLOSE);
  lv_obj_align(lbl_exit_symbol, LV_ALIGN_TOP_RIGHT, 5, -10);  

  lbl_header = lv_label_create(lv_layer_top());
  lv_obj_set_style_text_font(lbl_header, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl_header, getAccentBlue(), LV_PART_MAIN); 
  lv_obj_align(lbl_header, LV_ALIGN_TOP_LEFT, 5, 3);

  go_intro();
}

void loop() {
  lv_timer_handler();  

  if (introAtiva) {
    if (millis() - introMillis > 3000) { 
      introAtiva = false;
      go_main();
    }
    return; 
  }

  if (millis() - ultimoSegundoMillis >= 60000) {
    ultimoSegundoMillis = millis();
    relogioMinuto++;
    if (relogioMinuto >= 60) {
      relogioMinuto = 0;
      relogioHora++;
      if (relogioHora >= 24) relogioHora = 0;
    }
  }

  // Calcula velocidade e distância sempre que houver pulso no pino 35
  if (novaVoltaDetectada) {
    distanciaAcumulada += (PERIMETRO_RODA_METROS / 1000.0); 
    novaVoltaDetectada = false;
  }

  if (millis() - ultimoTempoPulsoRoda > 2000 || ultimoTempoPulsoRoda == 0) {
    velocidadeReal = 0.0;
  } else if (intervaloVolta > 0) {
    float tempoSegundos = intervaloVolta / 1000.0;
    velocidadeReal = (PERIMETRO_RODA_METROS / tempoSegundos) * 3.6;
  }

  if (corridaAtiva) {
    tempoExibicaoCorrida = (millis() - tempoInicioCorrida) + tempoAcumuladoCorrida;
  }

  static unsigned long ultimaAtualizacao = 0;
  if (millis() - ultimaAtualizacao > 200) {
    ultimaAtualizacao = millis();
    if (paginaAtualAtiva() == 1) atualizarDashboardDinamico();
    if (paginaAtualAtiva() == 2) atualizarConfiguracoesDinamico();
  }

  static unsigned long ultimoEnvioEspNow = 0;
  if (millis() - ultimoEnvioEspNow > 300) {
    ultimoEnvioEspNow = millis();
    transmitirDadosEspNow();
  }

  if (millis() - ultimoPrintSerial > 2000) {
    ultimoPrintSerial = millis();
    Serial.println("------------------------------------------------");
    Serial.printf("🖥️ [PAINEL GUIDAO] ID Bike: 777 | Modo: %s | S.O.S: %s\n", 
                  (modoPiloto == 0 ? "ECO" : "SPORT"), 
                  (sosAtivoLocal ? "ACTIVATED" : "OFF"));
    Serial.printf("🚴 Velocidade: %.1f km/h | Distancia: %.2f km\n", velocidadeReal, distanciaAcumulada);
    Serial.println("------------------------------------------------");
  }
}

lv_obj_t * new_screen(lv_obj_t * parent) {
  lv_obj_t * obj = lv_obj_create(parent);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(obj, getBgColor(), LV_PART_MAIN);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(obj, 0, 0);
  return obj;
}

int paginaAtualAtiva() {
  lv_obj_t * scr_ativa = lv_scr_act();
  if (scr_ativa == scr_main) return 1;
  if (scr_ativa == scr_cfg) return 2;
  if (scr_ativa == scr_modos) return 4;
  return 0;
}

void go_intro() {
  scr_intro = new_screen(NULL);

  lv_obj_t * stripe_cyan = lv_obj_create(scr_intro); lv_obj_set_size(stripe_cyan, 70, 6); lv_obj_set_pos(stripe_cyan, 55, 140); lv_obj_set_style_bg_color(stripe_cyan, getAccentBlue(), LV_PART_MAIN); lv_obj_set_style_border_width(stripe_cyan, 0, LV_PART_MAIN);
  lv_obj_t * stripe_blue = lv_obj_create(scr_intro); lv_obj_set_size(stripe_blue, 70, 6); lv_obj_set_pos(stripe_blue, 125, 140); lv_obj_set_style_bg_color(stripe_blue, getGreenECO(), LV_PART_MAIN); lv_obj_set_style_border_width(stripe_blue, 0, LV_PART_MAIN);
  lv_obj_t * stripe_red = lv_obj_create(scr_intro); lv_obj_set_size(stripe_red, 70, 6); lv_obj_set_pos(stripe_red, 195, 140); lv_obj_set_style_bg_color(stripe_red, getRedSOS(), LV_PART_MAIN); lv_obj_set_style_border_width(stripe_red, 0, LV_PART_MAIN);

  lv_obj_t * lbl_title1 = lv_label_create(scr_intro); lv_label_set_text(lbl_title1, "E-BIKE"); lv_obj_set_style_text_font(lbl_title1, &lv_font_montserrat_24, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_title1, getTextColor(), LV_PART_MAIN); lv_obj_align(lbl_title1, LV_ALIGN_CENTER, 0, -40);
  lv_obj_t * lbl_title2 = lv_label_create(scr_intro); lv_label_set_text(lbl_title2, "SMART TCC"); lv_obj_set_style_text_font(lbl_title2, &lv_font_montserrat_24, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_title2, getAccentBlue(), LV_PART_MAIN); lv_obj_align(lbl_title2, LV_ALIGN_CENTER, 0, 0);
  lv_obj_t * lbl_sub = lv_label_create(scr_intro); lv_label_set_text(lbl_sub, "SISTEMA INICIALIZANDO..."); lv_obj_set_style_text_font(lbl_sub, &lv_font_montserrat_10, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_sub, getSubTextColor(), LV_PART_MAIN); lv_obj_align(lbl_sub, LV_ALIGN_BOTTOM_MID, 0, -20);

  lv_label_set_text(lbl_header, ""); lv_obj_add_flag(btn_exit, LV_OBJ_FLAG_HIDDEN);
  introMillis = millis();
  lv_scr_load(scr_intro);
}

void go_main() {
  if (!scr_main) {
    scr_main = new_screen(NULL);
    lv_obj_set_size(scr_main, LV_HOR_RES, LV_VER_RES);

    lv_obj_t * sep = lv_obj_create(scr_main); lv_obj_set_size(sep, 320, 2); lv_obj_set_pos(sep, 0, 25); lv_obj_set_style_bg_color(sep, getAccentBlue(), LV_PART_MAIN); lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);

    lbl_status_left = lv_label_create(scr_main); 
    lv_obj_set_style_text_color(lbl_status_left, getTextColor(), LV_PART_MAIN); 
    lv_obj_set_pos(lbl_status_left, 10, 5);

    lbl_status_center = lv_label_create(scr_main); 
    lv_obj_set_style_text_color(lbl_status_center, getTextColor(), LV_PART_MAIN); 
    lv_obj_align(lbl_status_center, LV_ALIGN_TOP_MID, 0, 5);

    lbl_speed_val = lv_label_create(scr_main); lv_obj_set_style_text_font(lbl_speed_val, &lv_font_montserrat_48, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_speed_val, getTextColor(), LV_PART_MAIN); lv_obj_align(lbl_speed_val, LV_ALIGN_TOP_MID, 0, 32);
    lbl_speed_unit = lv_label_create(scr_main); lv_obj_set_style_text_font(lbl_speed_unit, &lv_font_montserrat_14, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_speed_unit, getAccentBlue(), LV_PART_MAIN); lv_obj_align(lbl_speed_unit, LV_ALIGN_TOP_MID, 0, 85);

    lv_obj_t * lbl_pace_title = lv_label_create(scr_main); lv_label_set_text(lbl_pace_title, "PACE"); lv_obj_set_style_text_font(lbl_pace_title, &lv_font_montserrat_10, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_pace_title, getSubTextColor(), LV_PART_MAIN); lv_obj_set_pos(lbl_pace_title, 25, 110);
    lbl_pace_val = lv_label_create(scr_main); lv_obj_set_style_text_font(lbl_pace_val, &lv_font_montserrat_18, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_pace_val, getTextColor(), LV_PART_MAIN); lv_obj_set_pos(lbl_pace_val, 25, 128);

    lv_obj_t * lbl_dist_title = lv_label_create(scr_main); lv_label_set_text(lbl_dist_title, "DISTANCIA"); lv_obj_set_style_text_font(lbl_dist_title, &lv_font_montserrat_10, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_dist_title, getSubTextColor(), LV_PART_MAIN); lv_obj_set_pos(lbl_dist_title, 120, 110);
    lbl_dist_val = lv_label_create(scr_main); lv_obj_set_style_text_font(lbl_dist_val, &lv_font_montserrat_18, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_dist_val, getTextColor(), LV_PART_MAIN); lv_obj_set_pos(lbl_dist_val, 130, 128);

    lv_obj_t * lbl_time_title = lv_label_create(scr_main); lv_label_set_text(lbl_time_title, "TEMPO"); lv_obj_set_style_text_font(lbl_time_title, &lv_font_montserrat_10, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_time_title, getSubTextColor(), LV_PART_MAIN); lv_obj_set_pos(lbl_time_title, 240, 110);
    lbl_time_val = lv_label_create(scr_main); lv_obj_set_style_text_font(lbl_time_val, &lv_font_montserrat_18, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_time_val, getTextColor(), LV_PART_MAIN); lv_obj_set_pos(lbl_time_val, 242, 128);

    lv_obj_t * btn_cfg = lv_btn_create(scr_main); lv_obj_set_size(btn_cfg, 40, 26); lv_obj_set_pos(btn_cfg, 280, 0); lv_obj_set_style_bg_color(btn_cfg, getDarkBtnBg(), LV_PART_MAIN);
    lv_obj_t * lbl_btn_cfg = lv_label_create(btn_cfg); lv_label_set_text(lbl_btn_cfg, LV_SYMBOL_SETTINGS); lv_obj_set_style_text_color(lbl_btn_cfg, getTextColor(), LV_PART_MAIN); lv_obj_align(lbl_btn_cfg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_cfg, [](lv_event_t * e) -> void { go_cfg(); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * sep2 = lv_obj_create(scr_main); lv_obj_set_size(sep2, 320, 1); lv_obj_set_pos(sep2, 0, 175); lv_obj_set_style_bg_color(sep2, getSubTextColor(), LV_PART_MAIN); lv_obj_set_style_border_width(sep2, 0, LV_PART_MAIN);

    btn_modo = lv_btn_create(scr_main); lv_obj_set_size(btn_modo, 65, 45); lv_obj_set_pos(btn_modo, 10, 185); lv_obj_set_style_pad_all(btn_modo, 0, LV_PART_MAIN); 
    lv_obj_t * lbl_btn_modo_title = lv_label_create(btn_modo); lv_label_set_text(lbl_btn_modo_title, "MODO"); lv_obj_set_style_text_font(lbl_btn_modo_title, &lv_font_montserrat_12, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_btn_modo_title, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_btn_modo_title, LV_ALIGN_TOP_MID, 0, 6); 
    lbl_btn_modo_sub = lv_label_create(btn_modo); lv_obj_set_style_text_font(lbl_btn_modo_sub, &lv_font_montserrat_10, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_btn_modo_sub, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_btn_modo_sub, LV_ALIGN_BOTTOM_MID, 0, -6); 
    lv_obj_add_event_cb(btn_modo, [](lv_event_t * e) -> void { go_modos(); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_start = lv_btn_create(scr_main); lv_obj_set_size(btn_start, 65, 45); lv_obj_set_pos(btn_start, 85, 185); lv_obj_set_style_bg_color(btn_start, getGreenECO(), LV_PART_MAIN); 
    lbl_btn_start_text = lv_label_create(btn_start); lv_label_set_text(lbl_btn_start_text, "START"); lv_obj_set_style_text_color(lbl_btn_start_text, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_btn_start_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_start, [](lv_event_t * e) -> void {
      if (!corridaAtiva) {
        corridaAtiva = true;
        tempoInicioCorrida = millis();
        lv_label_set_text(lbl_btn_start_text, "PAUSE");
      } else {
        corridaAtiva = false;
        tempoAcumuladoCorrida = tempoExibicaoCorrida;
        lv_label_set_text(lbl_btn_start_text, "START");
      }
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_stop = lv_btn_create(scr_main); lv_obj_set_size(btn_stop, 65, 45); lv_obj_set_pos(btn_stop, 160, 185); lv_obj_set_style_bg_color(btn_stop, getOrangeStop(), LV_PART_MAIN); 
    lv_obj_t * lbl_btn_stop = lv_label_create(btn_stop); lv_label_set_text(lbl_btn_stop, "STOP"); lv_obj_set_style_text_color(lbl_btn_stop, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_btn_stop, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_stop, [](lv_event_t * e) -> void {
      corridaAtiva = false;
      tempoAcumuladoCorrida = 0; tempoExibicaoCorrida = 0; distanciaAcumulada = 0;
      lv_label_set_text(lbl_btn_start_text, "START");
      transmitirDadosEspNow();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_sos = lv_btn_create(scr_main); lv_obj_set_size(btn_sos, 75, 45); lv_obj_set_pos(btn_sos, 235, 185); lv_obj_set_style_bg_color(btn_sos, getRedSOS(), LV_PART_MAIN); 
    lbl_btn_sos = lv_label_create(btn_sos); lv_label_set_text(lbl_btn_sos, "S.O.S"); lv_obj_set_style_text_color(lbl_btn_sos, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_btn_sos, LV_ALIGN_CENTER, 0, 0);
    
    // BOTÃO S.O.S
    lv_obj_add_event_cb(btn_sos, [](lv_event_t * e) -> void {
      lv_event_code_t code = lv_event_get_code(e);
      if (code == LV_EVENT_PRESSED) {
        sosPressStartTime = millis();
        sosIsPressing = true;
      }
      else if (code == LV_EVENT_PRESSING) {
        if (sosIsPressing) {
          unsigned long elapsed = millis() - sosPressStartTime;
          if (elapsed >= 3000) { 
            sosIsPressing = false;
            lv_label_set_text(lbl_btn_sos, "S.O.S"); 
            go_sos(); 
          } else {
            int segundosRestantes = 3 - (elapsed / 1000);
            char buf[12]; sprintf(buf, "SEGURE %d", segundosRestantes);
            lv_label_set_text(lbl_btn_sos, buf);
          }
        }
      }
      else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        sosIsPressing = false;
        lv_label_set_text(lbl_btn_sos, "S.O.S"); 
      }
    }, LV_EVENT_ALL, NULL);
  }

  lv_label_set_text(lbl_header, "PAINEL E-BIKE");
  lv_obj_add_flag(lbl_header, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(btn_exit, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load(scr_main);
}

void atualizarDashboardDinamico() {
  char buf[20];
  sprintf(buf, "BAT: %d%%", bateriaSimulada); lv_label_set_text(lbl_status_left, buf);
  sprintf(buf, "%02d:%02d", relogioHora, relogioMinuto); lv_label_set_text(lbl_status_center, buf);

  float velExibida = modoMetrico ? velocidadeReal : velocidadeReal * 0.621371;
  sprintf(buf, "%02.1f", velExibida); lv_label_set_text(lbl_speed_val, buf);
  lv_label_set_text(lbl_speed_unit, modoMetrico ? "KM/H" : "MPH");

  lv_label_set_text(lbl_pace_val, (corridaAtiva || velocidadeReal > 0.5) ? "04:15" : "--:--");
  float distExibida = modoMetrico ? distanciaAcumulada : distanciaAcumulada * 0.621371;
  sprintf(buf, "%02.2f", distExibida); lv_label_set_text(lbl_dist_val, buf);

  int segs = (tempoExibicaoCorrida / 1000) % 60; int mins = (tempoExibicaoCorrida / 60000) % 60;
  sprintf(buf, "%02d:%02d", mins, segs); lv_label_set_text(lbl_time_val, buf);

  if (modoPiloto == 0) {
    lv_label_set_text(lbl_btn_modo_sub, "ECO"); lv_obj_set_style_bg_color(btn_modo, getGreenECO(), LV_PART_MAIN); 
  } else {
    lv_label_set_text(lbl_btn_modo_sub, "SPORT"); lv_obj_set_style_bg_color(btn_modo, getRedSport(), LV_PART_MAIN); 
  }
}

void go_cfg() {
  if (!scr_cfg) {
    scr_cfg = new_screen(NULL);

    lv_obj_t * lbl_bl_title = lv_label_create(scr_cfg); lv_label_set_text(lbl_bl_title, "BRILHO DA TELA"); lv_obj_set_style_text_font(lbl_bl_title, &lv_font_montserrat_12, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_bl_title, getAccentBlue(), LV_PART_MAIN); lv_obj_set_pos(lbl_bl_title, 15, 15);
    slider_bl = lv_slider_create(scr_cfg); lv_obj_set_width(slider_bl, 290); lv_obj_set_pos(slider_bl, 15, 35); lv_slider_set_range(slider_bl, 15, 255); lv_slider_set_value(slider_bl, nivelBrilho, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider_bl, [](lv_event_t * e) -> void {
      nivelBrilho = lv_slider_get_value(slider_bl);
      ajustaBrilhoTela(nivelBrilho); 
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * lbl_hour_title = lv_label_create(scr_cfg); lv_label_set_text(lbl_hour_title, "AJUSTAR RELOGIO"); lv_obj_set_style_text_font(lbl_hour_title, &lv_font_montserrat_12, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_hour_title, getAccentBlue(), LV_PART_MAIN); lv_obj_set_pos(lbl_hour_title, 15, 75);

    lv_obj_t * btn_h_minus = lv_btn_create(scr_cfg); lv_obj_set_size(btn_h_minus, 40, 35); lv_obj_set_pos(btn_h_minus, 15, 95); lv_obj_set_style_bg_color(btn_h_minus, getDarkBtnBg(), LV_PART_MAIN);
    lv_obj_t * lbl_h_min = lv_label_create(btn_h_minus); lv_label_set_text(lbl_h_min, "H-"); lv_obj_set_style_text_color(lbl_h_min, getTextColor(), LV_PART_MAIN); lv_obj_align(lbl_h_min, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_h_minus, [](lv_event_t * e) -> void { relogioHora--; if (relogioHora < 0) relogioHora = 23; }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_h_plus = lv_btn_create(scr_cfg); lv_obj_set_size(btn_h_plus, 40, 35); lv_obj_set_pos(btn_h_plus, 65, 95); lv_obj_set_style_bg_color(btn_h_plus, getDarkBtnBg(), LV_PART_MAIN);
    lv_obj_t * lbl_h_pl = lv_label_create(btn_h_plus); lv_label_set_text(lbl_h_pl, "H+"); lv_obj_set_style_text_color(lbl_h_pl, getTextColor(), LV_PART_MAIN); lv_obj_align(lbl_h_pl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_h_plus, [](lv_event_t * e) -> void { relogioHora++; if (relogioHora >= 24) relogioHora = 0; }, LV_EVENT_CLICKED, NULL);

    lbl_time_cfg = lv_label_create(scr_cfg); lv_obj_set_style_text_font(lbl_time_cfg, &lv_font_montserrat_18, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_time_cfg, getTextColor(), LV_PART_MAIN); lv_obj_set_pos(lbl_time_cfg, 120, 100);

    lv_obj_t * btn_m_minus = lv_btn_create(scr_cfg); lv_obj_set_size(btn_m_minus, 40, 35); lv_obj_set_pos(btn_m_minus, 190, 95); lv_obj_set_style_bg_color(btn_m_minus, getDarkBtnBg(), LV_PART_MAIN);
    lv_obj_t * lbl_m_min = lv_label_create(btn_m_minus); lv_label_set_text(lbl_m_min, "M-"); lv_obj_set_style_text_color(lbl_m_min, getTextColor(), LV_PART_MAIN); lv_obj_align(lbl_m_min, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_m_minus, [](lv_event_t * e) -> void { relogioMinuto--; if (relogioMinuto < 0) relogioMinuto = 59; }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_m_plus = lv_btn_create(scr_cfg); lv_obj_set_size(btn_m_plus, 40, 35); lv_obj_set_pos(btn_m_plus, 240, 95); lv_obj_set_style_bg_color(btn_m_plus, getDarkBtnBg(), LV_PART_MAIN);
    lv_obj_t * lbl_m_pl = lv_label_create(btn_m_plus); lv_label_set_text(lbl_m_pl, "M+"); lv_obj_set_style_text_color(lbl_m_pl, getTextColor(), LV_PART_MAIN); lv_obj_align(lbl_m_pl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_m_plus, [](lv_event_t * e) -> void { relogioMinuto++; if (relogioMinuto >= 60) relogioMinuto = 0; }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_unit_title = lv_label_create(scr_cfg); lv_label_set_text(lbl_unit_title, "UNIDADE"); lv_obj_set_style_text_font(lbl_unit_title, &lv_font_montserrat_12, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_unit_title, getAccentBlue(), LV_PART_MAIN); lv_obj_set_pos(lbl_unit_title, 15, 145);

    btn_unit_kmh = lv_btn_create(scr_cfg); lv_obj_set_size(btn_unit_kmh, 70, 35); lv_obj_set_pos(btn_unit_kmh, 15, 165);
    lv_obj_t * lbl_kmh = lv_label_create(btn_unit_kmh); lv_label_set_text(lbl_kmh, "KM/H"); lv_obj_set_style_text_color(lbl_kmh, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_kmh, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_unit_kmh, [](lv_event_t * e) -> void { modoMetrico = true; atualizarBotoesUnidade(); }, LV_EVENT_CLICKED, NULL);

    btn_unit_mph = lv_btn_create(scr_cfg); lv_obj_set_size(btn_unit_mph, 70, 35); lv_obj_set_pos(btn_unit_mph, 95, 165);
    lv_obj_t * lbl_mph = lv_label_create(btn_unit_mph); lv_label_set_text(lbl_mph, "MPH"); lv_obj_set_style_text_color(lbl_mph, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_mph, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_unit_mph, [](lv_event_t * e) -> void { modoMetrico = false; atualizarBotoesUnidade(); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_theme_title = lv_label_create(scr_cfg); lv_label_set_text(lbl_theme_title, "TEMA"); lv_obj_set_style_text_font(lbl_theme_title, &lv_font_montserrat_12, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_theme_title, getAccentBlue(), LV_PART_MAIN); lv_obj_set_pos(lbl_theme_title, 185, 145);
    
    lv_obj_t * btn_theme = lv_btn_create(scr_cfg); lv_obj_set_size(btn_theme, 85, 35); lv_obj_set_pos(btn_theme, 185, 165); lv_obj_set_style_bg_color(btn_theme, getAccentBlue(), LV_PART_MAIN);
    lv_obj_t * lbl_theme = lv_label_create(btn_theme); lv_label_set_text(lbl_theme, temaBranco ? "BRANCO" : "ESCURO"); lv_obj_set_style_text_color(lbl_theme, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_theme, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_theme, [](lv_event_t * e) -> void {
      temaBranco = !temaBranco;
      lv_obj_t * lbl = lv_obj_get_child(lv_event_get_target(e), 0);
      lv_label_set_text(lbl, temaBranco ? "BRANCO" : "ESCURO");
      aplicarCoresDoTema();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_back = lv_btn_create(scr_cfg); lv_obj_set_size(btn_back, 150, 30); lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -5); lv_obj_set_style_bg_color(btn_back, getAccentBlue(), LV_PART_MAIN);
    lv_obj_t * lbl_back = lv_label_create(btn_back); lv_label_set_text(lbl_back, "VOLTAR"); lv_obj_set_style_text_color(lbl_back, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_back, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_back, [](lv_event_t * e) -> void { go_main(); }, LV_EVENT_CLICKED, NULL);
  }

  lv_label_set_text(lbl_header, "AJUSTES"); 
  lv_obj_clear_flag(lbl_header, LV_OBJ_FLAG_HIDDEN); 
  lv_obj_clear_flag(btn_exit, LV_OBJ_FLAG_HIDDEN);
  atualizarBotoesUnidade(); 
  lv_scr_load(scr_cfg);
}

void atualizarConfiguracoesDinamico() {
  char buf[20]; sprintf(buf, "%02d:%02d", relogioHora, relogioMinuto); lv_label_set_text(lbl_time_cfg, buf);
}

void atualizarBotoesUnidade() {
  if (modoMetrico) {
    lv_obj_set_style_bg_color(btn_unit_kmh, getAccentBlue(), LV_PART_MAIN); 
    lv_obj_set_style_bg_color(btn_unit_mph, getDarkBtnBg(), LV_PART_MAIN); 
  } else {
    lv_obj_set_style_bg_color(btn_unit_kmh, getDarkBtnBg(), LV_PART_MAIN); 
    lv_obj_set_style_bg_color(btn_unit_mph, getAccentBlue(), LV_PART_MAIN); 
  }
}

void go_modos() {
  modoPilotoTemp = modoPiloto; 

  if (!scr_modos) {
    scr_modos = new_screen(NULL);

    lv_obj_t * lbl_modos_title = lv_label_create(scr_modos); lv_label_set_text(lbl_modos_title, "MODO DE PILOTAGEM"); lv_obj_set_style_text_font(lbl_modos_title, &lv_font_montserrat_14, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_modos_title, getAccentBlue(), LV_PART_MAIN); lv_obj_align(lbl_modos_title, LV_ALIGN_TOP_MID, 0, 45);

    btn_eco = lv_btn_create(scr_modos); lv_obj_set_size(btn_eco, 120, 50); lv_obj_set_pos(btn_eco, 30, 95);
    lv_obj_t * lbl_eco = lv_label_create(btn_eco); lv_label_set_text(lbl_eco, "ECO"); lv_obj_set_style_text_font(lbl_eco, &lv_font_montserrat_16, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_eco, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_eco, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_eco, [](lv_event_t * e) -> void {
      modoPilotoTemp = 0; 
      atualizarVisuaisBotoesModo();
    }, LV_EVENT_CLICKED, NULL);

    btn_sport = lv_btn_create(scr_modos); lv_obj_set_size(btn_sport, 120, 50); lv_obj_set_pos(btn_sport, 170, 95);
    lv_obj_t * lbl_sport = lv_label_create(btn_sport); lv_label_set_text(lbl_sport, "SPORT"); lv_obj_set_style_text_font(lbl_sport, &lv_font_montserrat_16, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_sport, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_sport, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_sport, [](lv_event_t * e) -> void {
      modoPilotoTemp = 1; 
      atualizarVisuaisBotoesModo();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_confirm = lv_btn_create(scr_modos); lv_obj_set_size(btn_confirm, 150, 40); lv_obj_align(btn_confirm, LV_ALIGN_BOTTOM_MID, 0, -25); lv_obj_set_style_bg_color(btn_confirm, getAccentBlue(), LV_PART_MAIN);
    lv_obj_t * lbl_confirm = lv_label_create(btn_confirm); lv_label_set_text(lbl_confirm, "CONFIRMAR"); lv_obj_set_style_text_color(lbl_confirm, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_confirm, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_confirm, [](lv_event_t * e) -> void {
      if (modoPilotoTemp != modoPiloto) {
        modoPiloto = modoPilotoTemp;
        transmitirDadosEspNow();
      }
      go_main();
    }, LV_EVENT_CLICKED, NULL);
  }

  lv_label_set_text(lbl_header, "CONFIGURAR MODO");
  lv_obj_clear_flag(lbl_header, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(btn_exit, LV_OBJ_FLAG_HIDDEN);

  atualizarVisuaisBotoesModo(); 
  lv_scr_load(scr_modos);
}

void atualizarVisuaisBotoesModo() {
  if (modoPilotoTemp == 0) {
    lv_obj_set_style_bg_color(btn_eco, getGreenECO(), LV_PART_MAIN); 
    lv_obj_set_style_bg_color(btn_sport, getDarkBtnBg(), LV_PART_MAIN); 
  } else {
    lv_obj_set_style_bg_color(btn_eco, getDarkBtnBg(), LV_PART_MAIN); 
    lv_obj_set_style_bg_color(btn_sport, getRedSport(), LV_PART_MAIN); 
  }
}

void go_sos() {
  sosAtivoLocal = true;
  transmitirDadosEspNow();

  if (!scr_sos) {
    scr_sos = new_screen(NULL);
    lv_obj_set_style_bg_color(scr_sos, getRedSOS(), LV_PART_MAIN);

    lv_obj_t * lbl_sos_alert = lv_label_create(scr_sos); lv_label_set_text(lbl_sos_alert, "ALERTA S.O.S"); lv_obj_set_style_text_font(lbl_sos_alert, &lv_font_montserrat_24, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_sos_alert, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_sos_alert, LV_ALIGN_CENTER, 0, -40);
    lv_obj_t * lbl_sos_sub = lv_label_create(scr_sos); lv_label_set_text(lbl_sos_sub, "ENVIANDO COORDENADAS VIA WHATSAPP..."); lv_obj_set_style_text_font(lbl_sos_sub, &lv_font_montserrat_12, LV_PART_MAIN); lv_obj_set_style_text_color(lbl_sos_sub, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_sos_sub, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * btn_sos_cancel = lv_btn_create(scr_sos); lv_obj_set_size(btn_sos_cancel, 120, 40); lv_obj_align(btn_sos_cancel, LV_ALIGN_CENTER, 0, 50); lv_obj_set_style_bg_color(btn_sos_cancel, lv_color_black(), LV_PART_MAIN);
    lv_obj_t * lbl_sos_cancel = lv_label_create(btn_sos_cancel); lv_label_set_text(lbl_sos_cancel, "CANCELAR"); lv_obj_set_style_text_color(lbl_sos_cancel, lv_color_white(), LV_PART_MAIN); lv_obj_align(lbl_sos_cancel, LV_ALIGN_CENTER, 0, 0);
    
    lv_obj_add_event_cb(btn_sos_cancel, [](lv_event_t * e) -> void {
      sosAtivoLocal = false;
      transmitirDadosEspNow();
      go_main();
    }, LV_EVENT_CLICKED, NULL);
  }

  lv_label_set_text(lbl_header, "EMERGENCIA ACIONADA"); 
  lv_obj_add_flag(btn_exit, LV_OBJ_FLAG_HIDDEN); 
  lv_scr_load(scr_sos);
}
