#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// Keep all device-specific settings in this block.
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Do not include a trailing slash.
// Examples: http://192.168.1.20:8000 or https://your-domain.example
const char* SERVER_URL = "https://your-dojopaas-domain.example";

// Must match DEVICE_SHARED_TOKEN on the server. Do not put the Gemini API key here.
const char* DEVICE_SHARED_TOKEN = "replace-with-the-same-device-token";

WiFiClient httpClient;
WiFiClientSecure httpsClient;

bool isHttpsServer() {
  return String(SERVER_URL).startsWith("https://");
}

bool beginRequest(HTTPClient& http, const String& url) {
  if (isHttpsServer()) {
    // PoC convenience only: this skips TLS certificate verification.
    // Use a CA certificate or certificate pinning before production use.
    httpsClient.setInsecure();
    return http.begin(httpsClient, url);
  }

  return http.begin(httpClient, url);
}

void connectWiFi() {
  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long timeoutMs = 30000;
  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection failed. Restarting...");
    delay(1000);
    ESP.restart();
  }

  Serial.print("Wi-Fi connected, IP: ");
  Serial.println(WiFi.localIP());
}

bool checkHealth() {
  HTTPClient http;
  const String url = String(SERVER_URL) + "/health";

  if (!beginRequest(http, url)) {
    Serial.println("Could not start /health request");
    return false;
  }

  http.setTimeout(15000);
  const int statusCode = http.GET();
  const String body = http.getString();
  http.end();

  Serial.printf("GET /health -> %d\n", statusCode);
  Serial.println(body);
  return statusCode == HTTP_CODE_OK;
}

void sendChat() {
  HTTPClient http;
  const String url = String(SERVER_URL) + "/api/chat";

  if (!beginRequest(http, url)) {
    Serial.println("Could not start /api/chat request");
    return;
  }

  StaticJsonDocument<256> requestJson;
  requestJson["message"] = "こんにちは";

  String requestBody;
  serializeJson(requestJson, requestBody);

  http.setTimeout(60000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  if (strlen(DEVICE_SHARED_TOKEN) > 0) {
    http.addHeader("X-Device-Token", DEVICE_SHARED_TOKEN);
  }

  const int statusCode = http.POST(requestBody);
  const String responseBody = http.getString();
  http.end();

  Serial.printf("POST /api/chat -> %d\n", statusCode);
  Serial.println(responseBody);

  if (statusCode != HTTP_CODE_OK) {
    return;
  }

  StaticJsonDocument<1024> responseJson;
  const DeserializationError error = deserializeJson(responseJson, responseBody);
  if (error) {
    Serial.print("JSON parse failed: ");
    Serial.println(error.c_str());
    return;
  }

  const char* answer = responseJson["message"] | "";
  Serial.println("AI response:");
  Serial.println(answer);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  connectWiFi();

  if (!checkHealth()) {
    Serial.println("Health check failed; chat request will not be sent.");
    return;
  }

  sendChat();
}

void loop() {
  delay(10000);
}
