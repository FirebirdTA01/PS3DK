/*! \file cell/libnetctl.h
 \brief cellNetCtl network-control API declarations.

  Declarations are backed by libnetctl_stub.a, generated from
  tools/nidgen/nids/extracted/libnetctl_stub.yaml. Link with
  -lnetctl_stub and load CELL_SYSMODULE_NETCTL before calling this API.
*/

#ifndef __PS3DK_CELL_LIBNETCTL_H__
#define __PS3DK_CELL_LIBNETCTL_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes. */
#define CELL_NET_CTL_ERROR_NOT_INITIALIZED              0x80130101
#define CELL_NET_CTL_ERROR_NOT_TERMINATED               0x80130102
#define CELL_NET_CTL_ERROR_HANDLER_MAX                  0x80130103
#define CELL_NET_CTL_ERROR_ID_NOT_FOUND                 0x80130104
#define CELL_NET_CTL_ERROR_INVALID_ID                   0x80130105
#define CELL_NET_CTL_ERROR_INVALID_CODE                 0x80130106
#define CELL_NET_CTL_ERROR_INVALID_ADDR                 0x80130107
#define CELL_NET_CTL_ERROR_NOT_CONNECTED                0x80130108
#define CELL_NET_CTL_ERROR_NOT_AVAIL                    0x80130109
#define CELL_NET_CTL_ERROR_INVALID_TYPE                 0x8013010a
#define CELL_NET_CTL_ERROR_INVALID_SIZE                 0x8013010b
#define CELL_NET_CTL_ERROR_NET_DISABLED                 0x80130181
#define CELL_NET_CTL_ERROR_NET_NOT_CONNECTED            0x80130182
#define CELL_NET_CTL_ERROR_NP_NO_ACCOUNT                0x80130183
#define CELL_NET_CTL_ERROR_NP_RESERVED1                 0x80130184
#define CELL_NET_CTL_ERROR_NP_RESERVED2                 0x80130185
#define CELL_NET_CTL_ERROR_NET_CABLE_NOT_CONNECTED      0x80130186
#define CELL_NET_CTL_ERROR_DIALOG_CANCELED              0x80130190
#define CELL_NET_CTL_ERROR_DIALOG_ABORTED               0x80130191
#define CELL_NET_CTL_ERROR_WLAN_DEAUTHED                0x80130137
#define CELL_NET_CTL_ERROR_WLAN_KEYINFO_EXCHNAGE_TIMEOUT 0x8013013d
#define CELL_NET_CTL_ERROR_WLAN_ASSOC_FAILED            0x8013013e
#define CELL_NET_CTL_ERROR_WLAN_AP_DISAPPEARED          0x8013013f
#define CELL_NET_CTL_ERROR_PPPOE_SESSION_INIT           0x80130409
#define CELL_NET_CTL_ERROR_PPPOE_SESSION_NO_PADO        0x8013040a
#define CELL_NET_CTL_ERROR_PPPOE_SESSION_NO_PADS        0x8013040b
#define CELL_NET_CTL_ERROR_PPPOE_SESSION_GET_PADT       0x8013040d
#define CELL_NET_CTL_ERROR_PPPOE_SESSION_SERVICE_NAME   0x8013040f
#define CELL_NET_CTL_ERROR_PPPOE_SESSION_AC_SYSTEM      0x80130410
#define CELL_NET_CTL_ERROR_PPPOE_SESSION_GENERIC        0x80130411
#define CELL_NET_CTL_ERROR_PPPOE_STATUS_AUTH            0x80130412
#define CELL_NET_CTL_ERROR_PPPOE_STATUS_NETWORK         0x80130413
#define CELL_NET_CTL_ERROR_PPPOE_STATUS_TERMINATE       0x80130414
#define CELL_NET_CTL_ERROR_DHCP_LEASE_TIME              0x80130504

/* Connection states and events. */
#define CELL_NET_CTL_STATE_Disconnected       0
#define CELL_NET_CTL_STATE_Connecting         1
#define CELL_NET_CTL_STATE_IPObtaining        2
#define CELL_NET_CTL_STATE_IPObtained         3

#define CELL_NET_CTL_EVENT_CONNECT_REQ        0
#define CELL_NET_CTL_EVENT_ESTABLISH          1
#define CELL_NET_CTL_EVENT_GET_IP             2
#define CELL_NET_CTL_EVENT_DISCONNECT_REQ     3
#define CELL_NET_CTL_EVENT_ERROR              4
#define CELL_NET_CTL_EVENT_LINK_DISCONNECTED  5
#define CELL_NET_CTL_EVENT_AUTO_RETRY         6

/* cellNetCtlGetInfo selectors. */
#define CELL_NET_CTL_INFO_DEVICE              1
#define CELL_NET_CTL_INFO_ETHER_ADDR          2
#define CELL_NET_CTL_INFO_MTU                 3
#define CELL_NET_CTL_INFO_LINK                4
#define CELL_NET_CTL_INFO_LINK_TYPE           5
#define CELL_NET_CTL_INFO_BSSID               6
#define CELL_NET_CTL_INFO_SSID                7
#define CELL_NET_CTL_INFO_WLAN_SECURITY       8
#define CELL_NET_CTL_INFO_8021X_TYPE          9
#define CELL_NET_CTL_INFO_8021X_AUTH_NAME     10
#define CELL_NET_CTL_INFO_RSSI                11
#define CELL_NET_CTL_INFO_CHANNEL             12
#define CELL_NET_CTL_INFO_IP_CONFIG           13
#define CELL_NET_CTL_INFO_DHCP_HOSTNAME       14
#define CELL_NET_CTL_INFO_PPPOE_AUTH_NAME     15
#define CELL_NET_CTL_INFO_IP_ADDRESS          16
#define CELL_NET_CTL_INFO_NETMASK             17
#define CELL_NET_CTL_INFO_DEFAULT_ROUTE       18
#define CELL_NET_CTL_INFO_PRIMARY_DNS         19
#define CELL_NET_CTL_INFO_SECONDARY_DNS       20
#define CELL_NET_CTL_INFO_HTTP_PROXY_CONFIG   21
#define CELL_NET_CTL_INFO_HTTP_PROXY_SERVER   22
#define CELL_NET_CTL_INFO_HTTP_PROXY_PORT     23
#define CELL_NET_CTL_INFO_UPNP_CONFIG         24
#define CELL_NET_CTL_INFO_RESERVED1           25
#define CELL_NET_CTL_INFO_RESERVED2           26

/* Device and link metadata. */
#define CELL_NET_CTL_DEVICE_WIRED             0
#define CELL_NET_CTL_DEVICE_WIRELESS          1

#define CELL_NET_CTL_LINK_TYPE_AUTO           0
#define CELL_NET_CTL_LINK_TYPE_10BASE_HALF    1
#define CELL_NET_CTL_LINK_TYPE_10BASE_FULL    2
#define CELL_NET_CTL_LINK_TYPE_100BASE_HALF   3
#define CELL_NET_CTL_LINK_TYPE_100BASE_FULL   4
#define CELL_NET_CTL_LINK_TYPE_1000BASE_HALF  5
#define CELL_NET_CTL_LINK_TYPE_1000BASE_FULL  6

#define CELL_NET_CTL_LINK_DISCONNECTED        0
#define CELL_NET_CTL_LINK_CONNECTED           1

#define CELL_NET_CTL_WLAN_SECURITY_NOAUTH     0
#define CELL_NET_CTL_WLAN_SECURITY_WEP        1
#define CELL_NET_CTL_WLAN_SECURITY_WPAPSK_TKIP 2
#define CELL_NET_CTL_WLAN_SECURITY_WPAPSK_AES 3
#define CELL_NET_CTL_WLAN_SECURITY_UNSUPPORTED 4
#define CELL_NET_CTL_WLAN_SECURITY_WPA2PSK_TKIP 5
#define CELL_NET_CTL_WLAN_SECURITY_WPA2PSK_AES 6
#define CELL_NET_CTL_WLAN_SECURITY_WPAPSK_WPA2PSK 7

#define CELL_NET_CTL_8021X_NONE               0
#define CELL_NET_CTL_8021X_EAP_MD5            1

#define CELL_NET_CTL_IP_DHCP                  0
#define CELL_NET_CTL_IP_STATIC                1
#define CELL_NET_CTL_IP_PPPOE                 2

#define CELL_NET_CTL_HTTP_PROXY_OFF           0
#define CELL_NET_CTL_HTTP_PROXY_ON            1

#define CELL_NET_CTL_UPNP_ON                  0
#define CELL_NET_CTL_UPNP_OFF                 1

#define CELL_NET_CTL_ETHER_ADDR_LEN           6
#define CELL_NET_CTL_BSSID_LEN                6
#define CELL_NET_CTL_SSID_LEN                 32
#define CELL_NET_CTL_WLAN_SECURITY_KEY_LEN    65
#define CELL_NET_CTL_AUTH_NAME_LEN            128
#define CELL_NET_CTL_AUTH_KEY_LEN             128
#define CELL_NET_CTL_DHCP_HOSTNAME_LEN        255
#define CELL_NET_CTL_HOSTNAME_LEN             256
#define CELL_NET_CTL_IPV4_ADDR_STR_LEN        16

#ifndef __IN_H__
struct in_addr {
    uint32_t s_addr;
};
#endif

struct CellNetCtlEtherAddr {
    uint8_t data[CELL_NET_CTL_ETHER_ADDR_LEN];
    uint8_t padding[2];
};

struct CellNetCtlSSID {
    uint8_t data[CELL_NET_CTL_SSID_LEN];
    uint8_t term;
    uint8_t padding[3];
};

union CellNetCtlInfo {
    uint32_t device;
    struct CellNetCtlEtherAddr ether_addr;
    uint32_t mtu;
    uint32_t link;
    uint32_t link_type;
    struct CellNetCtlEtherAddr bssid;
    struct CellNetCtlSSID ssid;
    uint32_t wlan_security;
    uint32_t auth_8021x_type;
    char auth_8021x_auth_name[CELL_NET_CTL_AUTH_NAME_LEN];
    uint8_t rssi;
    uint8_t channel;
    uint32_t ip_config;
    char dhcp_hostname[CELL_NET_CTL_HOSTNAME_LEN];
    char pppoe_auth_name[CELL_NET_CTL_AUTH_NAME_LEN];
    char ip_address[CELL_NET_CTL_IPV4_ADDR_STR_LEN];
    char netmask[CELL_NET_CTL_IPV4_ADDR_STR_LEN];
    char default_route[CELL_NET_CTL_IPV4_ADDR_STR_LEN];
    char primary_dns[CELL_NET_CTL_IPV4_ADDR_STR_LEN];
    char secondary_dns[CELL_NET_CTL_IPV4_ADDR_STR_LEN];
    uint32_t http_proxy_config;
    char http_proxy_server[CELL_NET_CTL_HOSTNAME_LEN];
    uint16_t http_proxy_port;
    uint32_t upnp_config;
};

struct CellNetCtlNatInfo {
    uint32_t size;
    int upnp_status;
    int stun_status;
    int nat_type;
    struct in_addr mapped_addr;
};

#define CELL_NET_CTL_NATINFO_UPNP_UNCHECKED   0
#define CELL_NET_CTL_NATINFO_UPNP_NO          1
#define CELL_NET_CTL_NATINFO_UPNP_USED        2
#define CELL_NET_CTL_NATINFO_STUN_UNCHECKED   0
#define CELL_NET_CTL_NATINFO_STUN_FAILED      1
#define CELL_NET_CTL_NATINFO_STUN_OK          2
#define CELL_NET_CTL_NATINFO_NAT_TYPE_1       1
#define CELL_NET_CTL_NATINFO_NAT_TYPE_2       2
#define CELL_NET_CTL_NATINFO_NAT_TYPE_3       3

struct CellNetCtlNetStartDialogParam {
    uint32_t size;
    int type;
    uint32_t cid;
};

#define CELL_NET_CTL_NETSTART_TYPE_NET         0
#define CELL_NET_CTL_NETSTART_TYPE_NP          1
#define CELL_NET_CTL_NETSTART_TYPE_MAX         2

struct CellNetCtlNetStartDialogResult {
    uint32_t size;
    int result;
};

#define CELL_SYSUTIL_NET_CTL_NETSTART_LOADED   0x0801
#define CELL_SYSUTIL_NET_CTL_NETSTART_FINISHED 0x0802
#define CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED 0x0803

typedef void (*cellNetCtlHandler)(
    int prev_state,
    int new_state,
    int event,
    int error_code,
    void *arg);

int cellNetCtlInit(void);
void cellNetCtlTerm(void);
int cellNetCtlGetInfo(int code, union CellNetCtlInfo *info);
int cellNetCtlGetState(int *state);
int cellNetCtlAddHandler(cellNetCtlHandler handler, void *arg, int *hid);
int cellNetCtlDelHandler(int hid);
int cellNetCtlGetNatInfo(struct CellNetCtlNatInfo *natinfo);
int cellNetCtlNetStartDialogLoadAsync(const struct CellNetCtlNetStartDialogParam *param);
int cellNetCtlNetStartDialogAbortAsync(void);
int cellNetCtlNetStartDialogUnloadAsync(struct CellNetCtlNetStartDialogResult *result);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_LIBNETCTL_H__ */
