#include <Arduino.h>
#include <MQTT.h>
#include <IotWebConf.h>

// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char thingName[] = "AquaControllerSlave";

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "smrtTHNG8266";

#define STRING_LEN 128
#define NUMBER_LEN 32

// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "ACS3"

// -- When BUTTON_PIN is pulled to ground on startup, the Thing will use the initial
//      password to buld an AP. (E.g. in case of lost password)
#define BUTTON_PIN D2

// -- Status indicator pin.
//      First it will light up (kept LOW), on Wifi connection it will blink,
//      when connected to the Wifi it will turn off (kept HIGH).
#define STATUS_PIN LED_BUILTIN

// -- Connected ouput pin. See "Note on relay pin"!
#define AIR_PIN D5
#define CO2_PIN D6
#define PUMP_PIN D7

#define MQTT_TOPIC_PREFIX "/homie/"

// -- Ignore/limit status changes more frequent than the value below (milliseconds).
#define ACTION_FEQ_LIMIT 7000


// -- Callback method declarations.
void wifiConnected();
void configSaved();
boolean formValidator();
void mqttMessageReceived(String &topic, String &payload);

DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
WiFiClient net;
MQTTClient mqttClient;

char mqttServerValue[STRING_LEN];
char pumpCalibrationValue[NUMBER_LEN];

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
IotWebConfParameter mqttServerParam = IotWebConfParameter("MQTT server", "mqttServer", mqttServerValue, STRING_LEN);
IotWebConfSeparator separator1 = IotWebConfSeparator();
IotWebConfParameter pumpCalibrationParam = IotWebConfParameter("Pump calibration", "pumpcalibration", pumpCalibrationValue, NUMBER_LEN, "number", "e.g. 1.05", NULL, "step='0.01'");

boolean needMqttConnect = false;
boolean needReset = false;
unsigned long lastMqttConnectionAttempt = 0;

bool airState = LOW;
bool co2State = LOW;
bool pumpState = LOW;

char mqttStatusTopic[STRING_LEN];
char mqttRelay1Topic[STRING_LEN];
char mqttRelay2Topic[STRING_LEN];
char mqttPumpTopic[STRING_LEN];
char mqttDeviceWildcardTopic[STRING_LEN];


/**
 * Handle web requests to "/" path.
 */
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = F("<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>");
  s += iotWebConf.getHtmlFormatProvider()->getStyle();
  s += "<title>IotWebConf 07 MQTT Relay</title></head><body>";
  s += iotWebConf.getThingName();
  s += "<div>Air: ";
  s += (airState == HIGH ? "ON" : "OFF");
  s += "</div>";
  s += "<div>Co2: ";
  s += (co2State == HIGH ? "ON" : "OFF");
  s += "</div>";
  s += "<div>Pump: ";
  s += (pumpState == HIGH ? "Running" : "OFF");
  s += "</div>";
  s += "<div>Pump calibration: ";
  s += atof(pumpCalibrationValue);
  s += "</div>";
  s += "<button type='button' onclick=\"location.href='';\" >Refresh</button>";
  s += "<div>Go to <a href='config'>configure page</a> to change values.</div>";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

void wifiConnected()
{
  needMqttConnect = true;
}

void configSaved()
{
  Serial.println("Configuration was updated.");
  needReset = true;
}

boolean formValidator()
{
  Serial.println("Validating form.");
  boolean valid = true;

  int l = server.arg(mqttServerParam.getId()).length();
  if (l < 3)
  {
    mqttServerParam.errorMessage = "Please provide at least 3 characters!";
    valid = false;
  }

  return valid;
}

boolean connectMqtt() {
  unsigned long now = millis();
  if (1000 > now - lastMqttConnectionAttempt)
  {
    // Do not repeat within 1 sec.
    return false;
  }
  Serial.println("Connecting to MQTT server...");
  if (!mqttClient.connect(iotWebConf.getThingName())) {
    lastMqttConnectionAttempt = now;
    return false;
  }
  Serial.println("Connected!");

  mqttClient.subscribe(mqttDeviceWildcardTopic);
  mqttClient.publish(mqttStatusTopic, "online", true, 1);
  //mqttClient.publish(mqttActionTopic, airState == HIGH ? "ON" : "OFF", true, 1);


  return true;
}

void mqttMessageReceived(String &topic, String &payload)
{
  Serial.println("Incoming: " + topic + " - " + payload);

  if (topic.endsWith("air")) {
    if (payload.toInt() == 1){
      airState = HIGH;
      Serial.println("air high");
      digitalWrite(AIR_PIN, HIGH);
    }
    else {
      airState = LOW;
      Serial.println("air low");
      digitalWrite(AIR_PIN, LOW);
    }
  }
  else if (topic.endsWith("co2")) {
    Serial.println("co2");
    if (payload.toInt() == 1){
      co2State = HIGH;
      Serial.println("air high");
      digitalWrite(CO2_PIN, HIGH);
    }
    else {
      co2State = LOW;
      Serial.println("air low");
      digitalWrite(CO2_PIN, LOW);
    }
  }
   else if (topic.endsWith("pump")) {
    Serial.println("pump");
    digitalWrite(PUMP_PIN, HIGH);
    delay( payload.toFloat() * atof(pumpCalibrationValue) * 1000);
    digitalWrite(PUMP_PIN, LOW);
  }
}

void setup() 
{
  Serial.begin(115200); // See "Note on relay pin"!
  Serial.println();
  Serial.println("Starting up...");

  pinMode(AIR_PIN, OUTPUT);

  iotWebConf.setStatusPin(STATUS_PIN);
  iotWebConf.setConfigPin(BUTTON_PIN);
  iotWebConf.addParameter(&mqttServerParam);
  iotWebConf.addParameter(&separator1);
  iotWebConf.addParameter(&pumpCalibrationParam);
  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&wifiConnected);
  iotWebConf.setupUpdateServer(&httpUpdater);

  // -- Initializing the configuration.
  boolean validConfig = iotWebConf.init();
  if (!validConfig)
  {
    mqttServerValue[0] = '\0';
  }

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });

  // -- Prepare dynamic topic names
  String buf;

  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "/status";
  buf.toCharArray(mqttStatusTopic, STRING_LEN);

  // /homie/deviceid/#
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "/#";
  buf.toCharArray(mqttDeviceWildcardTopic, STRING_LEN);

  mqttClient.begin(mqttServerValue, net);
  mqttClient.onMessage(mqttMessageReceived);
  
  Serial.println("Ready.");
}

void loop() 
{
  // -- doLoop should be called as frequently as possible.
  iotWebConf.doLoop();
  mqttClient.loop();
  
  if (needMqttConnect)
  {
    if (connectMqtt())
    {
      needMqttConnect = false;
    }
  }
  else if ((iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE) && (!mqttClient.connected()))
  {
    Serial.println("MQTT reconnect");
    connectMqtt();
  }

  if (needReset)
  {
    Serial.println("Rebooting after 1 second.");
    iotWebConf.delay(1000);
    ESP.restart();
  }

  if (airState == HIGH)
  {
    iotWebConf.blink(2000, 90);
  }
  else
  {
    iotWebConf.stopCustomBlink();
  }
}
