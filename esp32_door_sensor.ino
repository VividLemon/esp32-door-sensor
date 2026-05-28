#include <WiFi.h>
#include <PubSubClient.h>
#include <string.h>
#include "secrets.h"

constexpr int REED_PIN = 23;
constexpr int RELAY_PIN = 13;

constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long MQTT_RETRY_MS = 5000;
constexpr unsigned long RELAY_PULSE_MS = 300;
constexpr unsigned long REED_DEBOUNCE_MS = 75;

constexpr const char* TOPIC_STATE  = "garage/door/state";
constexpr const char* TOPIC_STATUS = "garage/door/status";
constexpr const char* TOPIC_CMD    = "garage/door/set";
constexpr const char* DISCOVERY_TOPIC =
  "homeassistant/cover/garage_door/config";

WiFiClient espClient;
PubSubClient mqtt(espClient);

enum class DoorState {
  UNKNOWN,
  OPEN,
  CLOSED
};

DoorState currentDoorState = DoorState::UNKNOWN;

bool relayPulseActive = false;
unsigned long relayPulseStartMs = 0;
unsigned long lastWiFiAttemptMs = 0;
unsigned long lastMQTTAttemptMs = 0;

int stableReedState = HIGH;
int lastRawReedState = HIGH;
unsigned long lastReedChangeMs = 0;
bool wifiWasConnected = false;

char mqttClientId[32] = {0};

const char* doorStateToPayload(DoorState state) {
  switch (state) {
    case DoorState::OPEN:
      return "OPEN";
    case DoorState::CLOSED:
      return "CLOSED";
    default:
      return "UNKNOWN";
  }
}

DoorState doorStateFromReed(int reedValue) {
  return (reedValue == HIGH) ? DoorState::OPEN : DoorState::CLOSED;
}

void setDoorStateFromSensor(int reedValue) {
  DoorState nextState = doorStateFromReed(reedValue);
  currentDoorState = nextState;
}

void buildClientId() {
  uint32_t chipId = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
  snprintf(mqttClientId, sizeof(mqttClientId), "esp32-garage-%06lX", (unsigned long)chipId);
}

void triggerRelayPulse() {
  if (relayPulseActive) {
    Serial.println("Relay pulse ignored: already active");
    return;
  }

  relayPulseActive = true;
  relayPulseStartMs = millis();
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("Relay pulse started");
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  unsigned long now = millis();
  if (now - lastWiFiAttemptMs >= WIFI_RETRY_MS) {
    lastWiFiAttemptMs = now;
    Serial.println("WiFi reconnect attempt...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  return false;
}

void publishDiscovery() {
  static const char payload[] = R"rawliteral(
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
  "availability_mode": "latest",
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

  bool ok = mqtt.publish(DISCOVERY_TOPIC, payload, true);
  if (!ok) {
    Serial.print("Discovery publish failed, mqtt rc=");
    Serial.println(mqtt.state());
  }
}

void publishDoorState() {
  const char* payload = doorStateToPayload(currentDoorState);
  bool ok = mqtt.publish(TOPIC_STATE, payload, true);
  if (!ok) {
    Serial.print("State publish failed, mqtt rc=");
    Serial.println(mqtt.state());
  }

  Serial.print("STATE: ");
  Serial.println(payload);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_CMD) != 0) {
    return;
  }

  char cmd[16];
  size_t copyLen = (length < sizeof(cmd) - 1) ? length : sizeof(cmd) - 1;
  memcpy(cmd, payload, copyLen);
  cmd[copyLen] = '\0';

  Serial.print("MQTT Command: ");
  Serial.println(cmd);

  bool wantsOpen = strcmp(cmd, "OPEN") == 0;
  bool wantsClose = strcmp(cmd, "CLOSE") == 0;

  if (!wantsOpen && !wantsClose) {
    Serial.println("Ignored command: unsupported payload");
    return;
  }

  if ((wantsOpen && currentDoorState == DoorState::OPEN) ||
      (wantsClose && currentDoorState == DoorState::CLOSED)) {
    Serial.println("Ignored command: door already in requested state");
    return;
  }

  triggerRelayPulse();
}

void ensureMQTTConnected() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  if (now - lastMQTTAttemptMs < MQTT_RETRY_MS) {
    return;
  }

  lastMQTTAttemptMs = now;
  Serial.println("MQTT connecting...");

  if (mqtt.connect(mqttClientId, TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("MQTT Connected");

    bool statusOk = mqtt.publish(TOPIC_STATUS, "online", true);
    if (!statusOk) {
      Serial.print("Status publish failed, mqtt rc=");
      Serial.println(mqtt.state());
    }

    bool subOk = mqtt.subscribe(TOPIC_CMD);
    if (!subOk) {
      Serial.print("Subscribe failed, mqtt rc=");
      Serial.println(mqtt.state());
    }

    publishDiscovery();
    publishDoorState();
  } else {
    Serial.print("MQTT Failed, rc=");
    Serial.println(mqtt.state());
  }
}

void setup() {
  Serial.begin(9600);
  WiFi.mode(WIFI_STA);
  buildClientId();

  pinMode(REED_PIN, INPUT_PULLUP);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  int initialReed = digitalRead(REED_PIN);
  stableReedState = initialReed;
  lastRawReedState = initialReed;
  lastReedChangeMs = millis();
  setDoorStateFromSensor(initialReed);

  mqtt.setBufferSize(1024);
  mqtt.setServer(MQTT_SERVER_IP, MQTT_SERVER_PORT);
  mqtt.setCallback(mqttCallback);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();

  Serial.println("Ready");
}

void loop() {
  bool wifiConnected = ensureWiFiConnected();

  if (wifiConnected && !wifiWasConnected) {
    wifiWasConnected = true;
    Serial.println("WiFi Connected");
    Serial.println(WiFi.localIP());
  } else if (!wifiConnected && wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("WiFi disconnected");
    if (mqtt.connected()) {
      mqtt.disconnect();
    }
  }

  if (wifiConnected) {
    ensureMQTTConnected();

    if (mqtt.connected()) {
      mqtt.loop();
    }
  }

  unsigned long now = millis();

  if (relayPulseActive && now - relayPulseStartMs >= RELAY_PULSE_MS) {
    relayPulseActive = false;
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("Relay pulse ended");
  }

  int rawState = digitalRead(REED_PIN);
  if (rawState != lastRawReedState) {
    lastRawReedState = rawState;
    lastReedChangeMs = now;
  }

  if (rawState != stableReedState && now - lastReedChangeMs >= REED_DEBOUNCE_MS) {
    stableReedState = rawState;

    DoorState previous = currentDoorState;
    setDoorStateFromSensor(stableReedState);

    if (previous != currentDoorState && mqtt.connected()) {
      publishDoorState();
    }
  }

}
