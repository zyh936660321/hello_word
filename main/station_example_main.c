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

#include "driver/uart.h"

// is2引入
#include "driver/i2s_std.h" // 新版标准I2S头文件
// #include "driver/i2s.h"
#include "esp_log.h"
/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID "D2-401"
#define EXAMPLE_ESP_WIFI_PASS "d2888888"
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

// I2S基础配置（根据实际硬件调整）
// 定义 I2S 使用的端口号。
// ESP32 有两个 I2S 外设模块：I2S_NUM_0 和 I2S_NUM_1。
// 这里定义使用第一个 I2S 模块（I2S_NUM_0）。如果需要使用第二个模块，则可将其改为 I2S_NUM_1
#define I2S_PORT_NUM I2S_NUM_0
// 采样率，即每秒采集或生成的音频样本数,44100 Hz（44.1kHz）是音频的常见采样率，用于 CD 品质的音频。
#define SAMPLE_RATE 16000 // INMP441支持的采样率，例如16kHz
// (INMP441输出24位，但16位常用)
// 每个采样的比特数。 定义为 16-bit（16 位），表示每个音频样本由 16 个二进制位表示。
// 16 位采样是高质量音频的标准，能提供动态范围较好的音频效果。可选值包括：
// I2S_BITS_PER_SAMPLE_16BIT（16 位）
// I2S_BITS_PER_SAMPLE_24BIT（24 位）
// I2S_BITS_PER_SAMPLE_32BIT（32 位）
#define BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT // 新版位深度定义
                                                 // #define BITS_PER_SAMPLE   I2S_BITS_PER_SAMPLE_16BIT

// 定义日志标签
static const char *TAG = "wifi station";

// 标记wifi重连次数
static int s_retry_num = 0;

static i2s_chan_handle_t rx_handle = NULL; // I2S通道句柄

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
            esp_wifi_connect(); // 尝试重新连接到 AP
            s_retry_num++;      // 增加重试计数
                                // ESP_LOGI(TAG, "retry to connect to the AP"); // 打印日志，表示正在重试
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
    // 创建事件循环
    // 事件循环是一个异步机制，允许程序在事件发生时调用预先注册的处理函数（回调函数），而不是通过轮询或阻塞等待。
    // 单核就是并发。事件循环机制是默认事件循环运行在一个独立的 FreeRTOS 任务中（由 ESP-IDF 自动创建）。
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

    //(TAG, "wifi_init_sta finished.");

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
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void init_i2s()
{
    // 1. 初始化I2S通道配置
    // i2s_chan_config_t 是通道通用配置结构体，用于设置通道基本属性
    // 在ESP32的I2S驱动中，“通道”（channel）指的是I2S硬件的一个独立工作单元。
    // ESP32有两个I2S外设（I2S0和I2S1），每个外设可以配置为发送（TX）或接收（RX）通道。一个通道负责特定的数据流方向：
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_NUM, I2S_ROLE_MASTER);
    // I2S_CHANNEL_DEFAULT_CONFIG 宏生成默认通道配置：
    // - I2S_PORT_NUM：指定I2S端口（I2S_NUM_0 或 I2S_NUM_1）
    // - I2S_ROLE_MASTER：ESP32作为主设备，生成BCK和WS时钟信号
    chan_cfg.auto_clear = false; // RX模式下无需自动清理发送缓冲区，设为false

    // 2. 配置标准I2S模式
    // i2s_std_config_t 是标准I2S模式专用配置结构体
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // 时钟配置，默认配置采样率：
        // - SAMPLE_RATE：设置音频采样率，例如16000Hz
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BITS_PER_SAMPLE, I2S_SLOT_MODE_MONO),
        // 数据槽配置，使用Philips I2S标准格式：
        // - BITS_PER_SAMPLE：每个采样点的位深度（16位）
        // - I2S_SLOT_MODE_MONO：单声道模式，适配INMP441单声道麦克风
        .gpio_cfg = {
            // GPIO引脚配置
            .mclk = I2S_GPIO_UNUSED, // 主时钟未使用，INMP441不需要MCLK
            .bclk = 26,              // BCK（位时钟）引脚，连接到GPIO 26
            .ws = 25,                // WS（字选择，或LRCK）引脚，连接到GPIO 25
            .dout = I2S_GPIO_UNUSED, // 数据输出引脚未使用（仅接收）
            .din = 22,               // 数据输入引脚，连接INMP441的DOUT(SD)到GPIO 22
            .invert_flags = {
                // 信号反转标志
                .mclk_inv = false, // 主时钟不反转（未使用）
                .bclk_inv = false, // BCK信号不反转
                .ws_inv = false,   // WS信号不反转
            },
        },
    };
    // ESP_LOGI(TAG, "3. 创建I2S通道并应用配置");
    //  3. 创建I2S通道并应用配置
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));
    // i2s_new_channel：创建I2S通道
    // - &chan_cfg：通道配置
    // - NULL：不创建发送通道（只接收）
    // - &rx_handle：接收通道句柄，保存通道实例
    // ESP_ERROR_CHECK：检查函数执行是否成功，若失败则打印错误并中止

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    // i2s_channel_init_std_mode：初始化通道为标准I2S模式
    // - rx_handle：接收通道句柄
    // - &std_cfg：标准I2S配置，包括时钟、槽和GPIO设置

    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    // i2s_channel_enable：启用I2S通道，开始接收数据
    // - rx_handle：目标通道句柄
    ESP_LOGI(TAG, "I2S initialized successfully for INMP441");
    // 使用INFO级别日志输出初始化成功的消息
}

// 读取I2S数据,位深16 16000Hz
void read_i2s_data()
{
    uint8_t data[3500]; // 定义缓冲区，存储接收到的音频数据，1024字节
    size_t bytes_read;  // 记录实际读取的字节数

    // 从I2S通道读取数据
    esp_err_t ret = i2s_channel_read(rx_handle, data, sizeof(data), &bytes_read, portMAX_DELAY);
    // i2s_channel_read：读取I2S数据
    // - rx_handle：接收通道句柄
    // - data：目标缓冲区
    // - sizeof(data)：缓冲区大小（1024字节）
    // - &bytes_read：返回实际读取的字节数
    // - portMAX_DELAY：无限期等待，直到有数据

    if (ret == ESP_OK)
    {
        // 如果读取成功
        ESP_LOGI(TAG, "Read %d bytes from INMP441", bytes_read);
        // 输出读取到的字节数，用于调试
        // 这里可以进一步处理data，例如保存或分析音频
    }
    else
    {
        // 如果读取失败
        ESP_LOGE(TAG, "Failed to read I2S data");
        // 使用ERROR级别日志输出失败信息
    }
    for (int i = 0; i < bytes_read; i++)
    {
        printf("%u\n", data[i]);
    }
}

// 程序的主入口函数，ESP-IDF 的应用程序从这里开始执行
// void app_main(void)
// {
//     // 初始化 NVS（非易失性存储），用于保存 Wi-Fi 配置等数据
//     esp_err_t ret = nvs_flash_init();

//     // 检查 NVS 初始化是否失败
//     // ESP_ERR_NVS_NO_FREE_PAGES：NVS 分区没有空闲页面
//     // ESP_ERR_NVS_NEW_VERSION_FOUND：NVS 数据格式版本不兼容
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
//     {
//         // 如果初始化失败，擦除 NVS 分区以清除旧数据
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         // 擦除后重新初始化 NVS
//         ret = nvs_flash_init();
//     }
//     // 确保 NVS 初始化最终成功，否则程序终止
//     ESP_ERROR_CHECK(ret);

//     // 打印日志，表明程序将进入 Wi-Fi STA 模式
//     ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");

//     // 调用 Wi-Fi STA 模式的初始化函数，开始 Wi-Fi 配置和连接流程
//     wifi_init_sta();
//     // ESP_LOGI(TAG, "开始i2s链接");
//     init_i2s();

//     while (1)
//     {
//         // read_i2s_data();
//         uint16_t data[256]; // 定义缓冲区，存储接收到的音频数据，1024字节
//         size_t bytes_read;  // 记录实际读取的字节数

//         // 从I2S通道读取数据
//         esp_err_t ret = i2s_channel_read(rx_handle, data, sizeof(data), &bytes_read, portMAX_DELAY);
//         for(int i=0;i<bytes_read/2 ;i++){
//             printf("%d",data[i]);
//         }
//         //printf("wcnm\n");
//         vTaskDelay(10);
//     }
// }

// void app_main(void)
// {
//     // 初始化NVS和Wi-Fi（保持不变）
//     esp_err_t ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
//     {
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         ret = nvs_flash_init();
//     }
//     ESP_ERROR_CHECK(ret);
//     ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
//     wifi_init_sta();

//     // 初始化串口
//     // 配置并启用ESP32的UART0，用于通过串口输出日志或数据到终端
//     uart_config_t uart_config = {
//         .baud_rate = 115200,                  // 设置波特率为115200，决定了串口通信速度（比特/秒）
//         .data_bits = UART_DATA_8_BITS,        // 每个数据帧包含8位数据，常见设置，无奇偶校验时使用
//         .parity = UART_PARITY_DISABLE,        // 禁用奇偶校验，不添加校验位以验证数据完整性
//         .stop_bits = UART_STOP_BITS_1,        // 每个数据帧使用1个停止位，表示帧结束
//         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE // 禁用硬件流控制（RTS/CTS），依赖软件控制数据流
//     };
//     uart_param_config(UART_NUM_0, &uart_config);         // 将配置应用到UART0（默认串口，GPIO 1为TX，GPIO 3为RX）
//     uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0); // 安装UART驱动并启用串口
//                                                          // 参数说明：
//                                                          // - UART_NUM_0：使用UART0
//                                                          // - 256：接收缓冲区大小（字节），用于存储传入数据
//                                                          // - 0：发送缓冲区大小，设为0表示不使用发送缓冲（直接发送）
//                                                          // - 0：事件队列大小，设为0表示不使用事件队列
//                                                          // - NULL：无事件队列句柄
//                                                          // - 0：中断分配标志，默认值，表示不特殊配置中断
//     // 初始化I2S
//     init_i2s();
//     //static int16_t data[128]; // 全局变量存放在堆中，大小几百KB不会爆

//     // 主循环
//     while (1)
//     {
//         int16_t data[128]; // 3200字节 栈大小只有4KB这样爆栈了
//         size_t bytes_read;
//         esp_err_t ret = i2s_channel_read(rx_handle, data, sizeof(data), &bytes_read, portMAX_DELAY);
//         if (ret == ESP_OK)
//         {
//             // ESP_LOGI(TAG, "Read %d bytes", bytes_read);
//             for (int i = 0; i < bytes_read / 2; i++)
//            {
//                printf("%d\n", data[i]); // 打印16位样本值
//            }
//             fflush(stdout); // 强制刷新输出
//             //uart_write_bytes(UART_NUM_0, (const char *)data, bytes_read); // 发送二进制数据
//         }
//         else
//         {
//             ESP_LOGE(TAG, "I2S read failed");
//         }
//         vTaskDelay(pdMS_TO_TICKS(100)); // 延迟100ms，避免过快循环
//     }
// }

void usart_init()
{
    // 配置并初始化串口（UART0），用于将音频数据发送到电脑
    uart_config_t uart_config0 = {
        .baud_rate = 115200,                  // 设置波特率为921600 bps，提供更高的数据传输速度
        .data_bits = UART_DATA_8_BITS,        // 每个数据帧包含8位数据，标准设置
        .parity = UART_PARITY_DISABLE,        // 禁用奇偶校验，简化通信
        .stop_bits = UART_STOP_BITS_1,        // 每个数据帧使用1个停止位
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE // 禁用硬件流控制，依赖软件控制数据流
    };
    uart_param_config(UART_NUM_0, &uart_config0);        // 将配置应用到UART0（默认TX: GPIO 1, RX: GPIO 3）
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0); // 安装UART驱动并启用串口

    // 初始化 UART2 用于音频数据传输
    uart_config_t uart_config2 = {
        .baud_rate = 921600,                  // 波特率921600 bps，支持32KB/s音频数据
        .data_bits = UART_DATA_8_BITS,        // 8位数据
        .parity = UART_PARITY_DISABLE,        // 无奇偶校验
        .stop_bits = UART_STOP_BITS_1,        // 1个停止位
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE // 禁用硬件流控制
    };
    uart_param_config(UART_NUM_2, &uart_config2);                                               // 配置 UART2
    uart_set_pin(UART_NUM_2, GPIO_NUM_17, GPIO_NUM_16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE); // 配置它的gpio端口
    uart_driver_install(UART_NUM_2, 256, 0, 0, NULL, 0);                                        // 安装 UART2 驱动
    // 参数说明：
    // - UART_NUM_2：使用 UART2（默认 TX: GPIO 17, RX: GPIO 16）
    // - 256：接收缓冲区大小（字节）
    // - 0：发送缓冲区大小（直接发送）
    // - 0：事件队列大小（不使用）
    // - NULL：无事件队列句柄
    // - 0：默认中断标志
}

// ESP32的主入口函数，程序从这里开始执行
void app_main(void)
{

    // 初始化NVS（非易失性存储），用于保存Wi-Fi配置等数据
    esp_err_t ret = nvs_flash_init();

    // 检查NVS初始化是否失败
    // ESP_ERR_NVS_NO_FREE_PAGES：NVS分区没有空闲页面
    // ESP_ERR_NVS_NEW_VERSION_FOUND：NVS数据格式版本不兼容
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase()); // 擦除NVS分区以清除旧数据
        ret = nvs_flash_init();             // 擦除后重新初始化NVS
    }
    ESP_ERROR_CHECK(ret); // 确保NVS初始化成功，否则中止程序

    // 打印日志，表明程序将进入Wi-Fi Station模式（STA）
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");

    usart_init();
    // 调用Wi-Fi STA模式的初始化函数，开始Wi-Fi配置和连接流程
    wifi_init_sta();

    // 参数说明：
    // - UART_NUM_0：使用第一个UART通道（UART0）
    // - 256：接收缓冲区大小（字节），用于缓存从电脑接收的数据
    // - 0：发送缓冲区大小，设为0表示直接发送，不使用缓冲
    // - 0：事件队列大小，设为0表示不使用事件队列
    // - NULL：无事件队列句柄
    // - 0：中断分配标志，默认设置

    // 初始化I2S接口，用于从麦克风（如INMP441）采集音频数据
    init_i2s();

    // 主循环，持续采集并发送音频数据
    while (1)
    {
        int16_t data[64];  // 定义音频数据缓冲区，64个16位样本，总大小128字节
                           // 注释：减小缓冲区以匹配921600 bps串口带宽，避免数据丢失
        size_t bytes_read; // 记录实际读取的字节数
        // 从I2S通道读取音频数据到data缓冲区
        esp_err_t ret = i2s_channel_read(rx_handle, data, sizeof(data), &bytes_read, portMAX_DELAY);
        // 参数说明：
        // - rx_handle：I2S接收通道句柄，由init_i2s()初始化
        // - data：目标缓冲区，存储采集的PCM数据
        // - sizeof(data)：请求读取128字节（64个16位样本）
        // - &bytes_read：返回实际读取的字节数
        // - portMAX_DELAY：无限期等待，直到有数据可用

        if (ret == ESP_OK)
        { // 检查I2S读取是否成功
          // 通过串口二进制发送原始PCM数据到电脑
           int sent = uart_write_bytes(UART_NUM_2, (const char *)data, bytes_read);

            // if (sent != bytes_read)
            // { // 检查是否所有数据都成功发送
            //   // ESP_LOGE(TAG, "Sent %d of %d bytes", sent, bytes_read); // 如果失败，记录错误日志
            // }
        }
        else
        {
            // ESP_LOGE(TAG, "I2S read failed"); // 如果I2S读取失败，记录错误日志
        }

        // uart_write_bytes(UART_NUM_2, (const char *)"data\r\n", 6); // 调试
        vTaskDelay(pdMS_TO_TICKS(5)); // 延迟4毫秒，避免过快循环
        // 注释：4ms匹配64样本（64 ÷ 16000 = 0.004秒），与采样率同步
    }
}