/*
app_main()
  ├── 初始化 NVS (nvs_flash_init)
  │   ├── 失败 (无空闲页面/版本不符) → 擦除 (nvs_flash_erase) → 重试
  │   └── 成功 → 继续
  ├── 打印 "ESP_WIFI_MODE_STA"
  └── 调用 wifi_init_sta()
       ├── 创建事件组 (s_wifi_event_group)
       ├── 初始化网络接口 (esp_netif_init)
       ├── 创建事件循环 (esp_event_loop_create_default)
       ├── 创建 STA 接口 (esp_netif_create_default_wifi_sta)
       ├── 初始化 Wi-Fi (esp_wifi_init)
       ├── 注册事件处理程序 (WIFI_EVENT & IP_EVENT)
       ├── 配置 Wi-Fi (ssid, password, authmode 等)
       ├── 启动 Wi-Fi (esp_wifi_start)
       │   └── 触发 event_handler()
       │        ├── WIFI_EVENT_STA_START → esp_wifi_connect()
       │        ├── WIFI_EVENT_STA_DISCONNECTED → 重试或标记失败 (WIFI_FAIL_BIT)
       │        └── IP_EVENT_STA_GOT_IP → 打印 IP & 标记成功 (WIFI_CONNECTED_BIT)
       ├── 打印 "wifi_init_sta finished."
       └── 等待事件组状态 (xEventGroupWaitBits)
            ├── WIFI_CONNECTED_BIT → 打印连接成功
            ├── WIFI_FAIL_BIT → 打印连接失败
            └── 其他 → 打印意外事件
*/

/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif
#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* 事件组允许每个事件有多个比特位，但我们只关心两个事件：
   我们已连接到接入点（AP）并获得 IP 地址
   在达到最大重试次数后连接失败
   事件组标记，bit位置零置一，标记这个 WIFI_CONNECTED_BIT 是否成功
   WIFI_FAIL_BIT 标记链接是否失败
*/
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "wifi station";

// 标记wifi重连次数
static int s_retry_num = 0;

// 定义一个静态事件处理函数，处理 Wi-Fi 和 IP 相关事件
// 参数：
// - arg: 用户传递的自定义参数（这里未使用）
// - event_base: 事件的基础类型（如 WIFI_EVENT 或 IP_EVENT）
// - event_id: 具体的事件 ID（如 WIFI_EVENT_STA_START）
// - event_data: 事件附带的数据（如 IP 地址信息）
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    // 情况 1: Wi-Fi Station 模式启动事件
    // 当事件类型为 WIFI_EVENT 且具体事件为 WIFI_EVENT_STA_START 时触发
    // 表示 Wi-Fi STA 模式已启动，可以尝试连接到 AP（接入点）
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect(); // 发起连接到配置好的 Wi-Fi 网络（AP）
    }

    // 情况 2: Wi-Fi Station 断开连接事件
    // 当事件类型为 WIFI_EVENT 且具体事件为 WIFI_EVENT_STA_DISCONNECTED 时触发
    // 表示与 AP 的连接断开
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // 检查重试次数是否小于最大重试限制（EXAMPLE_ESP_MAXIMUM_RETRY）
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();                          // 尝试重新连接到 AP
            s_retry_num++;                               // 增加重试计数
            ESP_LOGI(TAG, "retry to connect to the AP"); // 打印日志，表示正在重试
        }
        // 如果重试次数达到上限，设置失败标志位
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT); // 设置事件组的失败位
        }
        ESP_LOGI(TAG, "connect to the AP fail"); // 打印日志，表示连接失败
    }

    // 情况 3: 获取 IP 地址事件
    // 当事件类型为 IP_EVENT 且具体事件为 IP_EVENT_STA_GOT_IP 时触发
    // 表示成功连接到 AP 并从 DHCP 获取到 IP 地址
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        // 将事件数据转换为 ip_event_got_ip_t 类型，包含 IP 地址信息
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        // 打印获取到的 IP 地址，使用 IPSTR 和 IP2STR 宏格式化输出
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0; // 重试计数清零，表示连接成功
        // 设置事件组的连接成功标志位
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    // 创建一个 FreeRTOS 事件组，用于管理 Wi-Fi 连接状态。
    s_wifi_event_group = xEventGroupCreate();
    // 初始化 ESP-IDF 的网络接口子系统。
    // 用于初始化网络接口模块（esp_netif）
    // 调用这个函数后，系统会为后续的网络操作（比如 Wi-Fi 连接、IP 地址分配）做好准备。
    ESP_ERROR_CHECK(esp_netif_init());
    //  创建事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    // 配置并初始化 Wi-Fi 驱动程序。
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // 注册事件处理程序 event_handler，处理 Wi-Fi 和 IP 事件。
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    // 注册 Wi-Fi 事件处理程序实例
    // 监听所有 Wi-Fi 事件（WIFI_EVENT），使用 event_handler 处理，并保存实例句柄到 instance_any_id
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,         // 事件基础类型：Wi-Fi 事件
                                                        ESP_EVENT_ANY_ID,   // 监听所有 Wi-Fi 事件 ID
                                                        &event_handler,     // 事件处理函数指针
                                                        NULL,               // 用户参数（未使用）
                                                        &instance_any_id)); // 保存注册实例的句柄

    // 注册 IP 事件处理程序实例
    // 监听特定 IP 事件（IP_EVENT_STA_GOT_IP），使用 event_handler 处理，并保存实例句柄到 instance_got_ip
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,            // 事件基础类型：IP 事件
                                                        IP_EVENT_STA_GOT_IP, // 监听特定事件：获取 IP 地址
                                                        &event_handler,      // 事件处理函数指针
                                                        NULL,                // 用户参数（未使用）
                                                        &instance_got_ip));  // 保存注册实例的句柄
    // 配置 Wi-Fi STA 的 SSID、密码和认证模式。
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* 如果密码符合 WPA2 标准（密码长度 >= 8），则认证模式阈值默认重置为 WPA2。
            如果你想将设备连接到已废弃的 WEP/WPA 网络，请将阈值设置为 WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK，
            并将密码设置为符合 WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK 标准的长度和格式。 */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };
    // 设置 Wi-Fi 工作模式为 Station 模式（STA）。
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // 设置 Wi-Fi 工作模式为 Station 模式（STA）。
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    // 启动 Wi-Fi 功能
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* 等待连接成功（WIFI_CONNECTED_BIT）或达到最大重试次数后连接失败（WIFI_FAIL_BIT）。
    这些事件标志位由 event_handler() 函数设置（见上文）。 */

    // 这段代码在等待 Wi-Fi 连接的过程中阻塞任务，直到成功连接到 Wi-Fi（WIFI_CONNECTED_BIT 被设置）
    // 或达到最大重试次数而连接失败（WIFI_FAIL_BIT 被设置）。
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,                 // 事件组的句柄
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, // 等待的事件位，即 Wi-Fi 连接成功或连接失败
                                           pdFALSE,                            // 退出时不清除事件位
                                           pdFALSE,                            // 是否等待所有事件位都被设置（这里是不需要）
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() 返回调用之前的事件位状态，因此我们可以测试实际发生了哪个事件。*/
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

// 程序的主入口函数，ESP-IDF 的应用程序从这里开始执行
void app_main(void)
{
    // 初始化 NVS（非易失性存储），用于保存 Wi-Fi 配置等数据
    esp_err_t ret = nvs_flash_init();
    
    // 检查 NVS 初始化是否失败
    // ESP_ERR_NVS_NO_FREE_PAGES：NVS 分区没有空闲页面
    // ESP_ERR_NVS_NEW_VERSION_FOUND：NVS 数据格式版本不兼容
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // 如果初始化失败，擦除 NVS 分区以清除旧数据
        ESP_ERROR_CHECK(nvs_flash_erase());
        // 擦除后重新初始化 NVS
        ret = nvs_flash_init();
    }
    // 确保 NVS 初始化最终成功，否则程序终止
    ESP_ERROR_CHECK(ret);

    // 打印日志，表明程序将进入 Wi-Fi STA 模式
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    
    // 调用 Wi-Fi STA 模式的初始化函数，开始 Wi-Fi 配置和连接流程
    wifi_init_sta();
}
