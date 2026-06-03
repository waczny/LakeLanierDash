/*
 * ════════════════════════════════════════════════════════════
 *  Lake Lanier Infoscreen  v3
 *  Hardware : ESP32-2432S028R (CYD)
 *             ILI9341  320×240  2.8"  + XPT2046 touch
 *  IDE      : Arduino IDE ≥ 2.x  +  ESP32 board package
 *
 *  Libraries (Library Manager):
 *    • TFT_eSPI          (Bodmer)            – display driver
 *    • XPT2046_Touchscreen (Paul Stoffregen) – touch
 *    • ArduinoJson       (Benoit Blanchon)   – JSON parsing
 *    • WiFi / HTTPClient (built-in ESP32)
 *
 *  TFT_eSPI  User_Setup.h — paste these defines:
 *    #define ILI9341_DRIVER
 *    #define TFT_MOSI  13
 *    #define TFT_SCLK  14
 *    #define TFT_CS    15
 *    #define TFT_DC     2
 *    #define TFT_RST   -1
 *    #define TFT_BL    21
 *    #define LOAD_GLCD
 *    #define LOAD_FONT2
 *    #define LOAD_FONT4
 *    #define LOAD_FONT6
 *    #define SPI_FREQUENCY  40000000
 *
 *  Touch CS pin:  GPIO 33  (CYD standard)
 *  Touch SPI shares the same bus as TFT (HSPI).
 *
 *  Screens:
 *    MAIN  — data display with surfer every 5 min
 *    WIFI  — scan + select + connect (touch-driven)
 *
 *  Data sources (free, no API key):
 *    Water level / temp : USGS Instantaneous Values API
 *    Air temp / wind    : Open-Meteo forecast API
 *    3-day trend        : USGS period-of-record comparison
 * ════════════════════════════════════════════════════════════
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>

// ── Touch ────────────────────────────────────────────────────
// CYD touch is on VSPI (separate bus from TFT which uses HSPI)
#define TOUCH_CS    33
#define TOUCH_IRQ   36
#define TOUCH_CLK   25
#define TOUCH_MOSI  32
#define TOUCH_MISO  39

SPIClass touchVSPI(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

// Calibration from measured corner values
// Top-left=(140,200)  Top-right=(3710,240)
// Bot-left=(150,3875) Bot-right=(3680,3850)
#define TS_MINX   140
#define TS_MAXX  3710
#define TS_MINY   200
#define TS_MAXY  3875

// ── Display ──────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

// Surfer sprite buffer (80×96 pixels, 16-bit)
#define SPR_W   80
#define SPR_H   96
#define WAVE_Y  130   // sprite top Y on main screen
static uint16_t surferBuf[SPR_W * SPR_H];
#define TRANSP  0x0020  // transparent key colour

// ── Timings ──────────────────────────────────────────────────
const unsigned long REFRESH_MS = 10UL * 60UL * 1000UL;
const unsigned long SURFER_MS  =  1UL * 60UL * 1000UL;

// ── GitHub OTA update settings ──────────────────────────────
// Upload this sketch by USB once. Future versions can update over WiFi from GitHub.
// GitHub setup:
//   firmware/version.txt must contain only the latest version, e.g. 1.0.1
//   Release asset must be named LakeLanierInfoscreen.ino.bin
//   Change FW_VERSION before compiling each new release.
#define FW_VERSION       "1.0.6"
#define GITHUB_OWNER     "waczny"
#define GITHUB_REPO      "LakeLanierDash"
#define FW_BIN_NAME      "LakeLanierInfoscreen.ino.bin"

const unsigned long OTA_CHECK_MS = 6UL * 60UL * 60UL * 1000UL; // check every 6 hours
unsigned long lastOtaCheck = 0;

const char* FW_VERSION_URL =
  "https://raw.githubusercontent.com/" GITHUB_OWNER "/" GITHUB_REPO "/main/firmware/version.txt";
const char* FW_BIN_URL =
  "https://github.com/" GITHUB_OWNER "/" GITHUB_REPO "/releases/latest/download/" FW_BIN_NAME;

// ── URLs ─────────────────────────────────────────────────────
// Water level / reservoir elevation (ft NAVD88)
const char* URL_LEVEL =
  "https://waterservices.usgs.gov/nwis/iv/"
  "?format=json&sites=02334400&parameterCd=00062&siteStatus=all";
// Water level fallback page (simple HTML with current MSL level)
const char* URL_LEVEL_FALLBACK =
  "https://www.discoverlanier.com/Level/";
// Lake temp from LakeMonster page. This is scraped as plain HTML text.
const char* URL_WTEMP =
  "https://lakemonster.com/lake/Georgia/Lake-Lanier-234";
// 3-day level trend
const char* URL_TREND =
  "https://waterservices.usgs.gov/nwis/iv/"
  "?format=json&sites=02334400&parameterCd=00062"
  "&period=P3D&siteStatus=all";
// Air temp + wind
const char* URL_WEATHER =
  "https://api.open-meteo.com/v1/forecast"
  "?latitude=34.1603&longitude=-84.0742"
  "&current=temperature_2m,wind_speed_10m,wind_direction_10m"
  "&temperature_unit=fahrenheit&wind_speed_unit=mph"
  "&timezone=America%2FNew_York";

// ── Palette (RGB565) ─────────────────────────────────────────
#define C_BG      0x0003  // near-black navy
#define C_CARD    0x0108  // dark card
#define C_BORDER  0x1249  // subtle border
#define C_ACCENT  0x0637  // cyan  #00CCFF
#define C_GREEN   0x0729  // #00DD66
#define C_AMBER   0xFD20  // amber
#define C_RED     0xF984  // #FF3333
#define C_WHITE   0xFFFF
#define C_DIM     0x8C92  // #889AA0
#define C_VDIM    0x3224  // #334455
#define C_HEADER  0x0186  // header bg
// Surfer
#define C_WAVE1   0x01D5  // #003A6E
#define C_WAVE2   0x0132  // #002650
#define C_FOAM    0xC7FF  // #C8F0FF
#define C_SPLASH  0x2E76  // #55CCEE
#define C_SKIN    0xF482  // #F0A060
#define C_BOARD   0xF80F  // hot pink
#define C_SUIT    0x0009  // #00082A
#define C_HAIR    0x38C0  // #3A1A00
#define C_STRIPE  0xFFE0  // yellow

// ── Data ─────────────────────────────────────────────────────
struct LakeData {
  float levelFt    = 0;
  float fullPool   = 1071.0f;
  float trendFt    = 0;      // change over last 3 days
  float airTempF   = 0;
  float windMph    = 0;
  int   windDeg    = 0;
  float waterTempF = 0;
  bool  levelOk    = false;
  bool  weatherOk  = false;
  bool  wTempOk    = false;
  bool  trendOk    = false;
  String updated   = "--:--";
};
LakeData lk;
unsigned long lastFetch  = 0;
unsigned long lastSurfer = 0;

// API debug status shown when data cannot be loaded
int httpLevelCode = 0;
int httpTrendCode = 0;
int httpWeatherCode = 0;
int httpWTempCode = 0;
String apiDebug = "API: not loaded";

// ── Screen enum ───────────────────────────────────────────────
enum Screen { SCR_MAIN, SCR_WIFI, SCR_KB };
Screen curScreen = SCR_MAIN;

// ── WiFi scan state ───────────────────────────────────────────
struct WifiNet { String ssid; int32_t rssi; bool secured; };
#define MAX_NETS 5
WifiNet nets[MAX_NETS];
int netCount     = 0;
int netSelected  = 0;

// ── Password keyboard state ───────────────────────────────────
String  pwdBuffer    = "";          // typed password so far
bool    pwdShift     = false;       // shift/caps active
bool    pwdShowPwd   = false;       // toggle show/hide

// ─────────────────────────────────────────────────────────────
//  HELPERS
// ─────────────────────────────────────────────────────────────
const char* windName(int deg){
  const char* d[]={"N","NNE","NE","ENE","E","ESE","SE","SSE",
                   "S","SSW","SW","WSW","W","WNW","NW","NNW"};
  return d[((deg+11)/22)%16];
}
String nowTime(){
  struct tm t; if(getLocalTime(&t,100)){char b[6];strftime(b,6,"%H:%M",&t);return b;}
  unsigned long s=millis()/1000; char b[6];
  snprintf(b,6,"%02lu:%02lu",(s/3600)%24,(s/60)%60); return b;
}
uint16_t levelColour(float diff){
  return diff>=-1?C_GREEN:diff>=-3?C_ACCENT:diff>=-6?C_AMBER:C_RED;
}

// Map touch raw → screen pixel using measured calibration
bool getTouchPoint(int &tx, int &ty){
  if(!touch.touched()) return false;
  TS_Point p = touch.getPoint();
  if(p.z < 300) return false;   // ignore light/noisy contacts
  tx = map(p.x, TS_MINX, TS_MAXX, 0, 320);
  ty = map(p.y, TS_MINY, TS_MAXY, 0, 240);
  tx = constrain(tx, 0, 319);
  ty = constrain(ty, 0, 239);
  return true;
}

// ─────────────────────────────────────────────────────────────
//  DRAW TREND TRIANGLE
//  cx,cy = centre, size = half-height
// ─────────────────────────────────────────────────────────────
void drawTrend(int cx, int cy, int sz, float trend){
  uint16_t col = trend>0.02f?C_GREEN:trend<-0.02f?C_RED:C_AMBER;
  if(trend > 0.02f){
    // Up arrow
    tft.fillTriangle(cx, cy-sz, cx-sz, cy+sz, cx+sz, cy+sz, col);
  } else if(trend < -0.02f){
    // Down arrow
    tft.fillTriangle(cx, cy+sz, cx-sz, cy-sz, cx+sz, cy-sz, col);
  } else {
    // Flat diamond
    tft.fillTriangle(cx, cy-sz, cx+sz, cy, cx, cy+sz, col);
    tft.fillTriangle(cx, cy-sz, cx-sz, cy, cx, cy+sz, col);
  }
  // Numeric label below triangle
  char buf[10];
  snprintf(buf,10,"%+.2f",trend);
  tft.setTextColor(col, C_CARD);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(buf, cx, cy+sz+8, 1);
}

// ─────────────────────────────────────────────────────────────
//  DRAW MAIN UI
// ─────────────────────────────────────────────────────────────
void drawMainUI(){
  // Modern compact dashboard with metric cards ABOVE the water level.
  tft.fillScreen(C_BG);

  float diff = lk.levelFt - lk.fullPool;
  uint16_t diffCol = levelColour(diff);
  uint16_t trendCol = lk.trendFt > 0.02f ? C_ACCENT : lk.trendFt < -0.02f ? C_RED : C_AMBER;

  // ── Header ─────────────────────────────────────────────────
  tft.fillRect(0, 0, 320, 26, 0x0002);
  tft.drawLine(0, 26, 320, 26, 0x39E7);

  // Small wave icon
  for(int r=0; r<3; r++){
    int y = 7 + r*5;
    tft.drawLine(10, y+2, 15, y, C_DIM);
    tft.drawLine(15, y, 20, y+2, C_DIM);
    tft.drawLine(20, y+2, 25, y, C_DIM);
    tft.drawLine(25, y, 30, y+2, C_DIM);
  }

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_WHITE, 0x0002);
  tft.drawString("LAKE LANIER", 38, 13, 2);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(C_DIM, 0x0002);
  tft.drawString(lk.updated, 312, 13, 2);

  // Wifi status indicator: simple wifi icon in upper right if not connected
  if (WiFi.status() != WL_CONNECTED) {
    int wx = 300, wy = 10;
    tft.drawCircle(wx, wy + 8, 6, C_RED);
    tft.drawCircle(wx, wy + 8, 4, C_RED);
    tft.drawLine(wx, wy + 14, wx, wy + 10, C_RED);
    tft.drawLine(wx-2, wy + 10, wx+2, wy + 10, C_RED);
  }

  // Add OTA update indicator in bottom right if update available
  static bool otaUpdateAvailable = false;
  static String otaLatestVersion = "";
  if(otaUpdateAvailable) {
    tft.setTextDatum(BR_DATUM);
    tft.setTextColor(C_ACCENT, C_BG);
    String s = "OTA: v" + otaLatestVersion;
    tft.drawString(s.c_str(), 319, 239, 2);
  }

  // ── Metric cards now ABOVE the water level ─────────────────
  const int CY=32, CW=98, CH=54, CG=7;
  const int CX1=7, CX2=CX1+CW+CG, CX3=CX2+CW+CG;

  auto cardBase = [&](int x){
    tft.fillRoundRect(x, CY, CW, CH, 9, C_CARD);
    tft.drawRoundRect(x, CY, CW, CH, 9, 0x6B4D);
    tft.drawRoundRect(x+1, CY+1, CW-2, CH-2, 8, 0x1908);
  };

  auto cardTitle = [&](int x, const char* title){
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(C_DIM, C_CARD);
    tft.drawString(title, x+CW/2, CY+6, 1);
  };

  auto thermoIconSmall = [&](int x, int y, uint16_t col){
    tft.drawRoundRect(x, y, 8, 20, 4, col);
    tft.fillCircle(x+4, y+22, 6, col);
    tft.fillRect(x+3, y+8, 3, 14, col);
    tft.fillCircle(x+4, y+22, 2, C_CARD);
  };

  auto windIconSmall = [&](int x, int y, uint16_t col){
    tft.drawFastHLine(x, y, 18, col);
    tft.drawFastHLine(x, y+7, 23, col);
    tft.drawFastHLine(x, y+14, 17, col);
    tft.drawCircle(x+22, y-2, 4, col);
    tft.drawCircle(x+27, y+7, 4, col);
  };

  auto dropIconSmall = [&](int x, int y, uint16_t col){
    tft.drawLine(x+9, y,   x+2,  y+14, col);
    tft.drawLine(x+9, y,   x+16, y+14, col);
    tft.drawCircle(x+9, y+16, 8, col);
    tft.fillRect(x+1, y+10, 17, 8, C_CARD);
    tft.drawCircle(x+9, y+16, 8, col);
  };

  auto tempCard = [&](int x, const char* title, const char* value, uint16_t col, bool lake){
    cardBase(x);
    cardTitle(x, title);
    if(lake) dropIconSmall(x+11, CY+25, col);
    else     thermoIconSmall(x+13, CY+23, col);

    String v = String(value);
    v.replace("\xB0", "");
    v.replace("°", "");
    tft.setTextColor(C_WHITE, C_CARD);
    tft.setTextDatum(ML_DATUM);

    if(v == "--"){
      tft.drawString("--", x+47, CY+34, 4);
    } else {
      int numX = (v.length() >= 3) ? x+34 : x+43;
      tft.drawString(v.c_str(), numX, CY+34, 4);
      int degX = (v.length() >= 3) ? x+78 : x+76;
      tft.drawString("°", degX, CY+27, 2);
    }
  };

  auto windCard = [&](int x, const char* value, const char* dir){
    cardBase(x);
    cardTitle(x, "WIND");
    windIconSmall(x+9, CY+33, C_ACCENT);

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_WHITE, C_CARD);
    String v = String(value);
    int valueX = (v.length() >= 2) ? x+39 : x+45;
    tft.drawString(v.c_str(), valueX, CY+34, 4);

    tft.setTextColor(C_DIM, C_CARD);
    tft.drawString("mph", x+72, CY+36, 1);
    String d = String(dir);
    if(d.length() > 3) d = d.substring(0,3);
    tft.drawString(d.c_str(), x+72, CY+48, 1);
  };

  char airV[10] = "--";
  char waterV[10] = "--";
  char windV[10] = "--";
  char windD[8] = "";
  if(lk.weatherOk){
    snprintf(airV, sizeof(airV), "%.0f", lk.airTempF);
    snprintf(windV, sizeof(windV), "%.0f", lk.windMph);
    snprintf(windD, sizeof(windD), "%s", windName(lk.windDeg));
  }
  if(lk.wTempOk) snprintf(waterV, sizeof(waterV), "%.0f", lk.waterTempF);

  tempCard(CX1, "AIR TEMP", airV, C_AMBER, false);
  windCard(CX2, windV, windD);
  tempCard(CX3, "LAKE TEMP", waterV, C_ACCENT, true);

  // ── Hero water level below the cards ───────────────────────
  if(lk.levelOk){
    char lb[16];
    snprintf(lb, sizeof(lb), "%.2f", lk.levelFt);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(0x18C3, C_BG);   // soft shadow
    tft.drawString(lb, 159, 121, 6);
    tft.setTextColor(C_WHITE, C_BG);
    tft.drawString(lb, 157, 119, 6);

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString("ft", 272, 129, 4);
  } else {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_AMBER, C_BG);
    tft.drawString("data unavailable", 160, 119, 4);
  }

  // ── Rounded status pills under water level ─────────────────
  auto drawArrow = [&](int cx, int cy, bool up, uint16_t col){
    if(up){
      tft.fillTriangle(cx, cy-7, cx-7, cy+3, cx+7, cy+3, col);
      tft.fillRect(cx-2, cy+1, 5, 10, col);
    } else {
      tft.fillTriangle(cx, cy+7, cx-7, cy-3, cx+7, cy-3, col);
      tft.fillRect(cx-2, cy-11, 5, 10, col);
    }
  };

  auto pill = [&](int x, int y, int w, const char* text, uint16_t col, bool up){
    uint16_t bg = (col == C_ACCENT || col == C_GREEN) ? 0x0127 : 0x2100;
    tft.fillRoundRect(x, y, w, 29, 14, bg);
    tft.drawRoundRect(x, y, w, 29, 14, col);
    tft.drawRoundRect(x+1, y+1, w-2, 27, 13, bg == 0x0127 ? 0x0451 : 0x4200);
    drawArrow(x+17, y+14, up, col);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(col, bg);
    tft.drawString(text, x+34, y+14, 1);
  };

  if(lk.levelOk){
    char db[36];
    snprintf(db, sizeof(db), diff>=0 ? "+%.2f ft above full" : "%.2f ft below full", fabs(diff));
    pill(7, 151, 151, db, diffCol, diff >= 0);
  } else {
    pill(7, 151, 151, "level unavailable", C_AMBER, false);
  }

  if(lk.trendOk){
    char tb[36];
    snprintf(tb, sizeof(tb), "%+.2f ft / 3d", lk.trendFt);
    pill(162, 151, 151, tb, trendCol, lk.trendFt >= 0);
  } else {
    // Avoid showing "pending" forever; this means the USGS trend call did not return usable points.
    pill(162, 151, 151, "3d trend --", C_ACCENT, true);
  }

  // ── Full pool and progress ─────────────────────────────────
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("FULL POOL 1071.0 ft", 11, 194, 2);

  float pct = lk.levelOk ? constrain((lk.levelFt - 1050.0f) / 21.0f, 0, 1) : 0.0f;
  char pb[18];
  snprintf(pb, sizeof(pb), "%.1f%% full", pct * 100.0f);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.drawString(lk.levelOk ? pb : "--", 309, 194, 2);

  tft.fillRoundRect(11, 212, 298, 13, 6, 0x31A6);
  tft.drawRoundRect(11, 212, 298, 13, 6, 0x6B4D);
  if(lk.levelOk){
    int fillW = (int)(298 * pct);
    fillW = constrain(fillW, 0, 298);
    tft.fillRoundRect(11, 212, fillW, 13, 6, C_ACCENT);
    tft.drawFastHLine(17, 214, max(0, fillW-12), 0xAFFF);
  }

  // Debug stays tiny and only appears when something is missing.
  if(!lk.levelOk || !lk.wTempOk || !lk.trendOk){
    tft.setTextDatum(BR_DATUM);
    tft.setTextColor(C_VDIM, C_BG);
    tft.drawString(apiDebug.c_str(), 319, 239, 1);
  }
}

// ─────────────────────────────────────────────────────────────
//  WIFI SETUP SCREEN
// ─────────────────────────────────────────────────────────────
void drawWifiScreen(){
  tft.fillScreen(C_BG);

  // Header
  tft.fillRect(0,0,320,26, C_HEADER);
  tft.drawLine(0,26,320,26,0x225F);
  tft.setTextColor(0x739F,C_HEADER);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("WiFi SETUP", 160,13,4);

  // Hint line
  tft.setTextColor(C_DIM,C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Tap network  then  CONNECT", 160,33,1);

  // Layout: 5 rows max, each 28px tall, starting y=40
  // 5 rows = 140px  →  ends at y=180
  // Connect button at y=188, height=24  → ends at y=212  (safe margin to 240)
  const int ITH=28, LT=40, SHOW=min(netCount,5);

  for(int i=0;i<SHOW;i++){
    int iy=LT+i*ITH;
    bool active=(i==netSelected);
    uint16_t cardCol = active ? 0x0214 : C_CARD;
    uint16_t brdCol  = active ? 0x739F : C_BORDER;
    tft.fillRoundRect(4,iy,312,ITH-2,3,cardCol);
    tft.drawRoundRect(4,iy,312,ITH-2,3,brdCol);

    // Lock icon (asterisk = secured)
    tft.setTextColor(nets[i].secured?0x739F:C_VDIM, cardCol);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(nets[i].secured?"*":" ", 10, iy+7, 1);

    // SSID — truncate if too long
    tft.setTextColor(active?C_WHITE:C_DIM, cardCol);
    tft.setTextDatum(ML_DATUM);
    String ssid=nets[i].ssid;
    if(ssid.length()>22) ssid=ssid.substring(0,20)+"..";
    tft.drawString(ssid.c_str(), 20, iy+7, 2);

    // Signal bars (right side)
    int bars=(nets[i].rssi>-50)?4:(nets[i].rssi>-65)?3:(nets[i].rssi>-75)?2:1;
    for(int b=0;b<4;b++){
      int bh=3+b*3, bx=284+b*8, by=iy+ITH-4-bh;
      tft.fillRect(bx,by,6,bh, b<bars?(active?0x739F:C_ACCENT):C_VDIM);
    }
  }

  // Show "+N more" if list was truncated
  if(netCount>5){
    char more[20]; snprintf(more,20,"+ %d more networks",netCount-5);
    tft.setTextColor(C_VDIM,C_BG); tft.setTextDatum(MC_DATUM);
    tft.drawString(more, 160, LT+SHOW*ITH+4, 1);
  }

  // CONNECT button — always at fixed position with clear space
  const int BTN_Y=188;
  tft.fillRoundRect(50,BTN_Y,220,26,5,0x0214);
  tft.drawRoundRect(50,BTN_Y,220,26,5,0x739F);
  tft.setTextColor(0x739F,0x0214);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("CONNECT", 160, BTN_Y+13, 2);

  // Back hint
  tft.setTextColor(C_VDIM,C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("tap header to go back", 160, 224, 1);
}

void scanNetworks(){
  // Show scanning message
  tft.fillScreen(C_BG);
  tft.setTextColor(C_ACCENT,C_BG); tft.setTextDatum(MC_DATUM);
  tft.drawString("Scanning WiFi...", 160,120,4);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true); delay(100);
  int n=WiFi.scanNetworks();
  netCount=min(n, MAX_NETS);
  for(int i=0;i<netCount;i++){
    nets[i].ssid    = WiFi.SSID(i);
    nets[i].rssi    = WiFi.RSSI(i);
    nets[i].secured = (WiFi.encryptionType(i)!=WIFI_AUTH_OPEN);
  }
  netSelected=0;
}

// ─────────────────────────────────────────────────────────────
//  ON-SCREEN QWERTY KEYBOARD
//
//  Layout (5 rows), each key ~26×22px
//  Row 0 (y 70):  1 2 3 4 5 6 7 8 9 0
//                  with SHF: ! @ # $ % ^ & * ( )
//  Row 1 (y 98):  q w e r t y u i o p
//  Row 2 (y 126): a s d f g h j k l
//  Row 3 (y 154): SHF z x c v b n m DEL
//  Row 4 (y 184): SHOW  [space]  OK
//
//  Password field at top (y 30-58)
// ─────────────────────────────────────────────────────────────

// Key layout
const char* ROW_NUM = "1234567890"; // 10 keys
const char* ROW_SYM = "!@#$%^&*()"; // shown when SHF is active on number row
const char* ROW0 = "qwertyuiop";    // 10 keys
const char* ROW1 = "asdfghjkl";     //  9 keys
const char* ROW2 = "zxcvbnm";       //  7 keys  (+SHF left, DEL right)

// Returns pixel x-centre for key index in a row of n keys
// Row is centred in 320px with 28px per key
int keyX(int idx, int n){ return (320 - n*28)/2 + idx*28 + 14; }
int keyY(int row){
  const int ys[]={70,98,126,154,184}; return ys[row];
}

void drawKey(int cx, int cy, const char* label, uint16_t bg, uint16_t fg){
  tft.fillRoundRect(cx-12, cy-11, 24, 22, 3, bg);
  tft.drawRoundRect(cx-12, cy-11, 24, 22, 3, C_BORDER);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, cx, cy, 1);
}

void drawWideKey(int x, int y, int w, const char* label, uint16_t bg, uint16_t fg){
  tft.fillRoundRect(x, y-11, w, 22, 3, bg);
  tft.drawRoundRect(x, y-11, w, 22, 3, C_BORDER);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x+w/2, y, 1);
}

void drawKeyboard(){
  tft.fillScreen(C_BG);

  // Header
  tft.fillRect(0,0,320,26,C_HEADER);
  tft.drawLine(0,26,320,26,0x225F);
  tft.setTextColor(0x739F,C_HEADER); tft.setTextDatum(ML_DATUM);
  tft.drawString("WiFi SETUP", 8,13,2);
  // SSID in header
  String s = nets[netSelected].ssid;
  if(s.length()>18) s=s.substring(0,16)+"..";
  tft.setTextColor(C_DIM,C_HEADER); tft.setTextDatum(MR_DATUM);
  tft.drawString(s.c_str(), 314,13,1);

  // Password field
  tft.fillRoundRect(4,30,312,28,4,C_CARD);
  tft.drawRoundRect(4,30,312,28,4,C_ACCENT);
  tft.setTextColor(C_DIM,C_CARD); tft.setTextDatum(ML_DATUM);
  tft.drawString("PWD:", 10,44,1);
  // Show typed chars (masked or plain)
  String disp="";
  if(pwdShowPwd){ disp=pwdBuffer; }
  else { for(int i=0;i<(int)pwdBuffer.length();i++) disp+="*"; }
  if(disp.length()==0){ tft.setTextColor(C_VDIM,C_CARD); disp="(tap keys below)"; }
  else tft.setTextColor(C_WHITE,C_CARD);
  // Truncate display to last 22 chars so it fits
  if(disp.length()>22) disp=".."+(disp.substring(disp.length()-20));
  tft.drawString(disp.c_str(), 46,44,2);

  // Number / symbol row
  int nn=strlen(ROW_NUM);
  for(int i=0;i<nn;i++){
    char k[3]={pwdShift?ROW_SYM[i]:ROW_NUM[i],0};
    drawKey(keyX(i,nn), keyY(0), k, C_CARD, C_WHITE);
  }

  // Row 1: qwerty
  int n0=strlen(ROW0);
  for(int i=0;i<n0;i++){
    char k[3]={pwdShift?(char)toupper(ROW0[i]):ROW0[i],0};
    drawKey(keyX(i,n0), keyY(1), k, C_CARD, C_WHITE);
  }
  // Row 2: asdf
  int n1=strlen(ROW1);
  for(int i=0;i<n1;i++){
    char k[3]={pwdShift?(char)toupper(ROW1[i]):ROW1[i],0};
    drawKey(keyX(i,n1), keyY(2), k, C_CARD, C_WHITE);
  }
  // Row 3: SHF | z..m | DEL
  uint16_t shfCol = pwdShift ? C_ACCENT : C_CARD;
  drawWideKey(4,      keyY(3), 30, "SHF", shfCol, C_WHITE);
  int n2=strlen(ROW2);
  int row2startX = 38;
  for(int i=0;i<n2;i++){
    int cx = row2startX + i*28 + 14;
    char k[3]={pwdShift?(char)toupper(ROW2[i]):ROW2[i],0};
    drawKey(cx, keyY(3), k, C_CARD, C_WHITE);
  }
  drawWideKey(286,    keyY(3), 30, "DEL", 0x1800, C_RED);

  // Row 4: SHOW | SPACE | OK
  drawWideKey(4,   keyY(4), 50, pwdShowPwd?"HIDE":"SHOW", C_CARD, C_DIM);
  drawWideKey(58,  keyY(4),204, "SPACE", C_CARD, C_WHITE);
  drawWideKey(266, keyY(4), 50, "OK",    0x0214, 0x739F);

  // Hint at very bottom
  tft.setTextColor(C_VDIM,C_BG); tft.setTextDatum(MC_DATUM);
  tft.drawString("SHF = caps/symbols", 160,228,1);
}

// Hit-test keyboard — returns the character/command tapped, or 0
// cmd output: 0=nothing, 1=regular char in *ch, 2=DEL, 3=OK, 4=SPACE, 5=SHIFT, 6=SHOW
int kbHitTest(int tx, int ty, char &ch){
  // Number / symbol row
  int nn=strlen(ROW_NUM);
  if(ty>=keyY(0)-13 && ty<=keyY(0)+12){
    for(int i=0;i<nn;i++){
      int cx=keyX(i,nn);
      if(tx>=cx-13&&tx<=cx+13){
        ch=pwdShift?ROW_SYM[i]:ROW_NUM[i];
        if(pwdShift) pwdShift=false;  // auto-release shift after one symbol
        return 1;
      }
    }
  }

  // Row 1: qwerty
  int n0=strlen(ROW0);
  if(ty>=keyY(1)-13 && ty<=keyY(1)+12){
    for(int i=0;i<n0;i++){
      int cx=keyX(i,n0);
      if(tx>=cx-13&&tx<=cx+13){
        ch=pwdShift?(char)toupper(ROW0[i]):ROW0[i]; return 1;
      }
    }
  }
  // Row 2: asdf
  int n1=strlen(ROW1);
  if(ty>=keyY(2)-13 && ty<=keyY(2)+12){
    for(int i=0;i<n1;i++){
      int cx=keyX(i,n1);
      if(tx>=cx-13&&tx<=cx+13){
        ch=pwdShift?(char)toupper(ROW1[i]):ROW1[i]; return 1;
      }
    }
  }
  // Row 3: SHF z..m DEL
  if(ty>=keyY(3)-13 && ty<=keyY(3)+12){
    if(tx>=4&&tx<=34)   return 5;   // SHIFT
    if(tx>=286&&tx<=316) return 2;  // DEL
    int n2=strlen(ROW2);
    for(int i=0;i<n2;i++){
      int cx=38+i*28+14;
      if(tx>=cx-13&&tx<=cx+13){
        ch=pwdShift?(char)toupper(ROW2[i]):ROW2[i]; return 1;
      }
    }
  }
  // Row 4: SHOW SPACE OK
  if(ty>=keyY(4)-13 && ty<=keyY(4)+12){
    if(tx>=4  &&tx<=54)  return 6;  // SHOW/HIDE
    if(tx>=58 &&tx<=262) return 4;  // SPACE
    if(tx>=266&&tx<=316) return 3;  // OK
  }
  return 0;
}

void fetchAll();  // forward declaration so WiFi connect can refresh data immediately
void checkForOtaUpdate(bool showScreen = true);

void doConnect(int idx, const String& password){
  tft.fillScreen(C_BG);
  tft.setTextColor(0x739F,C_BG); tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting...",160,70,4);
  tft.setTextColor(C_DIM,C_BG);
  tft.drawString(nets[idx].ssid.c_str(),160,100,2);

  WiFi.begin(nets[idx].ssid.c_str(), password.c_str());
  int tries=0;
  while(WiFi.status()!=WL_CONNECTED && tries<40){
    delay(500); tries++;
    tft.setTextColor((tries%2)?0x739F:C_BG,C_BG);
    tft.drawString("......",160,135,4);
  }
  if(WiFi.status()==WL_CONNECTED){
    configTime(-5*3600,3600,"pool.ntp.org");
    tft.setTextColor(C_GREEN,C_BG);
    tft.drawString("Connected!",160,175,4);
    tft.setTextColor(C_DIM,C_BG);
    tft.drawString("Loading lake data...",160,205,2);
    delay(800);

    // IMPORTANT: after manually connecting from the WiFi screen, fetch data immediately.
    // Without this, the main screen shows -- / data unavailable until the 10-minute refresh timer fires.
    curScreen=SCR_MAIN;
    fetchAll();
  } else {
    tft.setTextColor(C_RED,C_BG);
    tft.drawString("Failed.",160,175,4);
    tft.setTextColor(C_DIM,C_BG);
    tft.drawString("Wrong password?",160,200,2);
    delay(2500);
    curScreen=SCR_WIFI;
    drawWifiScreen();
  }
}

void connectToNetwork(int idx){
  if(nets[idx].secured){
    // Show keyboard for password entry
    pwdBuffer   = "";
    pwdShift    = false;
    pwdShowPwd  = false;
    curScreen   = SCR_KB;
    drawKeyboard();
  } else {
    // Open network — connect directly
    doConnect(idx, "");
  }
}

// ─────────────────────────────────────────────────────────────
//  TOUCH HANDLER
//  Uses a press/release state machine so each physical tap fires
//  exactly once, no matter how long the finger is held down.
//  This stops the WiFi screen from redrawing continuously.
// ─────────────────────────────────────────────────────────────
void handleTouch(){
  static bool wasPressed = false;

  bool nowPressed = touch.touched();

  // Rising edge only — act on finger-down, ignore held & release
  if(nowPressed && !wasPressed){
    wasPressed = true;

    int tx, ty;
    if(!getTouchPoint(tx, ty)) return;

    // ── MAIN SCREEN ──────────────────────────────────────────
    if(curScreen == SCR_MAIN){
      // Tap bottom-right corner (x>230, y>190) → open WiFi setup
      // If OTA update available, tap bottom-right also triggers update
      static bool otaUpdateAvailable = false;
      static String otaLatestVersion = "";
      if(tx > 230 && ty > 190){
        if(otaUpdateAvailable){
          // Confirm update tap area drawn in this zone
          checkForOtaUpdate(true);
        } else {
          curScreen = SCR_WIFI;
          scanNetworks();
          drawWifiScreen();
        }
      }
    }

    // ── KEYBOARD SCREEN ──────────────────────────────────────
    else if(curScreen == SCR_KB){
      char ch = 0;
      int cmd = kbHitTest(tx, ty, ch);
      switch(cmd){
        case 1:  // regular character
          pwdBuffer += ch;
          pwdShift = false;  // auto-release shift after one char
          drawKeyboard();
          break;
        case 2:  // DEL
          if(pwdBuffer.length() > 0) pwdBuffer.remove(pwdBuffer.length() - 1);
          drawKeyboard();
          break;
        case 3:  // OK — attempt connection
          doConnect(netSelected, pwdBuffer);
          break;
        case 4:  // SPACE
          pwdBuffer += ' ';
          drawKeyboard();
          break;
        case 5:  // SHIFT toggle
          pwdShift = !pwdShift;
          drawKeyboard();
          break;
        case 6:  // SHOW/HIDE password
          pwdShowPwd = !pwdShowPwd;
          drawKeyboard();
          break;
      }
    }

    // ── WIFI SCREEN ──────────────────────────────────────────
    else if(curScreen == SCR_WIFI){
      const int ITH = 28;
      const int LT  = 40;
      const int SHOW = min(netCount, 5);

      // Tap a network row → select it and redraw once
      bool rowHit = false;
      for(int i = 0; i < SHOW; i++){
        int iy = LT + i * ITH;
        if(ty >= iy && ty <= iy + ITH - 2){
          netSelected = i;
          drawWifiScreen();
          rowHit = true;
          break;
        }
      }

      // Tap CONNECT button (y 188-214, x 50-270)
      if(!rowHit && ty >= 188 && ty <= 214 && tx >= 50 && tx <= 270){
        connectToNetwork(netSelected);
      }

      // Tap header → back to main
      if(!rowHit && ty < 26){
        curScreen = SCR_MAIN;
        drawMainUI();
      }
    }
  }

  // Track release so next press is treated as a new tap
  if(!nowPressed){
    wasPressed = false;
  }
}

// ─────────────────────────────────────────────────────────────
//  SURFER PIXEL BUFFER
// ─────────────────────────────────────────────────────────────
inline void sp(int x,int y,uint16_t c){
  if(x>=0&&x<SPR_W&&y>=0&&y<SPR_H) surferBuf[y*SPR_W+x]=c;
}
void srect(int x,int y,int w,int h,uint16_t c){
  for(int dy=0;dy<h;dy++) for(int dx=0;dx<w;dx++) sp(x+dx,y+dy,c);
}
void sellipse(int cx,int cy,int rx,int ry,uint16_t c){
  for(int dy=-ry;dy<=ry;dy++)
    for(int dx=-rx;dx<=rx;dx++)
      if((float)(dx*dx)/(rx*rx)+(float)(dy*dy)/(ry*ry)<=1.f)
        sp(cx+dx,cy+dy,c);
}

void buildSurferBuffer(){
  for(int i=0;i<SPR_W*SPR_H;i++) surferBuf[i]=TRANSP;

  // Wave curl — bottom 24 rows
  for(int x=0;x<SPR_W;x++){
    float t=(float)x/(SPR_W-1);
    int wt=SPR_H-24+(int)(8.f*t*t);
    for(int y=wt;y<SPR_H;y++){
      float f=(float)(y-wt)/(SPR_H-wt);
      sp(x,y, f<.18f?C_FOAM:f<.48f?C_WAVE1:C_WAVE2);
    }
  }

  // Board  9-70, y 62-66
  srect(9,62,60,5,C_BOARD);
  sp(8,63,C_BOARD); sp(8,64,C_BOARD); sp(70,63,C_BOARD); sp(71,64,C_BOARD);

  // Legs
  for(int y=50;y<63;y++) srect(28,y,5,1,C_SUIT);
  for(int y=46;y<63;y++) srect(38,y,5,1,C_SUIT);
  srect(24,60,8,4,C_SUIT); srect(36,60,8,4,C_SUIT);

  // Torso + stripe
  srect(25,29,20,22,C_SUIT); srect(31,29,4,22,C_STRIPE);

  // Back arm (down)
  for(int i=0;i<13;i++) sp(24-i, 37+(int)(i*.6f), C_SUIT);
  // Front arm (up-forward)
  for(int i=0;i<16;i++) sp(46+i, 32-(int)(i*.35f), C_SKIN);
  srect(61,26,5,4,C_SKIN);

  // Neck + head
  srect(31,20,8,10,C_SKIN);
  sellipse(35,13,8,9,C_SKIN);
  srect(27,5,16,7,C_HAIR); sellipse(35,7,8,6,C_HAIR);
  srect(39,11,5,2,0x0000); srect(33,11,4,2,0x0000); // sunglasses
  sp(33,17,0x0000); sp(34,18,0x0000); sp(35,18,0x0000); sp(36,17,0x0000); // smile

  // Spray
  sp(6,59,C_FOAM); sp(5,57,C_FOAM); sp(4,61,C_FOAM);
  sp(3,55,C_SPLASH); sp(7,53,C_SPLASH);
}

// Blit surfer (skip TRANSP)
void blitSurfer(int sx,int sy){
  for(int y=0;y<SPR_H;y++){
    int sy2=sy+y; if(sy2<0||sy2>=240) continue;
    for(int x=0;x<SPR_W;x++){
      int sx2=sx+x; if(sx2<0||sx2>=320) continue;
      uint16_t c=surferBuf[y*SPR_W+x];
      if(c!=TRANSP) tft.drawPixel(sx2,sy2,c);
    }
  }
}

// ─────────────────────────────────────────────────────────────
//  SURFER ANIMATION — clean pass-through via viewport restore
// ─────────────────────────────────────────────────────────────
struct Px{int16_t x,y;uint8_t life;uint16_t col;};
#define MAXP 30
Px pts[MAXP]; int np=0;

void spawnSpray(int sx,int baseY){
  const uint16_t sc[]={C_FOAM,C_SPLASH,C_WHITE,C_ACCENT};
  for(int i=0;i<8&&np<MAXP;i++){
    pts[np++]={(int16_t)(sx+random(-10,4)),
               (int16_t)(baseY+random(-18,8)),
               (uint8_t)(4+random(4)),
               sc[random(4)]};
  }
}

// Restore a strip by re-drawing only the UI elements that
// intersect x..x+w.  Uses TFT_eSPI setViewport for clipping.
void restoreStrip(int x,int w);  // forward decl

void runSurferAnimation(){
  // Safe animation mode:
  // The previous version restored narrow viewport strips. On some TFT_eSPI/CYD
  // combinations those clipped redraws shift/blank the UI. This version redraws
  // the complete main UI each frame, overlays the surfer, then redraws once more
  // at the end. It is a little less fancy, but it cannot push/erase the screen.
  if(curScreen != SCR_MAIN) return;
  np = 0;

  for(int sx = -SPR_W; sx <= 320 + 4; sx += 6){
    drawMainUI();

    if(sx > -SPR_W && sx < 320) spawnSpray(sx, WAVE_Y + 58);

    // Draw particles for this frame only. They are cleared by the next drawMainUI().
    for(int i = 0; i < np; ){
      if(pts[i].x >= 0 && pts[i].x < 320 && pts[i].y >= 0 && pts[i].y < 240){
        tft.drawPixel(pts[i].x, pts[i].y, pts[i].col);
      }
      if(pts[i].life > 0) pts[i].life--;
      if(pts[i].life == 0){ pts[i] = pts[--np]; }
      else i++;
    }

    if(sx + SPR_W > 0 && sx < 320) blitSurfer(sx, WAVE_Y);
    delay(35);
  }

  np = 0;
  drawMainUI();
  lastSurfer = millis();
}

// ─────────────────────────────────────────────────────────────
//  RESTORE STRIP  — re-draw UI elements clipped to x..x+w
// ─────────────────────────────────────────────────────────────
void restoreStrip(int x,int w){
  if(w<=0||x>=320) return;
  x=constrain(x,0,319); w=constrain(w,1,320-x);
  tft.setViewport(x,0,w,240);

  tft.fillScreen(C_BG);

  // Header
  tft.fillRect(0,0,320,28,C_HEADER);
  tft.drawLine(0,28,320,28,C_ACCENT);
  tft.setTextColor(C_ACCENT,C_HEADER); tft.setTextDatum(MC_DATUM);
  tft.drawString("LAKE LANIER",160,14,4);
  tft.setTextColor(C_DIM,C_HEADER); tft.setTextDatum(MR_DATUM);
  tft.drawString(lk.updated,314,14,1);

  // Level card
  float diff=lk.levelFt-lk.fullPool;
  uint16_t lvlCol=levelColour(diff);
  tft.fillRoundRect(6,33,308,80,5,C_CARD);
  tft.drawRoundRect(6,33,308,80,5,lvlCol);
  tft.setTextColor(C_DIM,C_CARD); tft.setTextDatum(TL_DATUM);
  tft.drawString("WATER LEVEL",14,38,1);
  if(lk.levelOk){
    char lb[12]; snprintf(lb,12,"%.2f",lk.levelFt);
    tft.setTextColor(lvlCol,C_CARD); tft.setTextDatum(MR_DATUM);
    tft.drawString(lb,208,78,6);
    tft.setTextColor(C_DIM,C_CARD); tft.setTextDatum(TL_DATUM);
    tft.drawString("ft",214,62,2); tft.drawString("MSL",214,79,1);
    char db[32]; snprintf(db,32,diff>=0?"+%.2f ft above full":"%.2f ft below full",diff);
    tft.setTextColor(lvlCol,C_CARD); tft.setTextDatum(BC_DATUM);
    tft.drawString(db,160,108,1);
    if(lk.trendOk) drawTrend(286,68,10,lk.trendFt);
    float pct=constrain((lk.levelFt-1050)/21.f,0,1);
    tft.fillRect(6,115,308,4,C_VDIM);
    tft.fillRect(6,115,(int)(308*pct),4,lvlCol);
  }

  // Three cards
  const int CY=125,CW=100,CH=68,CG=4;
  const int CX1=6,CX2=CX1+CW+CG,CX3=CX2+CW+CG;
  auto sc2=[&](int cx,const char* t,const char* v,const char* s,uint16_t ac){
    tft.fillRoundRect(cx,CY,CW,CH,4,C_CARD);
    tft.drawRoundRect(cx,CY,CW,CH,4,C_BORDER);
    tft.setTextColor(C_DIM,C_CARD); tft.setTextDatum(TC_DATUM);
    tft.drawString(t,cx+CW/2,CY+4,1);
    tft.drawLine(cx+8,CY+14,cx+CW-8,CY+14,C_BORDER);
    tft.setTextColor(ac,C_CARD); tft.drawString(v,cx+CW/2,CY+22,4);
    tft.setTextColor(C_DIM,C_CARD); tft.setTextDatum(BC_DATUM);
    tft.drawString(s,cx+CW/2,CY+CH-3,1);
  };
  if(lk.weatherOk){char v[10];snprintf(v,10,"%.0f\xB0",lk.airTempF);sc2(CX1,"AIR TEMP",v,"Fahrenheit",C_ACCENT);}
  else sc2(CX1,"AIR TEMP","--","",C_VDIM);
  if(lk.weatherOk){
    char v[8];snprintf(v,8,"%.0f",lk.windMph);
    char s[14];snprintf(s,14,"%s  %d\xB0",windName(lk.windDeg),lk.windDeg);
    sc2(CX2,"WIND",v,s,C_ACCENT);
    tft.setTextColor(C_DIM,C_CARD);tft.setTextDatum(TC_DATUM);
    tft.drawString("mph",CX2+CW/2,CY+50,1);
  } else sc2(CX2,"WIND","--","",C_VDIM);
  if(lk.wTempOk){char v[10];snprintf(v,10,"%.1f\xB0",lk.waterTempF);sc2(CX3,"LAKE TEMP",v,"Fahrenheit",C_AMBER);}
  else sc2(CX3,"LAKE TEMP","--","LakeMonster",C_VDIM);

  // Bottom bar
  tft.fillRect(0,202,320,38,C_CARD);
  tft.drawLine(0,202,320,202,C_BORDER);
  tft.setTextColor(C_VDIM,C_CARD); tft.setTextDatum(ML_DATUM);
  tft.drawString("FULL: 1071ft",10,221,1);
  if(lk.levelOk){
    float pct2=constrain((lk.levelFt-1050)/21.f*100,0,100);
    uint16_t pCol=pct2>=95?C_GREEN:pct2>=80?C_ACCENT:pct2>=60?C_AMBER:C_RED;
    char pb[14];snprintf(pb,14,"%.1f%% full",pct2);
    tft.setTextColor(pCol,C_CARD);tft.setTextDatum(MC_DATUM);
    tft.drawString(pb,160,221,2);
  } else {
    tft.setTextColor(C_VDIM,C_CARD);tft.setTextDatum(MC_DATUM);
    tft.drawString("--.-% full",160,221,2);
  }
  if(lk.trendOk){
    uint16_t tc=lk.trendFt>0.02f?C_GREEN:lk.trendFt<-0.02f?C_RED:C_AMBER;
    char tb[14];snprintf(tb,14,"%+.2fft/3d",lk.trendFt);
    tft.setTextColor(tc,C_CARD);tft.setTextDatum(MR_DATUM);
    tft.drawString(tb,312,221,1);
  }

  tft.resetViewport();
}

// ─────────────────────────────────────────────────────────────
//  DATA FETCHERS
// ─────────────────────────────────────────────────────────────
// This version downloads the JSON into a String first, then parses it.
// That is more reliable on ESP32 than parsing directly from a TLS stream.

bool getApiPayload(const char* label, const char* url, String &payload, int &httpCode, uint32_t timeoutMs){
  payload = "";
  httpCode = -999;

  WiFiClientSecure client;
  client.setInsecure();  // avoids certificate bundle problems on ESP32/CYD
  client.setTimeout(timeoutMs / 1000);

  HTTPClient h;
  h.setTimeout(timeoutMs);
  h.setReuse(false);
  h.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  if(!h.begin(client, url)){
    httpCode = -100; // begin failed
    Serial.printf("%s begin failed\n", label);
    return false;
  }

  h.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 ESP32 LakeLanierInfoscreen");
  h.addHeader("Accept", "text/html,application/json,text/plain,*/*");
  h.addHeader("Accept-Encoding", "identity");
  h.addHeader("Cache-Control", "no-cache");

  httpCode = h.GET();
  Serial.printf("%s HTTP: %d\n", label, httpCode);

  if(httpCode == 200){
    payload = h.getString();
    Serial.printf("%s bytes: %d\n", label, payload.length());
    Serial.println(payload.substring(0, min(180, (int)payload.length())));
  } else {
    String err = h.getString();
    Serial.printf("%s error body: %s\n", label, err.substring(0, min(180, (int)err.length())).c_str());
  }

  h.end();
  return httpCode == 200 && payload.length() > 20;
}

bool parseUSGSLastValue(const String &payload, float &outValue){
  JsonDocument d;
  DeserializationError err = deserializeJson(d, payload);
  if(err){
    Serial.printf("USGS JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray series = d["value"]["timeSeries"].as<JsonArray>();
  if(series.isNull() || series.size() == 0) return false;

  bool found = false;
  float last = 0;
  for(JsonObject ts : series){
    JsonArray valuesGroups = ts["values"].as<JsonArray>();
    if(valuesGroups.isNull()) continue;
    for(JsonObject group : valuesGroups){
      JsonArray vals = group["value"].as<JsonArray>();
      if(vals.isNull()) continue;
      for(JsonObject v : vals){
        const char* raw = v["value"];
        if(raw && strlen(raw) > 0){
          last = atof(raw);
          found = true;
        }
      }
    }
  }

  if(found) outValue = last;
  return found;
}

bool parseUSGSTrend(const String &payload, float &trend){
  JsonDocument d;
  DeserializationError err = deserializeJson(d, payload);
  if(err){
    Serial.printf("USGS trend JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray series = d["value"]["timeSeries"].as<JsonArray>();
  if(series.isNull() || series.size() == 0) return false;

  bool haveFirst = false;
  float first = 0, last = 0;
  for(JsonObject ts : series){
    JsonArray valuesGroups = ts["values"].as<JsonArray>();
    if(valuesGroups.isNull()) continue;
    for(JsonObject group : valuesGroups){
      JsonArray vals = group["value"].as<JsonArray>();
      if(vals.isNull()) continue;
      for(JsonObject v : vals){
        const char* raw = v["value"];
        if(raw && strlen(raw) > 0){
          float val = atof(raw);
          if(!haveFirst){ first = val; haveFirst = true; }
          last = val;
        }
      }
    }
  }

  if(haveFirst){
    trend = last - first;
    return true;
  }
  return false;
}

bool parseDiscoverLanierLevel(const String &payload, float &outValue){
  int idx = payload.indexOf("WATER LEVEL");
  if(idx < 0) idx = payload.indexOf("Feet MSL");
  if(idx < 0) idx = 0;

  int end = min((int)payload.length(), idx + 1800);
  for(int i = idx; i < end; i++){
    if(isDigit(payload[i])){
      String num = "";
      int j = i;
      while(j < end && (isDigit(payload[j]) || payload[j] == ',' || payload[j] == '.')){
        if(payload[j] != ',') num += payload[j];
        j++;
      }
      float v = num.toFloat();
      if(v > 1000.0f && v < 1100.0f){
        outValue = v;
        return true;
      }
      i = j;
    }
  }
  return false;
}

void fetchLevel(){
  String payload;
  bool ok = false;

  // First try USGS official instantaneous value.
  if(getApiPayload("USGS level", URL_LEVEL, payload, httpLevelCode, 12000)){
    float v;
    if(parseUSGSLastValue(payload, v)){
      lk.levelFt = v;
      lk.levelOk = true;
      ok = true;
    }
  }

  // Fallback: DiscoverLanier/LakesOnline HTML page if USGS JSON fails on-device.
  if(!ok && getApiPayload("DiscoverLanier level", URL_LEVEL_FALLBACK, payload, httpLevelCode, 12000)){
    float v;
    if(parseDiscoverLanierLevel(payload, v)){
      lk.levelFt = v;
      lk.levelOk = true;
    }
  }
}

void fetchTrend(){
  String payload;
  if(getApiPayload("USGS trend", URL_TREND, payload, httpTrendCode, 15000)){
    float v;
    if(parseUSGSTrend(payload, v)){
      lk.trendFt = v;
      lk.trendOk = true;
    }
  }
}

void fetchWeather(){
  String payload;
  if(getApiPayload("Open-Meteo", URL_WEATHER, payload, httpWeatherCode, 12000)){
    JsonDocument d;
    DeserializationError err = deserializeJson(d, payload);
    if(err){
      Serial.printf("Open-Meteo JSON parse error: %s\n", err.c_str());
      return;
    }

    // Current Open-Meteo format
    if(!d["current"].isNull()){
      lk.airTempF = d["current"]["temperature_2m"].as<float>();
      lk.windMph  = d["current"]["wind_speed_10m"].as<float>();
      lk.windDeg  = d["current"]["wind_direction_10m"].as<int>();
      lk.weatherOk = true;
      return;
    }

    // Fallback for older Open-Meteo current_weather format
    if(!d["current_weather"].isNull()){
      lk.airTempF = d["current_weather"]["temperature"].as<float>();
      lk.windMph  = d["current_weather"]["windspeed"].as<float>();
      lk.windDeg  = d["current_weather"]["winddirection"].as<int>();
      lk.weatherOk = true;
    }
  }
}

int indexOfIgnoreCase(const String &haystack, const char* needle, int fromIndex = 0){
  String h = haystack;
  String n = String(needle);
  h.toLowerCase();
  n.toLowerCase();
  return h.indexOf(n, fromIndex);
}

bool extractDegreeTempNear(const String &payload, int idx, int before, int after, float &outValue){
  // Finds a Fahrenheit-looking value only when it is clearly marked as a temperature
  // by a degree sign, &deg;, or F/f shortly after the number. This prevents grabbing
  // unrelated numbers like air temp, counts, dates, or CSS values.
  if(idx < 0) return false;
  int start = max(0, idx - before);
  int end   = min((int)payload.length(), idx + after);

  for(int i = start; i < end; i++){
    if(isDigit(payload[i])){
      String num = "";
      int j = i;
      while(j < end && (isDigit(payload[j]) || payload[j] == '.')){
        num += payload[j];
        j++;
      }

      float v = num.toFloat();
      if(v >= 35.0f && v <= 95.0f){
        String tail = payload.substring(j, min(end, j + 24));
        if(tail.indexOf("°") >= 0 || tail.indexOf("&deg") >= 0 ||
           tail.indexOf("&#176") >= 0 || tail.indexOf("F") >= 0 || tail.indexOf("f") >= 0){
          outValue = v;
          return true;
        }
      }
      i = j;
    }
  }
  return false;
}

bool extractFirstNumberAfterGT(const String &payload, int idx, float &outValue){
  // For fragments like: <p class="...font-bold...">72°</p>
  int gt = payload.indexOf('>', idx);
  if(gt < 0) return false;
  return extractDegreeTempNear(payload, gt + 1, 0, 60, outValue);
}

bool parseTempAfterMarker(const String &payload, const char* marker, float &outValue){
  int idx = indexOfIgnoreCase(payload, marker);
  while(idx >= 0){
    if(extractDegreeTempNear(payload, idx + strlen(marker), 0, 650, outValue)) return true;
    idx = indexOfIgnoreCase(payload, marker, idx + strlen(marker));
  }
  return false;
}

bool parseLakeMonsterWaterTemp(const String &payload, float &outValue){
  // LakeMonster has changed layout a few times. The important part is to grab the
  // number in the WATER card, not the nearby air temp. The user-provided current
  // HTML example is:
  //   <p class="mt-0.5 text-sm font-bold text-gray-900">72°</p>

  // 1) Best human-readable markers.
  if(parseTempAfterMarker(payload, "Current water temp", outValue)) return true;
  if(parseTempAfterMarker(payload, "Water read", outValue)) return true;

  // 2) Card structure: word Water, then the next bold gray p contains 72°.
  int idx = indexOfIgnoreCase(payload, "Water");
  while(idx >= 0){
    int cls = indexOfIgnoreCase(payload, "text-sm font-bold text-gray-900", idx);
    if(cls >= 0 && cls - idx < 900){
      if(extractFirstNumberAfterGT(payload, cls, outValue)) return true;
    }
    idx = indexOfIgnoreCase(payload, "Water", idx + 5);
  }

  // 3) Simple text pattern after a Water label, but require a degree/F marker.
  idx = indexOfIgnoreCase(payload, "Water");
  while(idx >= 0){
    if(extractDegreeTempNear(payload, idx, 0, 260, outValue)) return true;
    idx = indexOfIgnoreCase(payload, "Water", idx + 5);
  }

  // 4) Header pattern: 72°F water or 72° water. Look a little before the word.
  idx = indexOfIgnoreCase(payload, "water");
  while(idx >= 0){
    if(extractDegreeTempNear(payload, idx, 90, 40, outValue)) return true;
    idx = indexOfIgnoreCase(payload, "water", idx + 5);
  }

  // No broad fallback here; returning the wrong value is worse than showing --.
  return false;
}

void fetchWaterTemp(){
  lk.wTempOk = false;
  httpWTempCode = -999;

  // Stream the LakeMonster page instead of reading the whole HTML into one String.
  // The page can be large, and on ESP32 h.getString() can fail or fragment memory.
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  HTTPClient h;
  h.setTimeout(15000);
  h.setReuse(false);
  h.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  if(!h.begin(client, URL_WTEMP)){
    httpWTempCode = -100;
    Serial.println("LakeMonster temp begin failed");
    return;
  }

  h.addHeader("User-Agent", "Mozilla/5.0 ESP32 LakeLanierInfoscreen");
  h.addHeader("Accept", "text/html,text/plain,*/*");
  h.addHeader("Accept-Encoding", "identity");
  h.addHeader("Cache-Control", "no-cache");

  httpWTempCode = h.GET();
  Serial.printf("LakeMonster temp HTTP: %d\n", httpWTempCode);

  if(httpWTempCode != 200){
    String err = h.getString();
    Serial.printf("LakeMonster error body: %s\n", err.substring(0, min(180, (int)err.length())).c_str());
    h.end();
    return;
  }

  WiFiClient *stream = h.getStreamPtr();
  String rolling = "";
  unsigned long startMs = millis();
  uint8_t buf[256];

  while(h.connected() && millis() - startMs < 15000){
    int avail = stream->available();
    if(avail > 0){
      int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
      if(n > 0){
        for(int i=0; i<n; i++) rolling += (char)buf[i];
        if(rolling.length() > 3000) rolling = rolling.substring(rolling.length() - 3000);

        float v;
        if(parseLakeMonsterWaterTemp(rolling, v)){
          lk.waterTempF = v;
          lk.wTempOk = true;
          Serial.printf("LakeMonster temp parsed: %.1f F\n", v);
          break;
        }
      }
    } else {
      delay(10);
    }
  }

  if(!lk.wTempOk){
    Serial.println("LakeMonster temp parse failed from stream");
    Serial.println(rolling.substring(0, min(300, (int)rolling.length())));
  }

  h.end();
}


void fetchAll(){
  if(WiFi.status()!=WL_CONNECTED){
    apiDebug = "WiFi lost";
    return;
  }

  // Reset flags so stale values do not look fresh after a failed update.
  lk.levelOk = lk.weatherOk = lk.wTempOk = lk.trendOk = false;

  // Show updating notice quietly in bottom bar
  tft.setViewport(0,202,320,38);
  tft.fillRect(0,202,320,38,C_CARD);
  tft.setTextColor(C_ACCENT,C_CARD); tft.setTextDatum(MC_DATUM);
  tft.drawString("Updating...",160,221,2);
  tft.resetViewport();

  fetchLevel();
  fetchTrend();
  fetchWeather();
  fetchWaterTemp();

  char dbg[42];
  snprintf(dbg, sizeof(dbg), "L%d T%d W%d H%d", httpLevelCode, httpTrendCode, httpWeatherCode, httpWTempCode);
  apiDebug = dbg;
  Serial.printf("API debug: %s\n", apiDebug.c_str());

  lk.updated=nowTime();
  lastFetch=millis();
  drawMainUI();
}

// ─────────────────────────────────────────────────────────────
//  GITHUB OTA UPDATE
// ─────────────────────────────────────────────────────────────
bool isNewerVersion(const String& latest, const String& current){
  int la[3] = {0,0,0};
  int cu[3] = {0,0,0};
  sscanf(latest.c_str(),  "%d.%d.%d", &la[0], &la[1], &la[2]);
  sscanf(current.c_str(), "%d.%d.%d", &cu[0], &cu[1], &cu[2]);
  for(int i=0;i<3;i++){
    if(la[i] > cu[i]) return true;
    if(la[i] < cu[i]) return false;
  }
  return false;
}

// These variables track OTA state for main UI indicator and touch handler.
bool otaUpdateAvailable = false;
String otaLatestVersion = "";

String getLatestFirmwareVersion(){
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient h;
  h.setTimeout(10000);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if(!h.begin(client, FW_VERSION_URL)){
    Serial.println("OTA version check: begin failed");
    return "";
  }

  int code = h.GET();
  Serial.printf("OTA version HTTP: %d\n", code);
  if(code != HTTP_CODE_OK){
    h.end();
    return "";
  }

  String v = h.getString();
  h.end();
  v.trim();
  v.replace("\r", "");
  v.replace("\n", "");
  return v;
}

void showOtaMessage(const char* line1, const char* line2 = ""){
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.drawString(line1, 160, 98, 2);
  if(strlen(line2)>0){
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString(line2, 160, 126, 1);
  }
}

bool downloadAndInstallFirmware(const char* url){
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient h;
  h.setTimeout(30000);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // needed for GitHub release asset redirects

  if(!h.begin(client, url)){
    Serial.println("OTA firmware: begin failed");
    return false;
  }

  int code = h.GET();
  Serial.printf("OTA firmware HTTP: %d\n", code);
  if(code != HTTP_CODE_OK){
    h.end();
    return false;
  }

  int len = h.getSize();
  Serial.printf("OTA firmware size: %d\n", len);
  if(len <= 0){
    h.end();
    return false;
  }

  if(!Update.begin(len)){
    Serial.printf("OTA Update.begin failed: %s\n", Update.errorString());
    h.end();
    return false;
  }

  WiFiClient* stream = h.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  Serial.printf("OTA written: %u / %d\n", (unsigned)written, len);

  bool ok = (written == (size_t)len) && Update.end() && Update.isFinished();
  if(!ok){
    Serial.printf("OTA update failed: %s\n", Update.errorString());
    Update.abort();
  }

  h.end();
  return ok;
}

// Sets global OTA tracking variables and triggers indicator/UI as needed.
void checkForOtaUpdate(bool showScreen){
  if(WiFi.status() != WL_CONNECTED) return;

  Serial.printf("OTA current version: %s\n", FW_VERSION);
  String latest = getLatestFirmwareVersion();
  if(latest.length() == 0){
    Serial.println("OTA: version unavailable");
    otaUpdateAvailable = false;
    otaLatestVersion = "";
    if(showScreen) drawMainUI();
    return;
  }

  Serial.printf("OTA latest version: %s\n", latest.c_str());
  if(isNewerVersion(latest, FW_VERSION)){
    otaUpdateAvailable = true;
    otaLatestVersion = latest;
  } else {
    otaUpdateAvailable = false;
    otaLatestVersion = "";
  }

  if(!otaUpdateAvailable){
    Serial.println("OTA: already up to date");
    if(showScreen) drawMainUI();
    return;
  }

  if(showScreen){
    String msg = String("New version ") + latest;
    showOtaMessage("Updating firmware...", msg.c_str());
  }

  if(downloadAndInstallFirmware(FW_BIN_URL)){
    showOtaMessage("Update complete", "Rebooting...");
    delay(1000);
    ESP.restart();
  } else {
    if(showScreen){
      showOtaMessage("Update failed", "Check Serial Monitor");
      delay(2500);
      drawMainUI();
    }
  }
}

// ─────────────────────────────────────────────────────────────
//  WIFI CONNECT (initial, from saved or open)
// ─────────────────────────────────────────────────────────────
void initialWifiConnect(){
  tft.fillScreen(C_BG);
  tft.setTextColor(C_ACCENT,C_BG); tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting to WiFi",160,100,2);
  tft.drawString("Tap bottom-right to change",160,120,1);

  // Try auto-connect (saved credentials or open net)
  WiFi.mode(WIFI_STA); WiFi.begin();
  int t=0;
  while(WiFi.status()!=WL_CONNECTED&&t<20){delay(500);t++;}
  if(WiFi.status()==WL_CONNECTED){
    configTime(-5*3600,3600,"pool.ntp.org");
    tft.setTextColor(C_GREEN,C_BG); tft.drawString("Connected!",160,150,4);
    delay(800);
  } else {
    // Go straight to WiFi setup
    curScreen=SCR_WIFI;
    scanNetworks();
    drawWifiScreen();
    return;
  }
}

// ─────────────────────────────────────────────────────────────
//  SETUP / LOOP
// ─────────────────────────────────────────────────────────────
void setup(){
  Serial.begin(115200);
  randomSeed(analogRead(0)^millis());

  tft.init(); tft.setRotation(1); tft.fillScreen(C_BG);
  pinMode(21,OUTPUT); digitalWrite(21,HIGH);  // backlight

  // Start touch on VSPI — must be before touch.begin()
  touchVSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchVSPI);

  // HTTPS API calls use a local WiFiClientSecure inside getApiPayload().
  // That client calls setInsecure() there, so no global client setup is needed here.

  buildSurferBuffer();
  initialWifiConnect();

  if(curScreen==SCR_MAIN){
    fetchAll();
    checkForOtaUpdate(false);
    lastOtaCheck = millis();
    lastSurfer=millis();
  }
}

void loop(){
  handleTouch();

  if(curScreen!=SCR_MAIN) return;

  if(WiFi.status()!=WL_CONNECTED){
    // Try reconnect quietly
    WiFi.reconnect(); delay(3000); 
    drawMainUI();
    return;
  }

  unsigned long now=millis();

  if(now - lastOtaCheck >= OTA_CHECK_MS){
    lastOtaCheck = now;
    checkForOtaUpdate(true);
    drawMainUI();
  } else {
    // If OTA status changed, update indicator
    static bool previousOtaUpdateAvailable = false;
    if(previousOtaUpdateAvailable != otaUpdateAvailable) {
      previousOtaUpdateAvailable = otaUpdateAvailable;
      drawMainUI();
    }
  }

  // Surfer animation disabled for the premium dashboard layout.
  // The static dashboard now matches the mockup much more closely.
  /*
  if(now-lastSurfer>=SURFER_MS){
    runSurferAnimation(); return;
  }
  */
  if(now-lastFetch>=REFRESH_MS){
    fetchAll();
  }
  delay(1000);
}
