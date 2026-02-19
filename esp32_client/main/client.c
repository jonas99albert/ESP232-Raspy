/*
 * ESP32-C3 TCP-Client  (ESP-IDF Framework)
 *
 * Verbindet sich mit dem WLAN, meldet sich beim RPi-Server an
 * und sendet periodisch Sensordaten + Heartbeats.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "esp_spiffs.h"

/* Protokoll einbinden – Pfad relativ zum Projekt anpassen */
#include "protocol.h"

/* ---- Konfiguration ---- */
#define CONFIG_FILE     "/spiffs/wifi_config.txt"
#define DEVICE_ID       1                     /* Eindeutige ID dieses ESP */
#define SEND_INTERVAL_S 5

static const char *TAG = "esp_client";

static char wifi_ssid[64];
static char wifi_pass[64];
static char server_ip[64];

/* ---- Konfiguration aus Datei laden ---- */

static void trim_newline(char *s)
{
    char *p = strchr(s, '\n');
    if (p) *p = '\0';
    p = strchr(s, '\r');
    if (p) *p = '\0';
}

static void load_config(void)
{
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path       = "/spiffs",
        .partition_label = NULL,
        .max_files       = 5,
        .format_if_mount_failed = false,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs_cfg));

    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        ESP_LOGE(TAG, "wifi_config.txt nicht gefunden auf SPIFFS!");
        ESP_LOGE(TAG, "Bitte Datei mit: SSID, Passwort, Server-IP (je eine Zeile) anlegen.");
        abort();
    }

    if (!fgets(wifi_ssid, sizeof(wifi_ssid), f) ||
        !fgets(wifi_pass, sizeof(wifi_pass), f) ||
        !fgets(server_ip, sizeof(server_ip), f)) {
        ESP_LOGE(TAG, "wifi_config.txt unvollstaendig (braucht 3 Zeilen).");
        fclose(f);
        abort();
    }
    fclose(f);

    trim_newline(wifi_ssid);
    trim_newline(wifi_pass);
    trim_newline(server_ip);

    ESP_LOGI(TAG, "Config geladen: SSID=%s, Server=%s", wifi_ssid, server_ip);
}

/* ---- WLAN ---- */

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WLAN getrennt, verbinde erneut...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_ip));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, wifi_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, wifi_pass, sizeof(wifi_cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Warte bis verbunden */
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WLAN verbunden.");
}

/* ---- TCP-Kommunikation ---- */

static int tcp_connect(void)
{
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(PROTO_PORT),
    };
    inet_pton(AF_INET, server_ip, &dest.sin_addr);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket fehlgeschlagen: errno %d", errno);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "Verbindung fehlgeschlagen: errno %d", errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "Verbunden mit Server %s:%d", server_ip, PROTO_PORT);
    return sock;
}

static int send_msg(int sock, uint8_t type, void *payload, uint16_t len)
{
    msg_header_t hdr = {
        .magic       = PROTO_MAGIC,
        .type        = type,
        .device_id   = DEVICE_ID,
        .payload_len = len,
    };

    if (send(sock, &hdr, sizeof(hdr), 0) < 0) return -1;
    if (len > 0 && payload) {
        if (send(sock, payload, len, 0) < 0) return -1;
    }
    return 0;
}

static int wait_ack(int sock)
{
    msg_header_t ack;
    int n = recv(sock, &ack, sizeof(ack), MSG_WAITALL);
    if (n <= 0) return -1;
    if (ack.magic != PROTO_MAGIC || ack.type != MSG_ACK) return -1;
    return 0;
}

/* ---- Hauptlogik ---- */

static void communication_task(void *arg)
{
    while (1) {
        int sock = tcp_connect();
        if (sock < 0) {
            ESP_LOGW(TAG, "Verbindung fehlgeschlagen, versuche erneut...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        /* Registrierung senden */
        if (send_msg(sock, MSG_REGISTER, NULL, 0) < 0 ||
            wait_ack(sock) < 0) {
            ESP_LOGE(TAG, "Registrierung fehlgeschlagen.");
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        ESP_LOGI(TAG, "Registriert als Geraet %d", DEVICE_ID);

        /* Sende-Schleife */
        int errors = 0;
        while (errors < 3) {
            /* Sensordaten (hier Dummy-Werte – echte Sensoren einbinden) */
            sensor_data_t sd = {
                .temperature = 22.5f + (esp_random() % 100) / 10.0f,
                .humidity    = 40.0f + (esp_random() % 200) / 10.0f,
            };

            if (send_msg(sock, MSG_SENSOR_DATA, &sd, sizeof(sd)) < 0 ||
                wait_ack(sock) < 0) {
                ESP_LOGW(TAG, "Senden fehlgeschlagen.");
                errors++;
            } else {
                ESP_LOGI(TAG, "Gesendet: T=%.1f H=%.1f", sd.temperature, sd.humidity);
                errors = 0;
            }

            vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL_S * 1000));
        }

        ESP_LOGW(TAG, "Zu viele Fehler, trenne Verbindung.");
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ---- Entry Point ---- */

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    load_config();
    wifi_init();
    xTaskCreate(communication_task, "comm_task", 4096, NULL, 5, NULL);
}
