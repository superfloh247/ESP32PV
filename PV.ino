/*
    License: CC BY-NC-SA
*/

// installed via Arduino IDE Library manager
#include <TaskScheduler.h>
#include <TaskSchedulerDeclarations.h>

// install TFT_eSPI, in TFT_eSPI/User_Setup_Select.h, comment out the default settings #include <User_Setup.h>,
// select #include <User_Setups/Setup25_TTGO_T_Display.h> , Save Settings.
#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <IotWebConf.h>
#include <IotWebConfUsing.h>
#include <IotWebConfESP32HTTPUpdateServer.h>
// disable LED blinking
#define IOTWEBCONF_STATUS_ENABLED 0
#define IOTWEBCONF_CONFIG_VERSION "v002"
#define IOTWEBCONF_STRING_PARAM_LEN 512
#define IOTWEBCONF_DEBUG_TO_SERIAL
#define IOTWEBCONF_CONFIG_USE_MDNS

// version 6.x!
#include <ArduinoJson.h>

// get this from TFT_eSPI/examples/Smooth Fonts/FLASH_Array/Smooth_font_gradient
#include "NotoSansBold36.h"
#include "NotoSansBold15.h"
#define AA_FONT_LARGE NotoSansBold36
#define AA_FONT_SMALL NotoSansBold15

#ifndef TFT_DISPOFF
#define TFT_DISPOFF 0x28
#endif

#ifndef TFT_SLPIN
#define TFT_SLPIN   0x10
#endif

#define TFT_MOSI            19
#define TFT_SCLK            18
#define TFT_CS              5
#define TFT_DC              16
#define TFT_RST             23

#define TFT_BL          4  // Display backlight control pin
#define ADC_EN          14
#define ADC_PIN         34

#define DISPLAYWIDTH  135
#define DISPLAYHEIGHT 240

TFT_eSPI tft = TFT_eSPI(DISPLAYWIDTH, DISPLAYHEIGHT);
TFT_eSprite framebufferMain = TFT_eSprite(&tft);

Scheduler taskScheduler;
void httpClientCallback1power();
void httpClientCallback1day();
void httpClientCallback2power();
void httpClientCallback2day();
void updateDisplayCallback();
void iotWebConfLoopCallback();
Task httpClientTask1power(1850, TASK_FOREVER, &httpClientCallback1power); // 2 sec
Task httpClientTask1day(1950, TASK_FOREVER, &httpClientCallback1day); // 2 sec
Task httpClientTask2power(2050, TASK_FOREVER, &httpClientCallback2power); // 2 sec
Task httpClientTask2day(2150, TASK_FOREVER, &httpClientCallback2day); // 2 sec
Task updateDisplayTask(40, TASK_FOREVER, &updateDisplayCallback); // 25fps
Task iotWebConfLoopTask(100, TASK_FOREVER, &iotWebConfLoopCallback);

int alive = 0;

DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;

const char thingName[] = "PV";
const char wifiInitialPassword[] = "PVPVPVPV";

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialPassword, IOTWEBCONF_CONFIG_VERSION);
void handleRoot();
void configSaved();
void wifiConnected();
boolean formValidator();

const size_t capacity = (JSON_OBJECT_SIZE(48) + 101) * 2; // some extra space to store currently 31 fields, and 101 bytes for ESP32 architecture
DynamicJsonDocument doc(capacity);

#define RGB888TORGB565(r, g, b) ((((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))

uint8_t buf888[DISPLAYHEIGHT * 3];

String PV1power = "n/a";
String PV1day = "n/a";
String PV1Wp = "1380W";
String PV2power = "n/a";
String PV2day = "n/a";
String PV2Wp = "570W";

int PVhistory[2][100];
int PVindex[] = { 0, 0 };
int PVmin[] = { 0, 0 };
int PVmax[] = { 0, 0 };

void setup()
{
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  if (TFT_BL > 0) { // TFT_BL has been set in the TFT_eSPI library in the User Setup file TTGO_T_Display.h
    pinMode(TFT_BL, OUTPUT); // Set backlight pin to output mode
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON); // Turn backlight on. TFT_BACKLIGHT_ON has been set in the TFT_eSPI library in the User Setup file TTGO_T_Display.h
  }
  tft.setSwapBytes(true); // TODO why?
  tft.println("Starting ...");
  // initialize history arrays
  for (int i = 0; i < 100; i++) {
    PVhistory[0][i] = 0;
    PVhistory[1][i] = 0;
  }
  framebufferMain.createSprite(DISPLAYHEIGHT, DISPLAYWIDTH);
  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.getApTimeoutParameter()->visible = true;
  iotWebConf.setupUpdateServer(
    [](const char* updatePath) { httpUpdater.setup(&server, updatePath); },
    [](const char* userName, char* password) { httpUpdater.updateCredentials(userName, password); });
  iotWebConf.setWifiConnectionCallback(&wifiConnected);
  Serial.println("IotWebConf::init()");
  iotWebConf.init();
  tft.println("IotWebConf is set up");
  server.on("/status", []() {
    if (iotWebConf.handleCaptivePortal()) {
      return; // all set, nothing more to be done here
    }
    String html = String("<html><head><meta http-equiv=\"refresh\" content=\"30\"></head><body>");
    html += String("</body></html>");
    server.send(200, "text/html", html);
  });
  server.on("/", handleRoot);
  server.on("/config", [] { iotWebConf.handleConfig(); });
  server.onNotFound([]() {
    iotWebConf.handleNotFound();
  });
  taskScheduler.init();
  taskScheduler.addTask(httpClientTask1power);
  taskScheduler.addTask(httpClientTask2power);
  taskScheduler.addTask(httpClientTask1day);
  taskScheduler.addTask(httpClientTask2day);
  taskScheduler.addTask(updateDisplayTask);
  taskScheduler.addTask(iotWebConfLoopTask);
  iotWebConfLoopTask.enable();
  tft.println("Task Scheduler is started");
  tft.println("Setting up wifi ...");
}

void loop() {
  if (iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE) {
    if (httpClientTask1power.isEnabled() == false || httpClientTask1day.isEnabled() == false || httpClientTask2power.isEnabled() == false || httpClientTask2day.isEnabled() == false) {
      httpClientTask1power.enable();
      httpClientTask1day.enable();
      httpClientTask2power.enable();
      httpClientTask2day.enable();
    }
    taskScheduler.execute();
  }
  else {
    iotWebConf.doLoop();
  }
}

void httpClientCallback1power() {
  PV1power = httpClientCallback("http://bananapi:8082/get/fronius.0.inverter.1.PAC");
  String sPV1 = PV1power;
  sPV1.replace("W", "");
  addToHistory(0, sPV1.toInt());
}

void httpClientCallback1day() {
  PV1day = httpClientCallback("http://bananapi:8082/get/fronius.0.inverter.1.DAY_ENERGY");
}

void httpClientCallback2power() {
  PV2power = httpClientCallback("http://bananapi:8082/get/sonoff.0.SonOffROW2PV.ENERGY_Power");
  String sPV2 = PV2power;
  sPV2.replace("W", "");
  addToHistory(1, sPV2.toInt());
}

void httpClientCallback2day() {
  PV2day = httpClientCallback("http://bananapi:8082/get/sonoff.0.SonOffROW2PV.ENERGY_Today");
}

String httpClientCallback(String url) {
  alive++;
  if (alive == DISPLAYHEIGHT) {
    alive = 0;
  }
  HTTPClient http;
  String retVal = "n/a";
  http.useHTTP10(true);
  http.setConnectTimeout(500); 
  Serial.println("HTTP GET " + url);
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    if (payload.length() > 3) { // remove UTF8 BOM
      if (payload[0] == char(0xEF) && payload[1] == char(0xBB) && payload[2] == char(0xBF)) {
        payload = payload.substring(3);
      }
    }
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      //logToSerialAndBuffer("deserializeJson() failed: " + String(error.c_str()));
    }
    else {
      // Extract values
      retVal = doc["val"].as<String>() + doc["common"]["unit"].as<String>();
    }
  }
  else {
    //logToSerialAndBuffer("[HTTP] GET... failed, error: " + String(http.errorToString(httpCode).c_str()));
  }
  http.end();
  if (updateDisplayTask.isEnabled() == false) {
    updateDisplayTask.enable();
  }
  return retVal;
}

void drawGauges() {
  framebufferMain.fillSprite(TFT_BLACK);
  int maincenterx = DISPLAYHEIGHT * 1 / 4;
  int maincentery = DISPLAYWIDTH / 2;
  String sPV1 = PV1power;
  sPV1.replace("W", "");
  int PV1 = 100 * sPV1.toInt() / PV1Wp.toInt();
  // gauge 1
  // draw outer line
  const float degree = 1000.0 / 57296.0;
  int radius = 59;
  const int innerradius = 51;
  // draw inner bar background
  for (radius = 59; radius > innerradius; radius--) {
    for (float alpha = 10.0 * degree; alpha >= -190.0 * degree; alpha -= degree) {
      framebufferMain.fillRect(maincenterx + radius * cos(alpha), maincentery + radius * sin(alpha), 2, 2, TFT_DARKGREY);
    }
  }
  // draw inner bar foreground
  for (radius = 59; radius > innerradius; radius--) {
    for (float alpha = (-190.0 + PV1 * 2) * degree; alpha >= -190.0 * degree; alpha -= degree) {
      framebufferMain.fillRect(maincenterx + radius * cos(alpha), maincentery + radius * sin(alpha), 2, 2, TFT_GREEN);
    }
  }
  // draw life line 1
  float scale = 32.0 / max(PVmax[0] - PVmin[0], 1);
  for (int i = 99; i > 0; i--) {
    int x1 = maincenterx + 50 - i;
    int y1 = maincentery + 16 - (PVhistory[0][100 - i] - PVmin[0]) * scale;
    int x2 = maincenterx + 50 - i - 1;
    int y2 = maincentery + 16 - (PVhistory[0][100 - i - 1] - PVmin[0]) * scale;
    framebufferMain.drawLine(x1, y1, x2, y2, TFT_RED);
  }
  // draw string 1
  framebufferMain.setTextColor(TFT_WHITE, TFT_BLACK);
  framebufferMain.setTextDatum(MC_DATUM);
  framebufferMain.loadFont(AA_FONT_LARGE);
  framebufferMain.drawString(PV1power, maincenterx, maincentery + 42);
  framebufferMain.loadFont(AA_FONT_SMALL);
  framebufferMain.drawString(PV1day, maincenterx, maincentery + 62);
  framebufferMain.setTextDatum(CL_DATUM);
  framebufferMain.drawString((String)PVmin[0], maincenterx - 50, maincentery + 16);
  framebufferMain.drawString((String)PVmax[0], maincenterx - 50, maincentery - 16);
  // gauge 2
  maincenterx = DISPLAYHEIGHT * 3 / 4;
  String sPV2 = PV2power;
  sPV2.replace("W", "");
  int PV2 = 100 * sPV2.toInt() / PV2Wp.toInt();
  // draw outer line
  // draw inner bar background
  for (radius = 59; radius > innerradius; radius--) {
    for (float alpha = 10.0 * degree; alpha >= -190.0 * degree; alpha -= degree) {
      framebufferMain.fillRect(maincenterx + radius * cos(alpha), maincentery + radius * sin(alpha), 2, 2, TFT_DARKGREY);
    }
  }
  // draw inner bar foreground
  for (radius = 59; radius > innerradius; radius--) {
    for (float alpha = (-190.0 + PV2 * 2) * degree; alpha >= -190.0 * degree; alpha -= degree) {
      framebufferMain.fillRect(maincenterx + radius * cos(alpha), maincentery + radius * sin(alpha), 2, 2, TFT_GREEN);
    }
  }
  // draw life line 2
  scale = 32.0 / max(PVmax[1] - PVmin[1], 1);
  for (int i = 99; i > 0; i--) {
    int x1 = maincenterx + 50 - i;
    int y1 = maincentery + 16 - (PVhistory[1][100 - i] - PVmin[1]) * scale;
    int x2 = maincenterx + 50 - i - 1;
    int y2 = maincentery + 16 - (PVhistory[1][100 - i - 1] - PVmin[1]) * scale;
    framebufferMain.drawLine(x1, y1, x2, y2, TFT_RED);
  }
  // draw string 2
  framebufferMain.setTextColor(TFT_WHITE, TFT_BLACK);
  framebufferMain.setTextDatum(MC_DATUM);
  framebufferMain.loadFont(AA_FONT_LARGE);
  framebufferMain.drawString(PV2power, maincenterx, maincentery + 42);
  framebufferMain.loadFont(AA_FONT_SMALL);
  framebufferMain.drawString(PV2day, maincenterx, maincentery + 62);
  framebufferMain.setTextDatum(CL_DATUM);
  framebufferMain.drawString((String)PVmin[1], maincenterx - 50, maincentery + 16);
  framebufferMain.drawString((String)PVmax[1], maincenterx - 50, maincentery - 16);
  // draw keep alive
  framebufferMain.fillRect(alive, DISPLAYWIDTH - 1, 2, 2, TFT_DARKGREY);
  // draw framebuffer
  framebufferMain.pushSprite(0, 0);
}

void updateDisplayCallback() {
  drawGauges();
  if (PV1power.equals(PV2power) && PV1power.equals("0W")) {
    digitalWrite(TFT_BL, LOW);
  }
  else {
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  }
}

void handleRoot() {
  if (iotWebConf.handleCaptivePortal()) {
    return; // all set, nothing more to be done here
  }
  String html = String("<html><head></head><body>");
  html += "<h1>TESP32LA</h1>";
  html += "Go to <a href='config'>configuration</a> page to change settings.<br />";
  html += "Start <a href='demo'>DEMO</a> mode.<br />";
  html += "Check <a href='status'>status</a>.<br />";
  html += "Have a look at the <a href='log'>log</a>.<br />";
  html += String("</body></html>");
  server.send(200, "text/html", html);
}

void iotWebConfLoopCallback() {
  iotWebConf.doLoop();
}

void configSaved() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(0, 0);
  tft.println("Wifi configuration saved");
  tft.println("TeslaLogger configuration saved");
}

void wifiConnected() {
  tft.println("Wifi connected");
  WiFi.setHostname(iotWebConf.getThingName());
  httpClientTask1power.enable();
  httpClientTask1day.enable();
  httpClientTask2power.enable();
  httpClientTask2day.enable();
}

void addToHistory(int history, int value) {
  PVhistory[history][PVindex[history]] = value;
  int lmin = value;
  int lmax = value;
  for (int i = 0; i < 100; i++) {
    if (PVhistory[history][i] > lmax) {
      lmax = PVhistory[history][i];
    }
    if (PVhistory[history][i] < lmin) {
      lmin = PVhistory[history][i];
    }
  }
  PVmax[history] = lmax;
  PVmin[history] = lmin;
  if (PVindex[history] == 99) {
    // shift all data
    for (int i = 1; i < 100; i++) {
      PVhistory[history][i - 1] = PVhistory[history][i];
    }
  }
  else {
    PVindex[history]++;
  }
}
