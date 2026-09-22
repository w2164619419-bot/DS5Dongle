//
// wol_config.h —— Pico 2 W Remote Wake 的用户配置（编进固件的那一份）
//
// 把这个文件和 lwipopts.h / wol_net.h / wol_net.cpp 一起放进 DS5Dongle 的 src/。
// 只有本文件需要你改。
//

#ifndef DS5_WOL_CONFIG_H
#define DS5_WOL_CONFIG_H

// ============================================================
// 1. Wi-Fi
// ============================================================
// Pico 2 W 只支持 2.4GHz。如果路由器把 2.4G/5G 分成两个 SSID，填 2.4G 那个。
#define WOL_WIFI_SSID        "你的WiFi名称"
#define WOL_WIFI_PASSWORD    "你的WiFi密码"

// ★★★ Wi-Fi 总开关 ★★★
// 0 = 完全不启动 Wi-Fi（lwIP 仍然链接着，但不发任何连接/扫描请求）
// 1 = 正常连接家庭 Wi-Fi
//
// 为什么需要它：Pico 2 W 的蓝牙和 Wi-Fi 共用同一颗 CYW43 芯片的射频。
// Wi-Fi 的扫描/关联会抢占射频；如果 Wi-Fi 因为 SSID 或密码不对而反复重连，
// 会明显干扰 DS5 的蓝牙连接（实测出现过"设备无法识别、手柄连接不正常"）。
// 用法：先用 0 确认蓝牙/音频一切正常，填好 SSID 密码后再改成 1。
#define WOL_WIFI_ENABLE      0

// 连接超时（毫秒）。超时后进入退避重连，不会阻塞主循环。
#define WOL_WIFI_TIMEOUT_MS  15000

// 失败重连退避（毫秒），越失败等越久
#define WOL_WIFI_BACKOFF_MS  { 2000, 5000, 10000, 20000, 30000, 60000 }
#define WOL_WIFI_BACKOFF_LEN 6

// 快速重试超过这个次数后，转入超长退避。
// 目的是：配置错了也不让无线一直抢占射频去干扰蓝牙。
#define WOL_WIFI_FAST_RETRIES    4
#define WOL_WIFI_LONG_BACKOFF_MS 300000

// DHCP。想要固定 IP 就把下面改成 0，并填好静态地址。
#define WOL_USE_DHCP         1
#define WOL_STATIC_IP        "192.168.5.50"
#define WOL_STATIC_NETMASK   "255.255.255.0"
#define WOL_STATIC_GATEWAY   "192.168.5.1"

// 路由器 DHCP 列表里显示的名字
#define WOL_HOSTNAME         "pico-ds5-bridge"

// Wi-Fi 区域。中国填 "CN"。
#define WOL_WIFI_COUNTRY     "CN"

// ============================================================
// 2. Wake-on-LAN
// ============================================================
// 目标网卡 MAC：机械革命翼龙 15 Pro 2024 / MediaTek Wi-Fi 6E MT7922
#define WOL_TARGET_MAC       "1C-CE-51-39-FB-6A"

// Magic Packet 的 UDP 端口（标准 9，备用 7）
#define WOL_UDP_PORT         9

// 一次唤醒连发几个包、包间隔多少毫秒。
// 默认 0 间隔：3 个包几乎瞬间发完，完全不占用主循环时间。
// 如果你的网卡必须靠间隔才认，再把它调成 20~50（代价是主循环会被占用
// 几十毫秒，对 DS5 音频核心跑在 core1 上这一点来说通常无所谓）。
#define WOL_REPEAT           3
#define WOL_GAP_MS           0

// 护栏：主循环里有 1 秒看门狗（main.cpp 的 watchdog_enable(1000, true)），
// 而发包是在 lwIP 回调里同步执行的，累计阻塞一旦接近 1 秒，Pico 会被复位。
// 直接在编译期拦住不安全的组合，别等到真机上"莫名其妙重启"。
#if (WOL_REPEAT > 1) && (((WOL_REPEAT) - 1) * ((WOL_GAP_MS) + 12) > 800)
#error "WOL_REPEAT x WOL_GAP_MS 太大会阻塞主循环、喂不上 1 秒看门狗，请调小"
#endif

// ============================================================
// 3. HTTP 服务
// ============================================================
#define WOL_HTTP_ENABLE      1
#define WOL_HTTP_PORT        80

// 防误触口令：只有 /wake?key= 后面跟着这个值才会真的发包。
// ！！必须改掉，别用下面这个示例值！！（保持示例值时程序会拒绝所有唤醒）
#define WOL_KEY              "change-me-8f3a"

// 同一个 IP 一小时内最多唤醒几次（0 = 不限制）
#define WOL_MAX_PER_HOUR     10

// 最多同时允许几个 HTTP 连接（每个连接会占一点内存，别调大）
#define WOL_HTTP_MAX_CONN    2

// ============================================================
// 4. 调试
// ============================================================
// 串口日志。UART 已经在这个项目里开着（pico_enable_stdio_uart(ds5-bridge 1)），
// 用 USB-TTL 接 GPIO0(TX)/GPIO1(GND) 就能看到。
#define WOL_DEBUG_LOG        1

#endif // DS5_WOL_CONFIG_H
