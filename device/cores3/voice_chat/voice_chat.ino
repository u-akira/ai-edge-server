#include <ArduinoJson.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>

#include <cstring>

// Configure these values before flashing. Keep the token out of source control.
const char *SERVER_HOST = "your-dojopaas-domain.example";
constexpr uint16_t SERVER_PORT = 443;
const char *WS_PATH = "/ws/voice";
const char *DEVICE_SHARED_TOKEN = "replace-with-the-same-device-token";

// Use the CA chain for the WSS endpoint. Do not replace this with setInsecure().
// For a Tailscale Funnel endpoint, use the current public certificate chain.
const char *TLS_ROOT_CA = R"EOF(
-----BEGIN CERTIFICATE-----
REPLACE_WITH_SERVER_ROOT_CA
-----END CERTIFICATE-----
)EOF";

const char *CONFIG_PORTAL_SSID = "AI-Edge-CoreS3";
const char *CONFIG_PORTAL_PASSWORD = "configureme";

namespace
{

  constexpr uint32_t INPUT_SAMPLE_RATE = 16000;
  constexpr uint32_t OUTPUT_SAMPLE_RATE = 24000;
  constexpr size_t INPUT_SAMPLES_PER_CHUNK = 320; // 20 ms at 16 kHz
  constexpr size_t INPUT_CHUNK_BYTES = INPUT_SAMPLES_PER_CHUNK * sizeof(int16_t);
  constexpr size_t MAX_OUTPUT_FRAME_BYTES = 4096;
  constexpr size_t AUDIO_QUEUE_DEPTH = 12;
  constexpr size_t CONTROL_QUEUE_DEPTH = 4;

  constexpr uint16_t COLOR_BACKGROUND = TFT_BLACK;
  constexpr uint16_t COLOR_TEXT = TFT_WHITE;
  constexpr uint16_t COLOR_ACCENT = TFT_CYAN;
  constexpr uint16_t COLOR_OK = TFT_GREEN;
  constexpr uint16_t COLOR_ERROR = TFT_RED;
  constexpr uint16_t COLOR_BUTTON = 0x7BEF;
  constexpr int BUTTON_MARGIN = 16;
  constexpr int BUTTON_HEIGHT = 56;

  enum class DeviceState : uint8_t
  {
    CONNECTING,
    READY,
    RECORDING,
    WAITING,
    RESPONDING,
    ERROR_STATE,
  };

  enum class ControlCommand : uint8_t
  {
    START_RECORDING,
    STOP_RECORDING,
    CANCEL_RESPONSE,
  };

  struct AudioChunk
  {
    uint16_t length = 0;
    uint8_t data[MAX_OUTPUT_FRAME_BYTES] = {};
  };

  WebSocketsClient webSocket;
  QueueHandle_t microphoneQueue = nullptr;
  QueueHandle_t playbackQueue = nullptr;
  QueueHandle_t controlQueue = nullptr;

  volatile DeviceState deviceState = DeviceState::CONNECTING;
  volatile bool websocketConnected = false;
  volatile bool connectionResetRequested = false;
  volatile bool recordingRequested = false;
  volatile bool micCapturing = false;
  volatile bool responseActive = false;
  volatile bool responseEndReceived = false;
  volatile bool responseCancelRequested = false;

  volatile uint32_t recordingStoppedAt = 0;
  volatile uint32_t firstAudioReceivedAt = 0;
  volatile uint32_t firstAudioPlayedAt = 0;
  volatile uint32_t droppedOutputFrames = 0;
  volatile uint32_t microphoneQueueDrops = 0;

  DeviceState drawnState = DeviceState::CONNECTING;
  uint32_t lastUnderflowLogAt = 0;
  bool speakerStarted = false;

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

  void drawButtonScreen(
      const char *title,
      const char *detail,
      const char *buttonLabel,
      uint16_t titleColor)
  {
    const int width = M5.Display.width();
    const int height = M5.Display.height();
    const int buttonWidth = width - BUTTON_MARGIN * 2;
    const int buttonY = height - BUTTON_HEIGHT - BUTTON_MARGIN;

    M5.Display.fillScreen(COLOR_BACKGROUND);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(titleColor, COLOR_BACKGROUND);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(16, 18);
    M5.Display.println(title);

    M5.Display.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(16, 58);
    M5.Display.println(detail);

    M5.Display.fillRoundRect(
        BUTTON_MARGIN, buttonY, buttonWidth, BUTTON_HEIGHT, 8, COLOR_BUTTON);
    M5.Display.setTextColor(COLOR_TEXT, COLOR_BUTTON);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(BUTTON_MARGIN + 70, buttonY + 18);
    M5.Display.println(buttonLabel);
  }

  void drawForState(DeviceState state)
  {
    switch (state)
    {
    case DeviceState::CONNECTING:
      drawStatus("Connecting", "Waiting for the voice server...", COLOR_ACCENT);
      break;
    case DeviceState::READY:
      drawButtonScreen("Voice chat", "Tap TALK and speak Japanese.", "TALK", COLOR_OK);
      break;
    case DeviceState::RECORDING:
      drawButtonScreen("Listening", "Tap STOP when you finish speaking.", "STOP", COLOR_ACCENT);
      break;
    case DeviceState::WAITING:
      drawStatus("Thinking...", "Waiting for Gemini Live audio", COLOR_ACCENT);
      break;
    case DeviceState::RESPONDING:
      drawButtonScreen("Speaking", "Tap CANCEL to interrupt the answer.", "CANCEL", COLOR_OK);
      break;
    case DeviceState::ERROR_STATE:
      drawStatus("Connection error", "Reconnecting automatically...", COLOR_ERROR);
      break;
    }
  }

  bool touchClickedInButton()
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
      drawStatus("Wi-Fi setup timed out", "Restarting...", COLOR_ERROR);
      Serial.println("Wi-Fi setup timed out. Restarting...");
      delay(1000);
      ESP.restart();
    }

    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
  }

  void sendControl(ControlCommand command)
  {
    if (controlQueue != nullptr)
    {
      xQueueSend(controlQueue, &command, 0);
    }
  }

  void sendJsonEvent(const char *type)
  {
    StaticJsonDocument<256> document;
    document["type"] = type;
    String payload;
    serializeJson(document, payload);
    webSocket.sendTXT(payload);
  }

  void sendAuth()
  {
    StaticJsonDocument<384> document;
    document["type"] = "auth";
    document["token"] = DEVICE_SHARED_TOKEN;
    String payload;
    serializeJson(document, payload);
    webSocket.sendTXT(payload);
    Serial.println("WebSocket auth sent");
  }

  void setErrorState()
  {
    deviceState = DeviceState::ERROR_STATE;
    recordingRequested = false;
    responseCancelRequested = true;
    responseEndReceived = true;
  }

  void handleServerText(uint8_t *payload, size_t length)
  {
    StaticJsonDocument<768> document;
    const DeserializationError error = deserializeJson(document, payload, length);
    if (error)
    {
      Serial.printf("WebSocket JSON parse failed: %s\n", error.c_str());
      setErrorState();
      return;
    }

    const char *type = document["type"] | "";
    if (strcmp(type, "session.ready") == 0)
    {
      deviceState = DeviceState::READY;
      Serial.println("Voice session ready");
    }
    else if (strcmp(type, "response.start") == 0)
    {
      responseActive = true;
      responseEndReceived = false;
      responseCancelRequested = false;
      deviceState = DeviceState::RESPONDING;
      Serial.printf(
          "Response audio format: %lu Hz, %d-bit, %d channel(s)\n",
          document["sample_rate"] | OUTPUT_SAMPLE_RATE,
          document["bits_per_sample"] | 16,
          document["channels"] | 1);
    }
    else if (strcmp(type, "response.end") == 0)
    {
      responseEndReceived = true;
      responseCancelRequested = responseCancelRequested || (document["cancelled"] | false);
      Serial.printf("Response ended%s\n", (document["cancelled"] | false) ? " (cancelled)" : "");
    }
    else if (strcmp(type, "transcript") == 0)
    {
      const char *text = document["text"] | "";
      if (strlen(text) > 0)
      {
        Serial.printf("Transcript: %s\n", text);
      }
    }
    else if (strcmp(type, "error") == 0)
    {
      Serial.printf("Voice server error: %s\n", document["message"] | "unknown");
      setErrorState();
    }
  }

  void queueOutputAudio(uint8_t *payload, size_t length)
  {
    if (length == 0)
    {
      return;
    }

    if (!responseActive)
    {
      responseActive = true;
      deviceState = DeviceState::RESPONDING;
    }

    if (firstAudioReceivedAt == 0)
    {
      firstAudioReceivedAt = millis();
      if (recordingStoppedAt != 0)
      {
        Serial.printf(
            "Latency recording_stop_to_first_audio_received_ms=%lu\n",
            static_cast<unsigned long>(firstAudioReceivedAt - recordingStoppedAt));
      }
    }

    size_t offset = 0;
    while (offset < length)
    {
      const size_t remaining = length - offset;
      const size_t partLength = remaining < MAX_OUTPUT_FRAME_BYTES
                                    ? remaining
                                    : MAX_OUTPUT_FRAME_BYTES;
      if ((partLength & 1U) != 0U)
      {
        Serial.println("Odd-sized output PCM frame; dropping final byte");
      }

      AudioChunk chunk;
      chunk.length = static_cast<uint16_t>(partLength & ~static_cast<size_t>(1));
      memcpy(chunk.data, payload + offset, chunk.length);
      if (chunk.length == 0 || xQueueSend(playbackQueue, &chunk, 0) != pdTRUE)
      {
        droppedOutputFrames++;
        Serial.println("Audio playback queue full; dropping output frame");
        return;
      }
      offset += partLength;
    }
  }

  void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
  {
    switch (type)
    {
    case WStype_DISCONNECTED:
      websocketConnected = false;
      connectionResetRequested = true;
      recordingRequested = false;
      responseCancelRequested = true;
      responseEndReceived = true;
      deviceState = DeviceState::CONNECTING;
      Serial.println("WebSocket disconnected; reconnecting...");
      break;
    case WStype_CONNECTED:
      websocketConnected = true;
      deviceState = DeviceState::CONNECTING;
      sendAuth();
      Serial.println("WebSocket connected");
      break;
    case WStype_TEXT:
      handleServerText(payload, length);
      break;
    case WStype_BIN:
      queueOutputAudio(payload, length);
      break;
    case WStype_ERROR:
      Serial.println("WebSocket error");
      setErrorState();
      break;
    default:
      break;
    }
  }

  void drainMicrophoneQueue()
  {
    AudioChunk chunk;
    while (xQueueReceive(microphoneQueue, &chunk, 0) == pdTRUE)
    {
      webSocket.sendBIN(chunk.data, chunk.length);
    }
  }

  void microphoneTask(void *)
  {
    AudioChunk chunk;
    for (;;)
    {
      if (!recordingRequested || !websocketConnected)
      {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }

      micCapturing = true;
      M5.Mic.record(
          reinterpret_cast<int16_t *>(chunk.data),
          INPUT_SAMPLES_PER_CHUNK,
          INPUT_SAMPLE_RATE);
      micCapturing = false;

      if (!recordingRequested || !websocketConnected)
      {
        continue;
      }

      chunk.length = INPUT_CHUNK_BYTES;
      if (xQueueSend(microphoneQueue, &chunk, 0) != pdTRUE)
      {
        microphoneQueueDrops++;
      }
    }
  }

  void speakerTask(void *)
  {
    AudioChunk chunk;
    for (;;)
    {
      if (responseCancelRequested)
      {
        if (speakerStarted)
        {
          M5.Speaker.stop();
        }
        xQueueReset(playbackQueue);
        speakerStarted = false;
        responseCancelRequested = false;
      }

      if (xQueueReceive(playbackQueue, &chunk, pdMS_TO_TICKS(5)) == pdTRUE)
      {
        if (!speakerStarted)
        {
          // M5Unified's shared audio hardware must not capture and play at once.
          M5.Mic.end();
          M5.Speaker.begin();
          speakerStarted = true;
        }

        if (firstAudioPlayedAt == 0)
        {
          firstAudioPlayedAt = millis();
          if (recordingStoppedAt != 0)
          {
            Serial.printf(
                "Latency recording_stop_to_first_audio_played_ms=%lu\n",
                static_cast<unsigned long>(firstAudioPlayedAt - recordingStoppedAt));
          }
        }

        M5.Speaker.playRaw(
            reinterpret_cast<int16_t *>(chunk.data),
            chunk.length / sizeof(int16_t),
            OUTPUT_SAMPLE_RATE);

        while (M5.Speaker.isPlaying())
        {
          if (responseCancelRequested)
          {
            M5.Speaker.stop();
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(1));
        }
        continue;
      }

      if (responseActive && !responseEndReceived && !M5.Speaker.isPlaying())
      {
        const uint32_t now = millis();
        if (now - lastUnderflowLogAt > 500)
        {
          lastUnderflowLogAt = now;
          Serial.println("Audio playback buffer underrun/waiting for next frame");
        }
      }

      if (responseEndReceived && !M5.Speaker.isPlaying())
      {
        if (speakerStarted)
        {
          M5.Speaker.stop();
          M5.Speaker.end();
          M5.Mic.begin();
          speakerStarted = false;
        }
        responseActive = false;
        responseEndReceived = false;
        firstAudioReceivedAt = 0;
        firstAudioPlayedAt = 0;
        recordingStoppedAt = 0;
        if (deviceState != DeviceState::ERROR_STATE)
        {
          deviceState = websocketConnected ? DeviceState::READY : DeviceState::CONNECTING;
        }
      }
    }
  }

  void processControlCommand(ControlCommand command, bool &recordingProtocolActive)
  {
    if (!websocketConnected)
    {
      return;
    }

    switch (command)
    {
    case ControlCommand::START_RECORDING:
      if (!recordingProtocolActive)
      {
        sendJsonEvent("recording.start");
        recordingProtocolActive = true;
      }
      break;
    case ControlCommand::STOP_RECORDING:
      if (recordingProtocolActive)
      {
        // Wait for the current 20 ms capture to finish so STOP remains last.
        const uint32_t waitStartedAt = millis();
        while (micCapturing && millis() - waitStartedAt < 100)
        {
          vTaskDelay(pdMS_TO_TICKS(1));
        }
        drainMicrophoneQueue();
        sendJsonEvent("recording.stop");
        recordingProtocolActive = false;
      }
      break;
    case ControlCommand::CANCEL_RESPONSE:
      responseCancelRequested = true;
      responseEndReceived = true;
      sendJsonEvent("response.cancel");
      break;
    }
  }

  void networkTask(void *)
  {
    bool recordingProtocolActive = false;
    for (;;)
    {
      webSocket.loop();

      if (connectionResetRequested)
      {
        connectionResetRequested = false;
        recordingProtocolActive = false;
        recordingRequested = false;

        AudioChunk discardedAudio;
        while (xQueueReceive(microphoneQueue, &discardedAudio, 0) == pdTRUE)
        {
        }

        ControlCommand discardedCommand;
        while (xQueueReceive(controlQueue, &discardedCommand, 0) == pdTRUE)
        {
        }
      }

      ControlCommand command;
      while (xQueueReceive(controlQueue, &command, 0) == pdTRUE)
      {
        processControlCommand(command, recordingProtocolActive);
      }

      if (websocketConnected && recordingProtocolActive && recordingRequested)
      {
        drainMicrophoneQueue();
      }

      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }

  void startWebSocket()
  {
    webSocket.beginSslWithCA(
        SERVER_HOST,
        SERVER_PORT,
        WS_PATH,
        TLS_ROOT_CA,
        "arduino");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    Serial.printf("Connecting to wss://%s:%u%s\n", SERVER_HOST, SERVER_PORT, WS_PATH);
  }

} // namespace

void setup()
{
  Serial.begin(115200);
  delay(1000);

  auto config = M5.config();
  M5.begin(config);
  M5.Display.setRotation(1);
  drawForState(DeviceState::CONNECTING);

  connectWiFi();

  microphoneQueue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(AudioChunk));
  playbackQueue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(AudioChunk));
  controlQueue = xQueueCreate(CONTROL_QUEUE_DEPTH, sizeof(ControlCommand));
  if (microphoneQueue == nullptr || playbackQueue == nullptr || controlQueue == nullptr)
  {
    drawStatus("Memory error", "Could not create audio queues", COLOR_ERROR);
    Serial.println("Could not create FreeRTOS queues");
    while (true)
    {
      M5.delay(1000);
    }
  }

  auto micConfig = M5.Mic.config();
  micConfig.sample_rate = INPUT_SAMPLE_RATE;
  M5.Mic.config(micConfig);
  M5.Mic.begin();

  startWebSocket();
  xTaskCreatePinnedToCore(microphoneTask, "voice-mic", 4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(speakerTask, "voice-speaker", 4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(networkTask, "voice-network", 6144, nullptr, 3, nullptr, 1);
}

void loop()
{
  M5.update();

  const DeviceState currentState = deviceState;
  if (currentState != drawnState)
  {
    drawForState(currentState);
    drawnState = currentState;
  }

  if (touchClickedInButton())
  {
    if (currentState == DeviceState::READY)
    {
      AudioChunk discarded;
      while (xQueueReceive(microphoneQueue, &discarded, 0) == pdTRUE)
      {
      }
      recordingStoppedAt = 0;
      recordingRequested = true;
      deviceState = DeviceState::RECORDING;
      sendControl(ControlCommand::START_RECORDING);
      Serial.println("Recording started");
    }
    else if (currentState == DeviceState::RECORDING)
    {
      recordingRequested = false;
      recordingStoppedAt = millis();
      firstAudioReceivedAt = 0;
      firstAudioPlayedAt = 0;
      deviceState = DeviceState::WAITING;
      sendControl(ControlCommand::STOP_RECORDING);
      Serial.printf("Recording stopped at %lu ms\n", static_cast<unsigned long>(recordingStoppedAt));
    }
    else if (currentState == DeviceState::RESPONDING)
    {
      responseCancelRequested = true;
      responseEndReceived = true;
      deviceState = DeviceState::READY;
      sendControl(ControlCommand::CANCEL_RESPONSE);
      Serial.println("Response cancellation requested");
    }
  }

  M5.delay(10);
}
