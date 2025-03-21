#ifdef ARDUINO_M5Stick_C_Plus
#include <M5StickCPlus.h>
#elif ARDUINO_M5Stick_C
#include <M5StickC.h>
#else
#include <Arduino.h>
#endif

#include <HardwareSerial.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>

#include "setup.h" //<-- Configure your setup here
#include "mqttserial.h"
#include "converters.h"
#include "comm.h"
#include "mqtt.h"

Converter converter;
char registryIDs[32]; //Holds the registries to query
bool busy = false;

#if defined(ARDUINO_M5Stick_C) || defined(ARDUINO_M5Stick_C_Plus)
long LCDTimeout = 40000;//Keep screen ON for 40s then turn off. ButtonA will turn it On again.
#endif

bool contains(char array[], int size, int value)
{
  for (int i = 0; i < size; i++)
  {
    if (array[i] == value)
      return true;
  }
  return false;
}

//Converts to string and add the value to the JSON message
void updateValues(char regID)
{
  LabelDef *labels[128];
  int num = 0;
  converter.getLabels(regID, labels, num);
  for (size_t i = 0; i < num; i++)
  {
    bool alpha = false;
    for (size_t j = 0; j < strlen(labels[i]->asString); j++)
    {
      char c = labels[i]->asString[j];
      if (!isdigit(c) && c!='.'){
        alpha = true;
      }
    }

    #ifdef ONEVAL_ONETOPIC
    char topicBuff[128] = MQTT_OneTopic;
    strcat(topicBuff,labels[i]->label);
    client.publish(topicBuff, labels[i]->asString);
    #endif
    
    if (alpha){      
      snprintf(jsonbuff + strlen(jsonbuff), MAX_MSG_SIZE - strlen(jsonbuff), "\"%s\":\"%s\",", labels[i]->label, labels[i]->asString);
    }
    else{//number, no quotes
      snprintf(jsonbuff + strlen(jsonbuff), MAX_MSG_SIZE - strlen(jsonbuff), "\"%s\":%s,", labels[i]->label, labels[i]->asString);

    }
  }
}

uint16_t loopcount =0;

void extraLoop()
{
  client.loop();
  ArduinoOTA.handle();
  while (busy)
  { //Stop processing during OTA
    ArduinoOTA.handle();
  }

#if defined(ARDUINO_M5Stick_C) || defined(ARDUINO_M5Stick_C_Plus)
  if (M5.BtnA.wasPressed()){//Turn back ON screen
    M5.Axp.ScreenBreath(12);
    LCDTimeout = millis() + 30000;
  }else if (LCDTimeout < millis()){//Turn screen off.
    M5.Axp.ScreenBreath(0);
  }
  M5.update();
#endif
}

void setup_wifi()
{
  delay(10);
  // We start by connecting to a WiFi network
  mqttSerial.printf("Connecting to %s\n", WIFI_SSID);
  
  #if defined(WIFI_IP) && defined(WIFI_GATEWAY) && defined(WIFI_SUBNET)
    IPAddress local_IP(WIFI_IP);
    IPAddress gateway(WIFI_GATEWAY);
    IPAddress subnet(WIFI_SUBNET);

    #ifdef WIFI_PRIMARY_DNS
      IPAddress primaryDNS(WIFI_PRIMARY_DNS);
    #else
      IPAddress primaryDNS();
    #endif

    #ifdef WIFI_SECONDARY_DNS
      IPAddress secondaryDNS(WIFI_SECONDARY_DNS);
    #else
      IPAddress secondaryDNS();
    #endif

    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
      mqttSerial.println("Failed to set static ip!");
    }
  #endif  

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  int i = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if (i++ == 100)
    {
      esp_restart();
    }
  }
  mqttSerial.printf("Connected. IP Address: %s\n", WiFi.localIP().toString().c_str());
}

void initRegistries(){
    //getting the list of registries to query from the selected values  
  for (size_t i = 0; i < sizeof(registryIDs); i++)
  {
    registryIDs[i]=0xff;
  }
  
  int i = 0;
  for (auto &&label : labelDefs)
  {
    if (!contains(registryIDs, sizeof(registryIDs), label.registryID))
    {
      mqttSerial.printf("Adding registry 0x%2x to be queried.\n", label.registryID);
      registryIDs[i++] = label.registryID;
    }
  }
  if (i == 0)
  {
    mqttSerial.printf("ERROR - No values selected in the include file. Stopping.\n");
    while (true)
    {
      extraLoop();
    }
  }
}

void setupScreen(){
#if defined(ARDUINO_M5Stick_C) || defined(ARDUINO_M5Stick_C_Plus)
  M5.begin();
  M5.Axp.EnableCoulombcounter();
  M5.Lcd.setRotation(1);
  M5.Axp.ScreenBreath(12);
  M5.Lcd.fillScreen(TFT_WHITE);
  M5.Lcd.setFreeFont(&FreeSansBold12pt7b);
  m5.Lcd.setTextDatum(MC_DATUM);
  int xpos = M5.Lcd.width() / 2; // Half the screen width
  int ypos = M5.Lcd.height() / 2; // Half the screen width
  M5.Lcd.setTextColor(TFT_DARKGREY);
  M5.Lcd.drawString("ESPAltherma", xpos,ypos,1);
  delay(2000);
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_GREEN);
#endif
}

void setup()
{
  Serial.begin(115200);
  setupScreen();
  MySerial.begin(9600, SERIAL_8E1, RX_PIN, TX_PIN);
  pinMode(PIN_THERM, OUTPUT);
  digitalWrite(PIN_THERM, HIGH);

#ifdef PIN_SG1
  //Smartgrid pins - Set first to the inactive state, before configuring as outputs (avoid false triggering when initializing)
  digitalWrite(PIN_SG1, SG_RELAY_INACTIVE_STATE);
  digitalWrite(PIN_SG2, SG_RELAY_INACTIVE_STATE);
  pinMode(PIN_SG1, OUTPUT);
  pinMode(PIN_SG2, OUTPUT);
 
#endif
#ifdef ARDUINO_M5Stick_C_Plus
  gpio_pulldown_dis(GPIO_NUM_25);
  gpio_pullup_dis(GPIO_NUM_25);
#endif

  EEPROM.begin(10);
  readEEPROM();//Restore previous state
  mqttSerial.print("Setting up wifi...");
  setup_wifi();
  ArduinoOTA.setHostname("ESPAltherma");
  ArduinoOTA.onStart([]() {
    busy = true;
  });

  ArduinoOTA.onError([](ota_error_t error) {
    mqttSerial.print("Error on OTA - restarting");
    esp_restart();
  });
  ArduinoOTA.begin();

  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setBufferSize(MAX_MSG_SIZE); //to support large json message
  client.setCallback(callback);
  client.setServer(MQTT_SERVER, MQTT_PORT);
  mqttSerial.print("Connecting to MQTT server...");
  mqttSerial.begin(&client, "espaltherma/log");
  reconnect();
  mqttSerial.println("OK!");

  initRegistries();
  mqttSerial.print("ESPAltherma started!");
}

void waitLoop(uint ms){
      unsigned long start = millis();
      while (millis() < start + ms) //wait .5sec between registries
      {
        extraLoop();
      }
}

void loop()
{
  if (!client.connected())
  { //(re)connect to MQTT if needed
    reconnect();
  }
  //Querying all registries
  for (size_t i = 0; (i < 32) && registryIDs[i] != 0xFF; i++)
  {
    char buff[64] = {0};
    int tries = 0;
    while (!queryRegistry(registryIDs[i], buff) && tries++ < 3)
    {
      mqttSerial.println("Retrying...");
      waitLoop(1000);
    }
    if (registryIDs[i] == buff[1]) //if replied registerID is coherent with the command
    {
      converter.readRegistryValues(buff); //process all values from the register
      updateValues(registryIDs[i]);       //send them in mqtt
      waitLoop(500);//wait .5sec between registries
    }
  }
  sendValues();//Send the full json message
  mqttSerial.printf("Done. Waiting %d sec...\n", FREQUENCY / 1000);
  waitLoop(FREQUENCY);
}
