#include <Arduino.h>
/*
    ESP8266 Access Control Firmware for HSBNE's Sonoff TH10 based control hardware.
    Written by nog3 August 2018
    Refactored by nog3 Feb 2021
    Contribs: 
      pelrun (Sane rfid reading)
      jabeone (fix reset on some card reads bug)
      Skip-GCTS (Converted to Platformio) 27-4-2021
*/

// Uncomment the relevant device type for this device.
//#define DOOR
#define INTERLOCK
//#define KEYLOCKER

// Uncomment for RFID reader types.
//#define OLD
#define RF125PS

// Uncomment to enable serial messaging debug.
#define SERIALDEBUG

// Include all the libraries we need for this.
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <WebSocketsServer.h>
#include <WebSockets.h>

#ERROR Modify secrets.tmp and rename to secrets.h
#include "secrets.h" 

int checkinRate = 60; // How many seconds between standard server checkins.
int sessionCheckinRate = 60; // How many seconds between interlock session checkins.
int contact = 0; // Set default switch state, 1 for doors that are permanantly powered/fail-open.
uint32_t rfidSquelchTime = 5000; // How long after checking a card with the server should we ignore reads.

// Configure our output pins.
const int switchPin = 12; // This is the pin the relay is on in the TH10 board.
const int ledPin = 13; // This is an onboard LED, just to show we're alive.
const int statePin = 14; // This is the pin exposed on the TRRS plug on the sonoff, used for LED on interlocks.

// Initialise our base state vars.
int triggerFlag = 0; //State trigger for heartbeats and other useful blocking things.
uint32_t lastReadSuccess = 5000; // Set last read success base state. Setting to 5 seconds to make sure on boot it's going to ignore initial reads.
uint32_t lastId = 0; // Set lastID to nothing.
String sessionID = ""; // Set sessionID as null.
char currentColor = 'b'; // Default interlock status led color is blue, let's start there.
String curCacheHash = ""; // Current cache hash, pulled from cache file.
int useLocal = 0; // Whether or not to use local cache, set by heartbeat failures or manually.
uint8_t tagsLoaded = 0; // Whether or not we've loaded tags into memory.
uint32_t tagsArray[200]; // Where the int array of tags is loaded to from cache on heartbeat fail.

//Configure our objects.
HTTPClient client;
Adafruit_NeoPixel pixel(1, statePin, NEO_GRB + NEO_KHZ800);
ESP8266WebServer http(80);
WebSocketsServer webSocket = WebSocketsServer(81);
Ticker heartbeat;
Ticker heartbeatSession;

void ICACHE_RAM_ATTR log(String entry) {
#ifdef SERIALDEBUG
  Serial.println(entry);
#endif
  webSocket.broadcastTXT(String(millis()) + " " + entry);
  delay(10);
}



// ISR and RAM cached functions go here. Stuff we want to fire fast and frequently.
void ICACHE_RAM_ATTR idleHeartBeatFlag() {
  triggerFlag = 1;
}

void statusLight(char color)
{
	if (currentColor == color)
	{
		return;
	}
	else
	{
		switch (color)
		{
		case 'r':
		{
			pixel.setPixelColor(1, pixel.Color(255, 0, 0));
			break;
		}
		case 'g':
		{
			pixel.setPixelColor(1, pixel.Color(0, 255, 0));
			break;
		}
		case 'b':
		{
			pixel.setPixelColor(1, pixel.Color(0, 0, 255));
			break;
		}
		case 'y':
		{
			pixel.setPixelColor(1, pixel.Color(255, 100, 0));
			break;
		}
		case 'p':
		{
			pixel.setPixelColor(1, pixel.Color(128, 0, 128));
			break;
		}
		case 'w':
		{
			pixel.setPixelColor(1, pixel.Color(255, 255, 255));
			break;
		}
		}
		currentColor = color;
		pixel.show();
	}
}

void ICACHE_RAM_ATTR checkIn() {
  // Serial.println("[CHECKIN] Standard checkin begin");
  // Delay to clear wifi buffer.
  delay(10);
  String url = String(host) + "/api/" + deviceType + "/checkin/?secret=" + String(secret);
  log("[CHECKIN] Get:" + String(url));
  delay(10);
  std::unique_ptr<BearSSL::WiFiClientSecure>SSLclient(new BearSSL::WiFiClientSecure);
  SSLclient->setInsecure();
  client.begin(*SSLclient, url);

  // Start http request.
  int httpCode = client.GET();
  // httpCode will be negative on error
  delay(10);
  if (httpCode > 0) {
    // Serial.println("[CHECKIN] Code: " + String(httpCode));
    // Checkin succeeded.
    if (httpCode == HTTP_CODE_OK) {
      String payload = client.getString();
      log("[CHECKIN] Server response: " + payload);
      const size_t capacity = JSON_OBJECT_SIZE(3) + 70;
      DynamicJsonDocument doc(capacity);
      deserializeJson(doc, payload);
      if (doc["success"].as<String>() == "true") {
        String serverCacheHash = doc["hashOfTags"].as<String>();
        if (serverCacheHash != curCacheHash) {
          log("[CACHE] Cache hashes don't match, flagging update.");
          triggerFlag = 3;
        } else {
          triggerFlag = 0;
        }
      } else {
        triggerFlag = 4;
      }

    }
  } else {
    log("[CHECKIN] Error: " + client.errorToString(httpCode));
    statusLight('y');
    triggerFlag = 4;
  }
  client.end();
  // log("[CHECKIN] Checkin done.");
  delay(10);
}

// Cache Related Functions
void getCache() {
  log("[CACHE] Acquiring cache.");
  // Delay to clear wifi buffer.
  delay(10);
  String url = String(host) + "/api/" + deviceType + "/authorised/?secret=" + String(secret);
  log("[CACHE] Get:" + String(url));
  std::unique_ptr<BearSSL::WiFiClientSecure>SSLclient(new BearSSL::WiFiClientSecure);
  SSLclient->setInsecure();
  client.begin(*SSLclient, url);

  // Start http request.
  int httpCode = client.GET();
  // httpCode will be negative on error
  if (httpCode > 0) {
    // log("[SESSION] Code: " + String(httpCode));

    // Cache checkin succeeded.
    if (httpCode == HTTP_CODE_OK) {
      String payload = client.getString();
      log("[CACHE] Server Response: " + payload);
      File cacheFile = LittleFS.open("/authorised.json", "w");
      if (!cacheFile) {
        log("[CACHE] Error opening authorised json file.");
      } else {
        cacheFile.print(payload + '\n');
        cacheFile.close();
      }
      // Pull hash from the response, store in string in RAM.
      const size_t capacity = JSON_ARRAY_SIZE(200) + JSON_OBJECT_SIZE(2) + 1240;
      DynamicJsonDocument doc(capacity);
      deserializeJson(doc, payload);
      curCacheHash = doc["authorised_tags_hash"].as<String>();
      // Clear the json object now.
      doc.clear();
    }
  } else {
    log("[CACHE] Error: " + client.errorToString(httpCode));
  }
  client.end();
  log("[CACHE] Cache acquisition done.");
  delay(10);
}

void printCache() {
  String cacheContent;
  File cacheFile = LittleFS.open("/authorised.json", "r");
  if (!cacheFile) {
    cacheContent = "Error opening authorised json file.";
  } else {
    cacheContent = cacheFile.readStringUntil('\n');
  }
  cacheFile.close();

  String message = "<html><head><title>" + String(deviceName) + " Cache</title></head>";
  message += "<h2>Cache:</h2>";
  message += cacheContent;
  http.send(200, "text/html", message);
}

void loadTags() {
  String cacheContent;
  File cacheFile = LittleFS.open("/authorised.json", "r");
  if (!cacheFile) {
    cacheContent = "Error opening authorised json file.";
  } else {
    cacheContent = cacheFile.readStringUntil('\n');
  }
  cacheFile.close();
  const size_t capacity = JSON_ARRAY_SIZE(200) + JSON_OBJECT_SIZE(2) + 1240;
  DynamicJsonDocument doc(capacity);
  deserializeJson(doc, cacheContent);
  JsonArray authorised_tags = doc["authorised_tags"];
  copyArray(authorised_tags, tagsArray);
  //Reclaim some of that memory usage.
  doc.clear();
}

void printTags() {
  String message = "<html><head><title>" + String(deviceName) + " Tags</title></head>";
  message += "<h2>Tags:</h2>";
  if (tagsArray[0] > 0) {
    for (uint8_t i = 0; i < sizeof(tagsArray); i++) {
      if (tagsArray[i] > 0) {
        message += String(tagsArray[i]) + "<br />";

      }
    }
  } else {
    message += "No tag loaded in slot 0, assuming none loaded.";
  }

  http.send(200, "text/html", message);
}

void clearTags() {
  log("[CACHE] Clearing tags array, we're back online.");
  memset(tagsArray, 0, sizeof(tagsArray));
  useLocal = 0;
  tagsLoaded = 0;
}

#include "DoorFunctions.esp"
#include "InterlockFunctions.esp"
#include "KeylockerFunctions.esp"
#include "RFID_Functions.esp"

void startWifi () {
  delay(10);
  // We start by connecting to a WiFi network
#ifdef SERIALDEBUG
  Serial.println();
  Serial.println();
  Serial.print("[WIFI] Connecting to ");
  Serial.println(ssid);
#endif
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.hostname(deviceName);

  // If we're setup for static IP assignment, apply it.
#ifdef USE_STATIC
  WiFi.config(ip, gateway, subnet);
#endif

  // Interlock Only: While we're not connected breathe the status light and output to serial that we're still connecting.

  while (WiFi.status() != WL_CONNECTED) {
    delay(50);
  }
#ifdef SERIALDEBUG
  Serial.println("[WIFI] WiFi connected");
  Serial.print("[WIFI] IP address: ");
  Serial.println(WiFi.localIP());
#endif
#ifdef INTERLOCK
  statusLight('w');
#endif
  delay(10);
}



void httpRoot() {
  String message = "<html><head><script>var connection = new WebSocket('ws://'+location.hostname+':81/', ['arduino']);connection.onopen = function () {  connection.send('Connect ' + new Date()); }; connection.onerror = function (error) {    console.log('WebSocket Error ', error);};connection.onmessage = function (e) {  console.log('Server: ', e.data); var logObj = document.getElementById('logs'); logObj.insertAdjacentHTML('afterend', e.data + '</br>');;};</script><title>" + String(deviceName) + "</title></head>";
  message += "<h1>" + String(deviceName) + "</h1>";
  message += "Last swiped tag was " + String(lastId)  + "<br />";
  if (contact == 1) {
    message += "Interlock is Active, Session ID is " + String(sessionID) + "<br />";
  }
  if (useLocal == 1) {
    message += "Local cache in use, server heartbeat failed <br />";
  }
  message += "Current cache hash is " + curCacheHash + " <br /> ";
  message += "<h2>Logs: </h2> <div id ='logs'> </div> ";
  http.send(200, "text/html", message);
}



void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      log(num + " Disconnected!");
      break;
    case WStype_CONNECTED: {
        log("[DEBUG] Client connected.");
      }
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(9600);
  Serial.println("[SETUP] Serial Started");
  pixel.begin();
  statusLight('p');
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  Serial.setTimeout(500);
  startWifi();
  // Set switch pin to output.
  pinMode(switchPin, OUTPUT);
  if (!contact) {
    digitalWrite(switchPin, LOW); // Set base switch state.
  } else {
    digitalWrite(switchPin, HIGH); // Set base switch state.
  }
  // Configure OTA settings.
  ArduinoOTA.setHostname(deviceName);
  ArduinoOTA.setPassword(devicePassword);


  ArduinoOTA.onStart([]() {
    log("[OTA] Start");
    statusLight('p');
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] End");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    yield();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  //Setup Websocket debug logger
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  //Setup HTTP debug server.
  http.on("/", httpRoot);

  http.on("/reboot", []() {
    http.sendHeader("Location", "/");
    // Redirect back to root in case chrome refreshes.
    http.send(200, "text/plain", "[DEBUG] Rebooting.");
    log("[DEBUG] Rebooting");
    ESP.reset();
  });
#ifdef DOOR
  http.on("/bump", []() {
    if (deviceType == "door") {
      http.send(200, "text/plain", "Bumping door.");
      log("[DEBUG] Bumped lock.");
      pulseContact();
    }
  });
#endif
  http.on("/checkin", []() {
    http.sendHeader("Location", "/");
    // Redirect back to root in case chrome refreshes.
    http.send(200, "text/plain", "[DEBUG] Checking in.");
    idleHeartBeatFlag();
  });
  http.on("/getcache", []() {
    getCache();
  });
  http.on("/printcache", []() {
    printCache();
  });
  http.on("/loadtags", []() {
    loadTags();
  });
  http.on("/printtags", []() {
    printTags();
  });
  http.on("/uselocal", []() {
    triggerFlag = 4;
  });
  http.on("/cleartags", []() {
    clearTags();
  });
  http.on("/end", []() {
    contact = 0;
    digitalWrite(switchPin, LOW);
    statusLight('b');
    http.sendHeader("Location", "/");
    http.send(200, "text/plain", "Ending Session.");
  });
  http.on("/authas", HTTP_GET, []() {
    String value = http.arg("cardid"); //this lets you access a query param (http://x.x.x.x/action1?value=1)
    authCard(value.toInt());
    http.sendHeader("Location", "/");
    http.send(200, "text/plain", "Authenticating as:" + value);

  });
  http.begin();
  log("[SETUP] HTTP server started");
  heartbeat.attach(checkinRate, idleHeartBeatFlag);
  delay(10);
  // Assume server is up to begin with and clear tags array.
  useLocal = 0;

  // Handle caching functions.
  if (!LittleFS.begin()) {
    log("[STORAGE]Failed to mount file system");
    return;
  } else {
    File cacheFile = LittleFS.open("/authorised.json", "r");
    if (!cacheFile) {
      log("[CACHE] Error opening authorised json file.");
      return;
    } else {
      String cacheBuf = cacheFile.readStringUntil('\n');
      cacheFile.close();
      const size_t capacity = JSON_ARRAY_SIZE(200) + JSON_OBJECT_SIZE(2) + 1240;
      DynamicJsonDocument doc(capacity);
      deserializeJson(doc, cacheBuf);
      curCacheHash = doc["authorised_tags_hash"].as<String>();
      // Clear the json object now.
      doc.clear();
    }
  }
}

void loop() {
  delay(10);
  // Check if any of our interrupts or other event triggers have fired.
  checkStateMachine();

  // Yield for 10ms so we can then handle any wifi data.
  delay(10);
  ArduinoOTA.handle();
  http.handleClient();
  webSocket.loop();

  // If it's been more than rfidSquelchTime since we last read a card, then try to read a card.
  if (millis() > (lastReadSuccess + rfidSquelchTime)) {
    if (!contact) {
      statusLight('b');
    } else {
      statusLight('g');
    }
    if (Serial.available()) {
      readTag();
      delay(10);
    }
    // If there was nothing useful in the serial buffer lets just tidy it up anyway.
    flushSerial();
    delay(10);
  }
  delay(10);
}
