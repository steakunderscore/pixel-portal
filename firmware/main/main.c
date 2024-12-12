#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "secrets.h"
#include "led_strip.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

#define LED_STRIP_GPIO_PIN 14
#define LED_STRIP_LED_LEN 8
#define LED_STRIP_LED_WIDTH 8
#define LED_STRIP_LED_COUNT (LED_STRIP_LED_LEN * LED_STRIP_LED_WIDTH)

static const char *TAG = "pixel-portal";

static QueueHandle_t led_cmd_queue;

// Define command structure
typedef struct
{
    uint32_t led_index;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_command_t;

led_strip_handle_t configure_led(void)
{
    // LED strip general initialization, according to your led board design
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO_PIN, // The GPIO that connected to the LED strip's data line
        .max_leds = LED_STRIP_LED_COUNT,      // The number of LEDs in the strip,
        .led_model = LED_MODEL_WS2812,        // LED strip model
        // set the color order of the strip: GRB
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags = {
            .invert_out = false, // Don't invert the output signal on the data line
        }};

    // LED strip backend configuration: SPI
    led_strip_spi_config_t spi_config = {
        .clk_src = SPI_CLK_SRC_DEFAULT, // different clock source can lead to different power consumption
        .spi_bus = SPI2_HOST,           // SPI bus ID
        .flags = {
            .with_dma = true, // Using DMA can improve performance and help drive more LEDs
        }};

    led_strip_handle_t led_strip;
    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &led_strip));
    ESP_LOGI(TAG, "Created LED strip object with SPI backend");

    // clear the LED strip
    ESP_ERROR_CHECK(led_strip_clear(led_strip));
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));

    return led_strip;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying to connect to the AP");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: %s", ip4addr_ntoa((const ip4_addr_t *)&event->ip_info.ip));
    }
}

void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();

    esp_wifi_set_ps(WIFI_PS_NONE); // Disable power saving

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

// scale down brightness to avoid overloading the LEDs
#define LED_BRIGHTNESS_SCALER 127
#define LED_MAX_BRIGHTNESS 255 / 2
#define LED_MIN_BRIGHTNESS 0

int clamp(int value, int min, int max)
{
    return value < min ? min : value > max ? max
                                           : value;
}

// Define function to create a new LED command
static led_command_t new_led_command(int led_index, int red, int green, int blue, int brightness)
{
    // clamp the color values to LED_MIN_BRIGHTNESS-LED_MAX_BRIGHTNESS
    red = clamp((red * brightness) - LED_BRIGHTNESS_SCALER, LED_MIN_BRIGHTNESS, LED_MAX_BRIGHTNESS);
    green = clamp((green * brightness) - LED_BRIGHTNESS_SCALER, LED_MIN_BRIGHTNESS, LED_MAX_BRIGHTNESS);
    blue = clamp((blue * brightness) - LED_BRIGHTNESS_SCALER, LED_MIN_BRIGHTNESS, LED_MAX_BRIGHTNESS);

    led_command_t cmd = {
        .led_index = led_index,
        .red = red,
        .green = green,
        .blue = blue};
    return cmd;
}

static void websocket_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    if (event_id == WEBSOCKET_EVENT_DATA)
    {
        // Parse JSON data
        cJSON *json = cJSON_ParseWithLength((char *)data->data_ptr, data->data_len);
        if (json == NULL)
        {
            ESP_LOGW(TAG, "Empty JSON data received");
            return;
        }

        cJSON *x = cJSON_GetObjectItem(json, "x");
        cJSON *y = cJSON_GetObjectItem(json, "y");
        cJSON *color = cJSON_GetObjectItem(json, "color");
        cJSON *brightness = cJSON_GetObjectItem(json, "brightness");

        if (cJSON_IsNumber(x) && cJSON_IsNumber(y) && cJSON_IsString(color) && cJSON_IsNumber(brightness))
        {
            int led_index = x->valueint + y->valueint * 8; // Assuming an 8x8 LED matrix
            int red, green, blue;
            sscanf(color->valuestring, "#%02x%02x%02x", &red, &green, &blue);

            // log led_index, red, green, blue, brightness
            ESP_LOGI(TAG, "Received LED command, led_index: %d, red: %d, green: %d, blue: %d, brightness: %f", led_index, red, green, blue, brightness->valuedouble);

            led_command_t cmd = new_led_command(led_index, red, green, blue, brightness->valuedouble);

            // Send command to queue
            xQueueSend(led_cmd_queue, &cmd, portMAX_DELAY);
        }

        cJSON_Delete(json);
    }
}

// Square of white leds to initalise the LEDs matrix with
const uint8_t white_leds[LED_STRIP_LED_LEN][LED_STRIP_LED_WIDTH] = {
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10},
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10},
    {0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10},
    {0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10},
    {0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10},
    {0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10},
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10},
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}};

static void led_update_task(void *pvParameters)
{
    led_command_t cmd;
    led_strip_handle_t led_strip = configure_led();

    // Clear LED strip
    ESP_ERROR_CHECK(led_strip_clear(led_strip));
    for (int i = 0; i < LED_STRIP_LED_LEN; i++)
    {
        for (int j = 0; j < LED_STRIP_LED_WIDTH; j++)
        {
            // write initial logo to the LED strip
            ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, i * LED_STRIP_LED_WIDTH + j, white_leds[i][j], white_leds[i][j], white_leds[i][j]));
        }
    }
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));

    while (1)
    {
        if (xQueueReceive(led_cmd_queue, &cmd, (TickType_t)100))
        {
            ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, cmd.led_index,
                                                cmd.red, cmd.green, cmd.blue));
            ESP_ERROR_CHECK(led_strip_refresh(led_strip));
        }
    }
}

extern const char cacert_start[] asm("_binary_ca_cert_pem_start"); // CA certificate

void ws_init(void)
{
    // Initialize WebSocket client
    esp_websocket_client_config_t websocket_cfg = {
        .task_name = "ws_pixel_receiver",
        .uri = "wss://pixel-portal-xpv3wpdyrq-ew.a.run.app/ws",
        .cert_pem = (const char *)cacert_start,
        // .task_stack = 6144,          // Increase stack size
        .task_prio = 5,              // Higher priority (default is 3)
        .network_timeout_ms = 10000, // Shorter timeout
        .ping_interval_sec = 5       // More frequent keepalive
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    esp_websocket_client_start(client);

    ESP_LOGI(TAG, "WebSocket client started");
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Wi-Fi
    wifi_init_sta();

    // Create command queue
    led_cmd_queue = xQueueCreate(10, sizeof(led_command_t));

    // Create LED update task
    xTaskCreate(led_update_task, "led_update", 4096, NULL, 5, NULL);

    // Initialize WebSocket client
    ws_init();

    // Start the main loop
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}