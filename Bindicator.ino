#include <FS.h>
#include <WiFiManager.h>
#include <TaskScheduler.h>
#include <TaskSchedulerDeclarations.h>
#include <TaskSchedulerSleepMethods.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <Adafruit_NeoPixel.h>
#include <WS2812FX.h>
#include "HTTPSRedirect.h"
#include "LinkedList.h"
#include "DebugMacros.h"

#define SECOND            1000
#define MINUTE            SECOND * 60
#define HOUR              MINUTE * 60

//#define DISPLAY                       // Comment out if your ESP8266 doesn't have a display
#define POWER_LED         true          // Illuminate the built-in LED when power supplied       
#define VERSION           "v2.29b"      // Version information
#define PORTAL_SSID       "Bindicator"  // SSID for web portal
#define SCROLLING         true          // If false, use the button to change page
#define LED_PIN           15            // NeoPixel data pin
#define TOUCH_PIN         13            // Capacitive touch data pin
#define RESET_PIN         16            // Display reset
#define CLOCK_PIN         5             // Display clock
#define DATA_PIN          4             // Display data
#define LED_COUNT         12            // Number of pixels
#define BRIGHTNESS        255           // NeoPixel brightness (0 - 255)
#define SPEED             1000          // Animation speed (lower is faster)
#define WIFI_TIMEOUT      5 * SECOND    // Delay in seconds before WiFi connection times out
#define WATCHDOG_TIMEOUT  60 * SECOND   // Delay in seconds before watchdog reboots device
#define PULSE_DELAY       5             // Delay in ms between brightness step increments
#define CONFIG_DELAY      3 * SECOND    // Delay in seconds before enabling WiFi config at boot
#define LED_INTERVAL      2 * SECOND    // Interval in second between event colour changing
#define REFRESH_INTERVAL  10 * MINUTE   // Data refresh interval
#define NIGHT_BRT         10            // Nightlight brightness (0 - 255)

bool nightlight = true;
int modes[3] = { FX_MODE_STATIC, FX_MODE_RAINBOW, FX_MODE_CUSTOM };
int modeIndex = FX_MODE_STATIC; 

struct Event {                          // All events contain a title and associated colour
  const char* title;                    
  uint32_t color;
};

LinkedList<struct Event> eventList;     // Downloaded calendar events go here

Adafruit_NeoPixel pixel = Adafruit_NeoPixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
WS2812FX ws2812fx = WS2812FX(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

#ifdef DISPLAY
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, RESET_PIN, CLOCK_PIN, DATA_PIN);
int page;                               // Incremented to change page when button touched and no collection scheduled
#endif

const char* host = "script.google.com";                                           // Base URL for Google Apps
const int httpsPort = 443;                                                        // Default HTTPS port
char *GScriptId = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";     // Default script ID

String eventsUrl = String("/macros/s/") + GScriptId + "/exec";                    // URL to retrieve calendar entries [GET]
String clearUrl = String("/macros/s/") + GScriptId + "/exec?clear";               // URL to clear active events [POST]

HTTPSRedirect* client = nullptr;                                                  // This client handles the redirection used by Google
WiFiManagerParameter custom_gScriptId(                                            // Custom parameter if Google Script ID is changed
  "gscriptid", "Google App ID", GScriptId, 56);

void neopixelCallback();                                                          // Task scheduler function for updating the NeoPixel and display
void getEventsCallback();                                                         // Task scheduler function for refreshing event data
void cancelEventsCallback();                                                      // Task scheduler function for cancelling upcoming events
void wiFiWatchdogCallback();                                                      // Task scheduler function for monitoring WiFi connection

Scheduler ts;                                                                     // Responsible for all repeating functions
Task tShowEventColor(LED_INTERVAL, TASK_FOREVER, &neopixelCallback);              // Task to convert events to NeoPixel colours
Task tUpdateData(REFRESH_INTERVAL, TASK_FOREVER, &getEventsCallback);             // Task to update the list of calendar events
Task tCancelEvents(SECOND / 2, TASK_FOREVER, &cancelEventsCallback);              // Task to poll the capacitive button
Task tWiFiWatchdog(SECOND, TASK_FOREVER, &wiFiWatchdogCallback);                  // Task to monitor WiFi and reconnect if necessary
  

void setup() {
  pinMode(TOUCH_PIN, INPUT);                 // Set the pin connected to the touch sensor to input
  if (POWER_LED) {                           // Turn on the built-in LED if enabled above
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  }

  delay(1000);

#ifdef DEBUG
  Serial.begin(115200);                     
  Serial.flush();
  DPRINTLN("Serial Transmission Begin");
#endif

  initialiseNeoPixel();                     // NeoPixel won't work without initialisation

#ifdef DISPLAY
  u8g2.begin();
  u8g2.setFontMode(0);                      // Enable transparent mode, which is faster
  u8g2.clearBuffer();                       // Clear the internal memory
  u8g2.setFont(u8g2_font_helvR14_tr);       // Choose a suitable font
  u8g2.drawStr(20, 14, "Bindicator");
  u8g2.setFont(u8g2_font_helvR14_tr);
  u8g2.drawStr(38, 32, VERSION);
  u8g2.sendBuffer();                        // Transfer internal memory to the display
#endif

  DPRINTLN("Mounting FS...");
  if (SPIFFS.begin()) {                                                 // If file system is valid get configuration data
    DPRINTLN("Mounted file system");
    if (SPIFFS.exists("/config.json")) {                                // File exists, reading and loading

      DPRINTLN("Reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        DPRINTLN("Opened config file");
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);                   // Allocate a buffer to store contents of the file
        configFile.readBytes(buf.get(), size);

        DynamicJsonDocument doc(size);                                 // Deserialise JSON data
        deserializeJson(doc, buf.get());
        DeserializationError err = deserializeJson(doc, buf.get());

        if (!err) {
          JsonObject root = doc[0];                                    // If the config file is valid, copy the script ID
          strcpy(GScriptId, root["GScriptId"]);
        }
        configFile.close();
      }
    }
  } else {
    DPRINTLN("Failed to mount FS");
  }

  unsigned long startTime = millis();
  ws2812fx.setMode(FX_MODE_COLOR_WIPE);
  ws2812fx.setColor(MAGENTA);
  while (digitalRead(TOUCH_PIN) == HIGH)      // If so wait until approx 5 seconds have passed
  {                                           // then start WiFi Configuration
    DPRINTLN("Touch pin high");
    if (millis() > startTime + (CONFIG_DELAY))
    {
      configWiFi();   
      return;
    }    
    ws2812fx.service();
  }

  DPRINTLN();                               // Debugging info
  DPRINT("Connecting to WiFi: ");

  WiFi.softAPdisconnect(true);              // Stop broadcasting SSID from setup mode (ESP8266 bug)
  WiFi.begin();  
  
  ws2812fx.setColor(WHITE);
  ws2812fx.setMode(FX_MODE_FADE);           
  
  startTime = millis();  
  while (WiFi.status() != WL_CONNECTED) {
    ws2812fx.service();
    DPRINT(".");
    if (millis() > startTime + (WIFI_TIMEOUT)) 
    {                                                   // It's trivial to change this to start WiFi configuration
      ws2812fx.fill(ws2812fx.ColorHSV(RED));            // at this point.
      ws2812fx.show();
      DPRINTLN();
      DPRINTLN("WiFi Timeout");
      while (true) 
      { 
        if (millis() < WATCHDOG_TIMEOUT)        // Wait 60 seconds then reboot in attempt to recover WiFi
          ESP.wdtFeed(); 
        }
#ifdef DISPLAY
      u8g2.clearBuffer();                       // Clear the internal memory
      u8g2.setFont(u8g2_font_helvR14_tr);       // Choose a suitable font
      u8g2.drawStr(0, 14, "Error joining");     // Write title to the buffer
      u8g2.drawStr(0, 32, "WiFi network");
      u8g2.sendBuffer();                        // Transfer internal memory to the display
#endif
      return;
    }
  }
  DPRINTLN("");                             // Debugging info
  DPRINTLN("WiFi connected");
  DPRINTLN("IP address: ");
  DPRINTLN(WiFi.localIP());

  ts.init();                                // Task Scheduler initialisation
  ts.addTask(tShowEventColor);              // Add tasks
  ts.addTask(tUpdateData);                  // ...
  ts.addTask(tCancelEvents);                // ...
  ts.addTask(tWiFiWatchdog);                // ....
  tUpdateData.enable();                     // Connected and ready to start getting event data
  tCancelEvents.enable();                   // Allow use of capacitive button
  tWiFiWatchdog.enable();
}

void loop() {
  ts.execute();                               // Task scheduler handles all repeating functions
  ws2812fx.service();                         // WS2812 animation 
}

void neopixelCallback() {
  DPRINTLN(ESP.getFreeHeap());  
  if (eventList.getLength() > 0)              // If items to show...
  {
    switch (eventList.getLength())
    {
      case 1:
      if (ws2812fx.getMode() != FX_MODE_BREATH)
        ws2812fx.setMode(FX_MODE_BREATH);
      break;

      case 2:
      if (ws2812fx.getMode() != FX_MODE_STATIC)
        ws2812fx.setMode(FX_MODE_STATIC);
      break;

      default:
      if (ws2812fx.getMode() != FX_MODE_STATIC)
        ws2812fx.setMode(FX_MODE_STATIC);
    }
    
    struct Event e = eventList.getCurrent();
    ws2812fx.setColor(e.color);
    eventList.loop();                         // Move to the next item
#ifdef DISPLAY
    u8g2.clearBuffer();                       // Clear the internal memory
    u8g2.setFont(u8g2_font_helvR14_tr);       // Choose a suitable font
    u8g2.drawStr(0, 14, "Next collection");   // Write title to the buffer
    u8g2.drawStr(0, 32, e.title);
    DPRINTLN(e.title);
    u8g2.sendBuffer();                        // Transfer internal memory to the display
#endif
  } else
  {
    tidyUp();

#ifdef DISPLAY
    switch (page % 3)
    {
      case 0:                                     // Page 1
        u8g2.clearBuffer();                       // Clear the internal memory
        u8g2.setFont(u8g2_font_helvR14_tr);       // Choose a suitable font
        u8g2.drawStr(0, 14, "Free heap");         // Write the free heap to the buffer
        char buf[32];
        itoa(ESP.getFreeHeap(), buf, 10);
        u8g2.drawStr(0, 32, buf);
        u8g2.drawStr(53, 32, "kb");
        u8g2.sendBuffer();                        // Transfer internal memory to the display
        break;

      case 1:                                     // Page 2
        u8g2.clearBuffer();                       // Clear the internal memory
        u8g2.setFont(u8g2_font_helvR14_tr);       // Choose a suitable font
        u8g2.drawStr(0, 14, "IP address");        // Write the title to the buffer
        u8g2.drawStr(0, 32, 
        WiFi.localIP().toString().c_str());       // Write the IP address to the buffer
        u8g2.sendBuffer();                        // Transfer internal memory to the display
        break;

      case 2:                                     // Page 3
        long t = 
        ts.timeUntilNextIteration(tUpdateData);   // Time until task runs again in millis
        u8g2.clearBuffer();                       // Clear the internal memory
        u8g2.setFont(u8g2_font_helvR14_tr);       // Choose a suitable font
        if (t == 0) {
          u8g2.drawStr(0, 14, "Next check");
          u8g2.drawStr(0, 32, "in progress.");
        }
        if (t == -1) {
          u8g2.drawStr(0, 14, "Calendar sync");
          u8g2.drawStr(0, 32, "is disabled.");
        }
        else {
          const char* text = showTimeFormatted(t);
          int mins = t / 60000;
          DPRINTLN(text);
          u8g2.drawStr(0, 14, "Next update");
          u8g2.drawStr(22, 32, "mins");
          u8g2.setCursor(0, 32);       
          u8g2.print(mins);
        }
        u8g2.sendBuffer();
    }
  }
#endif
}

void cancelEventsCallback() {
  if (digitalRead(TOUCH_PIN) == HIGH && eventList.getLength() > 0)   // Only run if the button is held down
  {                                                                  // and there are events to cancel
#ifdef DISPLAY
    u8g2.clearBuffer();                       // Clear the internal memory
    u8g2.setFont(u8g2_font_helvR14_tr);       // Choose a suitable font
    u8g2.drawStr(0, 14, "Cancelling");        // Write title to the buffer
    u8g2.drawStr(0, 32, "reminder...");
    u8g2.sendBuffer();                        // Transfer internal memory to the display
#endif
    ws2812fx.setColor(MAGENTA);
    ws2812fx.setMode(FX_MODE_STATIC);
    ws2812fx.service();

    // Use HTTPSRedirect class to create a new TLS connection
    client = new HTTPSRedirect(httpsPort);
    client->setInsecure();
    client->setPrintResponseBody(true);
    client->setContentTypeHeader("application/json");

    DPRINT("Connecting to ");
    DPRINTLN(host);

    // Try to connect for a maximum of 5 times
    bool flag = false;
    for (int i = 0; i < 5; i++) {
      int retval = client->connect(host, httpsPort);
      if (retval == 1) {
        flag = true;
        break;
      }
      else
        DPRINTLN("Connection failed. Retrying...");
    }

    if (!flag) {
      DPRINT("Could not connect to server: ");
      DPRINTLN(host);
      DPRINTLN("Exiting...");
      return;
    }

    client->POST(clearUrl, host, "", false);        // Send empty POST string

    delete client;                                  // Delete HTTPSRedirect object
    client = nullptr;                               // COMMENT THIS LINE IF PROGRAM CRASHES

    tidyUp();

    eventList.Clear();                              // Empty the event list
    getEventsCallback();                            // Update events list to make sure they were
    return;                                         // successfully cancelled
  }

#ifdef DISPLAY
  if (!SCROLLING && digitalRead(TOUCH_PIN) == HIGH)
      page++;                                       // Increment page number (modulo)
      else
        page++;
#endif

  unsigned long startTime = millis();
  if (digitalRead(TOUCH_PIN) == HIGH && eventList.getLength() == 0) 
  {
      modeIndex++;                                  // Cycle nightlight modes
      modeIndex = modeIndex % 3;
      ws2812fx.setMode(modes[modeIndex]);
      tidyUp();
      DPRINT("Mode: ");
      DPRINTLN(modeIndex);
  }
  }

void getEventsCallback()
{
  DPRINTLN(ESP.getFreeHeap());

  client = new HTTPSRedirect(httpsPort);            
  client->setInsecure();
  client->setPrintResponseBody(true);
  client->setContentTypeHeader("application/json"); // Expecting a JSON response

  DPRINTLN("Connecting to ");
  DPRINTLN(host);

  // Try to connect for a maximum of 5 times
  bool flag = false;
  for (int i = 0; i < 5; i++) {
    int retval = client->connect(host, httpsPort);
    if (retval == 1) {
      flag = true;
      break;
    }
    else
      DPRINTLN("Connection failed. Retrying...");
  }

  if (!flag) {
    DPRINTLN("Could not connect to server: ");
    DPRINTLN(host);
    DPRINTLN("Exiting...");
    return;
  }

  if (client->GET(eventsUrl, host))
    if (client->getStatusCode() == 200)             // Valid response
    {
      DPRINTLN(ESP.getFreeHeap());
      DPRINTLN("Response Body: ");
      parseJson(client->getResponseBody());
      DPRINT(client->getResponseBody());
    }

    if (client->getStatusCode() == 404)             // 404 - script ID probably wrong
    {
      DPRINTLN("404 - confirm Gscript ID");
      pixel.fill(pixel.ColorHSV(RED));         
      pixel.show();
#ifdef DISPLAY
      u8g2.clearBuffer();                         // Clear the internal memory
      u8g2.setFont(u8g2_font_helvR14_tr);         // Choose a suitable font
      u8g2.drawStr(0, 14, "Update failed");       // Write title to the buffer
      u8g2.drawStr(0, 32, "Check API ID");
      u8g2.sendBuffer();                          // Transfer internal memory to the display
#endif
    }

  // Delete HTTPSRedirect object
  delete client;
  client = nullptr;
  tShowEventColor.enableDelayed(SECOND * 3);      // Brief delay so errors visible to user
}

void parseJson(String json) {

  const size_t capacity = JSON_ARRAY_SIZE(3) + 3 * JSON_OBJECT_SIZE(2) + 1024; // Additional bytes for string duplication
  DynamicJsonDocument doc(capacity);

  deserializeJson(doc, json);
  DeserializationError err = deserializeJson(doc, json);

  if (err) {
    DPRINTLN(F("deserializeJson() failed with code "));
    DPRINTLN(err.c_str());
    return;
  }

  JsonObject root;                                 // This object contains an array of JSON events
  eventList.Clear();                               
  for (int i = 0; i < doc.size(); i++)
  {
    DPRINTLN(ESP.getFreeHeap());
    struct Event e;                                // For each event, create a struct and populate data
    root = doc[i];
    e.title = root["title"];
    e.color = eventColor2RealColor(atoi(root["color"]));
    eventList.Append(e);                           // Add event to list
  }
}

/******************** WIFI FUNCTIONS ************************/

void configWiFi() {
#ifdef DISPLAY
    u8g2.clearBuffer();                       // Clear the internal memory
    u8g2.setFont(u8g2_font_helvR14_tr);       // Choose a suitable font
    u8g2.drawStr(24, 23, "WiFi Setup");       // Write title to the buffer
    u8g2.sendBuffer();                        // Transfer internal memory to the display
#endif

  WiFiManager wifiManager;                                 // Handles WiFi configuration via captive portal
  wifiManager.setSaveConfigCallback(saveConfigCallback);   // This function called if data updated
  wifiManager.addParameter(&custom_gScriptId);             // Add a custom parameter for the Google Apps ID
  wifiManager.startConfigPortal(PORTAL_SSID);              // Start an open access point
  DPRINTLN("Wifi Configuration started");
}

void saveConfigCallback()
{
  strcpy(GScriptId, custom_gScriptId.getValue());          // Read updated parameters

  DPRINTLN("Saving config");
  StaticJsonDocument<128> doc;                             
  doc["GScriptId"] = GScriptId;                            

  File configFile = SPIFFS.open("/config.json", "w");      
  if (!configFile) {
    DPRINTLN("Failed to open config file for writing");
  }

  if (serializeJson(doc, configFile) == 0) {
    DPRINTLN(F("Failed to write to file"));
  }
  configFile.close();

  DPRINTLN("Restarting in 5 seconds");
#ifdef DISPLAY
    u8g2.clearBuffer();                       // Clear the internal memory
    u8g2.setFont(u8g2_font_helvR14_tr);       // Choose a suitable font
    u8g2.drawStr(0, 14, "Restarting");        // Write title to the buffer
    u8g2.drawStr(0, 32, "in 5 seconds");
    u8g2.sendBuffer();                        // Transfer internal memory to the display
#endif
  delay(5000);
  ESP.restart();
}

void wiFiWatchdogCallback() {
  unsigned long startTime = millis();
  
  while (WiFi.status() != WL_CONNECTED)
  {
    if (millis() > startTime + WATCHDOG_TIMEOUT)
      ESP.restart();
    ws2812fx.setColor(RED);
    ws2812fx.setBrightness(BRIGHTNESS);
    ws2812fx.setMode(FX_MODE_BLINK);
    ws2812fx.service();
    WiFi.begin();
  }
}

/********************** NEOPIXEL HELPER FUNCTIONS ***************************/

// Needed to use the WS2812 LEDs
// MUST BE CALLED
void initialiseNeoPixel()
{
  ws2812fx.init();
  ws2812fx.setBrightness(BRIGHTNESS);
  ws2812fx.setSpeed(SPEED);
  ws2812fx.setColor(0x007BFF);
  ws2812fx.setMode(FX_MODE_STATIC);
  ws2812fx.setCustomMode(customEffect);
  ws2812fx.start();
}

uint32_t eventColor2RealColor(int eventColor)
{
  switch (eventColor)
  {
    case 0:
      return PINK;
      break;

    case 1:
      return CYAN;
      break;

    case 2:
      return ws2812fx.Color(75, 255, 120);
      break;

    case 3:
      return ws2812fx.Color(190, 175, 255);
      break;

    case 4:
      return ws2812fx.Color(255, 50, 50);
      break;

    case 5:
      return YELLOW;
      break;

    case 6:
      return ORANGE;
      break;

    case 7:
      return CYAN;
      break;

    case 8:
      return WHITE;
      break;

    case 9:
      return BLUE;
      break;

    case 10:
      return GREEN;
      break;

    case 11:
      return RED;
      break;

    default:
      return WHITE;
  }
}

void tidyUp() {
  if (ws2812fx.getMode() != modes[modeIndex])
      ws2812fx.setMode(modes[modeIndex]);
    if (ws2812fx.getMode() == FX_MODE_STATIC)
      ws2812fx.setColor(ws2812fx.ColorHSV(0, 0, NIGHT_BRT));
    if (ws2812fx.getMode() == FX_MODE_FIRE_FLICKER_SOFT)
      ws2812fx.setColor(0xE25822);
    if (ws2812fx.getMode() == FX_MODE_COMET || ws2812fx.getMode() == FX_MODE_LARSON_SCANNER)
      ws2812fx.setColor(0x0000FF);    
}

uint16_t customEffect(void) { 
  WS2812FX::Segment* seg = ws2812fx.getSegment(); 
  ws2812fx.clear();
  return seg->speed; // Return the delay until the next animation step (in ms)
}

const char* showTimeFormatted(long ms) {
  DPRINTLN(ms);
  long hours = 0;
  long mins = 0;
  long secs = 0;

  String minsText = " minutes"; 
  secs = ms / 1000;             
  mins = secs / 60;             
  hours = mins / 60;            
  secs = secs - (mins * 60);    // Subtract the coverted seconds to minutes in order to display 59 secs max
  mins = mins - (hours * 60);   // Subtract the coverted minutes to hours in order to display 59 minutes max

  // return days + hours_o + hours + mins_o + mins + secs_o + secs;
  String r;
  if (mins > 1)
    r = mins + minsText;
  else
    r = "one minute";
  DPRINTLN(r.c_str());
  return r.c_str();  

}
