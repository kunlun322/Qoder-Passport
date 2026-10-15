// main/qca_config.h —— QCA（Qoder Cloud Agents）接入常量。
//
// PAT 由热点配置页录入并保存在设备 NVS（见 qca_pat.h）；不编译进固件。
#pragma once

#define QCA_HOST           "api.qoder.com"
#define QCA_API_BASE       "https://" QCA_HOST "/api/v1"
#define QCA_WS_BASE        "wss://" QCA_HOST "/api/v1/forward/realtime"
#define QCA_PROTO_VERSION  "voice.realtime.v1"
#define QCA_CONV_TITLE     "Qoder Passport"
