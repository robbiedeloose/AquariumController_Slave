#include <Arduino.h>
#include <MQTT.h>
#include <IotWebConf.h>

#include <Adafruit_NeoPixel.h>
#ifdef __AVR__
 #include <avr/power.h> // Required for 16 MHz Adafruit Trinket
#endif



// Which pin on the Arduino is connected to the NeoPixels?
#define PIN        D1 // On Trinket or Gemma, suggest changing this to 1

// How many NeoPixels are attached to the Arduino?
#define NUMPIXELS 1 // Popular NeoPixel ring size

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);

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
boolean firstLoop = true;

bool airState = HIGH;
bool co2State = LOW;
bool pumpState = LOW;

char mqttStatusTopic[STRING_LEN];
char mqttAirTopic[STRING_LEN];
char mqttCo2Topic[STRING_LEN];
char mqttPumpTopic[STRING_LEN];
char mqttDeviceWildcardTopic[STRING_LEN];
char mqttLastWillTopic[STRING_LEN];


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
  
    //// send Homie messages
  String buf;
  char mqttHomieTopic[STRING_LEN];

  // /homie/deviceid/$homie
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "/$homie";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "4", true, 1);
  // /homie/deviceid/$name
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "/$name";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "AquaSlave-110", true, 1);
  // /homie/deviceid/$nodes
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "/$nodes";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "controller", true, 1);
  // /homie/deviceid/$implementation
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "/$implementation";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "ESP8266", true, 1);
  // /homie/deviceid/$state
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "/$state";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "init", true, 1);

  // /homie/deviceid/controller/$name
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/$name";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "AquaController", true, 1);
    // /homie/deviceid/controller/$properties 
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/$properties";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "air, co2, pump", true, 1);

  // /homie/deviceid/controller/air/$name
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/air/$name";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "Air", true, 1);
  // /homie/deviceid/controller/air/$unit
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/air/$unit";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "#", true, 1);
  // /homie/deviceid/controller/air/$datatype
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/air/$datatype";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "integer", true, 1);
   // /homie/deviceid/controller/air/$settable
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/air/$settable";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "true", true, 1);

  // /homie/deviceid/controller/co2/$name
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/co2/$name";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "co2", true, 1);
  // /homie/deviceid/controller/co2/$unit
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/co2/$unit";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "#", true, 1);
  // /homie/deviceid/controller/co2/$datatype
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/co2/$datatype";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "integer", true, 1);
   // /homie/deviceid/controller/co2/$settable
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/co2/$settable";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "true", true, 1);

  // /homie/deviceid/controller/pump/$name
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/pump/$name";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "pump", true, 1);
  // /homie/deviceid/controller/pump/$unit
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/pump/$unit";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "#", true, 1);
  // /homie/deviceid/controller/pump/$datatype
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/pump/$datatype";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "integer", true, 1);
   // /homie/deviceid/controller/pump/$settable
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "controller/pump/$settable";
  buf.toCharArray(mqttHomieTopic, STRING_LEN);
  mqttClient.publish(mqttHomieTopic, "true", true, 1);

  return true;
}

void mqttMessageReceived(String &topic, String &payload)
{
  Serial.println("Incoming: " + topic + " - " + payload);

  if (topic.endsWith("air/set")) {
    
    if (payload.toInt() == 1){
      airState = HIGH;
      co2State = LOW;
      Serial.println("air high");
      digitalWrite(AIR_PIN, LOW);
      Serial.println("co2 low because of air on");
      digitalWrite(CO2_PIN, HIGH);
      pixels.setPixelColor(0, pixels.Color(0, 0, 100));
      pixels.show();   
    }
    else {
      airState = LOW;
      Serial.println("air low");
      digitalWrite(AIR_PIN, HIGH);
      pixels.clear();
      pixels.show();
    }
  }
  else if (topic.endsWith("co2/set")) {
    if (payload.toInt() == 1){
      co2State = HIGH;
      airState = LOW;
      Serial.println("co2 high");
      digitalWrite(CO2_PIN, LOW);
      Serial.println("air low because of co2 on");
      digitalWrite(AIR_PIN, HIGH);
      pixels.setPixelColor(0, pixels.Color(0, 100, 0));
      pixels.show();  
    }
    else {
      co2State = LOW;
      Serial.println("co2 low");
      digitalWrite(CO2_PIN, HIGH);
      pixels.clear();
      pixels.show();
    }
  }
   else if (topic.endsWith("pump/set")) {
    Serial.print("pump ");

    // pause air and CO2
    Serial.println("Pause co2 or air");
    digitalWrite(AIR_PIN, HIGH);
    digitalWrite(CO2_PIN, HIGH);

    Serial.print("Pump ");
    Serial.print(payload);
    Serial.println("ml");
    digitalWrite(PUMP_PIN, HIGH);
    pixels.setPixelColor(0, pixels.Color(100, 0, 0));
    pixels.show();  
    delay(payload.toInt() * atof(pumpCalibrationValue) * 1000);
    digitalWrite(PUMP_PIN, LOW);
    pixels.clear();
    pixels.show();

    // resume air of co2
    if (airState == HIGH){
      Serial.println("air high");
      digitalWrite(AIR_PIN, LOW);
      pixels.setPixelColor(0, pixels.Color(0, 0, 100));
      pixels.show();  
    }
    if (co2State == HIGH) {
      Serial.println("co2 high");
      digitalWrite(CO2_PIN, LOW);
      pixels.setPixelColor(0, pixels.Color(0, 100, 0));
      pixels.show(); 
    }
  }
}

void setup() 
{
  Serial.begin(115200); // See "Note on relay pin"!
  Serial.println();
  Serial.println("Starting up...");

  pinMode(AIR_PIN, OUTPUT);
  pinMode(CO2_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);

  digitalWrite(AIR_PIN, HIGH);
  digitalWrite(CO2_PIN, HIGH);

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

  /* 
  homie/super-car/$homie → "2.1.0"
  homie/super-car/$name → "Super car"
  homie/super-car/$nodes → "wheels,engine,lights[]"
  homie/super-car/$implementation → "esp8266"
  homie/super-car/$state → "ready"
  */

  // /homie/deviceid/status
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "/status";
  buf.toCharArray(mqttStatusTopic, STRING_LEN);

  // /homie/deviceid/#
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "/#";
  buf.toCharArray(mqttDeviceWildcardTopic, STRING_LEN);

   // LAst Will /homie/deviceid/$state
  buf = String(MQTT_TOPIC_PREFIX);
  buf += iotWebConf.getThingName();
  buf += "/$state";
  buf.toCharArray(mqttLastWillTopic, STRING_LEN);
  mqttClient.setWill(mqttLastWillTopic, "lost", true, 1);

  mqttClient.begin(mqttServerValue, net);
  mqttClient.onMessage(mqttMessageReceived);

    pixels.begin(); // INITIALIZE NeoPixel strip object (REQUIRED)

  
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

  if (firstLoop) {

    String buf;
    char mqttHomieTopic[STRING_LEN];
    // /homie/deviceid/$state
    buf = String(MQTT_TOPIC_PREFIX);
    buf += iotWebConf.getThingName();
    buf += "/$state";
    buf.toCharArray(mqttHomieTopic, STRING_LEN);
    mqttClient.publish(mqttHomieTopic, "ready", true, 1);
      
    firstLoop = false;
  }

}

/**/
