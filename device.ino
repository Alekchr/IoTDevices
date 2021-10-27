
#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <Preferences.h>
#include <BLEAdvertising.h>
#include <WiFi.h>
#include "src/led.h"
#include <nvs.h>
#include <nvs_flash.h>

#define SERVICE_UUID  "0000aaaa-ead2-11e7-80c1-9a214cf093ae"
#define WIFI_UUID     "00005555-ead2-11e7-80c1-9a214cf093ae"

char dvName[] = "Device1";

// SSID and password of WiFi
String ssid = "";
String password = "";

String brickName = "";
/** Characteristic for digital output */
BLECharacteristic *pCharacteristicWiFi;

/**BLE services */
BLEAdvertising *pAdvertising;
BLEService *pService;
BLEServer *pServer;

static bool hasIoTHub = false;
static bool hasWifiCreds = false;
static bool hasWifi = false;
static bool needs_reconnect = false;
static bool conChanged = false;
static bool hasBrickName = false;

class theServerCallbacks : public BLEServerCallbacks
{
    // TODO this doesn't take into account several clients being connected
    void onConnect(BLEServer *pServer)
    {
        Serial.println("BLE client connected");
    };

    void onDisconnect(BLEServer *pServer)
    {
        Serial.println("BLE client disconnected");
        pAdvertising->start();
    }
};

class theCallbackHandler : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        Serial.print("Received over BLE: ");
        std::string value = pCharacteristic->getValue();
        if (value.length() == 0)
        {
            return;
        }
        Serial.println(String((char *)&value[0]));

        // Decode data
        int keyIndex = 0;
        for (int index = 0; index < value.length(); index++)
        {
            value[index] = (char)value[index] ^ (char)dvName[keyIndex];
            keyIndex++;
            if (keyIndex >= strlen(dvName))
                keyIndex = 0;
        }

        /** Json object for incoming data */
        StaticJsonDocument<200> jsondoc;
        auto error = deserializeJson(jsondoc, (char *)&value[0]);
        if (error)
        {
            Serial.print(F("deserializeJson() failed with code "));
            Serial.println(error.c_str());
            return;
        }

        if (jsondoc.containsKey("ssid") &&
            jsondoc.containsKey("password"))
        {
            ssid = jsondoc["ssid"].as<String>();
            password = jsondoc["password"].as<String>();
            brickName = jsondoc["brickName"].as<String>();
            Preferences preferences;
            preferences.begin("WiFiCreds", false);
            preferences.putString("ssid", ssid);
            preferences.putString("password", password);
            preferences.putString("brickName", brickName);
            preferences.putBool("valid", true);
            preferences.end();
            Serial.println("Info from bluetooth:");
            Serial.println("SSID: " + ssid + " password: " + password + ".");
            Serial.println("brickName" + brickName + ".");
            conChanged = true;
            hasWifiCreds = true;
            hasBrickName = true;
        }
        else if (jsondoc.containsKey("erase"))
        {
            Serial.println("Received erase command");
            Preferences preferences;
            preferences.begin("WiFiCreds", false);
            preferences.clear();
            preferences.end();
            conChanged = true;
            hasWifiCreds = false;
            hasBrickName = false;
            ssid = "";
            password = "";
            brickName = "";
            int err;
            err = nvs_flash_init();
            Serial.println("nvs_flash_init: " + err);
            err = nvs_flash_erase();
            Serial.println("nvs_flash_erase: " + err);
        }
        else if (jsondoc.containsKey("reset"))
        {
            WiFi.disconnect();
            esp_restart();
        }
        jsondoc.clear();
    };

    void onRead(BLECharacteristic *pCharacteristic)
    {
        Serial.println("BLE onRead request");
        String wifiCreds;

        /** Json object for outgoing data */
        StaticJsonDocument<200> jsondoc;
        jsondoc["ssid"] = ssid;
        jsondoc["password"] = password;
        jsondoc["brickName"] = brickName;
        // Convert JSON object into a string
        serializeJson(jsondoc, wifiCreds);

        // encode the data
        int keyIndex = 0;
        Serial.println("Stored settings: " + wifiCreds);
        for (int index = 0; index < wifiCreds.length(); index++)
        {
            wifiCreds[index] = (char)wifiCreds[index] ^ (char)dvName[keyIndex];
            keyIndex++;
            if (keyIndex >= strlen(dvName))
                keyIndex = 0;
        }
        pCharacteristicWiFi->setValue((uint8_t *)&wifiCreds[0], wifiCreds.length());
        jsondoc.clear();
    }
};

void setupBluetooth()
{

    BLEDevice::init(dvName);

    // Create BLE Server
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new theServerCallbacks());

    // Create BLE Service
    pService = pServer->createService(BLEUUID(SERVICE_UUID), 20);

    // Create BLE Characteristic for WiFi settings
    pCharacteristicWiFi = pService->createCharacteristic(
        BLEUUID(WIFI_UUID),
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE);
    pCharacteristicWiFi->setCallbacks(new theCallbackHandler());

    // Start the service
    pService->start();

    pAdvertising = pServer->getAdvertising();
    pAdvertising->start();
}

void gotIP(system_event_id_t event)
{
    hasWifi = true;
    conChanged = true;
}

void lostCon(system_event_id_t event)
{
    hasWifi = false;
    conChanged = true;
}

void setupWiFi()
{
    WiFi.onEvent(gotIP, SYSTEM_EVENT_STA_GOT_IP);
    WiFi.onEvent(lostCon, SYSTEM_EVENT_STA_DISCONNECTED);

    WiFi.disconnect(true);
    WiFi.enableSTA(true);
    WiFi.mode(WIFI_STA);

    Serial.print("Start connecting to ");
    Serial.println(ssid);
    WiFi.begin(ssid.c_str(), password.c_str());
    toggle_wifi_led(1);
}

void setup()
{
    // Initialize Serial port
    Serial.begin(115200);

    // Send some device info
    Preferences preferences;
    preferences.begin("WiFiCreds", false);
    bool hasPref = preferences.getBool("valid", false);
    if (hasPref)
    {
        ssid = preferences.getString("ssid", "");
        password = preferences.getString("password", "");
        brickName = preferences.getString("brickName", "");
        if (ssid.equals("") || password.equals(""))
        {
            Serial.println("Found preferences but credentials are invalid");
        }
        else
        {
            Serial.println("SSID: " + ssid + " password: " + password);
            hasWifiCreds = true;
        }
    }
    else
    {
        Serial.println("Could not find preferences, activate ble");
    }
    preferences.end();
    if (hasWifiCreds)
    {
        setupWiFi();
    }

    // Start BLE server
    setupBluetooth();
}

void loop()
{
    if (conChanged)
    {
        if (hasWifi)
        {
            Serial.print("Connected to AP: ");
            Serial.print(WiFi.SSID());
            Serial.print(" with IP: ");
            Serial.print(WiFi.localIP());
            Serial.print(" RSSI: ");
            Serial.println(WiFi.RSSI());
        }
        else
        {
            if (hasWifiCreds)
            {
                Serial.println("Lost WiFi connection");
                setupWiFi();
            }
        }
        conChanged = false;
    }
    delay(1000);
}
