#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h> // WebSocket client library
#include "driver/i2s_std.h"   // I2S library (from ESP-IDF)
#include <stdint.h>           // For int16_t, int32_t
#include <ArduinoJson.h>

// WiFi configuration
const char *ssid = "Home 54015 Private"; // Replace with your WiFi SSID
const char *password = "aimabiet";       // Replace with your WiFi password

// WebSocket server configuration (Python server)
const char *ws_host = "192.168.1.6"; // Replace with your computer's IP
const uint16_t ws_port = 8080;
const char *ws_path = "/ws";

// INMP441 pin configuration
#define I2S_SCK 2
#define I2S_WS 3
#define I2S_SD 5
// Assumes L/R pin connected to 3.3V

// I2S configuration
#define I2S_SAMPLE_RATE 16000
#define I2S_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT
#define I2S_CHANNEL_MODE I2S_SLOT_MODE_MONO

// Buffer size: 1600 bytes (50ms at 16kHz, 16-bit mono)
const int I2S_BUFFER_SIZE = 1600;

// Gain booster for audio amplification (try values from 16 to 48)
#define GAIN_BOOSTER 32

// Global handles and buffers
i2s_chan_handle_t rx_handle;
WebSocketsClient webSocket;
char i2s_read_buffer[I2S_BUFFER_SIZE];

// High-pass filter variables
static float last_filtered_sample = 0.0f;
const float HPF_ALPHA = 0.98f; // Cuts frequencies below ~50Hz

void setupI2S()
{
    Serial.println("Configuring I2S...");

    // Channel configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    // Standard configuration
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = {
            .data_bit_width = I2S_BITS_PER_SAMPLE,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
            .slot_mode = I2S_CHANNEL_MODE,
            .slot_mask = I2S_STD_SLOT_RIGHT, // Use right channel (L/R tied to 3.3V)
            .ws_width = I2S_BITS_PER_SAMPLE,
            .ws_pol = false,
            .bit_shift = true // Enable for Philips I2S standard
        },
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)I2S_SCK, .ws = (gpio_num_t)I2S_WS, .dout = I2S_GPIO_UNUSED, .din = (gpio_num_t)I2S_SD}};

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    Serial.println("I2S driver initialized.");
}

double isStarted = false;

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        Serial.println("[WSc] Disconnected!");
        break;
    case WStype_CONNECTED:
        Serial.printf("[WSc] Connected to url: %s\n", payload);
        break;
    case WStype_TEXT:
        Serial.printf("[WSc] Received text");

        StaticJsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload, length);

        if (error)
        {
            Serial.print("deserializeJson() failed: ");
            Serial.println(error.c_str());
            return;
        }

        if (doc.containsKey("start"))
        {
            isStarted = true;
        }

        if (doc.containsKey("stop"))
        {
            isStarted = false;
        }

        if (doc.containsKey("text"))
        {
            Serial.print("Text from server: ");
            Serial.println(doc["text"].as<const char *>());
        }

        break;
    case WStype_BIN:
        // No binary data expected
        break;
    default:
        break;
    }
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    // Initialize I2S
    setupI2S();

    // Connect to WiFi
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

    // Initialize WebSocket client
    webSocket.begin(ws_host, ws_port, ws_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000); // Reconnect after 5 seconds
}

void loop()
{
    webSocket.loop(); // Must call continuously

    if (webSocket.isConnected() && isStarted)
    {
        size_t bytes_read = 0;

        // Read from I2S microphone
        esp_err_t result = i2s_channel_read(
            rx_handle,
            i2s_read_buffer,
            I2S_BUFFER_SIZE,
            &bytes_read,
            pdMS_TO_TICKS(100));

        if (result == ESP_OK && bytes_read > 0)
        {
            int num_samples = bytes_read / 2;
            int16_t *samples = (int16_t *)i2s_read_buffer;

            // Audio processing
            for (int i = 0; i < num_samples; i++)
            {
                // High-pass filter
                float current_sample = (float)samples[i];
                float filtered_sample = HPF_ALPHA * (last_filtered_sample + current_sample - last_filtered_sample);
                last_filtered_sample = current_sample;

                // Apply gain booster
                int32_t boosted_sample = (int32_t)(filtered_sample * GAIN_BOOSTER);

                // Clamp to prevent clipping
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

            // Send processed data to Python server
            webSocket.sendBIN((uint8_t *)i2s_read_buffer, bytes_read);
        }
        else if (result != ESP_OK)
        {
            Serial.printf("I2S Read Error: %d\n", result);
        }
    }
}
