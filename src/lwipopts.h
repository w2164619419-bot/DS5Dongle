//
// lwipopts.h —— 为 DS5Dongle 定制的 lwIP 最小配置
//
// 为什么要自己写这个文件：
//   这个固件把 ~220KB 的 libopus 和大量 BT/USB 热路径搬进了 SRAM，
//   留给 lwIP 的 RAM 非常有限。lwIP 的默认配置（MEM_SIZE 1600、
//   PBUF_POOL_SIZE 16 等）在这里必须裁到最小可用。
//
// 放在 src/ 下即可生效：Pico SDK 的 lwIP 组件是 INTERFACE 库，源文件在
// 最终可执行目标 ds5-bridge 里编译，而 src/ 已经在它的 include 路径中，
// 所以 lwIP 的 opt.h 能自动找到本文件。
//
// 本配置下 lwIP 的静态 RAM 占用约 10~12KB。
//

#ifndef DS5_WOL_LWIPOPTS_H
#define DS5_WOL_LWIPOPTS_H

// 需要读 WOL_USE_DHCP 来决定是否编入 DHCP 客户端。
// wol_config.h 是纯宏文件，无其它依赖。
#include "wol_config.h"

// ---------------------------------------------------------- 运行模式
#define NO_SYS                      1   // 裸机，无操作系统
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_SOCKET                 0   // 不用 socket API（省内存）
#define LWIP_NETCONN                0   // 不用 netconn API
#define LWIP_TIMERS                 1   // 需要 sys_check_timeouts()

// 用 lwIP 自带的静态堆，而不是 libc malloc：
// 内存占用在编译期就确定（体现在 .bss 上），链接时超了会直接报错，
// 不会等到运行时才 OOM。
#define MEM_LIBC_MALLOC             0
#define MEM_SIZE                    1536
#define MEM_ALIGNMENT               4

// ---------------------------------------------------------- 协议裁剪
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0   // 关掉 IPv6
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1   // 留着，方便 ping 一下看通不通
#define LWIP_RAW                    0
#define LWIP_UDP                    1   // WoL 需要
#define LWIP_TCP                    1   // HTTP 需要
#define LWIP_DNS                    0   // 不用域名
#define LWIP_DHCP                   WOL_USE_DHCP
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_NETIF_HOSTNAME         1   // DHCP 上报主机名
#define LWIP_NETIF_TX_SINGLE_PBUF   1

// ---------------------------------------------------------- 缓冲区（最关键）
#define PBUF_POOL_SIZE              4      // 收包缓冲池
#define MEMP_NUM_UDP_PCB            2
#define MEMP_NUM_TCP_PCB            3
#define MEMP_NUM_TCP_PCB_LISTEN     1
#define MEMP_NUM_TCP_SEG            8
#define MEMP_NUM_ARP_QUEUE          4

#define TCP_MSS                     1460
#define TCP_WND                     (2 * TCP_MSS)
#define TCP_SND_BUF                 (2 * TCP_MSS)
#define TCP_SND_QUEUELEN            8
#define TCP_QUEUE_OOSEQ             0   // 不做乱序重排，省内存
#define TCP_LISTEN_BACKLOG          1
#define LWIP_TCP_SACK_OUT           0
#define LWIP_TCP_TIMESTAMPS         0
#define TCP_KEEPALIVE               0
#define LWIP_WND_SCALE              0
#define TCP_RCV_SCALE               0

// ---------------------------------------------------------- 校验和 / 统计 / 调试
#define LWIP_CHKSUM_ALGORITHM       3   // RP2350 上最快的一种
#define LWIP_STATS                  0
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define IP_STATS                    0
#define ICMP_STATS                  0
#define UDP_STATS                   0
#define TCP_STATS                   0
#define MIB2_STATS                  0
#define LWIP_DEBUG                  0

// ---------------------------------------------------------- DHCP 微调
#define LWIP_DHCP_DOES_ACD_CHECK    0   // 省掉地址冲突探测，快一点
#define LWIP_DHCP_GET_NTP_SRV       0

#endif // DS5_WOL_LWIPOPTS_H
