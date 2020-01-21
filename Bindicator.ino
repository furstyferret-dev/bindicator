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
#define MINUTE            60 * SECOND
#define HOUR              60 * MINUTE

#define DISPLAY                         // Comment out if the ESP8266 doesn't have a display
#define VERSION           "v0.20b"      // Version information
#define PORTAL_SSID       "Bindicator"  // SSID for web portal
#define NEOPIXEL_PIN      13            // NeoPixel data pin
#define TOUCH_PIN         12            // Capacitive touch data pin
#define RESET_PIN         16            // Display reset
#define CLOCK_PIN         5             // Display clock
#define DATA_PIN          4             // Display data
#define NUMPIXELS         7             // Number of pixels on NeoPixel Ring
#define BRIGHTNESS        125           // NeoPixel brightness (0 - 255)
#define WIFI_TIMEOUT      10            // Attempts before WiFi connection times out
#define PULSE_DELAY       5             // Delay in ms between brightness step increments
#define CONFIG_DELAY      5             // Arbitary delay before enabling WiFi config at boot
#define LED_INTERVAL      2 * SECOND    // Interval in second between event colour changing
#define REFRESH_INTERVAL  20 * MINUTE   // Data refresh interval

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

struct Event {                          // All events contain a title and associated colour
  const char* title;                    // Google Apps script deals with calendar logic
  EventColor color;
};

LinkedList<struct Event> eventList;     // Linked List containing active events

Adafruit_NeoPixel pixel = Adafruit_NeoPixel(NUMPIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
#ifdef DISPLAY
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, RESET_PIN, CLOCK_PIN, DATA_PIN);
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

#ifdef DEBUG
  Serial.begin(115200);                     // Start serial communication
  Serial.flush();
#endif

  initialiseNeoPixel();                     // NeoPixel won't work without initialisation

#ifdef DISPLAY
  u8g2.begin();
  u8g2.setFontMode(0);                      // Enable transparent mode, which is faster
  u8g2.clearBuffer();                       // Clear the internal memory
  u8g2.setFont(u8g2_font_helvR14_tf);       // Choose a suitable font
  u8g2.drawStr(24, 14, "Bindicator");
  u8g2.setFont(u8g2_font_profont17_tf);
  u8g2.drawStr(43, 28, "v0.19b");
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
  WiFi.begin();                             // saved in the EEPROM.
  while (WiFi.status() != WL_CONNECTED) {
    pulseNeoPixel();
    wifiTimeoutCounter++;
    DPRINT(".");
    if (wifiTimeoutCounter == WIFI_TIMEOUT) // If it doesn't connect, the NeoPixel will turn red.
    { // It's trivial to change this to start WiFi configuration
      pixel.fill(pixel.ColorHSV(RED_HUE));  // at this point.
      pixel.show();
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
  ts.execute();                             // Task scheduler handles all repeating functions
}

void neopixelCallback() {
  DPRINTLN(ESP.getFreeHeap());
  if (eventList.getLength() > 0)            // If items to show...
  {
    struct RGB rgb;
    struct Event e = eventList.getCurrent();
    rgb = eventColor2RealColor(e.color);
    pixel.fill(pixel.Color(rgb.r, rgb.g, rgb.b));
    pixel.show();
    eventList.loop();                       // Move to the next item
  } else
  {
    pixel.fill(pixel.Color(0, 0, 0));       // Nothing to show, turn light off
    pixel.show();
  }

#ifdef DISPLAY
  u8g2.clearBuffer();                      // Clear the internal memory
  u8g2.setFont(u8g2_font_helvR14_tf);      // Choose a suitable font
  u8g2.drawStr(0, 14, "Free heap");        // Write something to the internal memory
  char buf[8];
  itoa(ESP.getFreeHeap(), buf, 10);
  u8g2.drawStr(0, 32, buf);
  u8g2.drawStr(53, 32, "kb");
  u8g2.sendBuffer();                       // Transfer internal memory to the display
#endif
}

void cancelEventsCallback() {
  if (digitalRead(TOUCH_PIN) == HIGH && eventList.getLength() > 0)   // Only run if the button is held down
  { // and there are events to cancel
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

    pixel.fill(pixel.Color(0, 0, 0));               // Light off
    pixel.show();

    getEventsCallback();                            // Update events list to make sure they were
    return;                                         // successfully cancelled
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
    if (client->getStatusCode() == 200)
    {
      DPRINTLN(ESP.getFreeHeap());
      DPRINTLN("Response Body: ");
      parseJson(client->getResponseBody());
      DPRINT(client->getResponseBody());
    }

  // Delete HTTPSRedirect object
  delete client;
  client = nullptr;
  tShowEventColor.enable();
}

void parseJson(String json) {

  const size_t capacity = JSON_ARRAY_SIZE(3) + 3 * JSON_OBJECT_SIZE(2) + 213; // Additional byes for string duplication
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
// ColorHSV takes hue (16 bit), saturation (8 bit), and brightness (8 bit)
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
      color.r = 25;
      color.g = 25;
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
