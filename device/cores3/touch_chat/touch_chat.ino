#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>

// WiFiManager opens this AP when no saved Wi-Fi credentials are available.
// Connect to it and open http://192.168.4.1 from a phone or PC.
const char *CONFIG_PORTAL_SSID = "AI-Edge-CoreS3";
const char *CONFIG_PORTAL_PASSWORD = "configureme";

// Do not include a trailing slash.
// Examples: http://192.168.1.20:8000 or https://your-domain.example
const char *SERVER_URL = "https://your-dojopaas-domain.example";

// Must match DEVICE_SHARED_TOKEN on the server. Do not put the Gemini API key here.
const char *DEVICE_SHARED_TOKEN = "replace-with-the-same-device-token";

// This text is sent when the screen button is tapped.
const char *CHAT_MESSAGE = "こんにちは。CoreS3からのテストです。";

namespace
{

  constexpr uint16_t COLOR_BACKGROUND = TFT_BLACK;
  constexpr uint16_t COLOR_TEXT = TFT_WHITE;
  constexpr uint16_t COLOR_ACCENT = TFT_CYAN;
  constexpr uint16_t COLOR_OK = TFT_GREEN;
  constexpr uint16_t COLOR_ERROR = TFT_RED;
  constexpr uint16_t COLOR_BUTTON = 0x7BEF; // dark gray
  constexpr int BUTTON_MARGIN = 16;
  constexpr int BUTTON_HEIGHT = 56;

  WiFiClient httpClient;
  WiFiClientSecure httpsClient;
  bool serverReady = false;
  bool responseScreenActive = false;

  bool isHttpsServer()
  {
    return String(SERVER_URL).startsWith("https://");
  }

  bool beginRequest(HTTPClient &http, const String &url)
  {
    if (isHttpsServer())
    {
      // PoC convenience only: this skips TLS certificate verification.
      // Use a CA certificate or certificate pinning before production use.
      httpsClient.setInsecure();
      return http.begin(httpsClient, url);
    }

    return http.begin(httpClient, url);
  }

  void drawStatus(const String &title, const String &detail, uint16_t color)
  {
    M5.Display.fillScreen(COLOR_BACKGROUND);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(color, COLOR_BACKGROUND);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(16, 18);
    M5.Display.println(title);

    M5.Display.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(16, 58);
    M5.Display.println(detail);
  }

  void drawReadyScreen()
  {
    const int width = M5.Display.width();
    const int height = M5.Display.height();
    const int buttonWidth = width - BUTTON_MARGIN * 2;
    const int buttonY = height - BUTTON_HEIGHT - BUTTON_MARGIN;

    M5.Display.fillScreen(COLOR_BACKGROUND);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(COLOR_OK, COLOR_BACKGROUND);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(16, 18);
    M5.Display.println("AI Edge Server");

    M5.Display.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(16, 58);
    M5.Display.println("Tap the button to send a chat request.");

    M5.Display.fillRoundRect(
        BUTTON_MARGIN, buttonY, buttonWidth, BUTTON_HEIGHT, 8, COLOR_BUTTON);
    M5.Display.setTextColor(COLOR_TEXT, COLOR_BUTTON);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(BUTTON_MARGIN + 58, buttonY + 18);
    M5.Display.println("SEND CHAT");
  }

  void drawResponseScreen(const char *answer)
  {
    M5.Display.fillScreen(COLOR_BACKGROUND);
    M5.Display.setFont(&fonts::lgfxJapanGothic_16);
    M5.Display.setTextSize(1);
    M5.Display.setTextWrap(true, true);
    M5.Display.setTextColor(COLOR_OK, COLOR_BACKGROUND);
    M5.Display.setCursor(8, 8);
    M5.Display.println("Gemini response");

    M5.Display.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
    M5.Display.setCursor(8, 32);
    M5.Display.println(answer);

    M5.Display.setTextColor(COLOR_ACCENT, COLOR_BACKGROUND);
    M5.Display.setCursor(8, M5.Display.height() - 20);
    M5.Display.println("Touch to continue");
    responseScreenActive = true;
  }

  bool isAnyTouchClicked()
  {
    if (M5.Touch.getCount() == 0)
    {
      return false;
    }

    return M5.Touch.getDetail().wasClicked();
  }

  bool isSendButtonClicked()
  {
    if (M5.Touch.getCount() == 0)
    {
      return false;
    }

    const auto touch = M5.Touch.getDetail();
    if (!touch.wasClicked())
    {
      return false;
    }

    const int buttonY = M5.Display.height() - BUTTON_HEIGHT - BUTTON_MARGIN;
    return touch.x >= BUTTON_MARGIN && touch.x <= M5.Display.width() - BUTTON_MARGIN &&
           touch.y >= buttonY && touch.y <= buttonY + BUTTON_HEIGHT;
  }

  void connectWiFi()
  {
    drawStatus("Wi-Fi setup", "Connect to AI-Edge-CoreS3 if needed", COLOR_ACCENT);
    WiFi.mode(WIFI_STA);
    WiFiManager wifiManager;
    wifiManager.setConfigPortalTimeout(180);
    wifiManager.setConnectTimeout(30);

    Serial.printf("Connecting to saved Wi-Fi or opening %s...\n", CONFIG_PORTAL_SSID);
    if (!wifiManager.autoConnect(CONFIG_PORTAL_SSID, CONFIG_PORTAL_PASSWORD))
    {
      Serial.println("Wi-Fi setup timed out. Restarting...");
      drawStatus("Wi-Fi setup timed out", "Restarting...", COLOR_ERROR);
      delay(1000);
      ESP.restart();
    }

    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
  }

  bool checkHealth()
  {
    HTTPClient http;
    const String url = String(SERVER_URL) + "/health";

    if (!beginRequest(http, url))
    {
      Serial.println("Could not start /health request");
      drawStatus("Health check failed", "Could not start request", COLOR_ERROR);
      return false;
    }

    http.setTimeout(15000);
    const int statusCode = http.GET();
    const String body = http.getString();
    http.end();

    Serial.printf("GET /health -> %d\n", statusCode);
    Serial.println(body);

    if (statusCode != HTTP_CODE_OK)
    {
      drawStatus("Health check failed", "HTTP " + String(statusCode), COLOR_ERROR);
      return false;
    }

    return true;
  }

  void sendChat()
  {
    drawStatus("Sending...", "Waiting for the AI response", COLOR_ACCENT);

    HTTPClient http;
    const String url = String(SERVER_URL) + "/api/chat";

    if (!beginRequest(http, url))
    {
      Serial.println("Could not start /api/chat request");
      drawReadyScreen();
      return;
    }

    StaticJsonDocument<256> requestJson;
    requestJson["message"] = CHAT_MESSAGE;

    String requestBody;
    serializeJson(requestJson, requestBody);

    http.setTimeout(60000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    if (strlen(DEVICE_SHARED_TOKEN) > 0)
    {
      http.addHeader("X-Device-Token", DEVICE_SHARED_TOKEN);
    }

    const int statusCode = http.POST(requestBody);
    const String responseBody = http.getString();
    http.end();

    Serial.printf("POST /api/chat -> %d\n", statusCode);
    Serial.println(responseBody);

    if (statusCode != HTTP_CODE_OK)
    {
      drawStatus("Chat failed", "HTTP " + String(statusCode), COLOR_ERROR);
      delay(2000);
      drawReadyScreen();
      return;
    }

    StaticJsonDocument<4096> responseJson;
    const DeserializationError error = deserializeJson(responseJson, responseBody);
    if (error)
    {
      Serial.print("JSON parse failed: ");
      Serial.println(error.c_str());
      drawStatus("Chat failed", "Invalid JSON response", COLOR_ERROR);
      delay(2000);
      drawReadyScreen();
      return;
    }

    const char *answer = responseJson["message"] | "";
    Serial.println("AI response:");
    Serial.println(answer);

    drawResponseScreen(answer);
  }

} // namespace

void setup()
{
  Serial.begin(115200);
  delay(1000);

  auto config = M5.config();
  M5.begin(config);
  M5.Display.setRotation(1);
  drawStatus("Starting...", "AI Edge Server", COLOR_ACCENT);

  connectWiFi();

  serverReady = checkHealth();
  if (!serverReady)
  {
    Serial.println("Health check failed; tap is disabled.");
    return;
  }

  Serial.println("Ready. Tap SEND CHAT on the display.");
  drawReadyScreen();
}

void loop()
{
  M5.update();

  if (responseScreenActive)
  {
    if (isAnyTouchClicked())
    {
      responseScreenActive = false;
      drawReadyScreen();
    }
  }
  else if (serverReady && isSendButtonClicked())
  {
    sendChat();
  }

  M5.delay(20);
}
