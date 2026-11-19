/*
 * Motorwise read-only reader: the start-up for the Bluetooth-only build (CONFIG_MOTORWISE_BLE_ONLY).
 *
 * Replaces main.c in that build. ELM327 over Bluetooth LE is the only way in: no access point, no web
 * configuration, no Wi-Fi updates, no MQTT, no webhooks, no TCP port. Those sources are not compiled
 * (main/CMakeLists.txt). Every frame for the car still passes the read-only gate in can_send().
 *
 * Settings are built in (mw_settings.h). The Bluetooth passkey is read from NVS (written per unit at flash
 * time by tools/flash_unit.sh); without one, Bluetooth stays off. There is no default passkey.
 *
 * Based on main.c of the WiCAN project, Copyright (C) 2022 Meatpi Electronics, written by Ali Slim.
 * Licence: GPL-3.0, like the rest of wican-fw.
 */
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_gap_ble_api.h"
#include "types.h"
#include "can.h"
#include "ble.h"
#include "elm327.h"
#include "sleep_mode.h"
#include "dev_status.h"
#include "hw_config.h"
#include "mw_settings.h"

#define TAG "motorwise"

#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<CONNECTED_LED_GPIO_NUM) | (1ULL<<ACTIVE_LED_GPIO_NUM) | (1ULL<<PWR_LED_GPIO_NUM))

static QueueHandle_t xmsg_rx_queue, xmsg_ble_tx_queue, xmsg_obd_rx_queue;
static uint8_t ble_uid[33];

uint8_t project_hardware_rev;

void send_to_host(char* str, uint32_t len, QueueHandle_t *q)
{
	static xdev_buffer xsend_buffer;

	xsend_buffer.usLen = (len == 0) ? strlen(str) : len;
	memcpy(xsend_buffer.ucElement, str, xsend_buffer.usLen);
	xQueueSend(*q, (void *)&xsend_buffer, portMAX_DELAY);
	memset(xsend_buffer.ucElement, 0, sizeof(xsend_buffer.ucElement));
	xsend_buffer.usLen = 0;
}

static void process_led(bool state)
{
	static bool current_state;
	static int64_t last_change;

	if(!can_is_enabled())
	{
		gpio_set_level(ACTIVE_LED_GPIO_NUM, 1);
		current_state = 0;
		last_change = esp_timer_get_time();
	}
	if(esp_timer_get_time() - last_change < 20*1000)
	{
		return;
	}
	if(current_state == state)
	{
		return;
	}
	last_change = esp_timer_get_time();
	current_state = state;
	gpio_set_level(ACTIVE_LED_GPIO_NUM, state ? 0 : 1);
}

/* Commands from the app (Bluetooth) to the ELM327 interpreter. */
static void can_tx_task(void *pvParameters)
{
	static xdev_buffer rx_buffer;
	while(1)
	{
		twai_message_t tx_msg;

		memset(rx_buffer.ucElement, 0, DEV_BUFFER_LENGTH);
		xQueueReceive(xmsg_rx_queue, &rx_buffer, portMAX_DELAY);
		dev_status_wait_for_bits(DEV_AWAKE_BIT, portMAX_DELAY);

		if(rx_buffer.dev_channel == DEV_BLE)
		{
			elm327_process_cmd(rx_buffer.ucElement, rx_buffer.usLen, &tx_msg, &xmsg_ble_tx_queue);
		}
	}
}

/* Frames from the car to the ELM327 interpreter, which picks the answers it waits for. */
static void can_rx_task(void *pvParameters)
{
	static twai_message_t rx_msg;
	while(1)
	{
		process_led(0);
		dev_status_wait_for_bits(DEV_AWAKE_BIT, portMAX_DELAY);

		while(can_receive(&rx_msg, 0) == ESP_OK)
		{
			process_led(1);
			if(ble_connected() && elm327_ready_to_receive())
			{
				xQueueSend(xmsg_obd_rx_queue, (void *)&rx_msg, pdMS_TO_TICKS(0));
			}
		}
		vTaskDelay(pdMS_TO_TICKS(1));
	}
}

/* The unit's passkey from NVS; false when there is none or it isn't a 6-digit number. */
static bool read_passkey(uint32_t *passkey)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(MW_NVS_NAMESPACE, NVS_READONLY, &h);
	if(err != ESP_OK)
	{
		return false;
	}
	err = nvs_get_u32(h, MW_NVS_PASSKEY_KEY, passkey);
	nvs_close(h);
	return err == ESP_OK && *passkey <= 999999;
}

static void detect_hardware(void)
{
	const esp_partition_t *running = esp_ota_get_running_partition();
	esp_app_desc_t app;

	project_hardware_rev = WICAN_V300;
	if(esp_ota_get_partition_description(running, &app) == ESP_OK)
	{
		ESP_LOGI(TAG, "running from 0x%08lx, build %s", running->address, app.version);
		if(strstr(app.project_name, "usb") != 0)
		{
			project_hardware_rev = WICAN_USB_V100;
		}
		else if(strstr(app.project_name, "hv210") != 0)
		{
			project_hardware_rev = WICAN_V210;
		}
	}
}

void app_main(void)
{
	uint8_t mac[6] = {0};
	uint32_t passkey = 0;
	bool have_passkey = false;

	dev_status_init();
	dev_status_set_bits(DEV_AWAKE_BIT);

	ESP_LOGI(TAG, "%s", MW_DEVICE_DESCRIPTION);
	ESP_LOGI(TAG, "Wi-Fi: not in this build");

	// NVS holds the passkey and the Bluetooth bonds. Never erase it here: a unit that can't read it keeps
	// Bluetooth off until it is flashed again over USB.
	esp_err_t nvs_err = nvs_flash_init();
	if(nvs_err == ESP_OK)
	{
		have_passkey = read_passkey(&passkey);
	}
	else
	{
		ESP_LOGE(TAG, "NVS unreadable (%s)", esp_err_to_name(nvs_err));
	}

	gpio_config_t io_conf = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = GPIO_OUTPUT_PIN_SEL,
		.pull_down_en = 0,
		.pull_up_en = 0,
	};
	gpio_config(&io_conf);
	gpio_set_level(CONNECTED_LED_GPIO_NUM, 1);
	gpio_set_level(ACTIVE_LED_GPIO_NUM, 1);

	gpio_reset_pin(CAN_STDBY_GPIO_NUM);
	gpio_set_direction(CAN_STDBY_GPIO_NUM, GPIO_MODE_OUTPUT);
	gpio_set_level(CAN_STDBY_GPIO_NUM, 1);

	xmsg_rx_queue = xQueueCreate(16, sizeof(xdev_buffer));
	xmsg_ble_tx_queue = xQueueCreate(100, sizeof(xdev_buffer));
	xmsg_obd_rx_queue = xQueueCreate(32, sizeof(twai_message_t));

	esp_ota_mark_app_valid_cancel_rollback();
	detect_hardware();

	// Same name as the stock firmware (WiC_ + the access point's MAC), so paired phones still know the reader.
	ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
	sprintf((char *)ble_uid, "WiC_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	can_init(MW_CAN_RATE);
	can_set_bitrate(MW_CAN_RATE);
	can_set_silent(0);
	can_enable();
	elm327_init(&send_to_host, &xmsg_obd_rx_queue, NULL);
	ESP_LOGI(TAG, "ELM327 on CAN 500 kbit/s, read-only gate on");

	if(have_passkey)
	{
		ble_init(&xmsg_ble_tx_queue, &xmsg_rx_queue, CONNECTED_LED_GPIO_NUM, (int)passkey, &ble_uid[0]);
		passkey = 0;
		ESP_LOGI(TAG, "Bluetooth on as %s, passkey from NVS, bonds stored: %d", ble_uid, esp_ble_get_bond_device_num());
	}
	else
	{
		ESP_LOGE(TAG, "no passkey: Bluetooth stays off (flash the unit with tools/flash_unit.sh)");
	}

	xTaskCreate(can_rx_task, "can_rx_task", 1024*3, NULL, 5, NULL);
	xTaskCreate(can_tx_task, "can_tx_task", 1024*3, NULL, 5, NULL);

	sleep_mode_init(project_hardware_rev != WICAN_V210, MW_SLEEP_VOLTAGE);
	ESP_LOGI(TAG, "sleep on: below %.1f V and no request for %d min", MW_SLEEP_VOLTAGE, MW_SLEEP_MINUTES);

	gpio_set_level(PWR_LED_GPIO_NUM, 1);

	// After start-up only warnings; the Bluetooth stack's own lines stay quiet.
	esp_log_level_set("*", ESP_LOG_WARN);
}
