#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHTesp.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""
#define WIFI_CHANNEL 6
#define X_API_KEY ""
#endif

// Your server from Tutorial 6. http, not https, and no / at the end.
const char *API_URL = "http://iot-xackary.belgiumcentral.cloudapp.azure.com";

// Your name on the class broker: lowercase letters and digits only, no spaces.
const char *BROKER_HOST = "135.232.203.144";
const uint16_t BROKER_PORT = 1883;

const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long RETRY_EVERY_MS = 5000;    // wait between reconnect attempts
const int DHT_PIN = 15;                       // the sensor's data wire
const unsigned long SEND_EVERY_MS = 10000;    // one reading every 10 s
const int FAN_PIN = 2;                        // the fan, through a 220 ohm resistor

DHTesp dht;
WiFiClient net;                               // the TCP connection to the broker
PubSubClient mqtt(net);                       // MQTT on top of it

String statusTopic;                           // ecse3038/<name>/status
String settingsTopic;                         // ecse3038/<name>/fan/settings
String fanStateTopic;                         // ecse3038/<name>/fan/state

unsigned long lastSend = 0;
unsigned long lastAttempt = 0;

// Fan configuration state variables
String fanMode = "off";                       // default to off until first settings arrive
float maxHumidity = NAN;                      // no limit until broker provides one
float lastHumidity = NAN;                     // no reading yet
String reportedFanState = "";                 // what we last told the broker: "on", "off" or nothing yet

bool connectWiFi() {
  Serial.print("Connecting to ");
  Serial.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println("No Wi-Fi after 15 s. Trying again in 5 s.");
      return false;
    }
    delay(250);
    Serial.print(".");
  }

  Serial.println(" connected");
  Serial.print("MAC address: ");
  Serial.println(WiFi.macAddress());
  return true;
}

// Send humidity reading with X-API-Key header
void sendReading(float humidity) {
  char body[80];
  snprintf(body, sizeof(body), "{\"mac\": \"%s\", \"humidity\": %.1f}",
           WiFi.macAddress().c_str(), humidity);

  HTTPClient http;
  http.begin(String(API_URL) + "/readings");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", X_API_KEY);
  int status = http.POST(body);

  Serial.print("POST ");
  Serial.print(body);
  Serial.print(" -> ");
  Serial.println(status);
  if (status == 401) {
    Serial.println("Unauthorized: Check or register your X_API_KEY in include/secrets.h.");
  } else if (status == 404) {
    Serial.println("Unknown MAC: register this board with POST /devices first.");
  } else if (status < 0) {
    Serial.println(http.errorToString(status));
  }
  http.end();
}

// Evaluate fan state based on mode, humidity, and max_humidity, then publish changes
void updateFan() {
  bool targetState = false;

  if (fanMode == "on") {
    targetState = true;
  } else if (fanMode == "off") {
    targetState = false;
  } else if (fanMode == "auto") {
    if (!isnan(maxHumidity) && !isnan(lastHumidity) && lastHumidity > maxHumidity) {
      targetState = true;
    } else {
      targetState = false;
    }
  }

  String currentState = targetState ? "on" : "off";

  if (targetState) {
    digitalWrite(FAN_PIN, HIGH);
  } else {
    digitalWrite(FAN_PIN, LOW);
  }

  Serial.print("Mode: ");
  Serial.print(fanMode);
  Serial.print("  Max Humidity: ");
  Serial.print(maxHumidity);
  Serial.print("  Humidity: ");
  Serial.print(lastHumidity);
  Serial.print("  -> Fan State: ");
  Serial.println(currentState);

  // Publish on or off on fan/state, retained, only when the fan changes
  if (currentState != reportedFanState && mqtt.connected()) {
    mqtt.publish(fanStateTopic.c_str(), currentState.c_str(), true);
    reportedFanState = currentState;
  }
}

// Handle incoming MQTT messages (fan settings)
void onMessage(char *topic, byte *payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  Serial.print("Got ");
  Serial.print(topic);
  Serial.print(" = ");
  Serial.println(message);

  String t = topic;

  if (t == settingsTopic) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);

    // Refuse message if not valid JSON or missing fields
    if (error || !doc.containsKey("mode") || !doc.containsKey("max_humidity")) {
      Serial.println("Refused: Invalid JSON or missing fields; settings unchanged.");
      return;
    }

    String newMode = doc["mode"].as<String>();
    float newMaxHumidity = doc["max_humidity"].as<float>();

    // Refuse if mode is not exactly auto, on, or off
    if (newMode != "auto" && newMode != "on" && newMode != "off") {
      Serial.println("Refused: Invalid mode value; settings unchanged.");
      return;
    }

    // Apply new settings
    fanMode = newMode;
    maxHumidity = newMaxHumidity;

    // React immediately as soon as new settings arrive
    updateFan();
  }
}

bool connectMqtt() {
  String clientId = "esp32-" + String(MQTT_NAME) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Connecting to broker ");
  Serial.print(BROKER_HOST);
  Serial.print(" as ");
  Serial.println(clientId);

  // Connect with last will: offline, retained, on status topic
  bool ok = mqtt.connect(clientId.c_str(), statusTopic.c_str(), 1, true, "offline");
  
  if (!ok) {
    Serial.print("Broker said no, state ");
    Serial.println(mqtt.state());
    return false;
  }
  Serial.println("Connected to broker");

  // Publish online, retained, on status topic
  mqtt.publish(statusTopic.c_str(), "online", true);
  
  // Subscribe to fan/settings topic (retained settings arrive immediately)
  mqtt.subscribe(settingsTopic.c_str());
  reportedFanState = ""; // Reset reported state on new connection
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("Hello from the ESP32");
  dht.setup(DHT_PIN, DHTesp::DHT22);
  pinMode(FAN_PIN, OUTPUT);

  String base = "ecse3038/" + String(MQTT_NAME);
  statusTopic = base + "/status";
  settingsTopic = base + "/fan/settings";
  fanStateTopic = base + "/fan/state";

  mqtt.setServer(BROKER_HOST, BROKER_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(15);

  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastAttempt >= RETRY_EVERY_MS) {
      lastAttempt = millis();
      connectWiFi();
    }
    return;
  }

  if (mqtt.connected()) {
    mqtt.loop();
  } else if (millis() - lastAttempt >= RETRY_EVERY_MS) {
    lastAttempt = millis();
    connectMqtt();
  }

  if (millis() - lastSend < SEND_EVERY_MS) {
    return;
  }
  lastSend = millis();

  // Read humidity every 10 seconds
  float humidity = dht.getHumidity();
  if (isnan(humidity)) {
    Serial.println("No reading from the sensor. Check the three wires.");
    return;
  }
  lastHumidity = humidity;
  sendReading(humidity);
  updateFan();
}
