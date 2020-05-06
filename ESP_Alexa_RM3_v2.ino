#include <FS.h>                                   // This needs to be first, or it all crashes and burns...

// ***********************************************
const bool DEBUG = true;
// ***********************************************

#include <ESP8266WiFi.h>
//needed for library WiFiManager
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>

#include <PubSubClient.h>                         // See below about changing MQTT_MAX_PACKET_SIZE
#if MQTT_MAX_PACKET_SIZE < 1024
#error "MQTT_MAX_PACKET_SIZE in <PubSubClient.h> is too small. "\
  "Increase the value per comments."
#endif  // MQTT_MAX_PACKET_SIZE < 1024

#include "mqttSettings.h"                         // Contains the below defines
/* 
  #define MQTTSERVER     "IP ADDRESS STRING"
  #define MQTTSERVERPORT "PORT NUMBER"
  #define MQTTUSER       "USER STRING"
  #define MQTTPASSWORD   "PASSWORD STRING"
*/
#include <ArduinoJson.h>

#include <Espalexa.h>

// ***********************************************
// ***********************************************
#define ESPALEXA "ESP_ALEXA"                      // Base name (used for MQTT topic). Always allows MQTT to detect new devices
#define MQTT_MAX_PACKET_SIZE 1024                 // You must alter the PubSubClient to this to allow config download
#define irTx D2                                   // GPIO4 (D2) to be used for IR LED sending the message
#define irRx D5                                   // GPIO14 (D5) to IR receiver
#define rm3Led1 D1                                // RM3 LED 1 (Active low)
//#define rm3Reset D3                               // RM3 Reset switch (Active low?)
#define rm3Led2 D4                                // RM3 LED 2 (Active low)
#define kCaptureBufferSize 1024                   // IR receive buffer size
#define kTimeout 50                               // IR receive timout
#define kMinUnknownSize 40                        // IR receive minimum size
// ***********************************************
// ***********************************************

#include <IRremoteESP8266.h>
//#include <IRac.h>
#include <IRtext.h>
#include <IRutils.h>
#include <IRsend.h>
#include <IRrecv.h>
IRsend irsend(irTx);                              // Set the GPIO4 (D2) to be used for IR LED sending the message. (maybe move to config item)
IRrecv irrecv(irRx, kCaptureBufferSize, kTimeout, true);// Set GPIO14 (D5) to IR receiver
decode_results results;

char mqttServer[20] = MQTTSERVER;                 // Placeholders for MQTT server settings
char mqttPort[6] = MQTTSERVERPORT;
char mqttUsername[20] = MQTTUSER;
char mqttPassword[20] = MQTTPASSWORD;


WiFiClient espClient;
PubSubClient mqttClient(espClient);               // Create MQTT object

void deviceChanged(EspalexaDevice* dev);          // Espalexa callback functions

Espalexa espalexa;

struct irDevice 
{
  uint8_t ir_state;                               // Current device state. Only set when on/off codes sent and may not reflect the true state if device does not respond to the IR
  char ir_onCode[32];                             // IR on data code
  char ir_offCode[32];                            // IR off data code
  char ir_alexaName[20];                          // IR device Alexa name
};

char myDeviceName[40];                            // Initially store ESP device name (from JSON "devname") then converted to MQTT callback topic
irDevice myDevices[ESPALEXA_MAXDEVICES];          // No more than x devices

char irBlaster[32];                               // Store for MQTT Blast topic payload

bool readSettings()
{
  //read MQTT configuration from FS json
  if (SPIFFS.exists("/settings.json")) 
  {
    //file exists, reading and loading
    if(DEBUG){ Serial.println("Reading settings file");}
    File settingsFile = SPIFFS.open("/settings.json", "r"); // Open file for reading
    if (settingsFile) 
    {
      if(DEBUG){ Serial.println("Opened settings file");}
      size_t size = settingsFile.size();                  // Get file size
      // Allocate a buffer to store contents of the file.
      std::unique_ptr<char[]> buf(new char[size]);        // Allocate buffer space to read it
      
      settingsFile.readBytes(buf.get(), size);            // Read it

      DynamicJsonDocument doc(1024);                      // Allocate JSON buffers
      DeserializationError error = deserializeJson(doc, buf.get()); // Decode JSON config file
      // Test if parsing succeeds.
      if (error) 
      {
        Serial.print("DeserializeJson() failed: ");       // Flag any errors but keep going in setup
        Serial.println(error.c_str());
      }
      else
      {
        // Fetch values.
        strlcpy(mqttServer, doc["mqttServer"], sizeof(mqttServer));
        strlcpy(mqttPort, doc["mqttPort"], sizeof(mqttPort));
        strlcpy(mqttUsername, doc["mqttUsername"], sizeof(mqttUsername));
        strlcpy(mqttPassword, doc["mqttPassword"], sizeof(mqttPassword));
      }
      if(DEBUG){ Serial.println("Close settings file");}
      settingsFile.close();                               // Close settings file
    }
  }
  else
  {
    Serial.println(F("'/settings.json' missing."));        // Signal config missing but finish setup
  }
}

bool saveMySettings = false;
//callback notifying us of the need to save settings
void saveSettingsCallback () 
{
  saveMySettings = true;
}

void saveSettings () 
{
  //save the custom parameters to FS
  Serial.println("Saving settings");
  // DynamicJsonBuffer jsonBuffer;
  StaticJsonDocument<512> docS;
  //JsonObject& json = doc.createObject();
  docS["mqttServer"] = mqttServer;
  docS["mqttPort"] = mqttPort;
  docS["mqttUsername"] = mqttUsername;
  docS["mqttPassword"] = mqttPassword;

  File settingsFile = SPIFFS.open("/settings.json", "w");
  if (!settingsFile) 
  {
    Serial.println("failed to open settings.json file for writing");
  }

  if (serializeJson(docS, settingsFile) == 0) 
  {
    Serial.println(F("Failed to write to file"));
  }
  settingsFile.close();
}

void setup()
{
  Serial.begin(115200);
  
  Serial.println();
  Serial.println(F(ESPALEXA));
  Serial.println(F("Sketch = " __FILE__));
  Serial.println(F("Compile Date = " __DATE__ " " __TIME__));
  Serial.println(F("Booting"));
  
  pinMode(rm3Led1, OUTPUT);
  pinMode(rm3Led2, OUTPUT);
  //pinMode(rm3Reset, INPUT_PULLUP);
  digitalWrite(rm3Led1, HIGH);
  digitalWrite(rm3Led2, LOW);
 
  if (!SPIFFS.begin())                                      // Mount filesystem
  {
    Serial.println("Failed to mount FS");                   // Failed to mount FS?
    return;                                                 // Pointless continuing with setup (will kickup MQTT errors in loop)
  }

  readSettings();
  
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqttServer, sizeof(mqttServer));
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqttPort, sizeof(mqttPort));
  WiFiManagerParameter custom_mqtt_user("user", "mqtt user", mqttUsername, sizeof(mqttUsername));
  WiFiManagerParameter custom_mqtt_password("password", "mqtt password", mqttPassword, sizeof(mqttPassword));

  WiFiManager wifiManager;
  //wifiManager.resetSettings();
  
  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveSettingsCallback);
  
  //add all your parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_password);
  
  //fetches ssid and pass from eeprom and tries to connect
  //if it does not connect it starts an access point with the specified name
  //here  "ESP_AlexaWM"
  //and goes into a blocking loop awaiting configuration
  wifiManager.autoConnect("ESP_AlexaWM");

  if(saveMySettings)
  {
    saveSettings();
  }

  //read updated parameters
  strcpy(mqttServer, custom_mqtt_server.getValue());
  strcpy(mqttPort, custom_mqtt_port.getValue());
  strcpy(mqttUsername, custom_mqtt_user.getValue());
  strcpy(mqttPassword, custom_mqtt_password.getValue());
  
  //if you get here you have connected to the WiFi
  if(DEBUG){ Serial.print("\nLocal IP = ");}
  if(DEBUG){ Serial.println(WiFi.localIP());}
  
  //read IR configuration from FS json
  if(DEBUG){ Serial.println("Mounted file system");}
  if (SPIFFS.exists("/irconfig.json")) 
  {
    //file exists, reading and loading
    if(DEBUG){ Serial.println("Reading config file");}
    File configFile = SPIFFS.open("/irconfig.json", "r"); // Open file for reading
    if (configFile) 
    {
      if(DEBUG){ Serial.println("Opened config file");}
      size_t size = configFile.size();                    // Get file size
      // Allocate a buffer to store contents of the file.
      std::unique_ptr<char[]> buf(new char[size]);        // Allocate buffer space to read it
      
      configFile.readBytes(buf.get(), size);              // Read it
      
      DynamicJsonDocument doc(1024);                      // Allocate JSON buffers
      DeserializationError error = deserializeJson(doc, buf.get()); // Decode JSON config file
      // Test if parsing succeeds.
      if (error) 
      {
        Serial.print("DeserializeJson() failed: ");       // Flag any errors but keep going in setup
        Serial.println(error.c_str());
      }
      else
      {
        // Fetch values.
        const char* devname = doc["devname"];             // Read ESP device name
        strncpy(myDeviceName, devname, 20);
        WiFi.hostname(myDeviceName);                      // Set WiFi name to match device name 
        if(DEBUG)
        { 
          Serial.print("\ndevice name = ");
          Serial.println(myDeviceName);
        }
        
        JsonArray devices = doc["devices"];               // Get device array
        if(DEBUG)
        { 
          Serial.print("devices.size = ");
          Serial.println(devices.size());
          Serial.println();
        }
        
        for(uint8_t x = 0; x < devices.size(); x++)
        {
          if(x < ESPALEXA_MAXDEVICES)                     // espalexa device limit reached?
          {
            JsonObject devices_x = devices[x];
            const char* devices_x_name = devices_x["name"]; // Get Alexa name
            if(DEBUG){ Serial.print("name = ");}
            if(DEBUG){ Serial.println(devices_x_name);}
            strncpy(myDevices[x].ir_alexaName, devices_x_name, sizeof(myDevices[0].ir_alexaName)); // Store in structure
            
            const char* devices_x_on = devices_x["on"];   // IR on code (example "7,0x0000E0E040BF,32,3")
            if(DEBUG){ Serial.print("on = ");}
            if(DEBUG){ Serial.println(devices_x_on);}
            strncpy(myDevices[x].ir_onCode, devices_x_on, sizeof(myDevices[0].ir_onCode)); // Store in structure
            
            const char* devices_x_off = devices_x["off"]; // IR off code (example "7,0x0000E0E040BF,32,3")
            if(DEBUG){ Serial.print("off = ");}
            if(DEBUG){ Serial.println(devices_x_off);}
            strncpy(myDevices[x].ir_offCode, devices_x_off, sizeof(myDevices[0].ir_offCode)); // Store in structure
            
            if(DEBUG){ Serial.println();}
            
            // Define devices 
//  TEST          espalexa.addDevice(myDevices[x].ir_alexaName, deviceChanged, EspalexaDeviceType::onoff); //non-dimmable device
          }
        }
      }
      if(DEBUG){ Serial.println("Close config file");}
      configFile.close();                                 // Close config file
    }
  }
  else
  {
    Serial.println(F("'/irconfig.json' missing"));        // Signal config missing but finish setup
  }
  SPIFFS.end();                                           // Unmount FS
  
  irsend.begin();                                           // Init IR
  
  espalexa.begin();                                         // Init Alexa
  
  
  String scratch = (String)myDeviceName;                    // Get device name (will be empty if config load failed)
  if(scratch == "")
  {
    scratch = ESPALEXA + (String)"/BLANK" + String(ESP.getChipId(), HEX);  // Generate a unique device name for MQTT subscribe (so we can upload a config to a blank FS)
  }
  else
  {
    ArduinoOTA.setHostname(myDeviceName);                   // Set OTA devicename
    scratch = ESPALEXA + (String)"/" + (String)myDeviceName;// Create MQTT topic
  }
  scratch.toCharArray(myDeviceName, sizeof(myDeviceName));  // Store modified device name
  Serial.println(myDeviceName);
  
  int port = atoi(mqttPort);                                // Convert port number string to int
  mqttClient.setServer(mqttServer, port);                   // Setup MQTT stuff
  mqttClient.setCallback(mqttCallback);                     // Register a callback function for received MQTT messages
  
  irrecv.setUnknownThreshold(kMinUnknownSize);              // Ignore messages with less than minimum on or off pulses.
  irrecv.enableIRIn();                                      // Start the receiver

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();

  digitalWrite(rm3Led2, HIGH);
  Serial.println(F("Booted"));
}

void loop()
{
  if(!mqttClient.connected())                               // Is MQTT connected?
  {
    if(mqttConnect())                                       // Try to connect to MQTT
    {
      if(DEBUG){ Serial.print(F("Publish "));}              // Send MQTT message that we have connected
      if(DEBUG){ Serial.println(myDeviceName);}
      mqttClient.publish(myDeviceName, "Connected");
    }
    else
    {
      Serial.println(F("Reboot Code Here"));                // Reboot if MQTT connect fails?
    }
  }
  
  if (irrecv.decode(&results)) 
  {
    digitalWrite(rm3Led1, LOW);
    if(DEBUG){ Serial.println(resultToHumanReadableBasic(&results));}   // Display the basic output of what we found.
    if(results.bits > 0)
    {
      String topic = (String)myDeviceName + (String)"/IR_Receive/Info";
      String payload = resultToHumanReadableBasic(&results);
      mqttClient.publish(topic.c_str(), payload.c_str());   // Send MQTT Info
    }
    digitalWrite(rm3Led1, HIGH);
  }
  
  mqttClient.loop();                                        // MQTT housekeeping
  espalexa.loop();                                          // Alexa housekeeping
  
  ArduinoOTA.handle();
  //delay(10);
}

bool mqttConnect()
{
  if(DEBUG){ Serial.print(("Attempting MQTT connection... "));}
  if (mqttClient.connect("", mqttUsername, mqttPassword)) 
  {
    if(DEBUG){ Serial.println(F("Connected"));}
    String scratch = (String)myDeviceName + "/#";           // Substribe to all MQTT topics for this device
    mqttClient.subscribe(scratch.c_str());                  // Subscribe to MQTT input
    if(DEBUG){ Serial.print(F("Subscribe: "));}
    if(DEBUG){ Serial.println(scratch);}
  } 
  else 
  {
    Serial.print(F("Failed, rc="));
    Serial.println(mqttClient.state());
  }
  return mqttClient.connected();
}

// Alexa callback function
void deviceChanged(EspalexaDevice* d) 
{
  if (d == nullptr) return;                                 // This is good practice, but not required
  
  uint8_t devNum = d->getId();                              // Get device number for indexing into myDevices array
  sendIR(devNum, d->getValue());                            // Do the IR stuff
}

void sendIR(uint8_t devNo, uint8_t devState)                // Send IR commands and MQTT info
{
  if(DEBUG)
  { 
    Serial.print(myDevices[devNo].ir_alexaName);
    Serial.print(" changed to ");
    Serial.println(devState);
  }
  
  if(devState)
  {
    // Send on code
    decodeAndSendIR(myDevices[devNo].ir_onCode);
  }
  else
  {
    // Send off code
    decodeAndSendIR(myDevices[devNo].ir_offCode);
  }
  
  myDevices[devNo].ir_state = devState;                     // Store on/off state in device structure
  String topic = (String)myDeviceName + (String)"/" + (String)myDevices[devNo].ir_alexaName + (String)"/Info";
  String payload = (String)myDevices[devNo].ir_state;
  mqttClient.publish(topic.c_str(), payload.c_str());       // Send MQTT Info of state
  // device[i]->setValue(device[i]->getValue() > 0 ? 0 : 255);	// Value toggle
  // device[i]->doCallback();									              // and set device to value
}

void decodeAndSendIR(char* irString)
{
  uint8_t ir_type;                                          // See decode_type_t
  uint64_t ir_code;                                         // IR on code
  uint8_t ir_bits;                                          // Number of bits in the IR signal (uint32_t max)
  uint8_t ir_repeat;                                        // Number of times to repeat the IR signal
  char * strtokIndx;                                        // Used by strtok() as an index

  digitalWrite(rm3Led2, LOW);
  irrecv.disableIRIn();
  
  strtokIndx = strtok(irString, ",");                       // get the first part - the string
  if(strtokIndx != NULL)
  {
    ir_type = atoi(strtokIndx);                             // convert this part to an integer
                
    strtokIndx = strtok(NULL, ",");                         // this continues where the previous call left off
    if(strtokIndx != NULL)            
    {           
      ir_code = strtoull(strtokIndx, NULL, 16);             // Convert hex String to number
                  
      strtokIndx = strtok(NULL, ",");                       // this continues where the previous call left off
      if(strtokIndx != NULL)            
      {           
        ir_bits = atoi(strtokIndx);                         // convert this part to an integer
                  
        strtokIndx = strtok(NULL, ",");                     // this continues where the previous call left off
        if(strtokIndx != NULL)            
        {           
          ir_repeat = atoi(strtokIndx);                     // convert this part to an integer
        }
      }
    }
  }

  if(DEBUG)
  { 
    Serial.println(ir_type);
    Serial.print((uint32_t)(ir_code >> 32), HEX);
    Serial.println((uint32_t)ir_code, HEX);
    Serial.println(ir_bits);
    Serial.println(ir_repeat);
  }

  irsend.send((decode_type_t)ir_type, ir_code, ir_bits, ir_repeat);
  
  irrecv.enableIRIn();
  digitalWrite(rm3Led2, HIGH);
  
}


void mqttCallback(char* topic, byte * data, unsigned int length)// Callback for subscribed MQTT topics
{
  int8_t devNum = -1;                                       // Device number of matching name String
  uint8_t devState = 0;
  
  if (strncmp(topic, myDeviceName, strlen(myDeviceName)) == 0)    // Check if the topic is for me (really needed?)
  {
    data[length] = '\0';                                     // Terminate the payload (as from PubSub on github)
    String callback_topic = (String)topic;                  // Copy topic
    String callback_payload = String((char*)data);
    callback_topic.remove(0,strlen(myDeviceName) + 1);      // Remove device name and / from start of string
    
    for(uint8_t x = 0; x < ESPALEXA_MAXDEVICES; x++)        // Loop through devices
    {
      String device = (String)myDevices[x].ir_alexaName;    // Get device name
      if(callback_topic.startsWith(device))                 // Does device name match?
      { 
        devNum = x;                                         // Device found
        break;                                              // Drop out of for loop
      }
    }
    if(devNum >= 0)                                         // Did we find a device?
    {
      if(callback_topic.endsWith("/Set"))                   // Set device state
      {
        if(DEBUG){ Serial.println("Set");}
        devState = callback_payload.toInt();                // Get payload state
        sendIR(devNum, devState);                           // Send IR command
        return;
      }
      
      if(callback_topic.endsWith("/Get"))                   // Get device state
      {
        if(DEBUG){ Serial.println("Get");}
        devState = callback_payload.toInt();                // Get payload state
        String topic = (String)myDeviceName + (String)"/" + (String)myDevices[devNum].ir_alexaName + (String)"/Info"; // Construct info topic
        String payload = (String)myDevices[devNum].ir_state;// Info payload
        mqttClient.publish(topic.c_str(), payload.c_str()); // MQTT publish info
        return;
      }
    }
    
    // None device specific commands
    
    if(callback_topic.equals("PullConfig"))                 // Read config file and send over MQTT
    {
      if(DEBUG){ Serial.println("PullConfig");}
      if (SPIFFS.begin())                                   // Open FS
      {
        if (SPIFFS.exists("/irconfig.json"))                // Check config file exists
        {
          File configFile = SPIFFS.open("/irconfig.json", "r"); // Open config file
          if (configFile) 
          {
            size_t size = configFile.size();                // Get config file size
            char* config_copy = reinterpret_cast<char*>(malloc(size + 1));  // Allocate memory buffer for file + 1 for terminator
            if (config_copy != NULL)                        // Memory allocated
            {
              configFile.readBytes(config_copy, size);      // Read config file into buffer
              config_copy[size] = '\0';                     // Terminate String
              String config_string = String(config_copy);   // Convert to String
              String topic = (String)myDeviceName + (String)"/PulledConfig";  // Construct MQTT topic
              mqttClient.publish(topic.c_str(), config_string.c_str()); // Publish config file
              free(config_copy);                            // Free the buffer memory
            }
            configFile.close();                             // Close config file
          }
        }
        SPIFFS.end();                                       // Close FS
      }
      return;
    }
    
    if(callback_topic.equals("PushConfig"))                 // Set config file to MQTT payload
    {
      if(DEBUG){ Serial.println("PushConfig");}
      
      if(callback_payload.startsWith("{"))                  // Check payload begins with {
      {
        if(callback_payload.endsWith("}"))                  // Check payload ends with }
        {
          if (SPIFFS.begin())                               // Open FS
          {
            File configFile = SPIFFS.open("/irconfig.json", "w"); // Create new config file
            if (configFile) 
            {
              int size = configFile.print(callback_payload);// Write payload to file
              if(size == callback_payload.length())         // Check file write size matches payload size
              {
                String config_size = (String)size;          // Convert to String
                String topic = (String)myDeviceName + (String)"/PushedConfig";  // Construct topic
                mqttClient.publish(topic.c_str(), config_size.c_str()); // Publish
              }
              configFile.close();                           // Close config file
            }
            SPIFFS.end();                                   // Close FS
          }
        }
      }
      return;
    }
    
    if(callback_topic.equals("Reset"))                      // ESP reset (maybe require a magic number as the payload?)
    {
      if(DEBUG){ Serial.print("Reset");}
      for(uint8_t x = 5; x != 0; x--)                       // 5 second countdown
      {
        Serial.print(".");
        Serial.print(x);
        delay(1000);
      }
      ESP.reset();                                          // Reset
      return;                                               // Should never get here
    }
    
    if(callback_topic.equals("Mem"))                        // Check memory (for testing for memory leaks)
    {
      if(DEBUG){ Serial.println("Mem");}
      String free = (String)ESP.getFreeHeap();              // Get free heap space
      String topic = (String)myDeviceName + (String)"/Memory";  // Construct topic
      mqttClient.publish(topic.c_str(), free.c_str());      // Publish
      return;
    }
    
    if(callback_topic.equals("Blast"))                      // Send IR code
    {
      if(DEBUG){ Serial.println("Blast");}
      
      callback_payload.toCharArray(irBlaster, sizeof(irBlaster));          // Store payload in array
      decodeAndSendIR(irBlaster);
      String topic = (String)myDeviceName + (String)"/" + (String)"/Blasted";
      String payload = irBlaster;
      mqttClient.publish(topic.c_str(), payload.c_str());   // Send MQTT Info of state
      return;
    }

    if(DEBUG)
    { 
      if(callback_topic.equals("PullSettings"))               // Read MQTT settings file and send over MQTT
      {
        if(DEBUG){ Serial.println("PullSettings");}
        if (SPIFFS.begin())                                   // Open FS
        {
          if (SPIFFS.exists("/settings.json"))                // Check settings file exists
          {
            File settings = SPIFFS.open("/settings.json", "r"); // Open settings file
            if (settings) 
            {
              size_t size = settings.size();                  // Get settings file size
              char* settings_copy = reinterpret_cast<char*>(malloc(size + 1));  // Allocate memory buffer for file + 1 for terminator
              if (settings_copy != NULL)                      // Memory allocated
              {
                settings.readBytes(settings_copy, size);      // Read settings file into buffer
                settings_copy[size] = '\0';                   // Terminate String
                String settings_string = String(settings_copy);   // Convert to String
                String topic = (String)myDeviceName + (String)"/PulledSettings";  // Construct MQTT topic
                mqttClient.publish(topic.c_str(), settings_string.c_str()); // Publish settings file
                free(settings_copy);                          // Free the buffer memory
              }
              settings.close();                               // Close settings file
            }
          }
          SPIFFS.end();                                       // Close FS
        }
        return;
      }

      if(callback_topic.equals("WipeSPIFFS"))                 // Wipe SPIFFS
      {
        if(DEBUG){ Serial.print("WipeSPIFFS");}
        SPIFFS.format();
        WiFiManager wifiManager;
        wifiManager.resetSettings();
        delay(1000);
        ESP.reset();                                          // Reset
        return;                                               // Should never get here
      }
    }

    // Only gets here if not a supported topic
    if(DEBUG)
    { 
      Serial.print("Other ");
      Serial.print(callback_topic);
      Serial.print(": ");
      Serial.println(callback_payload);
    }
  }
}

/*
  enum decode_type_t {
  UNKNOWN = -1,
  UNUSED = 0,
  RC5,
  RC6,
  NEC,
  SONY,
  PANASONIC,  // (5)
  JVC,
  SAMSUNG,
  WHYNTER,
  AIWA_RC_T501,
  LG,  // (10)
  SANYO,
  MITSUBISHI,
  DISH,
  SHARP,
  COOLIX,  // (15)
  DAIKIN,
  DENON,
  KELVINATOR,
  SHERWOOD,
  MITSUBISHI_AC,  // (20)
  RCMM,
  SANYO_LC7461,
  RC5X,
  GREE,
  PRONTO,  // Technically not a protocol, but an encoding. (25)
  NEC_LIKE,
  ARGO,
  TROTEC,
  NIKAI,
  RAW,  // Technically not a protocol, but an encoding. (30)
  GLOBALCACHE,  // Technically not a protocol, but an encoding.
  TOSHIBA_AC,
  FUJITSU_AC,
  MIDEA,
  MAGIQUEST,  // (35)
  LASERTAG,
  CARRIER_AC,
  HAIER_AC,
  MITSUBISHI2,
  HITACHI_AC,  // (40)
  HITACHI_AC1,
  HITACHI_AC2,
  GICABLE,
  HAIER_AC_YRW02,
  WHIRLPOOL_AC,  // (45)
  SAMSUNG_AC,
  LUTRON,
  ELECTRA_AC,
  PANASONIC_AC,
  PIONEER,  // (50)
  LG2,
  MWM,
  DAIKIN2,
  VESTEL_AC,
  TECO,  // (55)
  SAMSUNG36,
  TCL112AC,
  LEGOPF,
  MITSUBISHI_HEAVY_88,
  MITSUBISHI_HEAVY_152,  // 60
  DAIKIN216,
  SHARP_AC,
  GOODWEATHER,
  INAX,
  DAIKIN160,  // 65
  NEOCLIMA,
  DAIKIN176,
  DAIKIN128,
  AMCOR,
  DAIKIN152,  // 70
  MITSUBISHI136,
  MITSUBISHI112,
  HITACHI_AC424,
  SONY_38K,
  EPSON,  // 75
  // Add new entries before this one, and update it to point to the last entry.
  kLastDecodeType = EPSON,
  };
  
*/


