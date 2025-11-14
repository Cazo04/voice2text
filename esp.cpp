#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include "driver/i2s_std.h"
#include <stdint.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>

// WiFi credentials for network connection
const char *ssid = "Home 54015 Private";
const char *password = "aimabiet";

// WebSocket server details for communication with backend
const char *ws_host = "192.168.1.6";
const uint16_t ws_port = 80;
const char *ws_path = "/wsesp";

#define OLED_SDA 7           // OLED display I2C SDA pin
#define OLED_SCL 44          // OLED display I2C SCL pin

// INMP441 microphone pins: L/R tied to 3.3V for right channel input
#define I2S_SCK 2            // I2S clock pin
#define I2S_WS 3             // I2S word select (LRCLK) pin
#define I2S_SD 5             // I2S data input pin

// I2S audio parameters: 16 kHz, 16-bit mono for voice capture
#define I2S_SAMPLE_RATE 16000
#define I2S_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT
#define I2S_CHANNEL_MODE I2S_SLOT_MODE_MONO

// Buffer size mapped to ~50 milliseconds of audio at 16kHz sample rate
const int I2S_BUFFER_SIZE = 1600;

// Amplification factor to boost microphone input signal amplitude
#define GAIN_BOOSTER 32

i2s_chan_handle_t rx_handle;         // I2S channel handle for reading samples
WebSocketsClient webSocket;          // WebSocket client instance
char i2s_read_buffer[I2S_BUFFER_SIZE]; // Buffer to hold raw audio data

// High-pass filter variable to remove DC offset and low freq noise (~below 50 Hz)
static float last_filtered_sample = 0.0f;
const float HPF_ALPHA = 0.98f;       // Filter coefficient

// OLED display controller for showing WiFi, status, and server messages
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

String oledText = "";                // Text content for OLED display update
volatile bool oledNeedsUpdate = false;  // Flag to trigger OLED refresh

// Setup I2S peripheral and OLED display, including I2C bus initialization
void setupI2S()
{
  Wire.begin(OLED_SDA, OLED_SCL);   // Initialize I2C for OLED
  Serial.println("Wire initialized.");

  u8g2.begin();                     // Initialize OLED display
  Serial.println("u8g2 initialized.");

  Serial.println("Configuring I2S...");

  // I2S RX channel configuration as master capturing from INMP441 mic
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

  // Standard I2S peripheral configuration: sample rate, mono channel, Philips standard
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
      .slot_cfg = {
          .data_bit_width = I2S_BITS_PER_SAMPLE,
          .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
          .slot_mode = I2S_CHANNEL_MODE,
          .slot_mask = I2S_STD_SLOT_RIGHT,    // Use right channel due to mic wiring
          .ws_width = I2S_BITS_PER_SAMPLE,
          .ws_pol = false,
          .bit_shift = true                   // Required Philips I2S bit shift setting
      },
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)I2S_SCK,
          .ws = (gpio_num_t)I2S_WS,
          .dout = I2S_GPIO_UNUSED,
          .din = (gpio_num_t)I2S_SD}};

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

  Serial.println("I2S driver initialized.");
}

bool isStarted = false;  // Flag controlling whether audio is being recorded and sent

// WebSocket event handler manages connection state and incoming commands
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
  switch (type)
  {
  case WStype_DISCONNECTED:
    Serial.println("Disconnected!");
    oledText = "Disconnected!";  // Show disconnect status on OLED
    oledNeedsUpdate = true;
    break;

  case WStype_CONNECTED:
    Serial.printf("Connected to url: %s\n", payload);
    break;

  case WStype_TEXT:
  {
    Serial.println("Received text");

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error)
    {
      Serial.print("deserializeJson() failed: ");
      Serial.println(error.c_str());
      return;
    }

    if (doc.containsKey("start"))
    {
      isStarted = true;          // Start sending audio data on command
      oledText = "Recording...";
      oledNeedsUpdate = true;
    }

    if (doc.containsKey("stop"))
    {
      isStarted = false;         // Stop sending audio data on command
      oledText = "Stopped.";
      oledNeedsUpdate = true;
    }

    if (doc.containsKey("text"))
    {
      oledText = doc["text"].as<String>();  // Display received text on OLED
      oledNeedsUpdate = true;
      Serial.print("Text from server: ");
      Serial.println(oledText);
    }

    break;
  }

  case WStype_BIN:
  default:
    break;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  setupI2S();

  // Initialize OLED font to support Vietnamese characters
  u8g2.setFont(u8g2_font_unifont_t_vietnamese2);
  u8g2.setFontMode(0);     // Transparent background for text
  u8g2.clearBuffer();
  u8g2.setCursor(0, 15);
  u8g2.print("Connecting WiFi...");
  u8g2.sendBuffer();

  // Connect to WiFi and wait until connected before proceeding
  Serial.printf("Connecting to %s ", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" CONNECTED!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Display WiFi connection status and IP on OLED
  u8g2.clearBuffer();
  u8g2.setCursor(0, 15);
  u8g2.print("WiFi Connected!");
  u8g2.setCursor(0, 31);
  u8g2.print(WiFi.localIP().toString());
  u8g2.sendBuffer();

  oledText = "Ready.";
  oledNeedsUpdate = true;

  // Initialize WebSocket client with reconnect interval for robustness
  webSocket.begin(ws_host, ws_port, ws_path);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void loop()
{
  webSocket.loop();

  if (webSocket.isConnected() && isStarted)
  {
    size_t bytes_read = 0;

    // Read audio samples from I2S microphone with 100ms timeout
    esp_err_t result = i2s_channel_read(
        rx_handle,
        i2s_read_buffer,
        I2S_BUFFER_SIZE,
        &bytes_read,
        pdMS_TO_TICKS(100));

    if (result == ESP_OK && bytes_read > 0)
    {
      int num_samples = bytes_read / 2;            // 2 bytes per sample (16-bit)
      int16_t *samples = (int16_t *)i2s_read_buffer;

      // Process each sample: apply high-pass filter and gain boost
      for (int i = 0; i < num_samples; i++)
      {
        float current_sample = (float)samples[i];
        float filtered_sample = HPF_ALPHA * (last_filtered_sample + current_sample - last_filtered_sample);
        last_filtered_sample = current_sample;

        int32_t boosted_sample = (int32_t)(filtered_sample * GAIN_BOOSTER);

        // Clamp samples to int16_t range to avoid clipping distortion
        if (boosted_sample > 32767)
        {
          samples[i] = 32767;
        }
        else if (boosted_sample < -32768)
        {
          samples[i] = -32768;
        }
        else
        {
          samples[i] = (int16_t)boosted_sample;
        }
      }

      // Send processed audio buffer over WebSocket as binary data
      webSocket.sendBIN((uint8_t *)i2s_read_buffer, bytes_read);
    }
    else if (result != ESP_OK)
    {
      Serial.printf("I2S Read Error: %d\n", result);
    }
  }

  // Update OLED display only when content changes to reduce flicker
  if (oledNeedsUpdate)
  {
    u8g2.clearBuffer();
    u8g2.setCursor(0, 15);
    u8g2.print(oledText);
    u8g2.sendBuffer();
    oledNeedsUpdate = false;
  }
}
