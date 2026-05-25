#include <WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"

constexpr int REED_PIN = 23;
constexpr int RELAY_PIN = 13;

constexpr const char* TOPIC_STATE  = "garage/door/state";
constexpr const char* TOPIC_STATUS = "garage/door/status";
constexpr const char* TOPIC_CMD    = "garage/door/set";
constexpr const char* DISCOVERY_TOPIC =
  "homeassistant/cover/garage_door/config";

WiFiClient espClient;
PubSubClient mqtt(espClient);

const char* currentDoorState = "UNKNOWN";

void connectWiFi() {
  Serial.println("WiFi Connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected");
  Serial.println(WiFi.localIP());
}

void publishDiscovery() {
  String payload = R"rawliteral(
{
  "name": "Garage Door",
  "unique_id": "esp32_garage_cover_01",

  "device_class": "garage",

  "command_topic": "garage/door/set",
  "state_topic": "garage/door/state",

  "payload_open": "OPEN",
  "payload_close": "CLOSE",

  "state_open": "OPEN",
  "state_closed": "CLOSED",

  "optimistic": false,

  "availability_topic": "garage/door/status",
  "payload_available": "online",
  "payload_not_available": "offline",

  "device": {
    "identifiers": ["esp32_garage_01"],
    "name": "ESP32 Garage Controller",
    "manufacturer": "DIY",
    "model": "ESP32 Relay Controller"
  }
}
)rawliteral";

  mqtt.publish(DISCOVERY_TOPIC, payload.c_str(), true);
}
void publishDoorState() {
  mqtt.publish(TOPIC_STATE, currentDoorState, true);

  Serial.print("STATE: ");
  Serial.println(currentDoorState);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;

  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("MQTT Command: ");
  Serial.println(msg);

  if (msg == "OPEN" || msg == "CLOSE") {
    Serial.println("Relay pulse triggered");

    digitalWrite(RELAY_PIN, LOW);
    delay(300);
    digitalWrite(RELAY_PIN, HIGH);
  }
}
void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.println("MQTT Connecting...");

    if (mqtt.connect("esp32-garage")) {
      Serial.println("MQTT Connected");

      mqtt.publish(TOPIC_STATUS, "online", true);

      mqtt.subscribe(TOPIC_CMD);

      publishDiscovery();
      publishDoorState();

    } else {
      Serial.print("MQTT Failed, rc=");
      Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(9600);

  pinMode(REED_PIN, INPUT_PULLUP);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  connectWiFi();

  mqtt.setBufferSize(1024);
  mqtt.setServer(MQTT_SERVER_IP, MQTT_SERVER_PORT);
  mqtt.setCallback(mqttCallback);

  connectMQTT();

  Serial.println("Ready");
}
void loop() {
  if (!mqtt.connected()) {
    connectMQTT();
  }

  mqtt.loop();

  static int lastState = -1;
  int state = digitalRead(REED_PIN);

  if (state != lastState) {
    lastState = state;

    currentDoorState = (state == HIGH) ? "OPEN" : "CLOSED";

    publishDoorState();
  }

  delay(500);
}
