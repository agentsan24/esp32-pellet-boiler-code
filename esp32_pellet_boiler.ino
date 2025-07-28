#include <WiFi.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <base64.h>
#include "secrets.h" // <<<--- WAŻNA ZMIANA: Dodano include dla pliku secrets.h

// Konfiguracja WiFi (zalecam przeniesienie do secrets.h)
// const char* ssid = WIFI_SSID;; // <<<--- TE LINIE MOŻESZ USUNĄĆ, JEŚLI PRZENIESIESZ DO SECRETS.H
// const char* password = WIFI_PASSWORD; // <<<---

// Jeśli przeniesiesz WiFi do secrets.h, użyj tak:
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;


// Konfiguracja MQTT (te dane są obecnie publiczne, ale rzadziej stanowią zagrożenie niż tokeny API)
const char* mqttServer = "broker.emqx.io";
const int mqttPort = 1883;
const char* mqttClientId = "ESP32_Pellet_Boiler";
const char* mqttUser = MQTT_USER; // <<<--- Też możesz przenieść do secrets.h, jeśli chcesz
const char* mqttPassword = MQTT_PASSWORD; // <<<---
const char* state_topic = "pellet/boiler/state";
const char* control_topic = "pellet/boiler/control";
const char* target_temp_topic = "pellet/boiler/target_temp";
const char* power_topic = "pellet/boiler/power";

const int ledPin = 2;

// GitHub configuration
const char* githubToken = GITHUB_TOKEN; // <<<--- WAŻNA ZMIANA: Teraz pobierany z secrets.h
const char* githubRepo = "agentsan24/sterownik-pieca-web"; // <<<--- WAŻNA ZMIANA: Uproszczono format!
const char* githubIssueId = "1"; // Numer issue w GitHub

// Parametry pieca
float waterTemp = 45.0;
float roomTemp = 22.0;
float burnerTemp = 40.0;
float exhaustTemp = 60.0;
float targetTemp = 70.0;
bool boilerPower = false;

// Stany dodatkowe
bool flame = false;
bool feeder_active = false;
bool blower_active = false;
bool maintenance_mode = false;
bool ash_cleaning = false;
String igniterState = "off";
float igniterTemp = 20.0;
float currentPelletConsumptionRate = 0.0;

// Czas pracy
unsigned long session_uptime_seconds = 0;
unsigned long total_uptime_seconds = 0;
unsigned long timeToClean = 3 * 3600;

WiFiClient espClient;
PubSubClient client(espClient);

void updateBoilerState();
void postToGitHub(String message);
void sendStatusToGitHub(); // Deklaracja funkcji, jeśli nie ma

void setup() {
    Serial.begin(115200);
    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, LOW);

    WiFi.begin(ssid, password);
    Serial.print("Łączenie z WiFi...");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nPołączono! IP: " + WiFi.localIP().toString());

    client.setServer(mqttServer, mqttPort);
    client.setCallback(mqttCallback);
    reconnectMQTT();

    // Pierwsze powiadomienie na GitHub
    postToGitHub("🔥 Sterownik pieca uruchomiony! IP: " + WiFi.localIP().toString());
}

void loop() {
    if (!client.connected()) reconnectMQTT();
    client.loop();

    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate > 5000) {
        updateBoilerState();
        lastUpdate = millis();
    }
    
    // Co 5 minut wysyłaj status na GitHub
    static unsigned long lastGitHubUpdate = 0;
    if (millis() - lastGitHubUpdate > 300000) {
        lastGitHubUpdate = millis();
        sendStatusToGitHub();
    }
    
    delay(100);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
    
    Serial.print("Topic: ");
    Serial.print(topic);
    Serial.print(" | Wiadomość: ");
    Serial.println(message);

    // Komendy sterujące
    if (strcmp(topic, control_topic) == 0) {
        DynamicJsonDocument doc(256);
        DeserializationError error = deserializeJson(doc, message);
        if (error) return;

        if (doc.containsKey("command")) {
            String command = doc["command"].as<String>();
            
            if (command == "power") {
                boilerPower = doc["value"];
                digitalWrite(ledPin, boilerPower ? HIGH : LOW);
                postToGitHub("🔄 Moc pieca: " + String(boilerPower ? "WŁĄCZONA" : "WYŁĄCZONA"));
            }
            else if (command == "set_clean_countdown" && doc.containsKey("value")) {
                timeToClean = doc["value"];
                postToGitHub("⏱️ Ustawiono czas do czyszczenia: " + String(timeToClean) + "s");
            }
        }
        if (doc.containsKey("target_temp")) {
            targetTemp = doc["target_temp"];
            postToGitHub("🌡️ Ustawiono temperaturę docelową: " + String(targetTemp) + "°C");
        }
    }
    
    // Włączanie/wyłączanie
    else if (strcmp(topic, power_topic) == 0) {
        if (message == "ON") {
            boilerPower = true;
            postToGitHub("🔌 Piec WŁĄCZONY");
        }
        else if (message == "OFF") {
            boilerPower = false;
            postToGitHub("🔌 Piec WYŁĄCZONY");
        }
        digitalWrite(ledPin, boilerPower ? HIGH : LOW);
    }
    
    // Ustawianie temperatury
    else if (strcmp(topic, target_temp_topic) == 0) {
        targetTemp = message.toFloat();
        postToGitHub("🌡️ Ustawiono temperaturę: " + String(targetTemp) + "°C");
    }
    
    updateBoilerState();
}

void reconnectMQTT() {
    while (!client.connected()) {
        Serial.print("Łączenie z MQTT...");
        if (client.connect(mqttClientId, mqttUser, mqttPassword)) {
            Serial.println("Połączono!");
            client.subscribe(control_topic);
            client.subscribe(power_topic);
            client.subscribe(target_temp_topic);
            updateBoilerState();
        } else {
            Serial.print("Błąd, rc=");
            Serial.print(client.state());
            Serial.println(" Ponawianie za 5s...");
            delay(5000);
        }
    }
}

void updateBoilerState() {
    // Aktualizacja temperatur
    if (boilerPower) {
        waterTemp += random(-5, 10) / 10.0;
        if (waterTemp > targetTemp) waterTemp = targetTemp;
        if (waterTemp > 85) waterTemp = 85;
        
        burnerTemp = waterTemp - random(0, 15);
        if (burnerTemp < 20) burnerTemp = 20;
        
        exhaustTemp = waterTemp + random(5, 20);
        roomTemp = waterTemp / 3 + random(15, 25);
    } else {
        waterTemp -= random(0, 20) / 10.0;
        if (waterTemp < 20) waterTemp = 20;
        
        burnerTemp = waterTemp - random(0, 5);
        exhaustTemp = waterTemp + random(0, 5);
        roomTemp = random(18, 24);
    }

    // Symulacja stanów
    flame = boilerPower && (random(0, 100) > 30);
    feeder_active = boilerPower && (random(0, 100) > 40);
    blower_active = boilerPower;
    
    if (boilerPower && !flame) {
        igniterState = "heating";
        igniterTemp = random(500, 800) / 10.0;
    } else if (flame) {
        igniterState = "off";
    } else {
        igniterState = "off";
    }
    
    currentPelletConsumptionRate = boilerPower ? random(10, 30) / 100.0 : 0.0;

    // Czas pracy
    if (boilerPower) {
        session_uptime_seconds += 5;
        total_uptime_seconds += 5;
    }
    
    if (timeToClean > 0) timeToClean -= 5;

    // Tworzenie JSON
    DynamicJsonDocument doc(1024);
    
    // Podstawowe parametry
    doc["power"] = boilerPower;
    doc["water_temp"] = waterTemp;
    doc["room_temp"] = roomTemp;
    doc["burner_temp"] = burnerTemp;
    doc["exhaust_temp"] = exhaustTemp;
    doc["target_temp"] = targetTemp;
    doc["ip_addr"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
    
    // Stany komponentów
    doc["flame"] = flame;
    doc["feeder_active"] = feeder_active;
    doc["blower_active"] = blower_active;
    doc["maintenance_active"] = maintenance_mode;
    doc["ash_cleaning"] = ash_cleaning;
    doc["igniter_heating"] = (igniterState == "heating");
    doc["igniter_ready"] = (igniterState == "ready");
    doc["igniterState"] = igniterState;
    doc["igniterTemp"] = igniterTemp;
    
    // Czasy pracy
    doc["session_uptime"] = session_uptime_seconds;
    doc["total_uptime"] = total_uptime_seconds;
    doc["clean_countdown"] = timeToClean;
    
    // Zużycie paliwa
    doc["efficiency"] = random(85, 95);
    doc["pellet_consumption_rate"] = currentPelletConsumptionRate;
    doc["pellet_amount"] = currentPelletConsumptionRate;

    // Publikacja
    String json;
    serializeJson(doc, json);
    client.publish(state_topic, json.c_str());
    Serial.println("Wysłano: " + json);
}

// Funkcja do wysyłania wiadomości na GitHub
void postToGitHub(String message) {
    if (WiFi.status() != WL_CONNECTED) return;
    
    HTTPClient http;
    // Ważne: Format URL do GitHub API dla repozytoriów to "owner/repo_name", a nie pełny URL
    String url = "https://api.github.com/repos/" + String(githubRepo) + "/issues/" + githubIssueId + "/comments";
    
    http.begin(url);
    http.addHeader("Authorization", "Bearer " + String(githubToken));
    http.addHeader("User-Agent", "ESP32-Pellet-Controller"); // Zawsze dodawaj User-Agent dla zapytań do API
    http.addHeader("Content-Type", "application/json");
    
    String payload = "{\"body\":\"" + message + "\"}";
    int httpCode = http.POST(payload);
    
    if (httpCode == HTTP_CODE_CREATED) {
        Serial.println("Wysłano wiadomość na GitHub");
    } else {
        Serial.print("Błąd GitHub: ");
        Serial.print(httpCode);
        Serial.print(" - ");
        Serial.println(http.errorToString(httpCode)); // Dodaj więcej informacji o błędzie
    }
    http.end();
}

// Funkcja do wysyłania pełnego statusu na GitHub
void sendStatusToGitHub() {
    String status = "## 📊 Aktualny status pieca\n\n";
    status += "### 🌡️ Temperatury\n";
    status += "- Woda: " + String(waterTemp) + "°C\n";
    status += "- Docelowa: " + String(targetTemp) + "°C\n";
    status += "- Palnik: " + String(burnerTemp) + "°C\n";
    status += "- Spaliny: " + String(exhaustTemp) + "°C\n";
    status += "- Pokój: " + String(roomTemp) + "°C\n\n";
    
    status += "### ⚙️ Stany\n";
    status += "- Moc: " + String(boilerPower ? "**WŁĄCZONA**" : "**WYŁĄCZONA**") + "\n";
    status += "- Płomień: " + String(flame ? "TAK" : "NIE") + "\n";
    status += "- Podajnik: " + String(feeder_active ? "AKTYWNY" : "NIEAKTYWNY") + "\n";
    status += "- Wentylator: " + String(blower_active ? "AKTYWNY" : "NIEAKTYWNY") + "\n\n";
    
    status += "### ⏱️ Czasy pracy\n";
    status += "- Bieżąca sesja: " + String(session_uptime_seconds / 3600.0, 1) + " h\n";
    status += "- Łączny czas: " + String(total_uptime_seconds / 3600.0, 1) + " h\n";
    status += "- Czas do czyszczenia: " + String(timeToClean / 3600.0, 1) + " h\n\n";
    
    status += "### 🌐 Sieć\n";
    status += "- IP: " + WiFi.localIP().toString() + "\n";
    status += "- Siła sygnału: " + String(WiFi.RSSI()) + " dBm";
    
    postToGitHub(status);
}