//
// wol_net.cpp —— Wi-Fi + Wake-on-LAN + 极简 HTTP，全部非阻塞
//
// 设计原则（对应需求里的"性能要求"）：
//   * 不阻塞：wol_net_poll() 单次耗时在微秒级，不调用任何带超时的阻塞 API。
//     Wi-Fi 用 cyw43_arch_wifi_connect_async()，结果靠轮询 link status 判断。
//   * 不频繁扫描：只在退避时间到点时发起一次连接，不调用 scan。
//   * 不重复初始化蓝牙、不重启 Pico、不为了发一次 WoL 重连 Wi-Fi。
//   * 不碰 DS5 的任何 Bluetooth 配置。
//
// 主循环里已有的 cyw43_arch_poll() 继续负责驱动 cyw43 和 lwIP 收包；
// wol_net_poll() 额外负责 lwIP 定时器（sys_check_timeouts）和 Wi-Fi 状态机。
//

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"

#include "wol_config.h"
#include "wol_net.h"

// ---------------------------------------------------------------- 日志

#if WOL_DEBUG_LOG
#define WOL_LOG(...) printf("[WoL] " __VA_ARGS__)
#else
#define WOL_LOG(...) ((void)0)
#endif

// ---------------------------------------------------------------- 常量

#define WOL_REQ_BUF_SIZE   512      // HTTP 请求缓冲
#define WOL_BODY_BUF_SIZE  1792     // HTTP 响应 body 缓冲
#define WOL_HEAD_BUF_SIZE  192      // HTTP 响应头缓冲
#define WOL_IP_STR_SIZE    20

// 同一个 IP 的唤醒时间戳记录条数
#define WOL_RATE_SLOTS     4
#define WOL_RATE_WINDOW_MS (3600u * 1000u)

// ---------------------------------------------------------------- 状态

enum WolWifiState {
    WIFI_IDLE = 0,
    WIFI_CONNECTING,
    WIFI_ONLINE,
    WIFI_BACKOFF,
};

static WolWifiState s_wifi_state = WIFI_IDLE;
static uint32_t     s_wifi_deadline_ms = 0;
static int          s_wifi_failures = 0;
static const uint32_t s_backoff_ms[] = WOL_WIFI_BACKOFF_MS;

static struct udp_pcb *s_udp = NULL;
static struct tcp_pcb *s_listen = NULL;
static int             s_http_conns = 0;

static char     s_ip_str[WOL_IP_STR_SIZE] = "0.0.0.0";
static char     s_bc_str[WOL_IP_STR_SIZE] = "-";
static ip4_addr_t s_broadcast;          // 由 IP + 掩码动态算出
static uint32_t s_boot_ms = 0;

// 这些缓冲放 .bss，不占栈（core0 的栈只有 2KB）
static char s_req_buf[WOL_REQ_BUF_SIZE];
static char s_body_buf[WOL_BODY_BUF_SIZE];
static char s_head_buf[WOL_HEAD_BUF_SIZE];
static char s_method[8];
static char s_target[256];
static char s_keybuf[64];

struct RateSlot {
    ip4_addr_t ip;
    uint32_t   stamps[WOL_MAX_PER_HOUR > 0 ? WOL_MAX_PER_HOUR : 1];
    int        count;
    bool       used;
};
static struct RateSlot s_rate[WOL_RATE_SLOTS];

// ---------------------------------------------------------------- 工具

static inline uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

// 时间比较，正确处理 32 位回绕
static inline bool deadline_passed(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

// 往缓冲追加（不做格式化，避免 CSS 里的 % 冲突）
static int buf_adds(char *buf, int off, int cap, const char *s) {
    if (off < 0) off = 0;
    while (*s && off < cap - 1) {
        buf[off++] = *s++;
    }
    if (off < cap) buf[off] = '\0';
    return off;
}

static int buf_addf(char *buf, int off, int cap, const char *fmt, ...) {
    if (off >= cap - 1) return off;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, (size_t)(cap - off), fmt, ap);
    va_end(ap);
    if (n < 0) return off;
    if (off + n >= cap) {
        buf[cap - 1] = '\0';
        return cap - 1;
    }
    return off + n;
}

// "1C-CE-51-39-FB-6A" / "1c:ce:51:39:fb:6a" / "1CCE5139FB6A" -> 6 字节
static bool parse_mac(const char *text, uint8_t out[6]) {
    uint8_t digits[12];
    int n = 0;

    for (const char *p = text; *p && n < 12; p++) {
        char c = *p;
        if (c == '-' || c == ':' || c == '.' || c == ' ' || c == '_') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        digits[n++] = (uint8_t)v;
    }
    if (n != 12) return false;

    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)((digits[i * 2] << 4) | digits[i * 2 + 1]);
    }
    return true;
}

// ---------------------------------------------------------------- Wi-Fi

static void wifi_enter_backoff(void) {
    const int len = WOL_WIFI_BACKOFF_LEN;
    int idx = s_wifi_failures;
    if (idx >= len) idx = len - 1;
    uint32_t wait = s_backoff_ms[idx];
    s_wifi_failures++;
    // 连续失败太多次说明配置有问题（SSID/密码不对）。
    // 转成超长退避，别让无线一直抢占射频去干扰蓝牙。
    if (s_wifi_failures > WOL_WIFI_FAST_RETRIES) {
        wait = WOL_WIFI_LONG_BACKOFF_MS;
    }
    s_wifi_state = WIFI_BACKOFF;
    s_wifi_deadline_ms = now_ms() + wait;
    snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");
    snprintf(s_bc_str, sizeof(s_bc_str), "-");
    s_broadcast.addr = 0;
    WOL_LOG("WiFi: 连接失败，%u 秒后重试（第 %d 次）\n",
            (unsigned)(wait / 1000), s_wifi_failures);
}

static void wifi_begin_connect(void) {
    // ★ 关键：必须先启用 STA 模式，否则永远拿不到 IP ★
    //
    // cyw43_arch_enable_sta_mode() -> cyw43_wifi_set_up(&cyw43_state, STA, true, ...)
    //   -> cyw43_ctrl.c:566-569  if ((itf_state & (1<<STA)) == 0)
    //                              cyw43_cb_tcpip_init(self, itf);
    //   -> cyw43_lwip.c:194-227   netif_add() + netif_set_default() + netif_set_up()
    //                              + dhcp_set_struct() + dhcp_start()
    //
    // 也就是说 netif 的挂载和 DHCP 的启动**都是这一步顺带完成的**，SDK 不需要
    // 用户自己调 netif_add（我原来以为要自己加，是查错了目录：netif_add 不在
    // pico-sdk/src 里，而在 pico-sdk/lib/cyw43-driver/src/cyw43_lwip.c）。
    //
    // 而 cyw43_arch_wifi_connect_async() 只是转手调用 cyw43_wifi_join()，
    // **完全不会**启用 STA 模式。所以漏掉这一句的后果是：无线可能确实连上了，
    // 但 lwIP 里根本没有 cyw43 的 netif，DHCP 不跑，IP 永远是 0.0.0.0，
    // HTTP 服务也就永远没人能访问到。
    //
    // 只做一次；重复调用虽然基本幂等，但没必要。
    static bool sta_mode_done = false;
    if (!sta_mode_done) {
        cyw43_arch_enable_sta_mode();
        sta_mode_done = true;
        WOL_LOG("WiFi: STA mode enabled (netif + DHCP 已就绪)\n");
    }

    WOL_LOG("WiFi connecting... (%s)\n", WOL_WIFI_SSID);

    // 非阻塞：立刻返回，连接过程由 cyw43_arch_poll() 在后台推进
    int err = cyw43_arch_wifi_connect_async(WOL_WIFI_SSID, WOL_WIFI_PASSWORD,
                                            CYW43_AUTH_WPA2_AES_PSK);
    if (err != 0) {
        WOL_LOG("WiFi: 发起连接失败 (%d)\n", err);
        wifi_enter_backoff();
        return;
    }
    s_wifi_state = WIFI_CONNECTING;
    s_wifi_deadline_ms = now_ms() + WOL_WIFI_TIMEOUT_MS;
}

static void wifi_go_online(void) {
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
    const ip4_addr_t *ip = netif_ip4_addr(nif);
    const ip4_addr_t *mask = netif_ip4_netmask(nif);

    // 关键：广播地址由 Pico 自己的 IP + 掩码动态算出，不硬编码网段
    s_broadcast.addr = (ip->addr & mask->addr) | ~mask->addr;

    snprintf(s_ip_str, sizeof(s_ip_str), "%s", ip4addr_ntoa(ip));
    snprintf(s_bc_str, sizeof(s_bc_str), "%s", ip4addr_ntoa(&s_broadcast));

    s_wifi_state = WIFI_ONLINE;
    s_wifi_failures = 0;

    WOL_LOG("WiFi connected\n");
    WOL_LOG("IP: %s\n", s_ip_str);
    WOL_LOG("Netmask: %s\n", ip4addr_ntoa(mask));
    WOL_LOG("Broadcast: %s\n", s_bc_str);
    if (WOL_HTTP_PORT == 80) {
        WOL_LOG("HTTP server started: http://%s/\n", s_ip_str);
    } else {
        WOL_LOG("HTTP server started: http://%s:%d/\n", s_ip_str, WOL_HTTP_PORT);
    }
}

static void wifi_poll(void) {
#if !WOL_WIFI_ENABLE
    // Wi-Fi 被配置关闭：一次都不碰无线，彻底保证不影响蓝牙
    return;
#else
    const uint32_t now = now_ms();

    switch (s_wifi_state) {
        case WIFI_IDLE:
            wifi_begin_connect();
            break;

        case WIFI_CONNECTING: {
            struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
            int st = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
            bool have_ip = (netif_ip4_addr(nif)->addr != 0);

            if (st == CYW43_LINK_UP && have_ip) {
                wifi_go_online();
            } else if (deadline_passed(now, s_wifi_deadline_ms)) {
                WOL_LOG("WiFi: 连接超时 (link_status=%d, ip=%s)\n",
                        st, ip4addr_ntoa(netif_ip4_addr(nif)));
                wifi_enter_backoff();
            }
            break;
        }

        case WIFI_ONLINE: {
            int st = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
            if (st != CYW43_LINK_UP) {
                WOL_LOG("WiFi: 链路断开 (status=%d)，进入重连退避\n", st);
                wifi_enter_backoff();
            }
            break;
        }

        case WIFI_BACKOFF:
            if (deadline_passed(now, s_wifi_deadline_ms)) {
                wifi_begin_connect();
            }
            break;
    }
#endif // WOL_WIFI_ENABLE
}

// ---------------------------------------------------------------- WoL 发送

static int wol_send_once(const uint8_t *packet, uint16_t len) {
    int sent = 0;

    // 目标 1：由 IP+掩码算出的本网段定向广播（例如 192.168.5.255）
    if (s_broadcast.addr != 0 && s_broadcast.addr != 0xFFFFFFFFu) {
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
        if (p != NULL) {
            memcpy(p->payload, packet, len);
            if (udp_sendto(s_udp, p, &s_broadcast, WOL_UDP_PORT) == ERR_OK) sent++;
            pbuf_free(p);
        }
    }

    // 目标 2：全网广播 255.255.255.255
    // 有些路由器只认定向广播、有些只认这一种，所以两种都发，不赌。
    {
        ip4_addr_t all;
        IP4_ADDR(&all, 255, 255, 255, 255);
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
        if (p != NULL) {
            memcpy(p->payload, packet, len);
            if (udp_sendto(s_udp, p, &all, WOL_UDP_PORT) == ERR_OK) sent++;
            pbuf_free(p);
        }
    }

    return sent;
}

int wol_send_magic_packet(void) {
    uint8_t mac[6];
    if (!parse_mac(WOL_TARGET_MAC, mac)) {
        WOL_LOG("WoL: 目标 MAC 解析失败: %s\n", WOL_TARGET_MAC);
        return 0;
    }

    // 标准 Magic Packet：6 字节 0xFF + MAC 重复 16 次 = 102 字节
    uint8_t packet[102];
    memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(packet + 6 + i * 6, mac, 6);
    }

    WOL_LOG("WoL:\n");
    WOL_LOG("Target MAC = %s\n", WOL_TARGET_MAC);
    WOL_LOG("Broadcast = %s\n", s_bc_str);
    WOL_LOG("Port = %d\n", WOL_UDP_PORT);
    WOL_LOG("Packet length = %d\n", (int)sizeof(packet));

    if (s_udp == NULL || s_wifi_state != WIFI_ONLINE) {
        WOL_LOG("Sent FAILED: Wi-Fi 未连接 (state=%d)\n", (int)s_wifi_state);
        return 0;
    }

    int sent = 0;
    for (int r = 0; r < WOL_REPEAT; r++) {
        sent += wol_send_once(packet, (uint16_t)sizeof(packet));
        if (r != WOL_REPEAT - 1 && WOL_GAP_MS > 0) {
            sleep_ms(WOL_GAP_MS);
        }
    }

    if (sent > 0) {
        WOL_LOG("Sent successfully\n");
    } else {
        WOL_LOG("Sent FAILED (udp_sendto 全部失败)\n");
    }
    return sent;
}

// ---------------------------------------------------------------- 限流

#if WOL_MAX_PER_HOUR > 0
static bool rate_limited(const ip4_addr_t *ip, uint32_t now) {
    for (int i = 0; i < WOL_RATE_SLOTS; i++) {
        if (s_rate[i].used && s_rate[i].ip.addr == ip->addr) {
            // 清掉过期记录
            int keep = 0;
            for (int k = 0; k < s_rate[i].count; k++) {
                if (now - s_rate[i].stamps[k] < WOL_RATE_WINDOW_MS) {
                    s_rate[i].stamps[keep++] = s_rate[i].stamps[k];
                }
            }
            s_rate[i].count = keep;
            return keep >= WOL_MAX_PER_HOUR;
        }
    }
    return false;
}

static void rate_record(const ip4_addr_t *ip, uint32_t now) {
    int slot = -1;
    for (int i = 0; i < WOL_RATE_SLOTS; i++) {
        if (s_rate[i].used && s_rate[i].ip.addr == ip->addr) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < WOL_RATE_SLOTS; i++) {
            if (!s_rate[i].used) { slot = i; break; }
        }
    }
    if (slot < 0) {
        // 槽位满了：简单复用一个 count 最小的
        int best = 0;
        for (int i = 1; i < WOL_RATE_SLOTS; i++) {
            if (s_rate[i].count < s_rate[best].count) best = i;
        }
        slot = best;
        s_rate[slot].count = 0;
    }
    s_rate[slot].used = true;
    s_rate[slot].ip = *ip;
    if (s_rate[slot].count < WOL_MAX_PER_HOUR) {
        s_rate[slot].stamps[s_rate[slot].count++] = now;
    }
}
#endif

// ---------------------------------------------------------------- 口令

// 等长逐字节比较，不因提前返回泄露内容
static bool key_equals(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la != lb) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < la; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

// ---------------------------------------------------------------- 页面

static const char HTML_HEAD[] =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Pico 2 W Remote Control</title><style>"
    "*{box-sizing:border-box}"
    "body{margin:0;padding:18px;background:#12151c;color:#e8ecf3;"
    "font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",\"PingFang SC\",\"Microsoft YaHei\",sans-serif}"
    ".card{max-width:430px;margin:0 auto;background:#1b202b;border:1px solid #2b3344;border-radius:14px;padding:20px}"
    "h1{font-size:19px;margin:0 0 4px}"
    ".sub{font-size:12px;color:#8994a8;margin-bottom:16px}"
    ".row{display:flex;justify-content:space-between;align-items:center;padding:9px 0;"
    "border-bottom:1px solid #242b39;font-size:14px}"
    ".row:last-of-type{border-bottom:none}"
    ".k{color:#8994a8}.v{font-weight:600;text-align:right;word-break:break-all}"
    ".ok{color:#41d18b}.bad{color:#ff6b6b}.warn{color:#ffb454}"
    "form{margin-top:18px;display:flex;gap:8px}"
    "input{flex:1;min-width:0;padding:12px;border-radius:9px;border:1px solid #2b3344;"
    "background:#12151c;color:#e8ecf3;font-size:15px}"
    "button{flex:0 0 auto;padding:12px 18px;border:0;border-radius:9px;background:#3b82f6;"
    "color:#fff;font-size:15px;font-weight:600}"
    ".hint{font-size:11.5px;color:#6b7688;margin:12px 0 0;line-height:1.5}"
    ".mono{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12.5px}"
    "a{color:#6aa6ff}</style></head><body><div class=\"card\">"
    "<h1>Pico 2 W Remote Control</h1><div class=\"sub\">DS5 Bridge + 局域网唤醒</div>";

static const char HTML_FOOT[] =
    "<form action=\"/wake\" method=\"get\">"
    "<input type=\"password\" name=\"key\" id=\"key\" placeholder=\"唤醒口令\">"
    "<button type=\"submit\">Wake PC</button></form>"
    "<p class=\"hint\">口令只存在你自己手机的浏览器里（localStorage），"
    "不写进这个页面，所以别人打开同一页面也拿不到口令。填一次就会记住。</p>"
    "</div><script>(function(){var i=document.getElementById('key');"
    "try{var s=localStorage.getItem('wolkey');if(s){i.value=s;}}catch(e){}"
    "document.querySelector('form').addEventListener('submit',function(){"
    "try{localStorage.setItem('wolkey',i.value);}catch(e){}});})();</script>"
    "</body></html>";

static const char HTML_DONE_HEAD[] =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Pico 2 W Remote Control</title><style>"
    "body{margin:0;padding:18px;background:#12151c;color:#e8ecf3;"
    "font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",\"PingFang SC\",\"Microsoft YaHei\",sans-serif}"
    ".card{max-width:430px;margin:0 auto;background:#1b202b;border:1px solid #2b3344;"
    "border-radius:14px;padding:20px}"
    "h1{font-size:18px;margin:0 0 14px}.ok{color:#41d18b}.bad{color:#ff6b6b}"
    ".row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #242b39;font-size:14px}"
    ".row:last-of-type{border-bottom:none}.k{color:#8994a8}"
    ".v{font-weight:600;text-align:right;word-break:break-all;"
    "font-family:ui-monospace,Menlo,Consolas,monospace;font-size:13px}"
    "a{display:inline-block;margin-top:16px;color:#6aa6ff}</style></head><body>"
    "<div class=\"card\">";

static const char HTML_DONE_FOOT[] = "<a href=\"/\">&larr; 返回</a></div></body></html>";

static int build_index_page(void) {
    int off = 0;
    off = buf_adds(s_body_buf, off, WOL_BODY_BUF_SIZE, HTML_HEAD);

    off = buf_adds(s_body_buf, off, WOL_BODY_BUF_SIZE, "<div class=\"row\"><span class=\"k\">Wi-Fi</span><span class=\"v ");
    off = buf_adds(s_body_buf, off, WOL_BODY_BUF_SIZE, s_wifi_state == WIFI_ONLINE ? "ok\">ONLINE" : "warn\">");
    if (s_wifi_state != WIFI_ONLINE) {
        const char *st = "IDLE";
        switch (s_wifi_state) {
            case WIFI_CONNECTING: st = "CONNECTING"; break;
            case WIFI_BACKOFF:    st = "RETRYING";   break;
            default: break;
        }
        off = buf_adds(s_body_buf, off, WOL_BODY_BUF_SIZE, st);
    }
    off = buf_adds(s_body_buf, off, WOL_BODY_BUF_SIZE, "</span></div>");

    off = buf_addf(s_body_buf, off, WOL_BODY_BUF_SIZE,
                   "<div class=\"row\"><span class=\"k\">IP</span><span class=\"v mono\">%s</span></div>",
                   s_ip_str);
    off = buf_addf(s_body_buf, off, WOL_BODY_BUF_SIZE,
                   "<div class=\"row\"><span class=\"k\">广播地址</span><span class=\"v mono\">%s</span></div>",
                   s_bc_str);
    off = buf_addf(s_body_buf, off, WOL_BODY_BUF_SIZE,
                   "<div class=\"row\"><span class=\"k\">目标 MAC</span><span class=\"v mono\">%s</span></div>",
                   WOL_TARGET_MAC);

    {
        int ds5 = wol_ds5_is_connected();
        const char *txt = (ds5 < 0) ? "N/A" : (ds5 ? "Connected" : "Disconnected");
        const char *cls = (ds5 < 0) ? "warn" : (ds5 ? "ok" : "bad");
        off = buf_addf(s_body_buf, off, WOL_BODY_BUF_SIZE,
                       "<div class=\"row\"><span class=\"k\">DS5 手柄</span><span class=\"v %s\">%s</span></div>",
                       cls, txt);
    }

    off = buf_addf(s_body_buf, off, WOL_BODY_BUF_SIZE,
                   "<div class=\"row\"><span class=\"k\">运行时间</span><span class=\"v\">%u 秒</span></div>",
                   (unsigned)((now_ms() - s_boot_ms) / 1000));

    off = buf_adds(s_body_buf, off, WOL_BODY_BUF_SIZE, HTML_FOOT);
    return off;
}

static int build_result_page(const char *title, bool ok, const char *k1, const char *v1,
                             const char *k2, const char *v2, const char *k3, const char *v3) {
    int off = 0;
    off = buf_adds(s_body_buf, off, WOL_BODY_BUF_SIZE, HTML_DONE_HEAD);
    off = buf_addf(s_body_buf, off, WOL_BODY_BUF_SIZE, "<h1 class=\"%s\">%s</h1>",
                   ok ? "ok" : "bad", title);
    if (k1) off = buf_addf(s_body_buf, off, WOL_BODY_BUF_SIZE,
                           "<div class=\"row\"><span class=\"k\">%s</span><span class=\"v\">%s</span></div>", k1, v1);
    if (k2) off = buf_addf(s_body_buf, off, WOL_BODY_BUF_SIZE,
                           "<div class=\"row\"><span class=\"k\">%s</span><span class=\"v\">%s</span></div>", k2, v2);
    if (k3) off = buf_addf(s_body_buf, off, WOL_BODY_BUF_SIZE,
                           "<div class=\"row\"><span class=\"k\">%s</span><span class=\"v\">%s</span></div>", k3, v3);
    off = buf_adds(s_body_buf, off, WOL_BODY_BUF_SIZE, HTML_DONE_FOOT);
    return off;
}

// ---------------------------------------------------------------- HTTP

static void http_close(struct tcp_pcb *pcb) {
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);        // 关不掉就强拆，避免连接泄漏
    }
    if (s_http_conns > 0) s_http_conns--;
}

static void http_send(struct tcp_pcb *pcb, int status, const char *reason,
                      const char *ctype, int body_len) {
    int hl = snprintf(s_head_buf, WOL_HEAD_BUF_SIZE,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %d\r\n"
                      "Connection: close\r\n"
                      "Cache-Control: no-store\r\n"
                      "\r\n",
                      status, reason, ctype, body_len);
    if (hl < 0) hl = 0;

    // 分段写：先头后身，省掉一个完整响应的缓冲
    if (tcp_write(pcb, s_head_buf, (u16_t)hl, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE) == ERR_OK &&
        body_len > 0) {
        tcp_write(pcb, s_body_buf, (u16_t)body_len, TCP_WRITE_FLAG_COPY);
    }
    tcp_output(pcb);
    http_close(pcb);
}

static void http_send_text(struct tcp_pcb *pcb, int status, const char *reason, const char *text) {
    int n = buf_adds(s_body_buf, 0, WOL_BODY_BUF_SIZE, text);
    http_send(pcb, status, reason, "text/plain; charset=utf-8", n);
}

// 从 path 里取 ?key=xxx 的值。返回静态缓冲里的副本。
static const char *query_value(const char *target, const char *name, char *out, size_t out_cap) {
    const char *q = strchr(target, '?');
    if (q == NULL) return NULL;
    q++;

    size_t nlen = strlen(name);
    while (*q) {
        const char *eq = strchr(q, '=');
        if (eq == NULL) break;
        size_t klen = (size_t)(eq - q);
        const char *amp = strchr(eq + 1, '&');
        const char *vend = amp ? amp : (eq + 1 + strlen(eq + 1));

        if (klen == nlen && strncmp(q, name, nlen) == 0) {
            size_t vlen = (size_t)(vend - (eq + 1));
            if (vlen >= out_cap) vlen = out_cap - 1;
            memcpy(out, eq + 1, vlen);
            out[vlen] = '\0';
            return out;
        }
        if (amp == NULL) break;
        q = amp + 1;
    }
    return NULL;
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;

    if (p == NULL) {
        // 对端关闭
        http_close(pcb);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        http_close(pcb);
        return err;
    }

    u16_t len = p->tot_len;
    if (len >= WOL_REQ_BUF_SIZE) len = WOL_REQ_BUF_SIZE - 1;
    pbuf_copy_partial(p, s_req_buf, len, 0);
    s_req_buf[len] = '\0';

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    // 浏览器的一个 GET 请求通常一个 TCP 段就发完。这里只处理完整请求头，
    // 不完整的直接丢弃，避免为每个连接再分配一个累积缓冲。
    if (strstr(s_req_buf, "\r\n\r\n") == NULL && strstr(s_req_buf, "\n\n") == NULL) {
        http_send_text(pcb, 400, "Bad Request", "400 Bad Request: 请求不完整");
        return ERR_OK;
    }

    // 解析请求行 METHOD PATH VERSION
    // 用 static 缓冲而不是栈数组：core0 的栈只有 2KB，这里不能大手大脚。
    s_method[0] = '\0';
    s_target[0] = '\0';
    {
        const char *sp1 = strchr(s_req_buf, ' ');
        if (sp1 == NULL) {
            http_send_text(pcb, 400, "Bad Request", "400 Bad Request");
            return ERR_OK;
        }
        size_t mlen = (size_t)(sp1 - s_req_buf);
        if (mlen >= sizeof(s_method)) mlen = sizeof(s_method) - 1;
        memcpy(s_method, s_req_buf, mlen);
        s_method[mlen] = '\0';

        const char *p2 = sp1 + 1;
        const char *sp2 = strchr(p2, ' ');
        size_t tlen = sp2 ? (size_t)(sp2 - p2) : strlen(p2);
        if (tlen >= sizeof(s_target)) tlen = sizeof(s_target) - 1;
        memcpy(s_target, p2, tlen);
        s_target[tlen] = '\0';
    }

    if (strcmp(s_method, "GET") != 0 && strcmp(s_method, "HEAD") != 0) {
        http_send_text(pcb, 405, "Method Not Allowed", "405 Method Not Allowed");
        return ERR_OK;
    }

    WOL_LOG("HTTP: %s %s\n", s_method, s_target);

    if (strcmp(s_target, "/") == 0 || strcmp(s_target, "/index.html") == 0) {
        int n = build_index_page();
        http_send(pcb, 200, "OK", "text/html; charset=utf-8", n);
        return ERR_OK;
    }

    if (strncmp(s_target, "/status", 7) == 0) {
        int n = snprintf(s_body_buf, WOL_BODY_BUF_SIZE,
                         "{\"wifi\":\"%s\",\"ip\":\"%s\",\"broadcast\":\"%s\","
                         "\"ds5\":%d,\"target_mac\":\"%s\",\"uptime_ms\":%u}",
                         s_wifi_state == WIFI_ONLINE ? "ONLINE" : "OFFLINE",
                         s_ip_str, s_bc_str,
                         wol_ds5_is_connected(), WOL_TARGET_MAC,
                         (unsigned)(now_ms() - s_boot_ms));
        if (n < 0) n = 0;
        http_send(pcb, 200, "OK", "application/json; charset=utf-8", n);
        return ERR_OK;
    }

    if (strncmp(s_target, "/wake", 5) == 0) {
        s_keybuf[0] = '\0';
        const char *key = query_value(s_target, "key", s_keybuf, sizeof(s_keybuf));

        if (strcmp(WOL_KEY, "change-me-8f3a") == 0) {
            WOL_LOG("WoL: 拒绝 —— WOL_KEY 还是示例值，请先改成你自己的口令\n");
            int n = build_result_page("拒绝：口令未配置", false,
                                      "原因", "wol_config.h 里的 WOL_KEY 还是示例值",
                                      "处理", "改成你自己的口令后重新编译", NULL, NULL);
            http_send(pcb, 403, "Forbidden", "text/html; charset=utf-8", n);
            return ERR_OK;
        }

        if (key == NULL || !key_equals(key, WOL_KEY)) {
            WOL_LOG("WoL: 拒绝 —— 口令不对\n");
            int n = build_result_page("Forbidden", false,
                                      "原因", "口令不对或没填", NULL, NULL, NULL, NULL);
            http_send(pcb, 403, "Forbidden", "text/html; charset=utf-8", n);
            return ERR_OK;
        }

#if WOL_MAX_PER_HOUR > 0
        {
            ip4_addr_t client = pcb->remote_ip;
            if (rate_limited(&client, now_ms())) {
                WOL_LOG("WoL: 拒绝 —— 触发频率限制\n");
                int n = build_result_page("Too Many Requests", false,
                                          "原因", "唤醒过于频繁", NULL, NULL, NULL, NULL);
                http_send(pcb, 429, "Too Many Requests", "text/html; charset=utf-8", n);
                return ERR_OK;
            }
            int sent = wol_send_magic_packet();
            if (sent > 0) rate_record(&client, now_ms());

            if (sent > 0) {
                char v1[32];
                snprintf(v1, sizeof(v1), "%d 次", sent);
                int n = build_result_page("Magic Packet sent.", true,
                                          "Target MAC", WOL_TARGET_MAC,
                                          "Broadcast", s_bc_str,
                                          "发送次数", v1);
                http_send(pcb, 200, "OK", "text/html; charset=utf-8", n);
            } else {
                int n = build_result_page("Magic Packet FAILED", false,
                                          "原因", "Wi-Fi 未连接或 UDP 发送失败",
                                          "Target MAC", WOL_TARGET_MAC, NULL, NULL);
                http_send(pcb, 503, "Service Unavailable", "text/html; charset=utf-8", n);
            }
        }
#else
        {
            int sent = wol_send_magic_packet();
            if (sent > 0) {
                int n = build_result_page("Magic Packet sent.", true,
                                          "Target MAC", WOL_TARGET_MAC,
                                          "Broadcast", s_bc_str, NULL, NULL);
                http_send(pcb, 200, "OK", "text/html; charset=utf-8", n);
            } else {
                int n = build_result_page("Magic Packet FAILED", false,
                                          "原因", "Wi-Fi 未连接或 UDP 发送失败", NULL, NULL, NULL, NULL);
                http_send(pcb, 503, "Service Unavailable", "text/html; charset=utf-8", n);
            }
        }
#endif
        return ERR_OK;
    }

    http_send_text(pcb, 404, "Not Found", "404 Not Found");
    return ERR_OK;
}

static void http_error(void *arg, err_t err) {
    (void)arg;
    (void)err;
    if (s_http_conns > 0) s_http_conns--;
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;

    if (s_http_conns >= WOL_HTTP_MAX_CONN) {
        // 连接太多：直接拒绝，保护内存
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    s_http_conns++;
    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, http_recv);
    tcp_err(newpcb, http_error);
    return ERR_OK;
}

// ---------------------------------------------------------------- 对外接口

int wol_net_is_up(void) {
    return s_wifi_state == WIFI_ONLINE ? 1 : 0;
}

const char *wol_net_ip_str(void) {
    return s_ip_str;
}

const char *wol_net_broadcast_str(void) {
    return s_bc_str;
}

void wol_net_init(void) {
    s_boot_ms = now_ms();

    WOL_LOG("\n");
    WOL_LOG("=== Pico 2 W Remote Wake (DS5Dongle 集成版) ===\n");
    WOL_LOG("Target MAC: %s\n", WOL_TARGET_MAC);
    WOL_LOG("WOL_KEY: %s\n", (strcmp(WOL_KEY, "change-me-8f3a") == 0)
                                 ? "!! 还是示例值，唤醒会被拒绝，请改 wol_config.h !!"
                                 : "已设置");

    // UDP 发送通道（WoL 用）
    s_udp = udp_new();
    if (s_udp == NULL) {
        WOL_LOG("WoL: udp_new() 失败，UDP 发送不可用\n");
    }

#if WOL_HTTP_ENABLE
    s_listen = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (s_listen == NULL) {
        WOL_LOG("HTTP: tcp_new() 失败\n");
    } else {
        if (tcp_bind(s_listen, IP_ANY_TYPE, WOL_HTTP_PORT) != ERR_OK) {
            WOL_LOG("HTTP: bind 端口 %d 失败\n", WOL_HTTP_PORT);
            tcp_close(s_listen);
            s_listen = NULL;
        } else {
            s_listen = tcp_listen_with_backlog(s_listen, WOL_HTTP_MAX_CONN);
            if (s_listen == NULL) {
                WOL_LOG("HTTP: listen 失败\n");
            } else {
                tcp_accept(s_listen, http_accept);
                WOL_LOG("HTTP server ready on port %d\n", WOL_HTTP_PORT);
            }
        }
    }
#endif

    // 首次 Wi-Fi 连接放到 poll 里发起，保证 init 本身不阻塞
    s_wifi_state = WIFI_IDLE;

#if !WOL_WIFI_ENABLE
    WOL_LOG("WiFi: 已被 WOL_WIFI_ENABLE=0 关闭 —— 完全不启动无线，蓝牙不受影响\n");
#endif
}

void wol_net_poll(void) {
    // 1) lwIP 定时器（NO_SYS=1 下必须手动驱动，ARP/DHCP/TCP 重传都靠它）
    sys_check_timeouts();

    // 2) Wi-Fi 状态机
    wifi_poll();
}

// 弱符号：main.cpp 里可以定义同名强符号来覆盖，让页面显示真实的 DS5 状态。
// 返回 1=已连接, 0=未连接, -1=未知。
extern "C" __attribute__((weak)) int wol_ds5_is_connected(void) {
    return -1;
}
