
// PressForMusic
// Copyright 2020 Tyler Gunn
// Distributed under the Apache License v2.0 (see LICENSE)
// tyler@egunn.com
//
// This code is intended for Christmas Light enthusiasts using the Falcon Pi Player software.
// Although many set up their light display with an FM transmitter alone, it is often nice to include
// outdoor speakers for individuals walking up to the display.  Of course, it is undesirable to play the
// music aloud all the time as it can be distracting to neighbors.  This is where PressForMusic comes in.
// The user is able to press a button (or step on a foot switch) to supply power to your outdoor speakers.
// Unlike timer-based setups, PressForMusic will active your speakers for a set number of songs after
// the user presses the button.

// If you do not wish to timestamp the MQTT messages published for button presses, change this to
// undef instead of define.
#define USE_NTP

#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncTCP.h>
#include <Ticker.h>
#include <AsyncMqttClient.h>
#include <FS.h>   // Include the SPIFFS library
#ifdef USE_NTP
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#endif
#include <ArduinoHA.h> // For Home Assistant Integration


// ------------------------------------------------------------------------------------------------
// This section contains your wifi credentials.
// It is assumed that you're using DHCP to get an IP address; there is no strict need to have a
// static IP for this application.

#define WIFI_SSID "iot.egunn.com"
#define WIFI_PASSWORD "blah"

// ------------------------------------------------------------------------------------------------
// This section contains settings for the MQTT server you wish the device to connect to.

#define MQTT_SERVER_IP IPAddress(192, 168, 0, 208)
#define MQTT_SERVER_PORT 1883

#ifdef USE_NTP
// You can also use pool.ntp.org; however that's going to be an actual internet query.  My own IOT
// network where the ESP8266 runs does not have access to the internet, so I used an internal raspberry
// pi that is also an NTP server.
#define NTP_SERVER_ADDRESS "192.168.0.125"

// Define your timezone offset.  It's super intuitive right? number of seconds offset from GMT.
// GMT +1 = 3600
// Pacific daylight time is UTC -7 hr = -25200
// Though when daylight savings is in place PST is -8 hr = -28800
#define GMT_OFFSET -28800
#endif

// ------------------------------------------------------------------------------------------------
// This section contains MQTT topic definitions.  You shouldn't need to look into this much.

// This is the topic we will subscribe to on which we expect to receive basic idle/playing indications.
#define MQTT_FPP_STATUS_TOPIC "christmas/falcon/player/FPP/status"

// This is the topic we will subscribe to for the purpose of determining the name of the current playlist.
#define MQTT_FPP_PLAYLIST_NAME_TOPIC "christmas/falcon/player/FPP/playlist/name/status"

// This is the topic we will subscribe to for the purpose of determining which song within the playlist is
// currently playing.
#define MQTT_FPP_PLAYLIST_POSITION_TOPIC "christmas/falcon/player/FPP/playlist/sectionPosition/status"

// This is the topic we will subscribe to for the purpose of determining the name of the media file
// currently playing.
#define MQTT_FPP_PLAYLIST_SEQUENCE_STATUS_TOPIC "christmas/falcon/player/FPP/playlist/sequence/status"

// We will publish push events to this topic when there is music playing; this can be used to get a
// count of how many people press the button during your show.
const char MQTT_BUTTON_PRESS_COUNT_PLAYING_TOPIC[] = "christmas/pressForMusic2/countPlaying";

// We will publish push events to this topic when there is NOT music playing.
// This can be used to get a count of how many people push the button outside of the show.
const char MQTT_BUTTON_PRESS_COUNT_IDLE_TOPIC[] = "christmas/pressForMusic2/countIdle";

// We will both publish and subscribe to this topic to allow multiple ESP8266-based "press for music" devices to
// operate on the same network.  
// When the button is pressed on a device, we publish the IP address of the triggering device to this topic.
// All subscribing devices which do not have the same IP address will treat an incoming triggerDevice
// message as if it was a local button press.
// This enables some interesting use-cases:
// 1. A button which is mounted remotely from the speakers.  An ESP8266 can be used as a means to trigger the
// relay for speakers on another ESP8266 device.
// 2. Sync multiple "press for music" button/speaker pairings.  When one button is pressed all will turn on at the
// same time.
const char MQTT_BUTTON_TRIGGER_DEVICE_TOPIC[] = "christmas/pressForMusic2/triggerDevice";

const char MQTT_TRIGGER_PAYLOAD[] = "{\"event_type\": \"press\"}";
const String MQTT_TRIGGER_PAYLOAD_PREFIX = "{\"event_type\": \"press\", \"time\": ";
const String MQTT_TRIGGER_PAYLOAD_SUFFIX = "\"}";

// This is the topic we will public a signal to indicating that there was a trigger on your blueiris server.
// Change undef to define if you want to do this type of publish.
#undef MQTT_PUBLISH_TO_BLUEIRIS
#define MQTT_BLUEIRIS_TRIGGER_TOPIC "BlueIris/admin"
#define MQTT_BLUEIRIS_TRIGGER_PAYLOAD "camera=drive&trigger"

// ------------------------------------------------------------------------------------------------
// Configuration below this line should not need to be changed if you setup the device in the
// usual manner.

// This is the pin on your ESP8266 which has a relay connected to it which controls your
// speakers.
// I used some cheap 3V 1 channel opto-isolated relays.
// https://www.amazon.com/gp/product/B07ZM84BVX
// These were designed specically for high level triggering from an
// ESP8266 DIO pin.
#define SPEAKER_RELAY_PIN D1

// This is the pin on your ESP8233 which has the momentary switch connected to it.
// I used an industrial foot switch for mine:
// https://www.amazon.com/gp/product/B00EF9D2DY
#define SWITCH_PIN D5  

// When set to 1, indicates that the speakers shall be active until the
// end of the current song.
// When set to 2, indicates that the speakers shall be active until the
// end of the next song.
// ... etc
int SONG_COUNT = 2;

// ------------------------------------------------------------------------------------------------
// Constants.  Yay.

// Status expected on the MQTT_FPP_STATUS_TOPIC topic when nothing is playing.
#define FPP_STATUS_IDLE "idle"
// Status expected on the MQTT_FPP_STATUS_TOPIC topic when something is playing.
#define FPP_STATUS_PLAYING "playing"

#define DEBUG_SERIAL

byte deviceMac[6];
AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;
WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;
Ticker wifiReconnectTimer;

#ifdef USE_NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER_ADDRESS);
#endif

// Whether we know FPP is playing music or not.
bool isFppPlaying = false;

// File name for the preferences file which stores... the prefs!  Yay.
#define PREFERENCES "/prefs.txt"

// We use the current position in the playlist to know when songs change.
int currentPlaylistPosition = -1;
// We track the current button state and do some debouncing to ensure we get a stable
// indication of a button press.
int currentButtonState = -1;
int lastButtonState = -1;
int newButtonState = -1;

// How many songs are remaining before we shut off the speakers.
int songsRemaining = -1;
boolean overrideOn = false;

// Last time the button changed state; used with below to debounce input.
unsigned long lastButtonStateChangeTime = 0;
unsigned long debounceTime = 50;
unsigned long deactivePauseRelayTime = 0;

// Some things related to HA auto-discovery.
WiFiClient wifiClient;
HADevice haDevice(deviceMac, sizeof(deviceMac));
HAMqtt mqtt(wifiClient, haDevice);
HABinarySensor pressForMusicButton("press_for_music");
HABinarySensor speakerStatus("speaker_status");
HASwitch speakerForceOnState("speaker_force_on_state");

// Used to track our own Ip address.
String myIpAddress;

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Used for building the web UX; returns number of songs remaining.
String getSongsLeft() {
  return String(songsRemaining);
}

// Used for building the web UX; returns the state of the speakers.
String getState() {
  if (!overrideOn && songsRemaining <= -1) {
    return "off";
  } else {
    return "on";
  }
}

// Used for building the web UX; returns the override state.
String getOverrideState() {
  if (overrideOn) {
    return "on";
  } else {
    return "off";
  }
}

// Used for building the web UX; returns the number of songs for which we will
// keep the speakers on.
String getSongsOnPref() {
  return String(SONG_COUNT);
}

void forceSpeakersOn() {
  #ifdef DEBUG_SERIAL
    Serial.println("forceSpeakersOn");
  #endif
  overrideOn = true;
  digitalWrite(SPEAKER_RELAY_PIN, HIGH);
  speakerStatus.setState(true);
  speakerForceOnState.setState(true);
}

void forceSpeakersOff() {
  #ifdef DEBUG_SERIAL
    Serial.println("forceSpeakersOff");
  #endif
  overrideOn = false;
  deactiveSpeakers();
  speakerForceOnState.setState(false);
}

// Shut off the speaker relay.
void deactiveSpeakers() {
  #ifdef DEBUG_SERIAL
    Serial.println("Deactivate speakers");
  #endif
  if (!overrideOn) {
    digitalWrite(SPEAKER_RELAY_PIN, LOW);
    speakerStatus.setState(false);
  }
  songsRemaining = -1;
}

// Connect to the wifis.
void connectToWifi() {
  #ifdef DEBUG_SERIAL
    Serial.println("Connecting to Wi-Fi...");
  #endif
  // Get the WIFI mac; this is required for 
  WiFi.macAddress(deviceMac);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  haDevice.setName("Push For Music Button");
  haDevice.setManufacturer("Tyler Gunn");
  haDevice.setSoftwareVersion("1.0.0");
  pressForMusicButton.setCurrentState(isButtonPressed());
  pressForMusicButton.setIcon("mdi:button-pointer");
  pressForMusicButton.setName("Press For Music");
  speakerStatus.setCurrentState(false);
  speakerStatus.setIcon("mdi:toggle-switch-variant");
  speakerStatus.setName("Speaker State");
  speakerForceOnState.setCurrentState(false);
  speakerForceOnState.setIcon("mdi:toggle-switch-variant");
  speakerForceOnState.setName("Speaker Force On");
  speakerForceOnState.onCommand(onSwitchCommand);
}

void saveSongCountPref() {
  File file = SPIFFS.open(PREFERENCES, "w");
  if (!file) {
    #ifdef DEBUG_SERIAL
      Serial.println("Failed to create prefs");
    #endif
    return;
  }
  file.println(SONG_COUNT);
  #ifdef DEBUG_SERIAL
    Serial.println("No preference found; created");
  #endif
}


// Handle incoming switch activations mentioned from Home Assistant.
void onSwitchCommand(bool state, HASwitch* sender)
{
    if (sender == &speakerForceOnState) {
        #ifdef DEBUG_SERIAL
          if (state) {
            Serial.println("Home Assistant asked for speakers on");
            forceSpeakersOn();
          } else {
            Serial.println("Home Assistant asked for speakers off");
            forceSpeakersOff();
          }
        #endif
        // Ack back to HA since we'll do what we are asked.
        speakerForceOnState.setState(state);
    }
}

// Async callback received when the wifis are up; connect to MQTT at this point.
void onWifiConnect(const WiFiEventStationModeGotIP& event) {
  #ifdef DEBUG_SERIAL
    Serial.println("Connected to Wi-Fi.");
    Serial.print("My IP: ");
    Serial.println(WiFi.localIP());
  #endif
  myIpAddress = WiFi.localIP().toString();
  connectToMqtt();

  // There's still time!
  #ifdef USE_NTP
    timeClient.begin();
    timeClient.setTimeOffset(GMT_OFFSET);
  #endif

  // Configure the various web requests.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Woot! Got http request");
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });
  
  // Route to load style.css file
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/style.css", "text/css");
  });

  // Dynamic query for songs remaining
  server.on("/songsleft", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", getSongsLeft().c_str());
  });

  // Dynamic query for speaker on state
  server.on("/state", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", getState().c_str());
  });

  // Dynamic query for speaker override on state
  server.on("/overridestate", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", getOverrideState().c_str());
  });

  // Turn speakers on!
  server.on("/on", HTTP_GET, [](AsyncWebServerRequest *request){
    forceSpeakersOn();
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });

  // Turn speakers off!
  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request){
    forceSpeakersOff();
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });

  // Change the song count preference
  server.on("/songsonpref", HTTP_GET, [](AsyncWebServerRequest *request){
    for(int i=0;i<request->params();i++){
      AsyncWebParameter* p = request->getParam(i);
      if (p->name() == "songsonpref") {
        Serial.print("Param name: ");
        Serial.println(p->name());
        Serial.print("Param value: ");
        Serial.println(p->value());
        SONG_COUNT = p->value().toInt();
        saveSongCountPref();
      }
    }
    
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });

  server.begin();
}

// Async callback received when wifi connection is lost; we attempt to auto-reconnect.
void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
  #ifdef DEBUG_SERIAL
    Serial.print("Disconnected from Wi-Fi; reason = ");
    Serial.println(event.reason);
  #endif
  mqttReconnectTimer.detach(); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  wifiReconnectTimer.once(2, connectToWifi);
}

// Pretty sure you can guess what this does.
void connectToMqtt() {
  #ifdef DEBUG_SERIAL
    Serial.println("Connecting to MQTT...");
  #endif
  mqttClient.connect();
}

// Async callback received when MQTT connection was successfully established.
// Subscribes to both the playing status and playlight status topics.
void onMqttConnect(bool sessionPresent) {
  #ifdef DEBUG_SERIAL
    Serial.println("Connected to MQTT.");
    Serial.print("Session present: ");
    Serial.println(sessionPresent);
  #endif
  uint16_t packetIdSub = mqttClient.subscribe(MQTT_FPP_STATUS_TOPIC, 2);
  packetIdSub = mqttClient.subscribe(MQTT_FPP_PLAYLIST_POSITION_TOPIC, 2);

  // Track triggers from all devices.
  packetIdSub = mqttClient.subscribe(MQTT_BUTTON_TRIGGER_DEVICE_TOPIC, 2);
}

// Async callback received when connection to MQTT server is lost.
// We try to reconnect.
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  #ifdef DEBUG_SERIAL
    Serial.println("Disconnected from MQTT.");
  #endif
  if (WiFi.isConnected()) {
    mqttReconnectTimer.once(2, connectToMqtt);
  }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  #ifdef DEBUG_SERIAL
    Serial.println("Subscribe acknowledged.");
    Serial.print("  packetId: ");
    Serial.println(packetId);
    Serial.print("  qos: ");
    Serial.println(qos);
  #endif
}

void onMqttUnsubscribe(uint16_t packetId) {
  #ifdef DEBUG_SERIAL
    Serial.println("Unsubscribe acknowledged.");
    Serial.print("  packetId: ");
    Serial.println(packetId);
  #endif
}

void onMqttPublish(uint16_t packetId) {
  Serial.println("Publish acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

// Handles incoming MQTT messages for our topics.
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  // Change in playing status
  if (strcmp(topic, MQTT_FPP_STATUS_TOPIC) == 0) {    
    if (strncmp(payload, FPP_STATUS_IDLE, len) == 0) {
      #ifdef DEBUG_SERIAL
        Serial.println("  << FPP is idle");
      #endif
      isFppPlaying = false;
      currentPlaylistPosition = -1;
      deactiveSpeakers();
    } else if (strncmp(payload, FPP_STATUS_PLAYING, len) == 0) {
      #ifdef DEBUG_SERIAL
        Serial.println("  << FPP Is playing");
      #endif
      isFppPlaying = true;
    }
  // Change in playlist position
  } else if (strcmp(topic, MQTT_FPP_PLAYLIST_POSITION_TOPIC) == 0) {
    char tempPosition[len + 1];
    strncpy(tempPosition, payload, len);
    tempPosition[len] = '\0';

    int newPosition = atoi(tempPosition);

    if (newPosition == 0 && tempPosition[0] != '0') {
      #ifdef DEBUG_SERIAL
        Serial.println("  << FPP position in playlist: invalid position");
      #endif
      return;
    }
    #ifdef DEBUG_SERIAL
    Serial.print("  << FPP position in playlist is: ");
    Serial.println(newPosition);
    #endif
    if (newPosition != currentPlaylistPosition && isFppPlaying) {
      songsRemaining--;
      if (songsRemaining < 0) {
        songsRemaining = 0;
      }
      #ifdef DEBUG_SERIAL
        Serial.println("-- New Song --");
        Serial.print("Songs remaining: ");
        Serial.println(songsRemaining);
      #endif
      if (songsRemaining == 0) {
        deactiveSpeakers();
      }
    }
  // Change in media playing
  } else if (strcmp(topic, MQTT_FPP_PLAYLIST_SEQUENCE_STATUS_TOPIC) == 0) {
    char tempMedia[len + 1];
    strncpy(tempMedia, payload, len);
    tempMedia[len] = '\0';

    #ifdef DEBUG_SERIAL
    Serial.print("  << FPP sequence play status:|");
    Serial.print(tempMedia);
    Serial.println("|");
    #endif

  // Monitor button presses on other devices (and our own).
  } else if (strcmp(topic, MQTT_BUTTON_TRIGGER_DEVICE_TOPIC) == 0) {
    char tempIp[len + 1];
    strncpy(tempIp, payload, len);
    tempIp[len] = '\0';

    if (strcmp(tempIp, myIpAddress.c_str()) == 0) {
      #ifdef DEBUG_SERIAL
        Serial.println(" Button press received via MQTT; local ignored.");
      #endif
    } else {
      #ifdef DEBUG_SERIAL
        Serial.print(" Button press from device: ");
        Serial.println(tempIp);
      #endif
      handleButtonDown(false);
    }
  }
}

void initializeFilesystem() {
  #ifdef DEBUG_SERIAL
    Serial.println("Filesystem initializing...");
  #endif
  if (!SPIFFS.exists(PREFERENCES)) {
    saveSongCountPref();
  } else {
    File file = SPIFFS.open(PREFERENCES, "r");
    if (!file) {
      #ifdef DEBUG_SERIAL
        Serial.println("Failed to read prefs");
      #endif
      return;
    }
    if (file.available()) {
      String songCount = file.readStringUntil('\n');
      SONG_COUNT = songCount.toInt();
    }
    #ifdef DEBUG_SERIAL
      Serial.print("Preferences exist; loaded soundCount=");
      Serial.println(SONG_COUNT);
    #endif

  }
  
}

// Replaces placeholder with LED state value
String processor(const String& var){
  Serial.println(var);
  if (var == "SONGSLEFT"){
    return getSongsLeft();
  } else if (var == "STATE"){ 
    return getState();
  } else if (var == "OVERRIDESTATE") {
    return getOverrideState();
  } else if (var == "SONGSONPREF") {
    return getSongsOnPref();
  }
  return "";
}

// Set things up.
void setup() {
  // Setup the relay and switch pins.
  pinMode(SPEAKER_RELAY_PIN, OUTPUT);
  // Let's just be sure that the relay is off on boot.
  digitalWrite(SPEAKER_RELAY_PIN, LOW);
  
  pinMode(SWITCH_PIN, INPUT_PULLUP);
  newButtonState = digitalRead(SWITCH_PIN);
  
  Serial.begin(115200);
  Serial.println();
  Serial.println();

  wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
  wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onSubscribe(onMqttSubscribe);
  mqttClient.onUnsubscribe(onMqttUnsubscribe);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onPublish(onMqttPublish);
  mqttClient.setServer(MQTT_SERVER_IP, MQTT_SERVER_PORT);

  mqtt.begin(MQTT_SERVER_IP);

  SPIFFS.begin();
  initializeFilesystem();

  connectToWifi();
}

void handleButtonDown(bool publishPress) {
  const char* topic;
  

  #ifdef USE_NTP
    // Since ESP8266 doesn't have a realtime clock, we need to query NTP for the time/date.
    timeClient.update();

    unsigned long epochTime = timeClient.getEpochTime();
    #ifdef DEBUG_SERIAL
    Serial.print("Epoch Time: ");
    Serial.println(epochTime);
    #endif
    struct tm *timeinfo = gmtime ((time_t *)&epochTime);
    char dateString[80];
    strftime(dateString,80,"%Y-%m-%dT%H:%M:%S", timeinfo);
    String dateStringObj = dateString;
    #ifdef DEBUG_SERIAL
    Serial.print("Formatted time: ");
    Serial.println(dateString);
    #endif
    String payload = "{\"event_type\": \"press\", \"time\":\"" + dateStringObj + "\"}";
  #else
    String payload = MQTT_TRIGGER_PAYLOAD;
  #endif

  if (isFppPlaying) {
    songsRemaining = SONG_COUNT;
    #ifdef DEBUG_SERIAL
      Serial.print("Activate speakers - songs remaining: ");
      Serial.println(songsRemaining);
    #endif
    digitalWrite(SPEAKER_RELAY_PIN, HIGH);
    topic = MQTT_BUTTON_PRESS_COUNT_PLAYING_TOPIC;
    #ifdef MQTT_PUBLISH_TO_BLUEIRIS
      // Only notify BlueIris if the press originated on this device to avoid over-triggering
      // the cameras
      if (publishPress) {
        mqttClient.publish(MQTT_BLUEIRIS_TRIGGER_TOPIC, 2, false, MQTT_BLUEIRIS_TRIGGER_PAYLOAD);
      }
    #endif
  } else {
    #ifdef DEBUG_SERIAL
      Serial.println("  << idle press");
    #endif
    topic = MQTT_BUTTON_PRESS_COUNT_IDLE_TOPIC;
  }
  // Only publish presses on the original triggering devices to avoid overstating presses.
  if (publishPress) {
    speakerStatus.setState(true);
    mqttClient.publish(topic, 2, false, payload.c_str());
  }

  // Let other devices know that the button was pressed, but only if we're not triggering because another
  // device sent an MQTT packet.  We don't want this to go on indefinitely.
  if (publishPress) {
    #ifdef DEBUG_SERIAL
      Serial.println("  Publishing local press");
    #endif
    mqttClient.publish(MQTT_BUTTON_TRIGGER_DEVICE_TOPIC, 2, false, myIpAddress.c_str());
  }
}

boolean isButtonPressed() {
  return newButtonState == LOW;
}

// The main event loop.
void loop() {
  newButtonState = digitalRead(SWITCH_PIN);

  // Track when the state changes; the DIO is dirty and will often transition states
  // a bunch of times when the button is pressed or released.
  if (newButtonState != lastButtonState && lastButtonState != -1) {
    lastButtonStateChangeTime = millis();
  }

  // Only handle the state change if it ocurred after the debounce time.
  if ((millis() - lastButtonStateChangeTime) > debounceTime && newButtonState != currentButtonState) {
    if (isButtonPressed()) {
      #ifdef DEBUG_SERIAL
        Serial.println("  << BUTTON DOWN");
      #endif
      handleButtonDown(true);
      pressForMusicButton.setState(true);
    } else {
      #ifdef DEBUG_SERIAL
        Serial.println("  << BUTTON UP");
      #endif
      pressForMusicButton.setState(false);
    }
    currentButtonState = newButtonState;
  } 
  lastButtonState = newButtonState;
  mqtt.loop();
}
