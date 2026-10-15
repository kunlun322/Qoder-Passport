// TLS 客户端请求 4KB 最大分片(RFC 6066 max_fragment_length)。
//
// 背景：服务端默认发满长 16KB TLS 记录。无 PSRAM 的 ESP32-C3 在实时语音中堆被
// Wi-Fi/lwIP 收包切碎，IDF 动态缓冲按记录临时申请约 16.7KB 连续块，实机多次
// alloc(16749) 失败、-0x7F00 断线。动态缓冲又不能与本扩展同用(握手重组会越界)，
// 所以 sdkconfig.defaults 改为固定缓冲：接收 8KB、发送 4KB，握手前一次分配。
//
// 做法：esp-tls 不提供设置分片长度的接口，这里在链接期包装 esp-tls 调用的
// mbedtls_ssl_config_defaults()（main/CMakeLists.txt 中 --wrap），客户端配置生成后
// 追加 mbedtls_ssl_conf_max_frag_len(4096)。api.qoder.com 已确认回应该扩展，入站
// 记录 ≤4KB；8KB 接收缓冲同时足以重组其约 5.6KB 的证书链握手消息。
//
// 不改变：证书校验、密码套件、协议版本。

#include <stdbool.h>

#include "esp_log.h"
#include "mbedtls/ssl.h"
#include "sdkconfig.h"

#if !defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
#error "tls_mfl.c 需要 CONFIG_MBEDTLS_SSL_MAX_FRAGMENT_LENGTH=y"
#endif
#if defined(CONFIG_MBEDTLS_DYNAMIC_BUFFER)
#error "max_fragment_length 与 CONFIG_MBEDTLS_DYNAMIC_BUFFER 不兼容(握手重组越界写堆)"
#endif

static const char *TAG = "tls_mfl";

int __real_mbedtls_ssl_config_defaults(mbedtls_ssl_config *conf, int endpoint, int transport,
                                       int preset);

int __wrap_mbedtls_ssl_config_defaults(mbedtls_ssl_config *conf, int endpoint, int transport,
                                       int preset) {
    const int ret = __real_mbedtls_ssl_config_defaults(conf, endpoint, transport, preset);
    if (ret != 0 || endpoint != MBEDTLS_SSL_IS_CLIENT || transport != MBEDTLS_SSL_TRANSPORT_STREAM) {
        return ret;
    }
    const int mfl = mbedtls_ssl_conf_max_frag_len(conf, MBEDTLS_SSL_MAX_FRAG_LEN_4096);
    static bool s_logged;
    if (mfl != 0) {
        // 接收缓冲只有 8KB：服务端若仍发 16KB 记录，连接会以记录过长失败并在界面报错。
        ESP_LOGW(TAG, "设置 TLS 最大分片失败 -0x%04X，大于 8KB 的记录将无法接收", (unsigned)-mfl);
    } else if (!s_logged) {
        s_logged = true;
        ESP_LOGI(TAG, "TLS 客户端请求最大分片 4096 字节");
    }
    return ret;
}
