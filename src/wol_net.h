//
// wol_net.h —— 网络唤醒模块对外接口
//
// 在 main.cpp 里只需要三处改动：
//   1. #include "wol_net.h"
//   2. main() 初始化段加一行 wol_net_init();
//   3. 主循环里加一行 wol_net_poll();
//
// 所有函数都保证不阻塞（单次调用耗时在微秒级），不会影响 DS5 的
// 蓝牙连接、音频和 USB 上报。
//

#ifndef DS5_WOL_NET_H
#define DS5_WOL_NET_H

#ifdef __cplusplus
extern "C" {
#endif

// 初始化：创建 HTTP 监听、UDP 发送通道，并启动首次 Wi-Fi 连接（异步）。
// 必须在 cyw43_arch_init() 之后调用。
void wol_net_init(void);

// 主循环里每圈调用一次。做三件很轻的事：
//   - 推进 Wi-Fi 连接状态机（含退避重连）
//   - 驱动 lwIP 定时器（sys_check_timeouts）
//   - 处理待发送的响应
void wol_net_poll(void);

// 主动发一次 Magic Packet（和 HTTP /wake 走同一条路径）。
// 返回实际发出的包数，0 表示失败（通常是 Wi-Fi 还没连上）。
int wol_send_magic_packet(void);

// Wi-Fi 是否已经拿到 IP
int wol_net_is_up(void);

// 当前 IP 字符串，未连接时返回 "0.0.0.0"
const char *wol_net_ip_str(void);

// 当前广播地址字符串（由 IP + 掩码动态算出），未连接时返回 "-"
const char *wol_net_broadcast_str(void);

// DS5 手柄状态由 main.cpp 那边提供，HTTP 页面上显示用。
// 这是个弱符号：如果你不实现它，页面会显示 "N/A"。
int wol_ds5_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // DS5_WOL_NET_H
