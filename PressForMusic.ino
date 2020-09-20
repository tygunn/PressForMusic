
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

#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <AsyncMqttClient.h>


// ------------------------------------------------------------------------------------------------
// This section contains your wifi credentials.
// It is assumed that you're using DHCP to get an IP address; there is no strict need to have a
// static IP for this application.

#define WIFI_SSID "supercoolssid"
#define WIFI_PASSWORD "supersecure"

// ------------------------------------------------------------------------------------------------
// This section contains settings for the MQTT server you wish the device to connect to.

#define MQTT_SERVER_IP IPAddress(192, 168, 0, 125)
#define MQTT_SERVER_PORT 1883

// This is the topic we will subscribe to on which we expect to receive basic idle/playing indications.
#define MQTT_FPP_STATUS_TOPIC "christmas/falcon/player/FPP/status"

// This is the topic we will subscribe to for the purpose of determining the name of the current playlist.
#define MQTT_FPP_PLAYLIST_NAME_TOPIC "christmas/falon/player/FPP/playlist/name/status"

// This is the topic we will subscribe to for the purpose of determining which song within the playlist is
// currently playing.
#define MQTT_FPP_PLAYLIST_POSITION_TOPIC "christmas/falcon/player/FPP/playlist/sectionPosition/status"

// We will publish push events to this topic when there is music playing; this can be used to get a
// count of how many people press the button during your show.
#define MQTT_BUTTON_PRESS_COUNT_PLAYING_TOPIC "christmas/pressForMusic/countPlaying"
// We will publish push events to this topic when there is NOT music playing.
// This can be used to get a count of how many people push the button outside of the show.
#define MQTT_BUTTON_PRESS_COUNT_IDLE_TOPIC "christmas/pressForMusic/countIdle"

// This is the topic we will public a signal to indicating that there was a trigger on your blueiris server.
// Change undef to define if you want to do this type of publish.
#undef MQTT_PUBLISH_TO_BLUEIRIS
#define MQTT_BLUEIRIS_TRIGGER_TOPIC "BlueIris/admin"
#define MQTT_BLUEIRIS_TRIGGER_PAYLOAD "camera=drive&trigger"

// ------------------------------------------------------------------------------------------------
// Configuration below this line should not need to be changed if you setup the device in the
// usual manner.

// This is the pin on your ESP8266 which has a relay connected to it.
// I used some cheap 3V 1 channel opto-isolated relays.
// https://www.amazon.com/gp/product/B07ZM84BVX
// These were designed specically for high level triggering from an
// ESP8266 DIO pin.
#define RELAY_PIN D1   

// This is the pin on your ESP8233 which has the momentary switch connected to it.
// I used an industrial foot switch for mine:
// https://www.amazon.com/gp/product/B00EF9D2DY

#define SWITCH_PIN D5  

// When set to 1, indicates that the speakers shall be active until the
// end of the current song.
// When set to 2, indicates that the speakers shall be active until the
// end of the next song.
// ... etc
#define SONG_COUNT 2   

// Status expected on the MQTT_FPP_STATUS_TOPIC topic when nothing is playing.
#define FPP_STATUS_IDLE "idle"
// Status expected on the MQTT_FPP_STATUS_TOPIC topic when something is playing.
#define FPP_STATUS_PLAYING "playing"

#define DEBUG_SERIAL

AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;
WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;
Ticker wifiReconnectTimer;

// Whether we know FPP is playing music or not.
bool isFppPlaying = false;

// We use the current position in the playlist to know when songs change.
int currentPlaylistPosition = -1;
// We track the current button state and do some debouncing to ensure we get a stable
// indication of a button press.
int currentButtonState = -1;
int lastButtonState = -1;
int newButtonState = -1;

// How many songs are remaining before we shut off the speakers.
int songsRemaining = -1;

// Last time the button changed state; used with below to debounce input.
unsigned long lastButtonStateChangeTime = 0;
unsigned long debounceTime = 50;

// Shut off the speaker relay.
void deactiveSpeakers() {
  #ifdef DEBUG_SERIAL
    Serial.println("Deactivate speakers");
  #endif
  digitalWrite(RELAY_PIN, LOW);
  songsRemaining = -1;
}

// Connect to the wifis.
void connectToWifi() {
  #ifdef DEBUG_SERIAL
    Serial.println("Connecting to Wi-Fi...");
  #endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Async callback received when the wifis are up; connect to MQTT at this point.
void onWifiConnect(const WiFiEventStationModeGotIP& event) {
  #ifdef DEBUG_SERIAL
    Serial.println("Connected to Wi-Fi.");
  #endif
  connectToMqtt();
}

// Async callback received when wifi connection is lost; we attempt to auto-reconnect.
void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
  #ifdef DEBUG_SERIAL
    Serial.println("Disconnected from Wi-Fi.");
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
      #ifdef DEBUG_SERIAL
        Serial.println("-- New Song --");
        Serial.print("Songs remaining: ");
        Serial.println(songsRemaining);
      #endif
      if (songsRemaining == 0) {
        deactiveSpeakers();
      }
    }
  }
}

// Set things up.
void setup() {
  // Setup the relay and switch pins.
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(SWITCH_PIN, INPUT);

  // Let's just be sure that the relay is off on boot.
  digitalWrite(RELAY_PIN, LOW);

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

  connectToWifi();
}

void handleButtonDown() {
  if (isFppPlaying) {
    songsRemaining = SONG_COUNT;
    #ifdef DEBUG_SERIAL
      Serial.print("Activate speakers - songs remaining: ");
      Serial.println(songsRemaining);
    #endif
    digitalWrite(RELAY_PIN, HIGH);
    mqttClient.publish(MQTT_BUTTON_PRESS_COUNT_PLAYING_TOPIC, 2, true, "trigger");
    #ifdef MQTT_PUBLISH_TO_BLUEIRIS
      mqttClient.publish(MQTT_BLUEIRIS_TRIGGER_TOPIC, 2, true, MQTT_BLUEIRIS_TRIGGER_PAYLOAD);
    #endif
  } else {
    // TODO: Rickroll someone who pushes the button during the day when the show is off.
    // I'm envisioning this will turn on the speakers and publish to FPP to start a playlist with
    // 'ol Rick on it.
    #ifdef DEBUG_SERIAL
      Serial.println("  << Rick-roll");
    #endif
    mqttClient.publish(MQTT_BUTTON_PRESS_COUNT_IDLE_TOPIC, 2, true, "trigger");
  }
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
    if (newButtonState == HIGH) {
      #ifdef DEBUG_SERIAL
        Serial.println("  << BUTTON DOWN");
      #endif
      handleButtonDown();
    } else {
      #ifdef DEBUG_SERIAL
        Serial.println("  << BUTTON UP");
      #endif
    }
    currentButtonState = newButtonState;
  } 
  lastButtonState = newButtonState;
}
