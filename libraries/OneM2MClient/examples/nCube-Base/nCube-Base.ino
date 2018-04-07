#include <Arduino.h>
#include <SPI.h>

/**
   Copyright (c) 2018, OCEAN
   All rights reserved.
   Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
   1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
   3. The name of the author may not be used to endorse or promote products derived from this software without specific prior written permission.
   THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <WiFi101.h>
#include <WiFiMDNSResponder.h>

#include "OneM2MClient.h"
#include "m0_ota.h"

#include "TasCO2.h"
#include "TasLED.h"

const int ledPin = 13; // LED pin for connectivity status indicator

uint8_t USE_WIFI = 1;

#define WINC_CS   8
#define WINC_IRQ  7
#define WINC_RST  4
#define WINC_EN   2

#define WIFI_INIT 1
#define WIFI_CONNECT 2
#define WIFI_CONNECTED 3
#define WIFI_RECONNECT 4
uint8_t WIFI_State = WIFI_INIT;

unsigned long wifi_previousMillis = 0;
const long wifi_interval = 30; // count
const long wifi_led_interval = 100; // ms
uint16_t wifi_wait_count = 0;

unsigned long req_previousMillis = 0;
const long req_interval = 1500; // ms

unsigned long chk_previousMillis = 0;
const long chk_interval = 1000; // ms

#define UPLOAD_UPLOADING 2
#define UPLOAD_UPLOADED 3
unsigned long uploading_previousMillis = 0;
const long uploading_interval = 100; // ms
uint8_t UPLOAD_State = UPLOAD_UPLOADING;

unsigned long generate_previousMillis = 0;
const long generate_interval = 5000; // ms

const String FIRMWARE_VERSION = "1.0.0.0";
const String AE_NAME = "base0";
const String AE_ID = "S" + AE_NAME;
const String MOBIUS_MQTT_BROKER_IP = "203.253.128.161";
const uint16_t MOBIUS_MQTT_BROKER_PORT = 1883;

// for MQTT
WiFiClient wifiClient;
PubSubClient mqtt;

OneM2MClient nCube(MOBIUS_MQTT_BROKER_IP, MOBIUS_MQTT_BROKER_PORT, AE_ID); // AE-ID

TasLED tasLed;
TasCO2 tasCO2Sensor;

char req_id[10];
String state = "create_ae";
uint8_t sequence = 0;

#define QUEUE_SIZE 8
typedef struct _queue_t {
    uint8_t pop_idx;
    uint8_t push_idx;
    String ref[QUEUE_SIZE];
    String con[QUEUE_SIZE];
    String rqi[QUEUE_SIZE];
} queue_t;

queue_t noti_q;
queue_t upload_q;

short control_flag = 0;

void rand_str(char *dest, size_t length) {
    char charset[] = "0123456789"
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    while (length-- > 0) {
        size_t index = (double) rand() / RAND_MAX * (sizeof charset - 1);
        *dest++ = charset[index];
    }
    *dest = '\0';
}

void WiFi_init() {
    if(USE_WIFI) {
        // begin WiFi
        digitalWrite(ledPin, HIGH);
        WiFi.setPins(WINC_CS, WINC_IRQ, WINC_RST, WINC_EN);
        if (WiFi.status() == WL_NO_SHIELD) { // check for the presence of the shield:
            Serial.println("WiFi shield not present");
            // don't continue:
            while (true) {
                digitalWrite(ledPin, HIGH);
                delay(100);
                digitalWrite(ledPin, LOW);
                delay(100);
            }
        }
        digitalWrite(ledPin, LOW);
    }

    WIFI_State = WIFI_INIT;
}

void WiFi_chkconnect() {
    if(USE_WIFI) {
        if(WIFI_State == WIFI_INIT) {
            digitalWrite(ledPin, HIGH);

            Serial.println("beginProvision - WIFI_INIT");
            WiFi.beginProvision();

            WIFI_State = WIFI_CONNECT;
            wifi_previousMillis = 0;
            wifi_wait_count = 0;
            noti_q.pop_idx = 0;
            noti_q.push_idx = 0;
            upload_q.pop_idx = 0;
            upload_q.push_idx = 0;

        }
        else if(WIFI_State == WIFI_CONNECTED) {
            if (WiFi.status() == WL_CONNECTED) {
                return;
            }

            wifi_wait_count = 0;
            if(WIFI_State == WIFI_CONNECTED) {
                WIFI_State = WIFI_RECONNECT;
                wifi_previousMillis = 0;
                wifi_wait_count = 0;
                noti_q.pop_idx = 0;
                noti_q.push_idx = 0;
                upload_q.pop_idx = 0;
                upload_q.push_idx = 0;
            }
            else {
                WIFI_State = WIFI_CONNECT;
                wifi_previousMillis = 0;
                wifi_wait_count = 0;
                noti_q.pop_idx = 0;
                noti_q.push_idx = 0;
                upload_q.pop_idx = 0;
                upload_q.push_idx = 0;
            }
            nCube.MQTT_init();
        }
        else if(WIFI_State == WIFI_CONNECT) {
            unsigned long currentMillis = millis();
            if (currentMillis - wifi_previousMillis >= wifi_led_interval) {
                wifi_previousMillis = currentMillis;
                if(wifi_wait_count++ >= wifi_interval) {
                    wifi_wait_count = 0;
                    if (WiFi.status() != WL_CONNECTED) {
                        Serial.println("Provisioning......");
                    }
                }
                else {
                    if(wifi_wait_count % 2) {
                        digitalWrite(ledPin, HIGH);
                    }
                    else {
                        digitalWrite(ledPin, LOW);
                    }
                }
            }
            else {
                if (WiFi.status() == WL_CONNECTED) {
                    // you're connected now, so print out the status:
                    printWiFiStatus();

                    uint8_t mac[6];
                    WiFi.macAddress(mac);

                    PubSubClient _mqtt(wifiClient);

                    char ip[16];
                    MOBIUS_MQTT_BROKER_IP.toCharArray(ip, 16);

                    nCube.MQTT_ready(_mqtt, ip, MOBIUS_MQTT_BROKER_PORT, mac);

                    digitalWrite(ledPin, LOW);

                    WIFI_State = WIFI_CONNECTED;
                    wifi_previousMillis = 0;
                    wifi_wait_count = 0;
                    noti_q.pop_idx = 0;
                    noti_q.push_idx = 0;
                    upload_q.pop_idx = 0;
                    upload_q.push_idx = 0;
                }
            }
        }
        else if(WIFI_State == WIFI_RECONNECT) {
            digitalWrite(ledPin, HIGH);

            unsigned long currentMillis = millis();
            if (currentMillis - wifi_previousMillis >= wifi_led_interval) {
                wifi_previousMillis = currentMillis;
                if(wifi_wait_count++ >= wifi_interval) {
                    wifi_wait_count = 0;
                    if (WiFi.status() != WL_CONNECTED) {
                        Serial.print("Attempting to connect to SSID: ");
                        Serial.println("previous SSID");

                        WiFi.begin();
                    }
                }
                else {
                    if(wifi_wait_count % 2) {
                        digitalWrite(ledPin, HIGH);
                    }
                    else {
                        digitalWrite(ledPin, LOW);
                    }
                }
            }
            else {
                if (WiFi.status() == WL_CONNECTED) {
                    Serial.println("Connected to wifi");
                    printWiFiStatus();

                    digitalWrite(ledPin, LOW);

                    WIFI_State = WIFI_CONNECTED;
                }
            }
        }
    }
}

void printWiFiStatus() {
    // print the SSID of the network you're attached to:
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());

    // print your WiFi shield's IP address:
    IPAddress ip = WiFi.localIP();
    Serial.print("IP Address: ");
    Serial.println(ip);

    // print the received signal strength:
    long rssi = WiFi.RSSI();
    Serial.print("signal strength (RSSI):");
    Serial.print(rssi);
    Serial.println(" dBm");
}

void resp_callback(String topic, JsonObject &root) {
    int response_code = root["rsc"];
    String request_id = String(req_id);
    String response_id = root["rqi"];

    Serial.println(response_code);

    if (request_id == response_id) {
        if (response_code == 2000 || response_code == 2001 || response_code == 2002 || response_code == 4105 || response_code == 4004) {
            if (state == "create_ae") {
                sequence++;
                if(sequence >= nCube.ae_count) {
                    state = "create_cnt";
                    sequence = 0;
                }
            }
            else if (state == "create_cnt") {
                sequence++;
                if(sequence >= nCube.cnt_count) {
                    state = "delete_sub";
                    sequence = 0;
                }
            }
            else if(state == "delete_sub") {
                sequence++;
                if(sequence >= nCube.sub_count) {
                    state = "create_sub";
                    sequence = 0;
                }
            }
            else if (state == "create_sub") {
                sequence++;
                if(sequence >= nCube.sub_count) {
                    state = "create_cin";
                    sequence = 0;
                }
            }
        }
        digitalWrite(ledPin, LOW);
    }
}

void noti_callback(String topic, JsonObject &root) {
    if (state == "create_cin") {
        String sur = root["pc"]["m2m:sgn"]["sur"];
        Serial.println(sur);
        if(sur.charAt(0) != '/') {
            sur = '/' + sur;
            Serial.println(sur);
        }

        if (nCube.validSur(sur) == ("/Mobius/"+AE_NAME+"/update")) { // for OTA, update <container> resource
            const char *rqi = root["rqi"];
            String con = root["pc"]["m2m:sgn"]["nev"]["rep"]["m2m:cin"]["con"];

            noti_q.ref[noti_q.push_idx] = "update";
            noti_q.con[noti_q.push_idx] = con;
            noti_q.rqi[noti_q.push_idx] = String(rqi);
            noti_q.push_idx++;
            if(noti_q.push_idx >= QUEUE_SIZE) {
                noti_q.push_idx = 0;
            }
            if(noti_q.push_idx == noti_q.pop_idx) {
                noti_q.pop_idx++;
            }
        }
        else if (nCube.validSur(sur) == ("/Mobius/"+AE_NAME+"/led")) { // guide: uri of subscription resource for notification
            const char *rqi = root["rqi"];
            String con = root["pc"]["m2m:sgn"]["nev"]["rep"]["m2m:cin"]["con"];

            noti_q.ref[noti_q.push_idx] = "led";
            noti_q.con[noti_q.push_idx] = con;
            noti_q.rqi[noti_q.push_idx] = String(rqi);
            noti_q.push_idx++;
            if(noti_q.push_idx >= QUEUE_SIZE) {
                noti_q.push_idx = 0;
            }
            if(noti_q.push_idx == noti_q.pop_idx) {
                noti_q.pop_idx++;
            }
        }
    }
}

void buildResource() {
    nCube.configResource(2, "/Mobius", AE_NAME);                    // AE resource

    nCube.configResource(3, "/Mobius/"+AE_NAME, "update");          // Container resource
    nCube.configResource(3, "/Mobius/"+AE_NAME, "co2");             // Container resource
    nCube.configResource(3, "/Mobius/"+AE_NAME, "led");             // Container resource

    nCube.configResource(23, "/Mobius/"+AE_NAME+"/update", "sub");  // Subscription resource
    nCube.configResource(23, "/Mobius/"+AE_NAME+"/led", "sub");     // Subscription resource
}

void publisher() {
    unsigned long currentMillis = millis();
    if (currentMillis - req_previousMillis >= req_interval) {
        req_previousMillis = currentMillis;

        if(WIFI_State == WIFI_CONNECTED && nCube.MQTT_State == _MQTT_CONNECTED) {
            if (state == "create_ae") {
                Serial.print(state + " - ");
                rand_str(req_id, 8);
                Serial.print(String(sequence));
                Serial.print(" - ");
                Serial.println(String(req_id));
                nCube.createAE(req_id, 0, "3.14");
                digitalWrite(ledPin, HIGH);
            }
            else if (state == "create_cnt") {
                Serial.print(state + " - ");
                rand_str(req_id, 8);
                Serial.print(String(sequence));
                Serial.print(" - ");
                Serial.println(String(req_id));
                nCube.createCnt(req_id, sequence);
                digitalWrite(ledPin, HIGH);
            }
            else if (state == "delete_sub") {
                Serial.print(state + " - ");
                rand_str(req_id, 8);
                Serial.print(String(sequence));
                Serial.print(" - ");
                Serial.println(String(req_id));
                nCube.deleteSub(req_id, sequence);
                digitalWrite(ledPin, HIGH);
            }
            else if (state == "create_sub") {
                Serial.print(state + " - ");
                rand_str(req_id, 8);
                Serial.print(String(sequence));
                Serial.print(" - ");
                Serial.println(String(req_id));
                nCube.createSub(req_id, sequence);
                digitalWrite(ledPin, HIGH);
            }
            else if (state == "create_cin") {
                //Serial.print(state + " - ");
            }
        }
    }
}

void chkState() {
    unsigned long currentMillis = millis();
    if (currentMillis - chk_previousMillis >= chk_interval) {
        chk_previousMillis = currentMillis;

        if(WIFI_State == WIFI_CONNECT) {
            Serial.println("WIFI_CONNECT");
        }
        else if(WIFI_State == WIFI_RECONNECT) {
            Serial.println("WIFI_RECONNECT");
        }

        if(nCube.MQTT_State == _MQTT_CONNECT) {
            Serial.println("_MQTT_CONNECT");
        }
    }
}

void tasCO2Sensor_upload_callback(String con) {
    if (state == "create_cin") {
        rand_str(req_id, 8);
        upload_q.ref[upload_q.push_idx] = "/Mobius/"+AE_NAME+"/led";
        upload_q.con[upload_q.push_idx] = "\"" + tasCO2Sensor.curValue + "\"";
        upload_q.rqi[upload_q.push_idx] = String(req_id);
        upload_q.push_idx++;
        if(upload_q.push_idx >= QUEUE_SIZE) {
            upload_q.push_idx = 0;
        }
        if(upload_q.push_idx == upload_q.pop_idx) {
            upload_q.pop_idx++;
        }
    }
}


void generateProcess() {
    unsigned long currentMillis = millis();
    if (currentMillis - generate_previousMillis >= generate_interval) {
        generate_previousMillis = currentMillis;
        if (state == "create_cin") {
            tasCO2Sensor.requestData();
        }
    }
    else {
        tasCO2Sensor.chkCO2Data();
    }
}

void notiProcess() {
    if(noti_q.pop_idx != noti_q.push_idx) {
        if(noti_q.ref[noti_q.pop_idx] == "led") {
            tasLed.setLED(noti_q.con[noti_q.pop_idx]);

            String resp_body = "";
            resp_body += "{\"rsc\":\"2000\",\"to\":\"\",\"fr\":\"" + nCube.getAeid() + "\",\"pc\":\"\",\"rqi\":\"" + noti_q.rqi[noti_q.pop_idx] + "\"}";
            nCube.response(resp_body);
        }
        else if(noti_q.ref[noti_q.pop_idx] == "update") {
            if (noti_q.con[noti_q.pop_idx] == "active") {
                OTAClient.start();   // active OTAClient upgrad process

                String resp_body = "";
                resp_body += "{\"rsc\":\"2000\",\"to\":\"\",\"fr\":\"" + nCube.getAeid() + "\",\"pc\":\"\",\"rqi\":\"" + noti_q.rqi[noti_q.pop_idx] + "\"}";
                nCube.response(resp_body);
            }
        }

        noti_q.pop_idx++;
        if(noti_q.pop_idx >= QUEUE_SIZE) {
            noti_q.pop_idx = 0;
        }
    }
}

void uploadProcess() {
    if(WIFI_State == WIFI_CONNECTED && nCube.MQTT_State == _MQTT_CONNECTED) {
        unsigned long currentMillis = millis();
        if (currentMillis - uploading_previousMillis >= uploading_interval) {
            uploading_previousMillis = currentMillis;

            if (state == "create_cin") {
                UPLOAD_State = UPLOAD_UPLOADED;
            }
        }

        if((UPLOAD_State == UPLOAD_UPLOADED) && (upload_q.pop_idx != upload_q.push_idx)) {
            nCube.createCin(upload_q.rqi[upload_q.pop_idx], upload_q.ref[upload_q.pop_idx], upload_q.con[upload_q.pop_idx]);
            digitalWrite(ledPin, HIGH);

            uploading_previousMillis = currentMillis;
            UPLOAD_State = UPLOAD_UPLOADING;

            upload_q.pop_idx++;
            if(upload_q.pop_idx >= QUEUE_SIZE) {
                upload_q.pop_idx = 0;
            }
        }
    }
}

void otaProcess() {
    if(WIFI_State == WIFI_CONNECTED && nCube.MQTT_State == _MQTT_CONNECTED) {
        if (OTAClient.finished()) {
        }
        else {
            OTAClient.poll();
        }
    }
}

void setup() {
    // configure the LED pin for output mode
    pinMode(ledPin, OUTPUT);

    //Initialize serial:
    Serial.begin(9600);

    noti_q.pop_idx = 0;
    noti_q.push_idx = 0;
    upload_q.pop_idx = 0;
    upload_q.push_idx = 0;

    WiFi_init();

    delay(1000);

    nCube.setCallback(resp_callback, noti_callback);

    delay(500);

    buildResource();
    //init OTA client
    OTAClient.begin(AE_NAME, FIRMWARE_VERSION);

    tasLed.init();
    tasCO2Sensor.init();
    tasCO2Sensor.setCallback(tasCO2Sensor_upload_callback);
    delay(500);
    tasLed.setLED("0");
}

void loop() {
    WiFi_chkconnect();
    nCube.MQTT_chkconnect();

    chkState();
    publisher();
    notiProcess();
    otaProcess();
    generateProcess();
    uploadProcess();
}



// #include <Arduino.h>
// #include <SPI.h>
//
// /**
//    Copyright (c) 2017, OCEAN
//    All rights reserved.
//    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
//    1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
//    2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
//    3. The name of the author may not be used to endorse or promote products derived from this software without specific prior written permission.
//    THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// */
//
// #include <ArduinoJson.h>
//
// #include "OneM2MClient.h"
// #include "m0_ota.h"
//
// #include "TasCO2.h"
//
// #define LEDPIN 13
//
// #define LED_RED_PIN 10
// #define LED_GREEN_PIN 9
// #define LED_BLUE_PIN 6
// #define LED_GND_PIN 5
//
// TasCO2 TasCO2Sensor;
//
// const String FIRMWARE_VERSION = "1.0.0.1";
//
// const String AE_ID = "Sedu5";
// const String AE_NAME = "edu5";
// const String MQTT_BROKER_IP = "203.253.128.161";
// const uint16_t MQTT_BROKER_PORT = 1883;
// OneM2MClient nCube(MQTT_BROKER_IP, MQTT_BROKER_PORT, AE_ID); // AE-ID
//
// unsigned long req_previousMillis = 0;
// const long req_interval = 2000;
//
// unsigned long co2_sensing_previousMillis = 0;
// const long co2_sensing_interval = (1000 * 5);
//
// short action_flag = 0;
// short sensing_flag = 0;
// short control_flag = 0;
//
// String noti_con = "";
//
// char body_buff[400];  //for inputting data to publish
// char req_id[10];       //for generating random number for request packet id
//
// String resp_rqi = "";
//
// String state = "init";
//
// uint8_t g_idx = 0;
//
// String curValue = "";
//
// /*************************** Sketch Code ************************************/
//
// void rand_str(char *dest, size_t length) {
//     char charset[] = "0123456789"
//             "abcdefghijklmnopqrstuvwxyz"
//             "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
//
//     while (length-- > 0) {
//         size_t index = (double) rand() / RAND_MAX * (sizeof charset - 1);
//         *dest++ = charset[index];
//     }
//     *dest = '\0';
// }
//
// void TasCO2Sensor_upload_callback(String con) {
//     if (state == "create_cin") {
//         curValue = "\"" + TasCO2Sensor.curValue + "\"";
//         sensing_flag = 1;
//     }
// }
//
// void resp_callback(String topic, JsonObject &root) {
//     int response_code = root["rsc"];
//     String request_id = String(req_id);
//     String response_id = root["rqi"];
//
//     if (request_id == response_id) {
//         if (action_flag == 0) {
//             if (response_code == 2000 || response_code == 2001 || response_code == 2002 || response_code == 4105) {
//                 action_flag = 1;
//                 if(nCube.resource[g_idx].status == 0) {
//                     nCube.resource[g_idx].status = 1;
//                 }
//                 else {
//                     nCube.resource[g_idx].status = 2;
//                 }
//             }
//             else if (response_code == 4004) {
//                 if(state == "delete_sub") {
//                     action_flag = 1;
//                     if(nCube.resource[g_idx].status == 0) {
//                         nCube.resource[g_idx].status = 1;
//                     }
//                     else {
//                         nCube.resource[g_idx].status = 2;
//                     }
//                 }
//             }
//         }
//
//         Serial.print(topic);
//         Serial.println(F(" - RESP_TOPIC receive a message."));
//     }
// }
//
// void noti_callback(String topic, JsonObject &root) {
//     if (state == "create_cin") {
//         if (root["pc"]["m2m:sgn"]["sur"] == (nCube.resource[5].to + "/" + nCube.resource[5].rn)) { // guide: uri of subscription resource for notification
//             String con = root["pc"]["m2m:sgn"]["nev"]["rep"]["m2m:cin"]["con"];
//             noti_con = con;
//
//             const char *rqi = root["rqi"];
//             resp_rqi = String(rqi);
//             control_flag = 2;
//         }
//         else if (root["pc"]["m2m:sgn"]["sur"] == (nCube.resource[4].to + "/" + nCube.resource[4].rn)) {
//             String con = root["pc"]["m2m:sgn"]["nev"]["rep"]["m2m:cin"]["con"];
//             noti_con = con;
//
//             const char *rqi = root["rqi"];
//             resp_rqi = String(rqi);
//             control_flag = 1;
//         }
//     }
// }
//
// void buildResource() {
//     // temperally build resource structure into Mobius as oneM2M IoT Platform
//
//     // AE resource
//     uint8_t index = 0;
//     nCube.resource[index].ty = "2";
//     nCube.resource[index].to = "/Mobius";
//     nCube.resource[index].rn = AE_NAME;
//     nCube.resource[index++].status = 0;
//
//     // Container resource
//     nCube.resource[index].ty = "3";
//     nCube.resource[index].to = "/Mobius/" + nCube.resource[0].rn;
//     nCube.resource[index].rn = "update";                 // guide: do no modify, fix container name for OTA - nCube.resource[1].rn
//     nCube.resource[index++].status = 0;
//
//     nCube.resource[index].ty = "3";
//     nCube.resource[index].to = "/Mobius/" + nCube.resource[0].rn;
//     nCube.resource[index].rn = "co2";
//     nCube.resource[index++].status = 0;
//
//     nCube.resource[index].ty = "3";
//     nCube.resource[index].to = "/Mobius/" + nCube.resource[0].rn;
//     nCube.resource[index].rn = "led";
//     nCube.resource[index++].status = 0;
//
//     // Subscription resource
//     nCube.resource[index].ty = "23";
//     nCube.resource[index].to = "/Mobius/" + nCube.resource[0].rn + '/' + nCube.resource[1].rn;
//     nCube.resource[index].rn = "sub";                   // guide: do not modify, fix subscripton name for OTA - nCube.resource[4].rn
//     nCube.resource[index++].status = 0;
//
//     nCube.resource[index].ty = "23";
//     nCube.resource[index].to = "/Mobius/" + nCube.resource[0].rn + '/' + nCube.resource[3].rn;
//     nCube.resource[index].rn = "sub";
//     nCube.resource[index++].status = 0;
//
//     nCube.resource_count = index;
// }
//
// uint32_t sequence = 0;
// void publisher() {
//     int i = 0;
//
//     if (state == "create_ae") {
//         Serial.print(state);
//         Serial.print(" - ");
//         if (action_flag == 1) {
//             for (i = 0; i < nCube.resource_count; i++) {
//                 if (nCube.resource[i].ty == "2" && nCube.resource[i].status == 0) {
//                     action_flag = 0;
//                     sequence = 0;
//
//                     g_idx = i;
//
//                     rand_str(req_id, 8);
//                     Serial.print(String(sequence));
//                     Serial.print(" - ");
//                     Serial.println(String(req_id));
//                     nCube.createAE(req_id, 0, "3.14");
//                     break;
//                 }
//             }
//
//             if(action_flag == 1) {
//                 state = "create_cnt";
//                 Serial.println("");
//             }
//         }
//         else {
//             sequence++;
//             if(sequence > 2) {
//                 action_flag = 1;
//             }
//             Serial.println(String(sequence));
//         }
//     }
//
//     if (state == "create_cnt") {
//         Serial.print(state);
//         Serial.print(" - ");
//         if (action_flag == 1) {
//             for (i = 0; i < nCube.resource_count; i++) {
//                 if (nCube.resource[i].ty == "3" && nCube.resource[i].status == 0) {
//                     action_flag = 0;
//                     sequence = 0;
//
//                     g_idx = i;
//
//                     rand_str(req_id, 8);
//                     Serial.print(String(sequence));
//                     Serial.print(" - ");
//                     Serial.println(String(req_id));
//                     nCube.createCnt(req_id, i);
//                     break;
//                 }
//             }
//
//             if(action_flag == 1) {
//                 state = "delete_sub";
//                 Serial.println("");
//             }
//         }
//         else {
//             sequence++;
//             if(sequence > 2) {
//                 action_flag = 1;
//             }
//             Serial.println(String(sequence));
//         }
//     }
//
//     if (state == "delete_sub") {
//         Serial.print(state);
//         Serial.print(" - ");
//         if (action_flag == 1) {
//             for (i = 0; i < nCube.resource_count; i++) {
//                 if (nCube.resource[i].ty == "23" && nCube.resource[i].status == 0) {
//                     action_flag = 0;
//                     sequence = 0;
//
//                     g_idx = i;
//
//                     rand_str(req_id, 8);
//                     Serial.print(String(sequence));
//                     Serial.print(" - ");
//                     Serial.println(String(req_id));
//                     nCube.deleteSub(req_id, i);
//                     break;
//                 }
//             }
//
//             if(action_flag == 1) {
//                 state = "create_sub";
//                 Serial.println("");
//             }
//         }
//         else {
//             sequence++;
//             if(sequence > 2) {
//                 action_flag = 1;
//             }
//             Serial.println(String(sequence));
//         }
//     }
//
//     if (state == "create_sub") {
//         Serial.print(state);
//         Serial.print(" - ");
//         if (action_flag == 1) {
//             for (i = 0; i < nCube.resource_count; i++) {
//                 if (nCube.resource[i].ty == "23" && nCube.resource[i].status == 1) {
//                     action_flag = 0;
//                     sequence = 0;
//
//                     g_idx = i;
//
//                     rand_str(req_id, 8);
//                     Serial.print(String(sequence));
//                     Serial.print(" - ");
//                     Serial.println(String(req_id));
//                     nCube.createSub(req_id, i);
//                     break;
//                 }
//             }
//
//             if(action_flag == 1) {
//                 state = "create_cin";
//                 Serial.println("");
//             }
//         }
//         else {
//             sequence++;
//             if(sequence > 2) {
//                 action_flag = 1;
//             }
//             Serial.println(String(sequence));
//         }
//     }
//
//     else if (state == "create_cin") {
//     }
// }
//
// void setup() {
//     pinMode(LED_BLUE_PIN, OUTPUT);
//     pinMode(LED_GREEN_PIN, OUTPUT);
//     pinMode(LED_RED_PIN, OUTPUT);
//     pinMode(LED_GND_PIN, OUTPUT);
//
//     digitalWrite(LED_BLUE_PIN, HIGH);
//     digitalWrite(LED_GREEN_PIN, HIGH);
//     digitalWrite(LED_RED_PIN, HIGH);
//     digitalWrite(LED_GND_PIN, LOW);
//
//     //while (!Serial);
//     Serial.begin(115200);
//
//     delay(10);
//
//     nCube.begin();
//
//     nCube.setCallback(resp_callback, noti_callback);
//
//     buildResource();
//
//     //init OTA client
//     OTAClient.begin(AE_NAME, FIRMWARE_VERSION);
//
//     state = "create_ae";
//     action_flag = 1;
//
//     TasCO2Sensor.init();
//     TasCO2Sensor.setCallback(TasCO2Sensor_upload_callback);
//
//     digitalWrite(LED_BLUE_PIN, LOW);
//     digitalWrite(LED_GREEN_PIN, LOW);
//     digitalWrite(LED_RED_PIN, LOW);
//     digitalWrite(LED_GND_PIN, LOW);
//
//     digitalWrite(LEDPIN, LOW);
// }
//
// void loop() {
//     if(nCube.chkConnect()) {
//         if (OTAClient.finished()) {
//             unsigned long currentMillis = millis();
//
//             if (currentMillis - req_previousMillis >= req_interval) {
//                 req_previousMillis = currentMillis;
//                 publisher();
//             }
//             else if (currentMillis - co2_sensing_previousMillis >= co2_sensing_interval) {
//                 co2_sensing_previousMillis = currentMillis;
//
//                 if (state == "create_cin") {
//                     // guide: in here generate sensing data
//                     // if get sensing data directly, assign curValue sensing data and set sensing_flag to 1
//                     // if request sensing data to sensor, set sensing_flag to 0, in other code of receiving sensing data, assign curValue sensing data and set sensing_flag to 1
//
//                     TasCO2Sensor.requestData();
//                     sensing_flag = 0;
//                 }
//             }
//             else {
//                 if (state == "create_cin") {
//                     if (sensing_flag == 1) {
//                         rand_str(req_id, 8);
//                         nCube.createCin(req_id, (nCube.resource[1].to + "/" + nCube.resource[1].rn), curValue);
//                         sensing_flag = 0;
//                     }
//
//                     if (control_flag == 1) {
//                         control_flag = 0;
//                         // guide: in here control action code along to noti_con
//
//                         if (noti_con == "active") {
//                             OTAClient.start();   // active OTAClient upgrad process
//                         }
//
//                         String resp_body = "";
//                         resp_body += "{\"rsc\":\"2000\",\"to\":\"\",\"fr\":\"" + nCube.getAeid() + "\",\"pc\":\"\",\"rqi\":\"" + resp_rqi + "\"}";
//                         resp_body.toCharArray(body_buff, resp_body.length() + 1);
//                         nCube.response(body_buff);
//                     }
//                     else if (control_flag == 2) {
//                         control_flag = 0;
//                         // guide: in here control action code along to noti_con
//
//                         if (noti_con == "0") {
//                             digitalWrite(LED_BLUE_PIN, LOW);
//                             digitalWrite(LED_GREEN_PIN, LOW);
//                             digitalWrite(LED_RED_PIN, LOW);
//                         }
//                         else if (noti_con == "1") {
//                             digitalWrite(LED_BLUE_PIN, LOW);
//                             digitalWrite(LED_GREEN_PIN, LOW);
//                             digitalWrite(LED_RED_PIN, HIGH);
//                         }
//                         else if (noti_con == "2") {
//                             digitalWrite(LED_BLUE_PIN, LOW);
//                             digitalWrite(LED_GREEN_PIN, HIGH);
//                             digitalWrite(LED_RED_PIN, LOW);
//                         }
//                         else if (noti_con == "3") {
//                             digitalWrite(LED_BLUE_PIN, HIGH);
//                             digitalWrite(LED_GREEN_PIN, LOW);
//                             digitalWrite(LED_RED_PIN, LOW);
//                         }
//                         else if (noti_con == "4") {
//                             digitalWrite(LED_BLUE_PIN, HIGH);
//                             digitalWrite(LED_GREEN_PIN, HIGH);
//                             digitalWrite(LED_RED_PIN, LOW);
//                         }
//                         else if (noti_con == "5") {
//                             digitalWrite(LED_BLUE_PIN, HIGH);
//                             digitalWrite(LED_GREEN_PIN, LOW);
//                             digitalWrite(LED_RED_PIN, HIGH);
//                         }
//                         else if (noti_con == "6") {
//                             digitalWrite(LED_BLUE_PIN, LOW);
//                             digitalWrite(LED_GREEN_PIN, HIGH);
//                             digitalWrite(LED_RED_PIN, HIGH);
//                         }
//                         else if (noti_con == "7") {
//                             digitalWrite(LED_BLUE_PIN, HIGH);
//                             digitalWrite(LED_GREEN_PIN, HIGH);
//                             digitalWrite(LED_RED_PIN, HIGH);
//                         }
//
//                         String resp_body = "";
//                         resp_body += "{\"rsc\":\"2000\",\"to\":\"\",\"fr\":\"" + nCube.getAeid() + "\",\"pc\":\"\",\"rqi\":\"" + resp_rqi + "\"}";
//                         resp_body.toCharArray(body_buff, resp_body.length() + 1);
//                         nCube.response(body_buff);
//                     }
//                     TasCO2Sensor.chkCO2Data();
//                 }
//             }
//         }
//         else {
//             OTAClient.poll();
//         }
//     }
//     else {
//         digitalWrite(LEDPIN, HIGH);
//         delay(100);
//         digitalWrite(LEDPIN, LOW);
//         delay(100);
//     }
// }
