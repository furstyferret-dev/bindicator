#include <FS.h>
#include <WiFiManager.h>
#include <TaskScheduler.h>
#include <TaskSchedulerDeclarations.h>
#include <TaskSchedulerSleepMethods.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <Adafruit_NeoPixel.h>
#include "HTTPSRedirect.h"
#include "LinkedList.h"
#include "DebugMacros.h"

#define SECOND            1000
#define MINUTE            SECOND * 60
#define HOUR              MINUTE * 60

//#define DISPLAY                       // Comment out if the ESP8266 doesn't have a display
#define POWER_LED         true          // Illuminate the built-in LED when power supplied       
#define VERSION           "v2.06b"      // Version information
#define PORTAL_SSID       "Bindicator"  // SSID for web portal
#define SCROLLING         true          // If false, use the button to change page
#define NEOPIXEL_PIN      15            // NeoPixel data pin
#define TOUCH_PIN         13            // Capacitive touch data pin
#define RESET_PIN         16            // Display reset
#define CLOCK_PIN         5             // Display clock
#define DATA_PIN          4             // Display data
#define NUMPIXELS         14            // Number of pixels on NeoPixel Ring
#define BRIGHTNESS        255           // NeoPixel brightness (0 - 255)
#define WIFI_TIMEOUT      10            // Attempts before WiFi connection times out
#define PULSE_DELAY       5             // Delay in ms between brightness step increments
#define CONFIG_DELAY      5             // Arbitary delay before enabling WiFi config at boot
#define LED_INTERVAL      2 * SECOND    // Interval in second between event colour changing
#define REFRESH_INTERVAL  20 * MINUTE   // Data refresh interval
#define NIGHT_BRT         20            // Nightlight brightness (0 - 255)

enum EventColor {                       // Colours used by Google Calendar
  OFF = 0,
  PALE_BLUE,
  PALE_GREEN,
  MAUVE,
  PALE_RED,
  YELLOW,
  ORANGE,
  CYAN,
  GRAY,
  BLUE,
  GREEN,
  RED
};

struct RGB {                            // Neopixel colours defined by 8bit RGB values
  byte r;
  byte g;
  byte b;
};

const uint32_t RED_HUE = 0;             // Some predefined Neopixel hues
const uint32_t GREEN_HUE = 21845;
const uint32_t BLUE_HUE = 43691;
const uint32_t MAGENTA_HUE = 54613;
const uint32_t CYAN_HUE = 32768;
const uint32_t YELLOW_HUE = 10923;

bool nightlight = true;

struct Event {                          // All events contain a title and associated colour
  const char* title;                    // Google Apps script deals with calendar logic
  EventColor color;
};

LinkedList<struct Event> eventList;     // Linked List containing active events

Adafruit_NeoPixel pixel = Adafruit_NeoPixel(NUMPIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
#ifdef DISPLAY
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, RESET_PIN, CLOCK_PIN, DATA_PIN);
int page;                               // Incremented to change page when button touched and no collection scheduled
#endif

const char* host = "script.google.com";                                           // Base URL for Google Apps
const int httpsPort = 443;                                                        // Default HTTPS port
char *GScriptId = "AKfycbzcU2LsYk0ZVltDtBHlbgde_9fXQvYMuddsvGhHFIGcSl3wr_5k";     // Default script ID

String eventsUrl = String("/macros/s/") + GScriptId + "/exec";                    // URL to retrieve calendar entries [GET]
String clearUrl = String("/macros/s/") + GScriptId + "/exec?clear";               // URL to clear active events [POST]

HTTPSRedirect* client = nullptr;                                                  // This client handles the redirection used by Google
WiFiManagerParameter custom_gScriptId(                                            // Custom parameter if Google Script ID is changed
  "gscriptid", "Google App ID", GScriptId, 56);

void neopixelCallback();                                                          // Task scheduler function for updating the NeoPixel and display
void getEventsCallback();                                                         // Task scheduler function for refreshing event data
void cancelEventsCallback();                                                      // Task scheduler function for cancelling upcoming events

Scheduler ts;                                                                     // Task Scheduler handles all repeating functions
Task tShowEventColor(LED_INTERVAL, TASK_FOREVER, &neopixelCallback);              // See above
Task tUpdateData(REFRESH_INTERVAL, TASK_FOREVER, &getEventsCallback);
Task tCancelEvents(SECOND / 2, TASK_FOREVER, &cancelEventsCallback);

void setup() {
  pinMode(TOUCH_PIN, INPUT);                 // Set the pin connected to the touch sensor to input
  if (POWER_LED) {                           // Turn on the built-in LED if enabled above
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  }

  delay(1000);

#ifdef DEBUG
  Serial.begin(115200);                     // Start serial communication
  Serial.flush();
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
        std::unique_ptr<char[]> buf(new char[size]);                    // Allocate a buffer to store contents of the file
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

  int configWiFiCounter = 0;                // Check to see if capacitative button held.
  while (digitalRead(TOUCH_PIN) == HIGH)    // If so wait until approx 5 seconds have passed
  { // then start WiFi Configuration
    DPRINTLN("Touch pin high");
    if (configWiFiCounter == CONFIG_DELAY)
      configWiFi();
    pulseNeoPixel(MAGENTA_HUE);             // Pulse magenta while sense line high (ie button pressed)
    configWiFiCounter++;
  }

  DPRINTLN();                               // Debugging info
  DPRINT("Connecting to WiFi: ");

  int wifiTimeoutCounter = 0;               // Connect to WiFi. WiFi.begin uses last valid ssid / password
  WiFi.softAPdisconnect(true);              // Stop broadcasting SSID from setup mode (ESP8266 bug)
  WiFi.begin();                             // saved in the EEPROM.
  while (WiFi.status() != WL_CONNECTED) {
    pulseNeoPixel();
    wifiTimeoutCounter++;
    DPRINT(".");
    if (wifiTimeoutCounter == WIFI_TIMEOUT) // If it doesn't connect, the NeoPixel will turn red.
    {                                       // It's trivial to change this to start WiFi configuration
      pixel.fill(pixel.ColorHSV(RED_HUE));  // at this point.
      pixel.show();
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

  pixel.fill(pixel.Color(255, 255, 255));   // Connected, change light to solid white
  pixel.show();

  ts.init();                                // Task Scheduler initialisation
  ts.addTask(tShowEventColor);              // Add tasks
  ts.addTask(tUpdateData);                  // ...
  ts.addTask(tCancelEvents);                // ...
  tUpdateData.enable();                     // Connected and ready to start getting event data
  tCancelEvents.enable();                   // Allow use of capacitive button
}

void loop() {
  ts.execute();                               // Task scheduler handles all repeating functions
}

void neopixelCallback() {
  DPRINTLN(ESP.getFreeHeap());
  if (eventList.getLength() > 0)              // If items to show...
  {
    struct RGB rgb;
    struct Event e = eventList.getCurrent();
    rgb = eventColor2RealColor(e.color);
    pixel.fill(pixel.Color(rgb.r, rgb.g, rgb.b));
    pixel.show();
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
    if (!nightlight) { pixel.fill(pixel.Color(0, 0, 0)); }         // Nothing to show, turn light off
    else { pixel.fill(pixel.ColorHSV(0, 0, NIGHT_BRT)); }          // Dim glow

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
  pixel.show();
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
    pixel.fill(pixel.ColorHSV(MAGENTA_HUE));
    pixel.show();

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

    if (!nightlight) { pixel.fill(pixel.Color(0, 0, 0)); }         // Nothing to show, turn light off
    else { pixel.fill(pixel.ColorHSV(0, 0, NIGHT_BRT)); }          // Dim glow
    pixel.show();

    eventList.Clear();                              // Empty the event list
    getEventsCallback();                            // Update events list to make sure they were
    return;                                         // successfully cancelled
  }

#ifdef DISPLAY
  if (!SCROLLING && digitalRead(TOUCH_PIN) == HIGH)
      page++;                                         // Increment page number (modulo use)
      else
        page++;
#endif

  if (digitalRead(TOUCH_PIN) == HIGH && eventList.getLength() == 0) 
  {
    nightlight = !nightlight;
    if (!nightlight) { pixel.fill(pixel.Color(0, 0, 0)); }         // Nothing to show, turn light off
    else { pixel.fill(pixel.ColorHSV(0, 0, NIGHT_BRT)); }          // Dim glow
    pixel.show();

    DPRINTLN(nightlight);
  }
  }

void getEventsCallback()
{
  DPRINTLN(ESP.getFreeHeap());

  client = new HTTPSRedirect(httpsPort);            // Use HTTPSRedirect class to create a new TLS connection
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

    if (client->getStatusCode() == 404)            // 404 - script ID probably wrong
    {
      DPRINTLN("404 - confirm Gscript ID");
      pixel.fill(pixel.ColorHSV(RED_HUE));        // Show red light (briefly) 
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

  JsonObject root;                                // This object contains an array of JSON events
  eventList.Clear();                               // Clear the existing Linked List of events
  for (int i = 0; i < doc.size(); i++)
  {
    DPRINTLN(ESP.getFreeHeap());
    struct Event e;                                // For each event, create a struct and populate data
    root = doc[i];
    e.title = root["title"];
    e.color = (EventColor) atoi(root["color"]);
    eventList.Append(e);                           // Add event to list
  }
}

/******************** WIFI FUNCTIONS ************************/

void configWiFi()
{
  pixel.fill(pixel.ColorHSV(MAGENTA_HUE));
  pixel.show();

#ifdef DISPLAY
    u8g2.clearBuffer();                       // Clear the internal memory
    u8g2.setFont(u8g2_font_helvR14_tr);       // Choose a suitable font
    u8g2.drawStr(24, 23, "WiFi Setup");       // Write title to the buffer
    u8g2.sendBuffer();                        // Transfer internal memory to the display
#endif

  WiFiManager wifiManager;                                // WiFiManager provides configuration on local webserver
  wifiManager.setSaveConfigCallback(saveConfigCallback);   // This function called if data updated
  wifiManager.addParameter(&custom_gScriptId);             // Add a custom parameter for the Google Apps ID
  wifiManager.startConfigPortal(PORTAL_SSID);              // Start an open access point
  DPRINTLN("Wifi Configuration started");
}

void saveConfigCallback()
{
  strcpy(GScriptId, custom_gScriptId.getValue());          // Read updated parameters

  DPRINTLN("Saving config");
  StaticJsonDocument<128> doc;                            // Create a JSON document
  doc["GScriptId"] = GScriptId;                            // Set the values in the document

  File configFile = SPIFFS.open("/config.json", "w");      // Serialize JSON to file
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

/********************** NEOPIXEL HELPER FUNCTIONS ***************************/

// Needed to use the NeoPixel Ring
// MUST BE CALLED
void initialiseNeoPixel()
{
  pixel.begin();
  pixel.setBrightness(BRIGHTNESS);
  pixel.clear();
}

// Pulses the NeoPixel white with speed according to PULSE_DELAY.
void pulseNeoPixel()
{
  for (int i = 255; i > 125; i--)
  {
    pixel.fill(pixel.ColorHSV(0, 0, i));
    pixel.show();
    delay(PULSE_DELAY);
  }

  for (int i = 125; i < 255; i++)
  {
    pixel.fill(pixel.ColorHSV(0, 0, i));
    pixel.show();
    delay(PULSE_DELAY);
  }
}

// Pulses the NeoPixel white with speed according to PULSE_DELAY
// and colour as 16 bit color value
void pulseNeoPixel(uint32_t color)
{
  for (int i = 125; i < 255; i++)
  {
    pixel.fill(pixel.ColorHSV(color, 255, i));
    pixel.show();
    delay(PULSE_DELAY);
  }

  for (int i = 255; i > 125; i--)
  {
    pixel.fill(pixel.ColorHSV(color, 255, i));
    pixel.show();
    delay(PULSE_DELAY);
  }
}

// Takes an EventColor from the Google Calendar range
// and returns the closest matching (by eye) RGB value
// as an RGB struct. Returns white if no match.
struct RGB eventColor2RealColor(EventColor eventColor)
{
  struct RGB color;
  switch (eventColor)
  {
    case OFF:
      color.r = 0;
      color.g = 0;
      color.b = 0;
      return color;
      break;

    case PALE_BLUE:
      color.r = 50;
      color.g = 50;
      color.b = 255;
      return color;
      break;

    case PALE_GREEN:
      color.r = 25;
      color.g = 255;
      color.b = 50;
      return color;
      break;

    case MAUVE:
      color.r = 189;
      color.g = 173;
      color.b = 255;
      return color;
      break;

    case PALE_RED:
      color.r = 255;
      color.g = 50;
      color.b = 50;
      return color;
      break;

    case YELLOW:
      color.r = 255;
      color.g = 255;
      color.b = 0;
      return color;
      break;

    case ORANGE:
      color.r = 255;
      color.g = 165;
      color.b = 0;
      return color;
      break;

    case CYAN:
      color.r = 0;
      color.g = 255;
      color.b = 255;
      return color;
      break;

    case GRAY:
      color.r = 255;
      color.g = 255;
      color.b = 255;
      return color;
      break;

    case BLUE:
      color.r = 0;
      color.g = 0;
      color.b = 255;
      return color;
      break;

    case GREEN:
      color.r = 10;
      color.g = 255;
      color.b = 10;
      return color;
      break;

    case RED:
      color.r = 255;
      color.g = 0;
      color.b = 0;
      return color;
      break;

    default:
      color.r = 255;
      color.g = 255;
      color.b = 255;
      return color;
  }
}


const char* showTimeFormatted(long ms) {
  DPRINTLN(ms);
  long hours = 0;
  long mins = 0;
  long secs = 0;

  String minsText = " minutes"; 
  secs = ms / 1000;             // Set the seconds remaining
  mins = secs / 60;             // Convert seconds to minutes
  hours = mins / 60;            // Convert minutes to hours
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
