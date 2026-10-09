// Plotter GPS nautico - versione LilyGO T-Deck Plus
// Porting del plotter per ESP32 CYD: display LCD 320x240, touch GT911, tastiera, trackball, GPS integrato.
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <TouchDrvGT911.hpp>
#include <TinyGPS++.h>
#include <TJpg_Decoder.h>
#include <vector>
#include <math.h>
#include "board.h"

// Dimensioni display (orizzontale)
#define SCREEN_W 320
#define SCREEN_H 240
#define SIDEBAR_W 40
#define STATUS_H 20
#define MAP_W (SCREEN_W - SIDEBAR_W)
#define MAP_H (SCREEN_H - STATUS_H)

// Colori
#define BLACK       TFT_BLACK
#define WHITE       TFT_WHITE
#define WATER_COLOR 0x9EDD      // azzurro chiaro
#define GRID_COLOR  0x52AA      // grigio
#define ROUTE_COLOR TFT_MAGENTA
#define WAYPOINT_COLOR TFT_RED
#define BOAT_COLOR  TFT_RED
#define ACCENT      TFT_NAVY

// Simbolo gradi nel font di default (codepage 437)
#define DEG_CHAR ((char)248)

// Aggiornamento display
#define MAP_REFRESH_MS 1000UL // ridisegna la mappa ogni secondo
#define BOAT_MOVE_PX   1

// Touch
#define LONG_PRESS_MS 800
#define TOUCH_MOVE_TOLERANCE 12
#define TOUCH_POLL_MS 12    // intervallo di lettura del touch
#define TOUCH_RELEASE_MS 80 // il dito è sollevato solo dopo questo tempo senza letture

// Mappe a tile su SD: /tiles/z/x/y.jpg (schema XYZ, Web Mercator, 256x256)
#define TILE_SIZE 256
#define TILE_ZOOM_MIN 3
#define TILE_ZOOM_MAX 18
#define TILE_ZOOM_DEFAULT 12
// La cache delle tile è più grande della mappa: la SD si rilegge solo ogni TILE_CACHE_MARGIN pixel di spostamento
#define TILE_CACHE_MARGIN 64
#define TILE_CACHE_W (MAP_W + 2 * TILE_CACHE_MARGIN)
#define TILE_CACHE_H (MAP_H + 2 * TILE_CACHE_MARGIN)

// Trackball: pixel di spostamento della mappa per ogni scatto
#define PAN_STEP_PX 12

// Oggetti globali
TFT_eSPI tft;
TFT_eSprite display(&tft);  // si disegna tutto qui e poi si copia sul display in un colpo: niente sfarfallio
TFT_eSprite mapCache(&tft); // sfondo della mappa (tile) già decodificato: si rilegge la SD solo quando la mappa si sposta
TouchDrvGT911 touch;
TinyGPSPlus gps;
HardwareSerial& SerialGPS = Serial1;

bool touchOk = false;
bool keypadOk = false;
bool sdOk = false;

// Strutture dati
struct Waypoint {
  int id = 0;
  char name[20] = "";
  double lat = 0;
  double lon = 0;
  bool visible = true;
};

struct Route {
  int id = 0;
  char name[20] = "";
  std::vector<int> waypointIds;
};

// Variabili globali
std::vector<Waypoint> waypoints;
std::vector<Route> routes;
int currentRoute = -1;
int selectedWaypoint = -1;
int activeWaypointId = -1; // destinazione ("vai a"), -1 = nessuna

// Prora: la scheda non ha una bussola magnetica, si usa la rotta GPS (valida solo in movimento)
#define COURSE_MIN_KNOTS 0.5
float lastCourse = 0;
bool courseKnown = false;

// Pulsanti della barra laterale
#define SIDE_BTN_COUNT 7
#define SIDE_BTN_PITCH 34
#define SIDE_BTN_H 30
#define SIDE_BTN_Y0 3

// Pagine con lista: riquadro dei dettagli e riga dei comandi in basso (la barra laterale resta sempre visibile)
#define LIST_Y0 30
#define LIST_ITEM_H 25
#define LIST_ROWS 5
#define DETAIL_Y 156
#define DETAIL_H 54
#define ACTION_Y 214
#define ACTION_H 24
#define ACTION_COUNT 4

// Coordinate per la mappa
double mapCenterLat = 0;
double mapCenterLon = 0;
float zoomLevel = 10000.0; // metri per pixel (ricavato da tileZoom, salvato per compatibilità con la CYD)
int tileZoom = TILE_ZOOM_DEFAULT; // livello di zoom delle tile
bool followBoat = false;   // la mappa segue la barca tenendola al centro (tasto c); si spegne spostando la mappa
bool mapCenterSet = false; // false finché non c'è una configurazione o un primo fix GPS

// Stato sistema
enum AppState {MAP_VIEW, WAYPOINT_MGR, ROUTE_MGR, MENU, TEXT_INPUT, COMPASS_VIEW};
AppState currentState = MAP_VIEW;

// Popup e messaggi temporanei (al posto dei delay bloccanti della versione CYD)
String messageText;
unsigned long messageUntil = 0;
bool showRouteInfo = false;

// Inserimento testo da tastiera (rinomina waypoint/rotte)
enum InputTarget {INPUT_WAYPOINT, INPUT_ROUTE};
InputTarget inputTarget = INPUT_WAYPOINT;
int inputIndex = -1;
String inputBuffer;
AppState inputReturnState = MAP_VIEW;

// Stato del display
bool needsRedraw = true;
unsigned long lastMapRedraw = 0;
int lastBoatX = -1000, lastBoatY = -1000;

// GPS
uint32_t gpsBaud = 0;
unsigned long lastGpsByte = 0;
String nmeaBuffer;

// Touch
bool touchDown = false;
bool longPressFired = false;
unsigned long touchStartMs = 0;
unsigned long touchLastSeenMs = 0;
int16_t touchStartX = 0, touchStartY = 0;
int16_t touchLastX = 0, touchLastY = 0;

// Tile
bool tilesOk = false;       // cartella /tiles presente e cache creata
bool mapCacheValid = false;
int tileZoomMin = TILE_ZOOM_MIN, tileZoomMax = TILE_ZOOM_MAX; // livelli presenti sulla SD
int mapCacheZoom = -1;
long long mapCacheWX = 0, mapCacheWY = 0; // coordinate mondo dell'angolo in alto a sinistra della cache

// Trackball
const uint8_t trackballPins[5] = {BOARD_TBOX_RIGHT, BOARD_TBOX_UP, BOARD_TBOX_LEFT, BOARD_TBOX_DOWN, BOARD_BOOT_PIN};
bool trackballLast[5];

// ============================================
// PROTOTIPI DI FUNZIONE
// ============================================
void initPower();
void scanI2C();
void handleTrackball();
void panMap(int dx, int dy);
void initDisplay();
void initTouch();
void initKeypad();
void initTrackball();
void initGPS();
bool detectGpsBaud();
void readGPS();
void handleTouch();
void handleKeypad();
void processTap(int x, int y);
void processLongPress(int x, int y);
void processKey(char c);
void updateDisplay();
void renderFrame();
void requestRedraw();

void drawMap();
void drawActionBar(const char* const labels[ACTION_COUNT]);
int actionButtonAt(int x, int y);
int listFirstRow(int selected);
void waypointAction(int i);
void routeAction(int i);
void drawCompassPage();
void drawActiveWaypointLine();
void setActiveWaypoint(int index);
Waypoint* activeWaypoint();
float boatHeading();
void initTiles();
void renderMapCache();
bool tileOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap);
void updateScale();
double lonToWorldX(double lon);
double latToWorldY(double lat);
void drawGrid();
double calculateLonStep();
double calculateLatStep();
void drawCompassRose();
String formatCoordinate(double coord, bool isLat);
void drawWaypoints();
void drawRoutes();
void drawRouteArrow(int x1, int y1, int x2, int y2);
void drawActiveRouteInfo();
void drawBoatPosition();
void drawBoatSymbol(int x, int y, float course, uint16_t color);
void drawMainUI();
void drawStatusBar();
void drawButton(int x, int y, int w, int h, const String& label, bool active);
void drawText(const String& s, int x, int y, uint8_t align, uint16_t fg, uint16_t bg);
void drawMessage();
void drawRouteInfoPopup();
void drawTextInput();

void handleMapTouch(int x, int y);
void addWaypointAtTouch(int x, int y);
void handleMenuButtonsTouch(int x, int y);
void handleWaypointTouch(int x, int y);
void handleRouteTouch(int x, int y);
void handleMenuTouch(int x, int y);

void drawWaypointManager();
void drawWaypointManagerButtons();
void handleWaypointManagerButtons(int x, int y);
void drawRouteManager();
void drawRouteManagerButtons();
void handleRouteManagerButtons(int x, int y);
void drawSettingsMenu();
void drawWaypointDetail(int index);
void drawRouteDetail(int index);

void showMessage(const String& message);
void editWaypoint(int index);
void editRoute(int index);
void goToState(AppState state);
void zoomIn();
void zoomOut();
void centerOnBoat();
void addWaypointAtCurrentPosition(String name);
void createRoute(String name);
void calculateRouteInfo();
void deleteWaypoint(int index);
void deleteRoute(int index);
Waypoint* findWaypoint(int id);
int nextWaypointId();
double routeDistance(const Route& route);
int lonToPixel(double lon);
int latToPixel(double lat);
double pixelToLon(int x);
double pixelToLat(int y);

void saveConfig();
void loadConfig();
void saveWaypointsToSD();
void loadWaypointsFromSD();
void saveRoutesToSD();
void loadRoutesFromSD();

// ============================================
// SETUP E LOOP
// ============================================

void setup() {
  Serial.begin(115200);
  delay(500);

  initPower();

  // SPI condiviso: display, SD e LoRa
  SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
  initDisplay();

  // I2C condiviso: touch e tastiera
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  scanI2C();
  initTouch();
  initKeypad();
  initTrackball();

  // SD card
  // Prima veloce (serve per leggere le tile in fretta), poi alla velocità prudente del firmware LilyGO
  sdOk = SD.begin(BOARD_SDCARD_CS, SPI, 20000000U);
  if(!sdOk) {
    SD.end();
    sdOk = SD.begin(BOARD_SDCARD_CS, SPI, 800000U);
  }
  if(!sdOk) {
    Serial.println("SD Card non rilevata");
  }

  initTiles();

  // Carica configurazione
  loadConfig();
  updateScale();
  loadWaypointsFromSD();
  loadRoutesFromSD();

  initGPS();

  if(!sdOk) showMessage("SD non rilevata");
  requestRedraw();

  Serial.println("Plotter GPS Nautico T-Deck pronto");
}

void loop() {
  readGPS();
  handleTouch();
  handleKeypad();
  handleTrackball();

  // Il messaggio temporaneo è scaduto: ridisegna senza
  if(messageUntil != 0 && millis() > messageUntil) {
    messageUntil = 0;
    messageText = "";
    requestRedraw();
  }

  updateDisplay();
}

// ============================================
// INIZIALIZZAZIONE HARDWARE
// ============================================

void initPower() {
  // Accende le periferiche della scheda (display, touch, tastiera, GPS, SD)
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);
  delay(100);

  // Tutti i CS del bus SPI alti prima di iniziare
  pinMode(BOARD_SDCARD_CS, OUTPUT); digitalWrite(BOARD_SDCARD_CS, HIGH);
  pinMode(BOARD_RADIO_CS, OUTPUT);  digitalWrite(BOARD_RADIO_CS, HIGH);
  pinMode(BOARD_TFT_CS, OUTPUT);    digitalWrite(BOARD_TFT_CS, HIGH);
  pinMode(BOARD_SPI_MISO, INPUT_PULLUP);
}

// Elenca i dispositivi I2C presenti (diagnostica sulla seriale)
void scanI2C() {
  Wire.setTimeOut(20);
  Serial.print("I2C trovati:");
  int found = 0;
  for(uint8_t addr = 1; addr < 0x7F; addr++) {
    Wire.beginTransmission(addr);
    if(Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", addr);
      found++;
    }
  }
  Serial.println(found ? "" : " nessuno");
}

void initDisplay() {
  tft.begin();
  tft.setRotation(1); // orizzontale, 320x240
  tft.fillScreen(TFT_BLACK);

  pinMode(BOARD_TFT_BACKLIGHT, OUTPUT);
  digitalWrite(BOARD_TFT_BACKLIGHT, HIGH);

  // Schermo intero in un buffer: 16 bit in PSRAM, altrimenti 8 bit in RAM
  display.setColorDepth(16);
  if(display.createSprite(SCREEN_W, SCREEN_H) == nullptr) {
    Serial.println("PSRAM non disponibile, uso colori a 8 bit");
    display.setColorDepth(8);
    display.createSprite(SCREEN_W, SCREEN_H);
  }
  display.setAttribute(CP437_SWITCH, true);
  display.setTextWrap(false);
  display.setTextFont(1);
}

void initTouch() {
  touch.setPins(-1, BOARD_TOUCH_INT);
  touchOk = touch.begin(Wire, GT911_SLAVE_ADDRESS_L, BOARD_I2C_SDA, BOARD_I2C_SCL);
  if(!touchOk) touchOk = touch.begin(Wire, GT911_SLAVE_ADDRESS_H, BOARD_I2C_SDA, BOARD_I2C_SCL);
  if(touchOk) {
    // Stessa configurazione del firmware LilyGO per la rotazione orizzontale
    touch.setMaxCoordinates(SCREEN_W, SCREEN_H);
    touch.setSwapXY(true);
    touch.setMirrorXY(false, true);
  }
  Serial.println(touchOk ? "Touch GT911 trovato" : "Touch non trovato");
  if(touchOk) {
    int16_t rx = 0, ry = 0;
    touch.getResolution(&rx, &ry);
    Serial.printf("GT911: chip %lu, fw 0x%04X, config %u, risoluzione %d x %d, punti max %u, refresh %u, INT mode %u, INT pin %d\n",
      (unsigned long)touch.getChipID(), touch.getFwVersion(), touch.getConfigVersion(), rx, ry,
      touch.getMaxTouchPoint(), touch.getRefreshRate(), touch.getInterruptMode(), digitalRead(BOARD_TOUCH_INT));
  }
}

void initKeypad() {
  // La tastiera è un ESP32-C3 che risponde all'indirizzo 0x55
  for(int i = 0; i < 3 && !keypadOk; i++) {
    Wire.beginTransmission(BOARD_KEYBOARD_ADDR);
    keypadOk = Wire.endTransmission() == 0;
    if(!keypadOk) delay(100);
  }
  Serial.println(keypadOk ? "Tastiera trovata" : "Tastiera non trovata");
}

void initTrackball() {
  for(int i = 0; i < 5; i++) {
    pinMode(trackballPins[i], INPUT_PULLUP);
    trackballLast[i] = digitalRead(trackballPins[i]);
  }
}

void initGPS() {
  if(!detectGpsBaud()) {
    Serial.println("GPS: nessun dato, riprovo più tardi");
  }
  lastGpsByte = millis();
}

// Il modulo GPS integrato può essere configurato a velocità diverse: le prova finché riceve NMEA
bool detectGpsBaud() {
  const uint32_t bauds[] = {38400, 9600, 115200};
  for(uint32_t baud : bauds) {
    SerialGPS.end();
    SerialGPS.begin(baud, SERIAL_8N1, BOARD_GPS_RX_PIN, BOARD_GPS_TX_PIN);
    unsigned long start = millis();
    char prev = 0;
    while(millis() - start < 1500) {
      while(SerialGPS.available()) {
        char c = SerialGPS.read();
        if(prev == '$' && c == 'G') {
          gpsBaud = baud;
          Serial.printf("GPS trovato a %lu baud\n", (unsigned long)baud);
          return true;
        }
        prev = c;
      }
      delay(5);
    }
  }
  gpsBaud = 0;
  return false;
}

// ============================================
// FUNZIONI CORE
// ============================================

void readGPS() {
  while(SerialGPS.available()) {
    char c = SerialGPS.read();
    gps.encode(c);
    lastGpsByte = millis();

    // Buffer per debug
    if(c == '\n') {
      Serial.println(nmeaBuffer);
      nmeaBuffer = "";
    } else if(c != '\r' && nmeaBuffer.length() < 120) {
      nmeaBuffer += c;
    }
  }

  // Nessun dato da 10 s: riprova a rilevare la velocità
  if(millis() - lastGpsByte > 10000) {
    detectGpsBaud();
    lastGpsByte = millis();
  }

  // Primo fix senza configurazione salvata: centra la mappa sulla barca
  if(!mapCenterSet && gps.location.isValid()) {
    mapCenterSet = true;
    followBoat = true;
  }

  // Inseguimento: la barca resta al centro della mappa mentre si naviga
  if(followBoat && gps.location.isValid() &&
     (gps.location.lat() != mapCenterLat || gps.location.lng() != mapCenterLon)) {
    mapCenterLat = gps.location.lat();
    mapCenterLon = gps.location.lng();
    if(currentState == MAP_VIEW) requestRedraw();
  }
}

void handleTouch() {
  if(!touchOk) return;

  // Il GT911 produce un dato ogni 10 ms: interrogarlo a ogni giro del loop (circa 1000 volte al secondo)
  // lo disturba e cancella i dati prima che siano pronti, così i tocchi non arrivano quasi mai.
  static unsigned long lastPoll = 0;
  if(millis() - lastPoll < TOUCH_POLL_MS) return;
  lastPoll = millis();

  int16_t x, y;
  uint8_t touched = touch.getPoint(&x, &y, 1);

  if(touched) {
    touchLastSeenMs = millis();
    x = constrain(x, 0, SCREEN_W - 1);
    y = constrain(y, 0, SCREEN_H - 1);
    if(!touchDown) {
      touchDown = true;
      longPressFired = false;
      touchStartMs = millis();
      touchStartX = x;
      touchStartY = y;
    }
    touchLastX = x;
    touchLastY = y;

    // Tocco lungo fermo: scatta una volta sola mentre il dito è ancora giù
    bool still = abs(x - touchStartX) < TOUCH_MOVE_TOLERANCE && abs(y - touchStartY) < TOUCH_MOVE_TOLERANCE;
    if(!longPressFired && still && millis() - touchStartMs > LONG_PRESS_MS) {
      longPressFired = true;
      Serial.printf("Tocco lungo: %d, %d\n", touchStartX, touchStartY);
      processLongPress(touchStartX, touchStartY);
    }
  } else if(touchDown && millis() - touchLastSeenMs > TOUCH_RELEASE_MS) {
    // Il GT911 aggiorna i dati ogni 10 ms circa: tra una scansione e l'altra la lettura torna vuota
    // anche con il dito giù, quindi il sollevamento vale solo dopo TOUCH_RELEASE_MS senza letture.
    // Dito sollevato: se non era un tocco lungo, è un tap
    touchDown = false;
    if(!longPressFired) {
      Serial.printf("Tap: %d, %d\n", touchStartX, touchStartY);
      processTap(touchStartX, touchStartY);
    }
  }
}

void handleKeypad() {
  if(!keypadOk) return;
  static unsigned long lastPoll = 0;
  if(millis() - lastPoll < 20) return;
  lastPoll = millis();

  Wire.requestFrom(BOARD_KEYBOARD_ADDR, 1);
  while(Wire.available() > 0) {
    char c = Wire.read();
    if(c == 0) continue;
    if(c == 0x0D) c = '\n';
    else if(c == 0x08) c = '\b';
    Serial.printf("Tasto: %d\n", (int)c);
    processKey(c);
  }
}

// La trackball cambia lo stato di un pin a ogni scatto
//   Mappa: sposta la mappa, pressione = centra sulla barca
//   Liste: su/giù per scegliere, pressione = mostra sulla mappa
void handleTrackball() {
  for(int i = 0; i < 5; i++) {
    bool v = digitalRead(trackballPins[i]);
    if(v == trackballLast[i]) continue;
    trackballLast[i] = v;
    if(i == 4 && v == HIGH) continue; // solo la pressione, non il rilascio

    if(showRouteInfo) {
      showRouteInfo = false;
      requestRedraw();
      return;
    }

    if(currentState == MAP_VIEW) {
      switch(i) {
        case 0: panMap(PAN_STEP_PX, 0); break;
        case 1: panMap(0, -PAN_STEP_PX); break;
        case 2: panMap(-PAN_STEP_PX, 0); break;
        case 3: panMap(0, PAN_STEP_PX); break;
        case 4: centerOnBoat(); break;
      }
    } else if(currentState == WAYPOINT_MGR || currentState == ROUTE_MGR) {
      int& sel = currentState == WAYPOINT_MGR ? selectedWaypoint : currentRoute;
      int count = currentState == WAYPOINT_MGR ? (int)waypoints.size() : (int)routes.size();
      if(count == 0) continue;
      if(i == 1 && sel > 0) { sel--; requestRedraw(); }
      if(i == 3 && sel < count - 1) { sel++; requestRedraw(); }
      if(i == 4 && currentState == WAYPOINT_MGR && sel >= 0) {
        mapCenterLat = waypoints[sel].lat;
        mapCenterLon = waypoints[sel].lon;
        mapCenterSet = true;
        followBoat = false;
        goToState(MAP_VIEW);
      } else if(i == 4 && currentState == ROUTE_MGR) {
        goToState(MAP_VIEW);
      }
    }
  }
}

void panMap(int dx, int dy) {
  mapCenterLon = pixelToLon(MAP_W / 2 + dx);
  mapCenterLat = pixelToLat(MAP_H / 2 + dy);
  mapCenterSet = true;
  followBoat = false; // spostando la mappa a mano si smette di seguire la barca
  requestRedraw();
}

void processTap(int x, int y) {
  // Un popup aperto si chiude con un tocco qualsiasi
  if(showRouteInfo) {
    showRouteInfo = false;
    requestRedraw();
    return;
  }

  // La barra laterale funziona allo stesso modo su tutte le pagine
  if(x >= MAP_W && currentState != COMPASS_VIEW) {
    handleMenuButtonsTouch(x, y);
    return;
  }

  switch(currentState) {
    case MAP_VIEW:
      handleMapTouch(x, y);
      break;
    case WAYPOINT_MGR:
      handleWaypointTouch(x, y);
      break;
    case ROUTE_MGR:
      handleRouteTouch(x, y);
      break;
    case MENU:
      handleMenuTouch(x, y);
      break;
    case COMPASS_VIEW:
      goToState(MAP_VIEW); // un tocco qualsiasi torna alla mappa
      break;
    case TEXT_INPUT:
      break;
  }
}

void processLongPress(int x, int y) {
  // Tocco lungo sulla mappa (non sulla sidebar né sulla barra di stato): nuovo waypoint
  if(currentState == MAP_VIEW && !showRouteInfo && x < MAP_W && y < MAP_H) {
    addWaypointAtTouch(x, y);
  }
}

// Scorciatoie da tastiera
//   Mappa:     i o + = zoom +, o o - = zoom -, c = centra su barca, a = waypoint qui
//   Ovunque:   m = mappa, w = waypoint, r = rotte, s = impostazioni
//   Liste:     e = rinomina selezionato
//   Testo:     lettere, spazio, Backspace, Enter per confermare
void processKey(char c) {
  if(currentState == TEXT_INPUT) {
    if(c == '\n') {
      if(inputBuffer.length() > 0) {
        if(inputTarget == INPUT_WAYPOINT && inputIndex >= 0 && inputIndex < (int)waypoints.size()) {
          strncpy(waypoints[inputIndex].name, inputBuffer.c_str(), 19);
          waypoints[inputIndex].name[19] = '\0';
          saveWaypointsToSD();
        } else if(inputTarget == INPUT_ROUTE && inputIndex >= 0 && inputIndex < (int)routes.size()) {
          strncpy(routes[inputIndex].name, inputBuffer.c_str(), 19);
          routes[inputIndex].name[19] = '\0';
          saveRoutesToSD();
        }
      }
      goToState(inputReturnState);
    } else if(c == '\b') {
      if(inputBuffer.length() > 0) inputBuffer.remove(inputBuffer.length() - 1);
      requestRedraw();
    } else if(inputBuffer.length() < 19 && c >= ' ' && c <= '~') {
      inputBuffer += c;
      requestRedraw();
    }
    return;
  }

  if(showRouteInfo) {
    showRouteInfo = false;
    requestRedraw();
    return;
  }

  switch(c) {
    case 'm': goToState(MAP_VIEW); break;
    case 'w': goToState(WAYPOINT_MGR); break;
    case 'r': goToState(ROUTE_MGR); break;
    case 's': goToState(MENU); break;
    case 'b': goToState(COMPASS_VIEW); break;
    case 'g':
      if(currentState == WAYPOINT_MGR) setActiveWaypoint(selectedWaypoint);
      else showMessage("Scegli il waypoint nella lista (w)");
      break;
    case 'i': case '+': zoomIn(); break;
    case 'o': case '-': zoomOut(); break;
    case 'c': centerOnBoat(); break;
    case 'a': addWaypointAtCurrentPosition("WP"); break;
    case 'e':
      if(currentState == WAYPOINT_MGR) editWaypoint(selectedWaypoint);
      else if(currentState == ROUTE_MGR) editRoute(currentRoute);
      break;
    case 'd':
      if(currentState == WAYPOINT_MGR) waypointAction(3);
      else if(currentState == ROUTE_MGR) routeAction(2);
      break;
    case 'n':
      if(currentState == ROUTE_MGR) routeAction(0);
      break;
    case 'k':
      if(currentState == ROUTE_MGR) routeAction(3);
      break;
    default: break;
  }
}

void requestRedraw() {
  needsRedraw = true;
}

// Ridisegna quando qualcosa è cambiato, e comunque ogni secondo sulla mappa
void updateDisplay() {
  unsigned long now = millis();

  if(!needsRedraw && (currentState == MAP_VIEW || currentState == COMPASS_VIEW) && !showRouteInfo) {
    if(currentState == MAP_VIEW && gps.location.isValid()) {
      int bx = lonToPixel(gps.location.lng());
      int by = latToPixel(gps.location.lat());
      if(abs(bx - lastBoatX) >= BOAT_MOVE_PX || abs(by - lastBoatY) >= BOAT_MOVE_PX) {
        needsRedraw = true;
      }
    }
    if(now - lastMapRedraw > MAP_REFRESH_MS) needsRedraw = true;
  }

  if(!needsRedraw) return;
  needsRedraw = false;

  renderFrame();
  display.pushSprite(0, 0);
  lastMapRedraw = millis();
}

// Disegna l'intera schermata nel buffer in RAM
void renderFrame() {
  display.fillSprite(WHITE); // fillScreen su uno sprite pulisce solo 240x320: restava sporca la fascia destra

  switch(currentState) {
    case MAP_VIEW:
      drawMap();
      drawRoutes();
      drawWaypoints();
      drawActiveWaypointLine();
      drawBoatPosition();
      drawStatusBar();
      break;
    case COMPASS_VIEW:
      drawCompassPage();
      break;
    case WAYPOINT_MGR:
      drawWaypointManager();
      break;
    case ROUTE_MGR:
      drawRouteManager();
      break;
    case MENU:
      drawSettingsMenu();
      break;
    case TEXT_INPUT:
      drawTextInput();
      break;
  }

  // Barra laterale uguale su tutte le pagine; la bussola invece è a tutto schermo
  if(currentState != COMPASS_VIEW) drawMainUI();

  if(showRouteInfo) drawRouteInfoPopup();
  if(messageUntil != 0) drawMessage();
}

// ============================================
// FUNZIONI TOUCH
// ============================================

void handleMapTouch(int x, int y) {
  // Il tocco lungo (waypoint) è gestito in processLongPress; la barra laterale in processTap
}

void addWaypointAtTouch(int x, int y) {
  Waypoint wp;
  wp.id = nextWaypointId();
  snprintf(wp.name, sizeof(wp.name), "WP%d", wp.id);
  wp.lat = pixelToLat(y);
  wp.lon = pixelToLon(x);
  wp.visible = true;

  waypoints.push_back(wp);
  saveWaypointsToSD();

  // Se c'è una rotta attiva, aggiungilo alla rotta
  if(currentRoute >= 0 && currentRoute < (int)routes.size()) {
    routes[currentRoute].waypointIds.push_back(wp.id);
    saveRoutesToSD();
    showMessage(String(wp.name) + " aggiunto alla rotta");
  } else {
    showMessage(String("Waypoint ") + wp.name + " aggiunto");
  }
}

void handleMenuButtonsTouch(int x, int y) {
  int i = (y - SIDE_BTN_Y0) / SIDE_BTN_PITCH;
  if(y < SIDE_BTN_Y0 || i >= SIDE_BTN_COUNT) return;
  switch(i) {
    case 0: goToState(MAP_VIEW); break;
    case 1: goToState(WAYPOINT_MGR); break;
    case 2: goToState(ROUTE_MGR); break;
    case 3: goToState(MENU); break;
    case 4: goToState(COMPASS_VIEW); break;
    case 5: zoomIn(); break;
    case 6: zoomOut(); break;
  }
}

void handleWaypointTouch(int x, int y) {
  int action = actionButtonAt(x, y);
  if(action >= 0) {
    waypointAction(action);
  } else if(y >= LIST_Y0 && y < LIST_Y0 + LIST_ROWS * LIST_ITEM_H) {
    int index = listFirstRow(selectedWaypoint) + (y - LIST_Y0) / LIST_ITEM_H;
    if(index < (int)waypoints.size()) {
      selectedWaypoint = index;
      requestRedraw();
    }
  }
}

void handleRouteTouch(int x, int y) {
  int action = actionButtonAt(x, y);
  if(action >= 0) {
    routeAction(action);
  } else if(y >= LIST_Y0 && y < LIST_Y0 + LIST_ROWS * LIST_ITEM_H) {
    int index = listFirstRow(currentRoute) + (y - LIST_Y0) / LIST_ITEM_H;
    if(index < (int)routes.size()) {
      currentRoute = index;
      requestRedraw();
    }
  }
}

// Menu impostazioni: due colonne di tre pulsanti
#define MENU_COL1 6
#define MENU_COL2 143
#define MENU_BTN_W 131
#define MENU_BTN_H 32

int menuButtonAt(int x, int y) {
  for(int i = 0; i < 6; i++) {
    int bx = (i % 2 == 0) ? MENU_COL1 : MENU_COL2;
    int by = 40 + (i / 2) * 42;
    if(x >= bx && x <= bx + MENU_BTN_W && y >= by && y <= by + MENU_BTN_H) return i;
  }
  return -1;
}

void handleMenuTouch(int x, int y) {
  switch(menuButtonAt(x, y)) {
    case 0: zoomIn(); break;
    case 1: zoomOut(); break;
    case 2: centerOnBoat(); break;
    case 3:
      saveConfig();
      showMessage("Configurazione salvata");
      break;
    case 4: goToState(WAYPOINT_MGR); break;
    case 5: goToState(MAP_VIEW); break;
  }
}

// ============================================
// FUNZIONI MANAGER
// ============================================

void drawWaypointManager() {
  display.setTextSize(2);
  drawText("WAYPOINT", 10, 5, 0, ACCENT, WHITE);
  display.setTextSize(1);

  int y = LIST_Y0;
  for(int i = listFirstRow(selectedWaypoint); i < (int)waypoints.size() && y < LIST_Y0 + LIST_ROWS * LIST_ITEM_H; i++) {
    Waypoint& wp = waypoints[i];
    uint16_t fg = BLACK, bg = WHITE;

    // Evidenzia se selezionato
    if(i == selectedWaypoint) {
      display.fillRect(0, y - 2, MAP_W, 20, ACCENT);
      fg = WHITE; bg = ACCENT;
    }

    display.setTextColor(fg, bg);
    display.setCursor(5, y);
    display.printf("%d. %s%s", wp.id, wp.name, wp.id == activeWaypointId ? "  <- DESTINAZIONE" : "");
    display.setCursor(5, y + 9);
    display.printf("%.4f, %.4f", wp.lat, wp.lon);

    y += LIST_ITEM_H;
  }

  if(selectedWaypoint >= 0) drawWaypointDetail(selectedWaypoint);
  drawWaypointManagerButtons();
}

// Riga dei comandi in basso: ogni pulsante mostra anche il tasto della tastiera che fa la stessa cosa
void drawActionBar(const char* const labels[ACTION_COUNT]) {
  int w = (MAP_W - 4 - (ACTION_COUNT - 1) * 4) / ACTION_COUNT;
  for(int i = 0; i < ACTION_COUNT; i++) {
    drawButton(2 + i * (w + 4), ACTION_Y, w, ACTION_H, labels[i], false);
  }
}

int actionButtonAt(int x, int y) {
  if(y < ACTION_Y || x >= MAP_W) return -1;
  int w = (MAP_W - 4 - (ACTION_COUNT - 1) * 4) / ACTION_COUNT;
  int i = (x - 2) / (w + 4);
  return (i >= 0 && i < ACTION_COUNT) ? i : -1;
}

// Prima riga visibile di una lista: scorre quando la selezione supera le righe a schermo
int listFirstRow(int selected) {
  return selected >= LIST_ROWS ? selected - LIST_ROWS + 1 : 0;
}

void drawWaypointManagerButtons() {
  static const char* const labels[ACTION_COUNT] = {"g Vai", "a Nuovo", "e Nome", "d Elimina"};
  drawActionBar(labels);
}

void waypointAction(int i) {
  switch(i) {
    case 0: setActiveWaypoint(selectedWaypoint); break;
    case 1: addWaypointAtCurrentPosition("WP"); break;
    case 2: editWaypoint(selectedWaypoint); break;
    case 3:
      if(selectedWaypoint >= 0 && selectedWaypoint < (int)waypoints.size()) {
        deleteWaypoint(selectedWaypoint);
        selectedWaypoint = -1;
      } else {
        showMessage("Seleziona un waypoint");
      }
      break;
  }
}

void handleWaypointManagerButtons(int x, int y) {
  waypointAction(actionButtonAt(x, y));
}

void drawRouteManager() {
  display.setTextSize(2);
  drawText("ROTTE", 10, 5, 0, ACCENT, WHITE);
  display.setTextSize(1);

  int y = LIST_Y0;
  for(int i = listFirstRow(currentRoute); i < (int)routes.size() && y < LIST_Y0 + LIST_ROWS * LIST_ITEM_H; i++) {
    Route& route = routes[i];
    uint16_t fg = BLACK, bg = WHITE;

    if(i == currentRoute) {
      display.fillRect(0, y - 2, MAP_W, 20, ACCENT);
      fg = WHITE; bg = ACCENT;
    }

    display.setTextColor(fg, bg);
    display.setCursor(5, y);
    display.printf("%d. %s", route.id, route.name);
    display.setCursor(5, y + 9);
    display.printf("WP: %d", (int)route.waypointIds.size());

    y += LIST_ITEM_H;
  }

  if(currentRoute >= 0) drawRouteDetail(currentRoute);
  drawRouteManagerButtons();
}

void drawRouteManagerButtons() {
  static const char* const labels[ACTION_COUNT] = {"n Nuova", "e Nome", "d Elimina", "k Info"};
  drawActionBar(labels);
}

void routeAction(int i) {
  switch(i) {
    case 0: createRoute("Rotta"); break;
    case 1: editRoute(currentRoute); break;
    case 2:
      if(currentRoute >= 0 && currentRoute < (int)routes.size()) deleteRoute(currentRoute);
      else showMessage("Seleziona una rotta");
      break;
    case 3: calculateRouteInfo(); break;
  }
}

void handleRouteManagerButtons(int x, int y) {
  routeAction(actionButtonAt(x, y));
}

void drawSettingsMenu() {
  display.setTextSize(2);
  drawText("IMPOSTAZIONI", 10, 8, 0, ACCENT, WHITE);
  display.setTextSize(1);

  const char* labels[6] = {"Zoom +", "Zoom -", "Centra su barca", "Salva configurazione", "Waypoint", "Torna alla mappa"};
  for(int i = 0; i < 6; i++) {
    int bx = (i % 2 == 0) ? MENU_COL1 : MENU_COL2;
    int by = 40 + (i / 2) * 42;
    drawButton(bx, by, MENU_BTN_W, MENU_BTN_H, labels[i], false);
  }

  // Informazioni sistema
  display.setTextColor(BLACK, WHITE);
  display.setCursor(4, 180);
  display.printf("Zoom %d (%.0f m/px) Tile:%s WP:%d Rotte:%d", tileZoom, zoomLevel, tilesOk ? "ok" : "no", (int)waypoints.size(), (int)routes.size());
  display.setCursor(4, 195);
  display.printf("GPS:%s SD:%s Touch:%s Tastiera:%s",
    gpsBaud ? String(gpsBaud).c_str() : "--", sdOk ? "ok" : "no", touchOk ? "ok" : "no", keypadOk ? "ok" : "no");
  display.setCursor(4, 215);
  display.print("Tasti: i/o zoom  c centra  a waypoint qui");
}

void drawWaypointDetail(int index) {
  if(index >= 0 && index < (int)waypoints.size()) {
    Waypoint& wp = waypoints[index];

    display.fillRect(0, DETAIL_Y, MAP_W, DETAIL_H, WHITE);
    display.drawRect(0, DETAIL_Y, MAP_W, DETAIL_H, BLACK);
    display.setTextColor(BLACK, WHITE);
    display.setCursor(5, DETAIL_Y + 5);
    display.printf("WP%d: %s", wp.id, wp.name);
    display.setCursor(5, DETAIL_Y + 17);
    display.print(formatCoordinate(wp.lat, true));
    display.setCursor(5, DETAIL_Y + 29);
    display.print(formatCoordinate(wp.lon, false));
    if(gps.location.isValid()) {
      double d = TinyGPSPlus::distanceBetween(gps.location.lat(), gps.location.lng(), wp.lat, wp.lon);
      double brg = TinyGPSPlus::courseTo(gps.location.lat(), gps.location.lng(), wp.lat, wp.lon);
      display.setCursor(5, DETAIL_Y + 41);
      display.printf("%.2f NM  %03.0f%c", d / 1852.0, brg, DEG_CHAR);
    }
  }
}

void drawRouteDetail(int index) {
  if(index >= 0 && index < (int)routes.size()) {
    Route& route = routes[index];

    display.fillRect(0, DETAIL_Y, MAP_W, DETAIL_H, WHITE);
    display.drawRect(0, DETAIL_Y, MAP_W, DETAIL_H, BLACK);
    display.setTextColor(BLACK, WHITE);
    display.setCursor(5, DETAIL_Y + 5);
    display.printf("Rotta %d: %s", route.id, route.name);
    display.setCursor(5, DETAIL_Y + 17);
    display.printf("Waypoint: %d", (int)route.waypointIds.size());
    display.setCursor(5, DETAIL_Y + 29);
    display.printf("Distanza: %.2f NM", routeDistance(route) / 1852.0);
  }
}

// ============================================
// FUNZIONI UTILITY
// ============================================

void showMessage(const String& message) {
  Serial.println(message);
  messageText = message;
  messageUntil = millis() + 2500;
  requestRedraw();
}

void drawMessage() {
  int y = SCREEN_H / 2 - 15;
  display.fillRect(0, y, SCREEN_W, 30, BLACK);
  display.drawRect(2, y + 2, SCREEN_W - 4, 26, WHITE);
  display.setTextSize(1);
  drawText(messageText, SCREEN_W / 2, SCREEN_H / 2, 2, WHITE, BLACK);
}

void editWaypoint(int index) {
  if(index < 0 || index >= (int)waypoints.size()) {
    showMessage("Seleziona un waypoint");
    return;
  }
  inputTarget = INPUT_WAYPOINT;
  inputIndex = index;
  inputBuffer = waypoints[index].name;
  inputReturnState = currentState;
  goToState(TEXT_INPUT);
}

void editRoute(int index) {
  if(index < 0 || index >= (int)routes.size()) {
    showMessage("Seleziona una rotta");
    return;
  }
  inputTarget = INPUT_ROUTE;
  inputIndex = index;
  inputBuffer = routes[index].name;
  inputReturnState = currentState;
  goToState(TEXT_INPUT);
}

void drawTextInput() {
  display.setTextSize(2);
  drawText(inputTarget == INPUT_WAYPOINT ? "NOME WAYPOINT" : "NOME ROTTA", 10, 5, 0, BLACK, WHITE);

  display.drawRect(5, 60, MAP_W - 10, 30, BLACK);
  display.setTextColor(BLACK, WHITE);
  display.setCursor(10, 68);
  display.print(inputBuffer);
  display.print("_");

  display.setTextSize(1);
  display.setCursor(10, 110);
  display.print("Scrivi con la tastiera");
  display.setCursor(10, 125);
  display.print("Enter = conferma");
  display.setCursor(10, 140);
  display.print("Backspace = cancella");
}

void goToState(AppState state) {
  currentState = state;
  showRouteInfo = false;
  requestRedraw();
}

void zoomIn() {
  if(tileZoom < tileZoomMax) tileZoom++;
  updateScale();
  requestRedraw();
}

void zoomOut() {
  if(tileZoom > tileZoomMin) tileZoom--;
  updateScale();
  requestRedraw();
}

// Metri per pixel alla latitudine del centro mappa, per il livello di zoom corrente
void updateScale() {
  zoomLevel = 156543.03392 * cos(mapCenterLat * DEG_TO_RAD) / (double)(1UL << tileZoom);
}

// Centra la mappa sulla barca e continua a seguirla finché la mappa non viene spostata a mano
void centerOnBoat() {
  followBoat = true;
  mapCenterSet = true;
  if(gps.location.isValid()) {
    mapCenterLat = gps.location.lat();
    mapCenterLon = gps.location.lng();
    showMessage("La mappa segue la barca");
  } else {
    showMessage("Seguiro' la barca al primo fix GPS");
  }
}

void saveConfig() {
  if(!sdOk) return;
  File configFile = SD.open("/config.txt", FILE_WRITE);
  if(configFile) {
    updateScale();
    configFile.printf("zoomLevel=%f\n", zoomLevel);
    configFile.printf("tileZoom=%d\n", tileZoom);
    configFile.printf("mapCenterLat=%.8f\n", mapCenterLat);
    configFile.printf("mapCenterLon=%.8f\n", mapCenterLon);
    configFile.close();
    Serial.println("Configurazione salvata");
  } else {
    Serial.println("Errore salvataggio configurazione");
  }
}

void loadConfig() {
  if(!sdOk || !SD.exists("/config.txt")) return;
  File configFile = SD.open("/config.txt");
  if(configFile) {
    bool hasTileZoom = false;
    while(configFile.available()) {
      String line = configFile.readStringUntil('\n');
      if(line.startsWith("zoomLevel=")) {
        zoomLevel = line.substring(10).toFloat();
      } else if(line.startsWith("tileZoom=")) {
        tileZoom = line.substring(9).toInt();
        hasTileZoom = true;
      } else if(line.startsWith("mapCenterLat=")) {
        mapCenterLat = line.substring(13).toDouble();
        mapCenterSet = true;
      } else if(line.startsWith("mapCenterLon=")) {
        mapCenterLon = line.substring(13).toDouble();
      }
    }
    configFile.close();
    // Configurazione scritta dalla CYD: c'è solo la scala in metri per pixel
    if(!hasTileZoom && zoomLevel > 0) {
      tileZoom = (int)lround(log2(156543.03392 * cos(mapCenterLat * DEG_TO_RAD) / zoomLevel));
    }
    tileZoom = constrain(tileZoom, tileZoomMin, tileZoomMax);
    Serial.println("Configurazione caricata");
  }
}

Waypoint* findWaypoint(int id) {
  for(auto& wp : waypoints) {
    if(wp.id == id) {
      return &wp;
    }
  }
  return nullptr;
}

// ID sempre nuovo, anche dopo eliminazioni (prima si usava size()+1 e potevano esserci duplicati)
int nextWaypointId() {
  int maxId = 0;
  for(const auto& wp : waypoints) {
    if(wp.id > maxId) maxId = wp.id;
  }
  return maxId + 1;
}

double routeDistance(const Route& route) {
  double total = 0;
  for(size_t i = 1; i < route.waypointIds.size(); i++) {
    Waypoint* wp1 = findWaypoint(route.waypointIds[i - 1]);
    Waypoint* wp2 = findWaypoint(route.waypointIds[i]);
    if(wp1 && wp2) {
      total += TinyGPSPlus::distanceBetween(wp1->lat, wp1->lon, wp2->lat, wp2->lon);
    }
  }
  return total;
}

// Proiezione Web Mercator, la stessa delle tile: coordinate "mondo" in pixel al livello tileZoom
double lonToWorldX(double lon) {
  return (lon + 180.0) / 360.0 * TILE_SIZE * (double)(1UL << tileZoom);
}

double latToWorldY(double lat) {
  lat = constrain(lat, -85.0511, 85.0511);
  double r = lat * DEG_TO_RAD;
  return (1.0 - log(tan(r) + 1.0 / cos(r)) / PI) / 2.0 * TILE_SIZE * (double)(1UL << tileZoom);
}

int lonToPixel(double lon) {
  return MAP_W / 2 + (int)lround(lonToWorldX(lon) - (double)llround(lonToWorldX(mapCenterLon)));
}

int latToPixel(double lat) {
  return MAP_H / 2 + (int)lround(latToWorldY(lat) - (double)llround(latToWorldY(mapCenterLat)));
}

double pixelToLon(int x) {
  double wx = (double)llround(lonToWorldX(mapCenterLon)) + (x - MAP_W / 2);
  return wx / (TILE_SIZE * (double)(1UL << tileZoom)) * 360.0 - 180.0;
}

double pixelToLat(int y) {
  double wy = (double)llround(latToWorldY(mapCenterLat)) + (y - MAP_H / 2);
  double n = PI * (1.0 - 2.0 * wy / (TILE_SIZE * (double)(1UL << tileZoom)));
  return atan(sinh(n)) * RAD_TO_DEG;
}

void deleteWaypoint(int index) {
  if(index >= 0 && index < (int)waypoints.size()) {
    // Rimuovi da tutte le rotte
    for(auto& route : routes) {
      for(auto it = route.waypointIds.begin(); it != route.waypointIds.end(); ) {
        if(*it == waypoints[index].id) {
          it = route.waypointIds.erase(it);
        } else {
          ++it;
        }
      }
    }

    if(waypoints[index].id == activeWaypointId) activeWaypointId = -1;
    waypoints.erase(waypoints.begin() + index);

    saveWaypointsToSD();
    saveRoutesToSD();

    showMessage("Waypoint eliminato");
  }
}

void deleteRoute(int index) {
  if(index >= 0 && index < (int)routes.size()) {
    routes.erase(routes.begin() + index);
    if(currentRoute >= (int)routes.size()) {
      currentRoute = (int)routes.size() - 1;
    }
    saveRoutesToSD();
    showMessage("Rotta eliminata");
  }
}

// ============================================
// FUNZIONI DISPLAY MAPPA
// ============================================

void drawText(const String& s, int x, int y, uint8_t align, uint16_t fg, uint16_t bg) {
  // align: 0 = in alto a sinistra, 1 = in alto al centro, 2 = centrato
  int w = display.textWidth(s);
  int h = display.fontHeight();
  int cx = x, cy = y;
  if(align == 1 || align == 2) cx = x - w / 2;
  if(align == 2) cy = y - h / 2;
  display.setTextColor(fg, bg);
  display.setCursor(cx, cy);
  display.print(s);
}

void drawMap() {
  updateScale();
  if(tilesOk) {
    // Angolo in alto a sinistra dell'area visibile, in coordinate mondo
    long long viewX = llround(lonToWorldX(mapCenterLon)) - MAP_W / 2;
    long long viewY = llround(latToWorldY(mapCenterLat)) - MAP_H / 2;
    bool inside = viewX >= mapCacheWX && viewX + MAP_W <= mapCacheWX + TILE_CACHE_W &&
                  viewY >= mapCacheWY && viewY + MAP_H <= mapCacheWY + TILE_CACHE_H;
    if(!mapCacheValid || mapCacheZoom != tileZoom || !inside) {
      mapCacheZoom = tileZoom;
      mapCacheWX = viewX - TILE_CACHE_MARGIN;
      mapCacheWY = viewY - TILE_CACHE_MARGIN;
      renderMapCache();
      mapCacheValid = true;
    }
    // La parte che sporge oltre la mappa viene coperta da barra di stato e barra laterale, disegnate dopo
    mapCache.pushToSprite(&display, (int32_t)(mapCacheWX - viewX), (int32_t)(mapCacheWY - viewY));
  } else {
    display.fillRect(0, 0, MAP_W, MAP_H, WATER_COLOR);
  }
  drawGrid();
  drawCompassRose();
}

// Prepara le tile: cache in memoria e decoder JPEG
void initTiles() {
  if(!sdOk) return;
  File dir = SD.open("/tiles");
  if(!dir || !dir.isDirectory()) {
    Serial.println("Cartella /tiles non trovata");
    return;
  }
  Serial.print("Tile, livelli di zoom su SD:");
  int zMin = 99, zMax = -1;
  for(File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    int z = String(f.name()).toInt();
    if(f.isDirectory() && z >= TILE_ZOOM_MIN && z <= TILE_ZOOM_MAX) {
      Serial.printf(" %d", z);
      if(z < zMin) zMin = z;
      if(z > zMax) zMax = z;
    }
    f.close();
  }
  Serial.println();
  dir.close();
  if(zMax < 0) {
    Serial.println("Nessun livello di zoom in /tiles");
    return;
  }

  mapCache.setColorDepth(16);
  if(mapCache.createSprite(TILE_CACHE_W, TILE_CACHE_H) == nullptr) {
    Serial.println("Memoria insufficiente per le tile");
    return;
  }
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tileOutput);
  tileZoomMin = zMin;
  tileZoomMax = zMax;
  tileZoom = constrain(tileZoom, tileZoomMin, tileZoomMax);
  tilesOk = true;
}

// Chiamata dal decoder JPEG per ogni blocco di pixel: lo copia nella cache (il ritaglio ai bordi è automatico)
bool tileOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if(y >= TILE_CACHE_H) return false; // il resto della tile è sotto la cache
  mapCache.pushImage(x, y, w, h, bitmap);
  return true;
}

// Ridisegna lo sfondo della mappa leggendo dalla SD le tile che coprono l'area visibile
void renderMapCache() {
  mapCache.fillSprite(WATER_COLOR);

  long long originX = mapCacheWX;
  long long originY = mapCacheWY;
  long tilesPerSide = 1L << tileZoom;

  long tx0 = (long)floor((double)originX / TILE_SIZE);
  long tx1 = (long)floor((double)(originX + TILE_CACHE_W - 1) / TILE_SIZE);
  long ty0 = (long)floor((double)originY / TILE_SIZE);
  long ty1 = (long)floor((double)(originY + TILE_CACHE_H - 1) / TILE_SIZE);

  int drawn = 0, missing = 0;
  unsigned long start = millis();
  for(long ty = ty0; ty <= ty1; ty++) {
    if(ty < 0 || ty >= tilesPerSide) continue;
    for(long tx = tx0; tx <= tx1; tx++) {
      long wrappedX = ((tx % tilesPerSide) + tilesPerSide) % tilesPerSide;
      char path[48];
      snprintf(path, sizeof(path), "/tiles/%d/%ld/%ld.jpg", tileZoom, wrappedX, ty);
      if(!SD.exists(path)) {
        missing++;
        continue;
      }
      JRESULT res = TJpgDec.drawSdJpg((int32_t)(tx * TILE_SIZE - originX), (int32_t)(ty * TILE_SIZE - originY), path);
      // JDR_INTR = interrotta dal callback perché il resto è fuori schermo: non è un errore
      if(res == JDR_OK || res == JDR_INTR) drawn++;
      else Serial.printf("Tile %s: errore JPEG %d\n", path, (int)res);
      readGPS(); // non perdere dati GPS mentre si legge la SD
    }
  }
  Serial.printf("Tile zoom %d: %d disegnate, %d mancanti, %lu ms\n", tileZoom, drawn, missing, millis() - start);
}

void drawGrid() {
  display.setTextSize(1);
  display.setTextColor(BLACK, WATER_COLOR);

  double lonStep = calculateLonStep();
  double latStep = calculateLatStep();

  // Linee verticali (longitudine), tratteggiate
  double startLon = floor(pixelToLon(0) / lonStep) * lonStep;
  for(double lon = startLon; lon <= pixelToLon(MAP_W); lon += lonStep) {
    int x = lonToPixel(lon);
    if(x >= 0 && x < MAP_W) {
      for(int y = 0; y < MAP_H; y += 3) display.drawPixel(x, y, GRID_COLOR);
      display.setCursor(x + 2, MAP_H - 10);
      display.print(formatCoordinate(lon, false));
    }
  }

  // Linee orizzontali (latitudine), tratteggiate
  double startLat = floor(pixelToLat(MAP_H) / latStep) * latStep;
  for(double lat = startLat; lat <= pixelToLat(0); lat += latStep) {
    int y = latToPixel(lat);
    if(y >= 0 && y < MAP_H) {
      for(int x = 0; x < MAP_W; x += 3) display.drawPixel(x, y, GRID_COLOR);
      display.setCursor(2, y - 9);
      display.print(formatCoordinate(lat, true));
    }
  }
}

// Passo della griglia in gradi: il più piccolo tra i valori "tondi" che lascia almeno 90 pixel tra due linee
static double gridStep(double pixelsPerDegree) {
  static const double steps[] = {1.0 / 600, 1.0 / 300, 1.0 / 120, 1.0 / 60, 1.0 / 30, 1.0 / 12, 1.0 / 6, 1.0 / 3, 0.5, 1, 2, 5, 10, 20};
  for(double step : steps) {
    if(step * pixelsPerDegree >= 90.0) return step;
  }
  return 45.0;
}

double calculateLonStep() {
  return gridStep(TILE_SIZE * (double)(1UL << tileZoom) / 360.0);
}

double calculateLatStep() {
  // In Mercator un grado di latitudine è più lungo di uno di longitudine di un fattore 1/cos(lat)
  double c = cos(mapCenterLat * DEG_TO_RAD);
  if(c < 0.05) c = 0.05;
  return gridStep(TILE_SIZE * (double)(1UL << tileZoom) / 360.0 / c);
}

void drawCompassRose() {
  // Rosa dei venti piccola in alto a destra della mappa, per non coprire la barca al centro
  int centerX = MAP_W - 28;
  int centerY = 28;
  int radius = 14;

  display.drawCircle(centerX, centerY, radius, BLACK);

  display.setTextSize(1);
  drawText("N", centerX, centerY - radius - 6, 2, BLACK, WATER_COLOR);

  for(int i = 0; i < 360; i += 45) {
    float rad = radians(i);
    int x1 = centerX + radius * sin(rad);
    int y1 = centerY - radius * cos(rad);
    int x2 = centerX + (radius - (i % 90 == 0 ? 6 : 3)) * sin(rad);
    int y2 = centerY - (radius - (i % 90 == 0 ? 6 : 3)) * cos(rad);
    display.drawLine(x1, y1, x2, y2, BLACK);
  }
}

String formatCoordinate(double coord, bool isLat) {
  char buffer[16];
  char direction;

  if(isLat) {
    direction = (coord >= 0) ? 'N' : 'S';
  } else {
    direction = (coord >= 0) ? 'E' : 'W';
  }
  coord = fabs(coord);

  int degrees = (int)coord;
  double minutes = (coord - degrees) * 60;

  snprintf(buffer, sizeof(buffer), "%02d%c%04.1f'%c", degrees, DEG_CHAR, minutes, direction);
  return String(buffer);
}

void drawWaypoints() {
  for(const auto& wp : waypoints) {
    if(!wp.visible) continue;
    int x = lonToPixel(wp.lon);
    int y = latToPixel(wp.lat);

    if(x >= 0 && x < MAP_W && y >= 0 && y < MAP_H) {
      // Simbolo waypoint: cerchio con croce
      display.drawCircle(x, y, 5, WAYPOINT_COLOR);
      display.fillCircle(x, y, 2, WAYPOINT_COLOR);
      display.drawLine(x - 8, y, x + 8, y, WAYPOINT_COLOR);
      display.drawLine(x, y - 8, x, y + 8, WAYPOINT_COLOR);

      if(y > 10) {
        display.setTextSize(1);
        display.setTextColor(BLACK, WATER_COLOR);
        display.setCursor(x + 8, y - 10);
        display.print(wp.name);
      }
    }
  }
}

void drawRoutes() {
  if(currentRoute < 0 || currentRoute >= (int)routes.size()) return;
  const Route& route = routes[currentRoute];

  // Parte da 1: con una rotta vuota il ciclo non esegue (prima size()-1 andava in overflow)
  for(size_t i = 1; i < route.waypointIds.size(); i++) {
    Waypoint* wp1 = findWaypoint(route.waypointIds[i - 1]);
    Waypoint* wp2 = findWaypoint(route.waypointIds[i]);

    if(wp1 && wp2 && wp1->visible && wp2->visible) {
      int x1 = lonToPixel(wp1->lon);
      int y1 = latToPixel(wp1->lat);
      int x2 = lonToPixel(wp2->lon);
      int y2 = latToPixel(wp2->lat);

      // Linea spessa
      display.drawLine(x1, y1, x2, y2, ROUTE_COLOR);
      display.drawLine(x1 + 1, y1, x2 + 1, y2, ROUTE_COLOR);
      display.drawLine(x1, y1 + 1, x2, y2 + 1, ROUTE_COLOR);

      drawRouteArrow(x1, y1, x2, y2);
    }
  }

  drawActiveRouteInfo();
}

void drawRouteArrow(int x1, int y1, int x2, int y2) {
  int midX = (x1 + x2) / 2;
  int midY = (y1 + y2) / 2;

  float angle = atan2(y2 - y1, x2 - x1);

  int arrowSize = 8;
  int x3 = midX - arrowSize * cos(angle - PI / 6);
  int y3 = midY - arrowSize * sin(angle - PI / 6);
  int x4 = midX - arrowSize * cos(angle + PI / 6);
  int y4 = midY - arrowSize * sin(angle + PI / 6);

  display.fillTriangle(midX, midY, x3, y3, x4, y4, ROUTE_COLOR);
}

// Riquadro piccolo in basso a sinistra, solo se la rotta attiva ha dei waypoint
void drawActiveRouteInfo() {
  if(currentRoute < 0 || currentRoute >= (int)routes.size()) return;
  const Route& route = routes[currentRoute];
  if(route.waypointIds.empty()) return;

  const int w = 106, h = 24, x = 2, y = MAP_H - h - 2;
  display.fillRect(x, y, w, h, WHITE);
  display.drawRect(x, y, w, h, BLACK);

  display.setTextColor(BLACK, WHITE);
  display.setTextSize(1);
  display.setCursor(x + 4, y + 3);
  display.print(String(route.name).substring(0, 16));
  display.setCursor(x + 4, y + 13);
  display.printf("%.2fNM %dWP", routeDistance(route) / 1852.0, (int)route.waypointIds.size());
}

void drawBoatPosition() {
  if(gps.location.isValid()) {
    int x = lonToPixel(gps.location.lng());
    int y = latToPixel(gps.location.lat());
    lastBoatX = x;
    lastBoatY = y;

    if(x >= 0 && x < MAP_W && y >= 0 && y < MAP_H) {
      float course = gps.course.isValid() ? gps.course.deg() : 0;
      drawBoatSymbol(x, y, course, BOAT_COLOR);

      // Cerchio di accuratezza se HDOP disponibile
      if(gps.hdop.isValid()) {
        int accuracyRadius = (int)(gps.hdop.hdop() * 3); // Scala arbitraria
        if(accuracyRadius > 10 && accuracyRadius < 50) {
          display.drawCircle(x, y, accuracyRadius, TFT_RED);
        }
      }

    } else {
      display.setTextSize(1);
      drawText("BARCA FUORI MAPPA (c = centra)", MAP_W / 2, 52, 1, TFT_RED, WATER_COLOR);
    }
  } else {
    display.setTextSize(1);
    drawText(gpsBaud ? "IN ATTESA DEL FIX GPS" : "NESSUN DATO GPS", MAP_W / 2, 52, 1, TFT_RED, WATER_COLOR);
  }
}

void drawBoatSymbol(int x, int y, float course, uint16_t color) {
  float rad = radians(course);

  // Triangolo orientato sulla rotta (più grande che sulla CYD per leggibilità)
  int x1 = x + 12 * sin(rad);
  int y1 = y - 12 * cos(rad);
  int x2 = x + 8 * sin(rad + 2.5);
  int y2 = y - 8 * cos(rad + 2.5);
  int x3 = x + 8 * sin(rad - 2.5);
  int y3 = y - 8 * cos(rad - 2.5);

  display.fillTriangle(x1, y1, x2, y2, x3, y3, color);
  display.fillCircle(x, y, 2, WHITE);
}


// ============================================
// FUNZIONI INTERFACCIA UTENTE
// ============================================

// ============================================
// DESTINAZIONE ("VAI A") E PAGINA BUSSOLA
// ============================================

Waypoint* activeWaypoint() {
  return activeWaypointId < 0 ? nullptr : findWaypoint(activeWaypointId);
}

// Imposta il waypoint come destinazione; ripetuto sullo stesso waypoint la annulla
void setActiveWaypoint(int index) {
  if(index < 0 || index >= (int)waypoints.size()) {
    showMessage("Seleziona un waypoint");
    return;
  }
  if(waypoints[index].id == activeWaypointId) {
    activeWaypointId = -1;
    showMessage("Destinazione annullata");
  } else {
    activeWaypointId = waypoints[index].id;
    showMessage(String("Destinazione: ") + waypoints[index].name);
  }
}

// Prora della barca = rotta GPS. Da fermi il GPS dà valori casuali: si tiene l'ultima buona.
float boatHeading() {
  if(gps.course.isValid() && gps.speed.isValid() && gps.speed.knots() >= COURSE_MIN_KNOTS) {
    lastCourse = gps.course.deg();
    courseKnown = true;
  }
  return lastCourse;
}

// Linea dalla barca alla destinazione sulla mappa
void drawActiveWaypointLine() {
  Waypoint* dest = activeWaypoint();
  if(!dest || !gps.location.isValid()) return;
  int bx = lonToPixel(gps.location.lng()), by = latToPixel(gps.location.lat());
  int wx = lonToPixel(dest->lon), wy = latToPixel(dest->lat);
  display.drawLine(bx, by, wx, wy, TFT_RED);
  display.drawLine(bx + 1, by, wx + 1, wy, TFT_RED);
  display.drawLine(bx, by + 1, wx, wy + 1, TFT_RED);
  if(wx >= 0 && wx < MAP_W && wy >= 0 && wy < MAP_H) display.drawCircle(wx, wy, 9, TFT_RED);
}

// Bussola con la prora in alto: sfondo scuro, la rosa ruota e il triangolo rosso della prua resta fermo
void drawCompassPage() {
  const uint16_t BG = BLACK, RING = TFT_LIGHTGREY, DIM = 0x8410;
  const int cx = 112, cy = 124, R = 104;
  float hdg = boatHeading();
  bool moving = gps.speed.isValid() && gps.speed.knots() >= COURSE_MIN_KNOTS;

  display.fillSprite(BG);

  // Cerchio chiaro con tacche ogni 10 gradi, più lunghe ogni 30
  display.drawCircle(cx, cy, R, RING);
  display.drawCircle(cx, cy, R - 1, RING);
  for(int deg = 0; deg < 360; deg += 10) {
    float a = radians(deg - hdg);
    int len = (deg % 90 == 0) ? 14 : (deg % 30 == 0) ? 10 : 5;
    display.drawLine(cx + (R - len) * sin(a), cy - (R - len) * cos(a), cx + (R - 2) * sin(a), cy - (R - 2) * cos(a), RING);
  }

  // Punti cardinali (N rossa) e intercardinali
  const char* names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  for(int i = 0; i < 8; i++) {
    float a = radians(i * 45 - hdg);
    bool main = (i % 2 == 0);
    display.setTextSize(main ? 2 : 1);
    drawText(names[i], cx + (R - 26) * sin(a), cy - (R - 26) * cos(a), 2, i == 0 ? TFT_RED : (main ? WHITE : DIM), BG);
  }

  // Waypoint attivo: pallino verde sul cerchio, nella direzione rispetto alla prua
  Waypoint* dest = activeWaypoint();
  double brg = 0, nm = 0;
  bool haveDest = dest && gps.location.isValid();
  if(haveDest) {
    brg = TinyGPSPlus::courseTo(gps.location.lat(), gps.location.lng(), dest->lat, dest->lon);
    nm = TinyGPSPlus::distanceBetween(gps.location.lat(), gps.location.lng(), dest->lat, dest->lon) / 1852.0;
    float a = radians(brg - hdg);
    int wx = cx + (R - 1) * sin(a), wy = cy - (R - 1) * cos(a);
    display.fillCircle(wx, wy, 7, TFT_GREEN);
    display.drawCircle(wx, wy, 7, WHITE);
  }

  // Triangolo rosso fisso in alto: la prua
  display.fillTriangle(cx, cy - R + 12, cx - 9, cy - R - 8, cx + 9, cy - R - 8, TFT_RED);

  // Prora grande al centro
  char buf[24];
  if(courseKnown) snprintf(buf, sizeof(buf), "%03d%c", (int)lround(hdg) % 360, DEG_CHAR);
  else snprintf(buf, sizeof(buf), "---%c", DEG_CHAR);
  display.setTextSize(4);
  drawText(buf, cx + 2, cy - 4, 2, moving ? WHITE : DIM, BG);
  display.setTextSize(1);
  drawText(moving ? "da COG" : "da COG (ferma)", cx, cy + 22, 2, DIM, BG);

  // Dati piccoli a destra
  const int px = 226;
  int y = 8;
  auto row = [&](const char* name, const String& text, uint16_t color) {
    display.setTextSize(1);
    display.setTextColor(DIM, BG);
    display.setCursor(px, y);
    display.print(name);
    display.setTextColor(color, BG);
    display.setCursor(px, y + 10);
    display.print(text);
    y += 28;
  };

  if(gps.speed.isValid()) snprintf(buf, sizeof(buf), "%.1f kt", gps.speed.knots());
  else snprintf(buf, sizeof(buf), "--.- kt");
  row("VELOCITA'", buf, WHITE);

  if(haveDest) {
    row("DESTINAZIONE", String(dest->name).substring(0, 15), TFT_GREEN);
    snprintf(buf, sizeof(buf), "%03d%c", (int)lround(brg) % 360, DEG_CHAR);
    row("RILEVAMENTO", buf, TFT_GREEN);
    snprintf(buf, sizeof(buf), "%.*f NM", nm < 10 ? 2 : nm < 100 ? 1 : 0, nm);
    row("DISTANZA", buf, TFT_GREEN);

    // Di quanto accostare, e da che parte
    if(courseKnown) {
      int turn = (int)lround(brg - hdg);
      while(turn > 180) turn -= 360;
      while(turn < -180) turn += 360;
      if(abs(turn) < 3) snprintf(buf, sizeof(buf), "in rotta");
      else snprintf(buf, sizeof(buf), "%d%c a %s", abs(turn), DEG_CHAR, turn > 0 ? "dritta" : "sinistra");
      row("ACCOSTA", buf, WHITE);
    }
  } else if(dest) {
    row("DESTINAZIONE", "attesa fix GPS", DIM);
  } else {
    row("DESTINAZIONE", "nessuna", DIM);
    row("PER SCEGLIERLA", "w, poi g", DIM);
  }

  display.setTextSize(1);
  display.setTextColor(DIM, BG);
  display.setCursor(px, SCREEN_H - 10);
  display.print("tocco = mappa");
}

void drawMainUI() {
  // Barra laterale per menu
  display.fillRect(MAP_W, 0, SIDEBAR_W, SCREEN_H, TFT_DARKGREY);

  // M mappa, W waypoint, R rotte, S impostazioni, B bussola, + e - zoom
  const char* labels[SIDE_BTN_COUNT] = {"M", "W", "R", "S", "B", "+", "-"};
  int active = -1; // pagina corrente evidenziata
  switch(currentState) {
    case MAP_VIEW: active = 0; break;
    case WAYPOINT_MGR: active = 1; break;
    case ROUTE_MGR: active = 2; break;
    case MENU: active = 3; break;
    case COMPASS_VIEW: active = 4; break;
    default: break;
  }
  for(int i = 0; i < SIDE_BTN_COUNT; i++) {
    drawButton(MAP_W + 5, SIDE_BTN_Y0 + i * SIDE_BTN_PITCH, 30, SIDE_BTN_H, labels[i], i == active);
  }

}

void drawStatusBar() {
  int barY = MAP_H;
  display.fillRect(0, barY, MAP_W, STATUS_H, BLACK);

  display.setTextColor(WHITE, BLACK);
  display.setTextSize(1);

  // Ora GPS
  display.setCursor(4, barY + 6);
  if(gps.time.isValid()) {
    display.printf("%02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
  } else {
    display.print("--:--:--");
  }

  display.setCursor(60, barY + 6);
  display.printf("SAT:%d", gps.satellites.isValid() ? (int)gps.satellites.value() : 0);

  // Velocità e rotta della barca (la rotta resta l'ultima valida quando si è fermi)
  float hdg = boatHeading();
  display.setCursor(106, barY + 6);
  if(gps.speed.isValid()) display.printf("%.1fkt ", gps.speed.knots());
  else display.print("--.-kt ");
  if(courseKnown) display.printf("%03d%c", (int)lround(hdg) % 360, DEG_CHAR);
  else display.printf("---%c", DEG_CHAR);

  // Rilevamento e distanza della destinazione
  Waypoint* dest = activeWaypoint();
  if(dest && gps.location.isValid()) {
    double brg = TinyGPSPlus::courseTo(gps.location.lat(), gps.location.lng(), dest->lat, dest->lon);
    double nm = TinyGPSPlus::distanceBetween(gps.location.lat(), gps.location.lng(), dest->lat, dest->lon) / 1852.0;
    display.setTextColor(TFT_YELLOW, BLACK);
    display.setCursor(196, barY + 6);
    display.printf("%03d%c %.*fNM", (int)lround(brg) % 360, DEG_CHAR, nm < 100 ? 2 : 0, nm);
  }
}

void drawButton(int x, int y, int w, int h, const String& label, bool active) {
  uint16_t fill = active ? ACCENT : TFT_LIGHTGREY;
  uint16_t text = active ? WHITE : BLACK;

  display.fillRoundRect(x, y, w, h, 5, fill);
  display.drawRoundRect(x, y, w, h, 5, active ? TFT_CYAN : BLACK);

  display.setTextSize(1);
  drawText(label, x + w / 2, y + h / 2, 2, text, fill);
}

// ============================================
// FUNZIONI GESTIONE DATI
// ============================================

void addWaypointAtCurrentPosition(String name) {
  if(gps.location.isValid()) {
    Waypoint wp;
    wp.id = nextWaypointId();
    snprintf(wp.name, sizeof(wp.name), "%s%d", name.c_str(), wp.id);
    wp.lat = gps.location.lat();
    wp.lon = gps.location.lng();
    wp.visible = true;

    waypoints.push_back(wp);
    saveWaypointsToSD();

    showMessage(String("Waypoint '") + wp.name + "' aggiunto");
  } else {
    showMessage("Posizione GPS non valida!");
  }
}

void createRoute(String name) {
  Route route;
  route.id = routes.empty() ? 1 : routes.back().id + 1;
  snprintf(route.name, sizeof(route.name), "%s%d", name.c_str(), route.id);

  routes.push_back(route);
  currentRoute = routes.size() - 1;

  saveRoutesToSD();

  showMessage(String("Rotta '") + route.name + "' creata");
}

void calculateRouteInfo() {
  if(currentRoute < 0 || currentRoute >= (int)routes.size()) {
    showMessage("Seleziona una rotta");
    return;
  }
  if(routes[currentRoute].waypointIds.size() < 2) {
    showMessage("Rotta con meno di 2 waypoint");
    return;
  }
  showRouteInfo = true;
  requestRedraw();
}

void drawRouteInfoPopup() {
  if(currentRoute < 0 || currentRoute >= (int)routes.size()) return;
  Route& route = routes[currentRoute];

  double totalDistance = routeDistance(route);
  double totalTime = 0;
  if(gps.speed.isValid() && gps.speed.knots() > 0.5) {
    totalTime = totalDistance / (gps.speed.knots() * 0.514444); // secondi
  }

  int bx = SCREEN_W / 2 - 105, by = SCREEN_H / 2 - 60;
  display.fillRect(bx, by, 210, 120, WHITE);
  display.drawRect(bx, by, 210, 120, BLACK);
  display.drawRect(bx + 2, by + 2, 206, 116, BLACK);

  display.setTextSize(1);
  drawText("INFORMAZIONI ROTTA", SCREEN_W / 2, by + 8, 1, BLACK, WHITE);

  display.setTextColor(BLACK, WHITE);
  display.setCursor(bx + 10, by + 28);
  display.printf("Rotta: %s", route.name);
  display.setCursor(bx + 10, by + 43);
  display.printf("Waypoint: %d", (int)route.waypointIds.size());
  display.setCursor(bx + 10, by + 58);
  display.printf("Distanza: %.2f NM", totalDistance / 1852.0);
  display.setCursor(bx + 10, by + 73);
  if(totalTime > 0) {
    int hours = totalTime / 3600;
    int minutes = (totalTime - hours * 3600) / 60;
    display.printf("Tempo stimato: %d:%02d h", hours, minutes);
    display.setCursor(bx + 10, by + 88);
    display.printf("a %.1f kt (velocita' attuale)", gps.speed.knots());
  } else {
    display.print("Tempo stimato: --:-- h");
  }

  drawText("Tocca o premi un tasto", SCREEN_W / 2, by + 105, 1, BLACK, WHITE);
}

// ============================================
// FUNZIONI GESTIONE FILE SD
// (formato file identico alla versione CYD: la stessa scheda funziona su entrambi)
// ============================================

void saveWaypointsToSD() {
  if(!sdOk) return;
  File file = SD.open("/waypoints.txt", FILE_WRITE);
  if(file) {
    for(const auto& wp : waypoints) {
      file.printf("%d,%s,%.8f,%.8f,%d\n",
        wp.id, wp.name, wp.lat, wp.lon, wp.visible);
    }
    file.close();
    Serial.println("Waypoint salvati su SD");
  } else {
    Serial.println("Errore salvataggio waypoint");
  }
}

void loadWaypointsFromSD() {
  if(!sdOk || !SD.exists("/waypoints.txt")) {
    Serial.println("File waypoints.txt non trovato");
    return;
  }

  File file = SD.open("/waypoints.txt");
  if(file) {
    waypoints.clear();

    while(file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();
      if(line.length() > 0) {
        Waypoint wp;

        int comma1 = line.indexOf(',');
        int comma2 = line.indexOf(',', comma1 + 1);
        int comma3 = line.indexOf(',', comma2 + 1);
        int comma4 = line.indexOf(',', comma3 + 1);

        if(comma1 > 0 && comma2 > 0 && comma3 > 0 && comma4 > 0) {
          wp.id = line.substring(0, comma1).toInt();
          line.substring(comma1 + 1, comma2).toCharArray(wp.name, 20);
          wp.lat = line.substring(comma2 + 1, comma3).toDouble();
          wp.lon = line.substring(comma3 + 1, comma4).toDouble();
          wp.visible = line.substring(comma4 + 1).toInt();

          waypoints.push_back(wp);
        }
      }
    }

    file.close();
    Serial.printf("Caricati %d waypoint\n", (int)waypoints.size());
  }
}

void saveRoutesToSD() {
  if(!sdOk) return;
  File file = SD.open("/routes.txt", FILE_WRITE);
  if(file) {
    for(const auto& route : routes) {
      file.printf("[ROUTE]\n");
      file.printf("ID=%d\n", route.id);
      file.printf("NAME=%s\n", route.name);
      file.printf("WAYPOINTS=");

      for(size_t i = 0; i < route.waypointIds.size(); i++) {
        if(i > 0) file.printf(",");
        file.printf("%d", route.waypointIds[i]);
      }
      file.printf("\n\n");
    }
    file.close();
    Serial.println("Rotte salvate su SD");
  } else {
    Serial.println("Errore salvataggio rotte");
  }
}

void loadRoutesFromSD() {
  if(!sdOk || !SD.exists("/routes.txt")) {
    Serial.println("File routes.txt non trovato");
    return;
  }

  File file = SD.open("/routes.txt");
  if(file) {
    routes.clear();
    Route currentRouteData;
    bool inRoute = false;

    while(file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();

      if(line.startsWith("[ROUTE]")) {
        if(inRoute) {
          routes.push_back(currentRouteData);
        }
        currentRouteData = Route();
        inRoute = true;
      } else if(line.startsWith("ID=")) {
        currentRouteData.id = line.substring(3).toInt();
      } else if(line.startsWith("NAME=")) {
        line.substring(5).toCharArray(currentRouteData.name, 20);
      } else if(line.startsWith("WAYPOINTS=")) {
        String wpList = line.substring(10);
        int start = 0;
        int commaPos;

        while((commaPos = wpList.indexOf(',', start)) != -1) {
          currentRouteData.waypointIds.push_back(wpList.substring(start, commaPos).toInt());
          start = commaPos + 1;
        }

        if(start < (int)wpList.length()) {
          currentRouteData.waypointIds.push_back(wpList.substring(start).toInt());
        }
      } else if(line.length() == 0 && inRoute) {
        routes.push_back(currentRouteData);
        inRoute = false;
      }
    }

    if(inRoute) {
      routes.push_back(currentRouteData);
    }

    file.close();
    Serial.printf("Caricate %d rotte\n", (int)routes.size());
  }
}
