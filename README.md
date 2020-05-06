# ESP_Alexa-v2

Use ESP Alexa lib to control hacked Broadlink RM3 Mini IR Blaster or any suitable IR Blaster

Configuration of Alexa device names and IR codes is via irconfig.json file stored in SPIFFS and you can also control devices using MQTT


On first run the ESP8266 will create an access point (ESP_AlexaWM) where you enter the WiFi credentials and MQTT broker details. 

After the WiFi has connected the ESP8266 tries to load a irconfig.json file from SPIFFS that contains the device name (used for MQTT) and also an array of Alexa device names and IR on/off control codes.

If no config.json file exists or it fails to load correctly then a MQTT topic that is compounded from ESPALEXA basename + "/BLANK" + (ESP.getChipId(), HEX) is created and connected to the broker. This allows a MQTT channel back to a blank device so a irconfig.json file can be uploaded.

If the irconfig file loads correctly then after reboot (also doable over MQTT) the MQTT topic will be ESPALEXA + "/" + devname

MQTT control topics are...
ESPALEXA + "/" + devname + "/" + name + "/Set" with a payload of the value (0-255) to set for the Alexa name device.


ESPALEXA + "/" + devname + "/" + name + "/Get" will return the Alexa name device state in payload.
This will be invalid for the first call and may not reflect the true state of the device if it did not receive the last IR transmission.


ESPALEXA + "/" + devname + "/PullConfig" will return the irconfig.json file from SPIFFS.

ESPALEXA + "/" + devname + "/PushConfig" with the payload of a irconfig.json file will create/overwrite the config.json file in SPIFFS.

ESPALEXA + "/" + devname + "/Reset" will start a 5 second countdown to the ESP8266 resetting.

ESPALEXA + "/" + devname + "/Blast" will decode the payload string and send its IR code.
      the payload format is the same as used in the irconfig.json file.
      example: "7,0xE0E0F00F,32,1" equates to... 
      7 = device type (SAMSUNG)
      0xE0E0F00F = IR code to send (Hex format)
      32 = Number of bits to send
      1 = Number of times to repeat IR code send.

ESPALEXA + "/" + devname + "/Mem" will return the free heap space. Used for testing for memory leaks.


The last two topics are used for testing and will be remove from finished project if DEBUG = false in main sketch.

ESPALEXA + "/" + devname + "/WipeSPIFFS" will format the SPIFFS filesystem and reboot the ESP8266.

ESPALEXA + "/" + devname + "/PullSettings" will pull the MQTT settings.json file from SPIFFS.


-----------------

If the IR receiver detects IR transmissions then it will try to decode the IR format and send results to MQTT. This allows you to get the thing running and then interogate the on/off IR codes for the devices to be edited into a irconfig.json that can then be uploaded over MQTT.
