#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_timer.h"
#include "esp_err.h"

// Sony A7R III fake GPS PoC v5 / Seeed XIAO ESP32-C6 / ESP-IDF Bluedroid.
//
// v5 deliberately follows the simplest proven Sony external-GPS ordering used
// by AlphaGPS' upstream Python implementation:
//
//   connect -> bond/encrypt -> exchange MTU -> discover Location service ->
//   (optionally read DD21 for diagnostics) -> write 95-byte location to DD11
//
// IMPORTANT differences from v3/v4:
//   * no EE00 / EE01 pairing command
//   * no GATT operation is attempted before the BLE bond completes
//   * no DD30 / DD31 requirement
//   * DD21 is diagnostic/optional; DD11 is the only required characteristic
//   * fixed 95-byte packet first, matching the public working implementation

namespace {

constexpr double FAKE_LAT = -77.841900;
constexpr double FAKE_LON = 166.686300;
constexpr time_t BASE_UTC_EPOCH = 1789452000; // 2026-09-15 06:00:00 UTC

constexpr uint16_t SONY_COMPANY_ID = 0x012d;
constexpr uint16_t SONY_CAMERA_TYPE = 0x0003;
constexpr uint16_t APP_ID = 0;
constexpr uint16_t UUID_DD11 = 0xdd11; // location update, write
constexpr uint16_t UUID_DD21 = 0xdd21; // location configuration, optional read

// Canonical UUID: 8000dd00-dd00-ffff-ffff-ffffffffffff
constexpr uint8_t GEO_SVC_UUID_LE[16] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x00, 0xdd, 0x00, 0xdd, 0x00, 0x80,
};
constexpr uint8_t GEO_SVC_UUID_BE[16] = {
    0x80, 0x00, 0xdd, 0x00, 0xdd, 0x00, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

struct __attribute__((packed)) SonyAdv {
  uint16_t company_id;
  uint16_t type;
  uint8_t protocol_version;
  uint8_t unused;
  uint16_t model;
  uint8_t tag22;
  uint8_t mode22;
  uint8_t zero0;
  uint8_t tag21;
  uint8_t mode21;
};

struct __attribute__((packed)) SonyGeo95 {
  uint8_t prefix[11];
  int32_t latitude;
  int32_t longitude;
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint8_t zero0[65];
  uint16_t timezone_offset;
  uint8_t dst_offset[2];
};
static_assert(sizeof(SonyGeo95) == 95, "Sony GPS packet must be 95 bytes");

enum class Stage : uint8_t {
  Boot,
  Scanning,
  Connecting,
  Bonding,
  Mtu,
  Discovering,
  ReadConfig,
  Ready,
  Closing,
};

volatile Stage g_stage = Stage::Boot;
volatile bool g_connected = false;
volatile bool g_post_bond_started = false;
volatile bool g_ready = false;
volatile bool g_gatt_op_inflight = false;
volatile bool g_tx_inflight = false;

esp_gatt_if_t g_gattc_if = ESP_GATT_IF_NONE;
uint16_t g_conn_id = 0;
esp_bd_addr_t g_camera_bda = {};
esp_ble_addr_type_t g_camera_addr_type = BLE_ADDR_TYPE_PUBLIC;
uint8_t g_protocol_version = 0;
uint8_t g_mode22 = 0;

enum class SecurityProfile : uint8_t {
  LegacyBond = 0,
  ScCapableBond = 1,
};
SecurityProfile g_security_profile = SecurityProfile::LegacyBond;
uint32_t g_pair_attempt = 0;

uint16_t g_service_start = 0;
uint16_t g_service_end = 0;
uint16_t g_dd11 = 0;
uint16_t g_dd21 = 0;

uint64_t g_rescan_due_us = 0;
uint64_t g_last_tx_us = 0;

esp_ble_scan_params_t g_scan_params{};

void startScan();
void beginPostBond();
void markLocationReady();
void closeAndRescan(const char* why, uint32_t delay_ms = 2500);
void applySecurityProfile();

void printAddr(const esp_bd_addr_t bda) {
  printf("%02X:%02X:%02X:%02X:%02X:%02X",
         bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

const char* authFailToString(esp_ble_auth_fail_rsn_t reason) {
  switch (reason) {
    case ESP_AUTH_SMP_PASSKEY_FAIL: return "PASSKEY_FAIL";
    case ESP_AUTH_SMP_OOB_FAIL: return "OOB_FAIL";
    case ESP_AUTH_SMP_PAIR_AUTH_FAIL: return "PAIR_AUTH_FAIL";
    case ESP_AUTH_SMP_CONFIRM_VALUE_FAIL: return "CONFIRM_VALUE_FAIL";
    case ESP_AUTH_SMP_PAIR_NOT_SUPPORT: return "PAIR_NOT_SUPPORTED";
    case ESP_AUTH_SMP_ENC_KEY_SIZE: return "ENC_KEY_SIZE";
    case ESP_AUTH_SMP_INVALID_CMD: return "INVALID_CMD";
    case ESP_AUTH_SMP_UNKNOWN_ERR: return "UNKNOWN_ERR";
    case ESP_AUTH_SMP_REPEATED_ATTEMPT: return "REPEATED_ATTEMPT";
    case ESP_AUTH_SMP_INVALID_PARAMETERS: return "INVALID_PARAMETERS";
    case ESP_AUTH_SMP_INTERNAL_ERR: return "INTERNAL_ERR";
    case ESP_AUTH_SMP_UNKNOWN_IO: return "UNKNOWN_IO";
    case ESP_AUTH_SMP_INIT_FAIL: return "INIT_FAIL";
    case ESP_AUTH_SMP_CONFIRM_FAIL: return "CONFIRM_FAIL";
    case ESP_AUTH_SMP_BUSY: return "BUSY";
    case ESP_AUTH_SMP_ENC_FAIL: return "ENC_FAIL";
    case ESP_AUTH_SMP_STARTED: return "STARTED";
    case ESP_AUTH_SMP_RSP_TIMEOUT: return "RSP_TIMEOUT";
    case ESP_AUTH_SMP_DIV_NOT_AVAIL: return "DIV_NOT_AVAIL";
    case ESP_AUTH_SMP_UNSPEC_ERR: return "UNSPEC_ERR";
    case ESP_AUTH_SMP_CONN_TOUT: return "CONN_TIMEOUT";
    default: return "OTHER";
  }
}

bool isGeoServiceUuid(const esp_bt_uuid_t& uuid) {
  if (uuid.len != ESP_UUID_LEN_128) return false;
  return memcmp(uuid.uuid.uuid128, GEO_SVC_UUID_LE, 16) == 0 ||
         memcmp(uuid.uuid.uuid128, GEO_SVC_UUID_BE, 16) == 0;
}

void printUuid(const esp_bt_uuid_t& uuid) {
  if (uuid.len == ESP_UUID_LEN_16) {
    printf("%04x", uuid.uuid.uuid16);
  } else if (uuid.len == ESP_UUID_LEN_32) {
    printf("%08" PRIx32, uuid.uuid.uuid32);
  } else if (uuid.len == ESP_UUID_LEN_128) {
    for (int i = 0; i < 16; ++i) printf("%02x", uuid.uuid.uuid128[i]);
  } else {
    printf("<uuid-len-%u>", static_cast<unsigned>(uuid.len));
  }
}

bool isCameraBonded() {
  const int n = esp_ble_get_bond_device_num();
  if (n <= 0) return false;

  auto* list = static_cast<esp_ble_bond_dev_t*>(
      calloc(static_cast<size_t>(n), sizeof(esp_ble_bond_dev_t)));
  if (!list) return false;

  int count = n;
  bool found = false;
  if (esp_ble_get_bond_device_list(&count, list) == ESP_OK) {
    for (int i = 0; i < count; ++i) {
      if (memcmp(list[i].bd_addr, g_camera_bda, ESP_BD_ADDR_LEN) == 0) {
        found = true;
        break;
      }
    }
  }
  free(list);
  return found;
}

bool resolveGeoChar(uint16_t uuid16, uint16_t* out_handle) {
  if (!out_handle || g_gattc_if == ESP_GATT_IF_NONE || !g_connected ||
      g_service_start == 0 || g_service_end == 0) {
    return false;
  }

  esp_bt_uuid_t uuid{};
  uuid.len = ESP_UUID_LEN_16;
  uuid.uuid.uuid16 = uuid16;

  esp_gattc_char_elem_t elem{};
  uint16_t count = 1;
  const esp_gatt_status_t st = esp_ble_gattc_get_char_by_uuid(
      g_gattc_if, g_conn_id, g_service_start, g_service_end,
      uuid, &elem, &count);
  if (st != ESP_GATT_OK || count == 0) {
    *out_handle = 0;
    return false;
  }
  *out_handle = elem.char_handle;
  return true;
}

SonyGeo95 makeGeoPacket() {
  SonyGeo95 geo{};

  // Same 95-byte family used by the public Sony external-GPS implementation.
  const uint8_t prefix[11] = {
      0x00, 0x5d, 0x08, 0x02, 0xfc, 0x03, 0x00, 0x00, 0x10, 0x10, 0x10};
  memcpy(geo.prefix, prefix, sizeof(prefix));

  const int32_t lat = static_cast<int32_t>(FAKE_LAT * 10000000.0);
  const int32_t lon = static_cast<int32_t>(FAKE_LON * 10000000.0);
  geo.latitude = static_cast<int32_t>(__builtin_bswap32(static_cast<uint32_t>(lat)));
  geo.longitude = static_cast<int32_t>(__builtin_bswap32(static_cast<uint32_t>(lon)));

  const time_t now = BASE_UTC_EPOCH +
                     static_cast<time_t>(esp_timer_get_time() / 1000000ULL);
  struct tm utc{};
  gmtime_r(&now, &utc);
  geo.year = __builtin_bswap16(static_cast<uint16_t>(utc.tm_year + 1900));
  geo.month = static_cast<uint8_t>(utc.tm_mon + 1);
  geo.day = static_cast<uint8_t>(utc.tm_mday);
  geo.hour = static_cast<uint8_t>(utc.tm_hour);
  geo.minute = static_cast<uint8_t>(utc.tm_min);
  geo.second = static_cast<uint8_t>(utc.tm_sec);

  // UTC for the PoC. Keep timezone and DST offsets at zero.
  geo.timezone_offset = 0;
  geo.dst_offset[0] = 0;
  geo.dst_offset[1] = 0;
  return geo;
}

void resetConnectionState() {
  g_connected = false;
  g_post_bond_started = false;
  g_ready = false;
  g_gatt_op_inflight = false;
  g_tx_inflight = false;
  g_conn_id = 0;
  g_service_start = 0;
  g_service_end = 0;
  g_dd11 = 0;
  g_dd21 = 0;
}

void scheduleRescan(uint32_t delay_ms) {
  g_rescan_due_us = esp_timer_get_time() +
                    static_cast<uint64_t>(delay_ms) * 1000ULL;
}

void closeAndRescan(const char* why, uint32_t delay_ms) {
  printf("[BLE] %s\n", why ? why : "Closing connection");
  g_ready = false;
  g_stage = Stage::Closing;
  scheduleRescan(delay_ms);

  if (g_connected && g_gattc_if != ESP_GATT_IF_NONE) {
    const esp_err_t err = esp_ble_gattc_close(g_gattc_if, g_conn_id);
    if (err != ESP_OK) {
      printf("[BLE] esp_ble_gattc_close failed: %s\n", esp_err_to_name(err));
      resetConnectionState();
    }
  } else {
    resetConnectionState();
  }
}

void setScanParamsAndStart() {
  g_scan_params = {};
  g_scan_params.scan_type = BLE_SCAN_TYPE_ACTIVE;
  g_scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
  g_scan_params.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
  g_scan_params.scan_interval = 0x50;
  g_scan_params.scan_window = 0x50;
  g_scan_params.scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE;

  const esp_err_t err = esp_ble_gap_set_scan_params(&g_scan_params);
  if (err != ESP_OK) {
    printf("[SCAN] set_scan_params failed: %s\n", esp_err_to_name(err));
    scheduleRescan(2000);
  }
}

void startScan() {
  if (g_connected || g_stage == Stage::Connecting || g_stage == Stage::Bonding ||
      g_stage == Stage::Mtu || g_stage == Stage::Discovering) {
    return;
  }

  g_rescan_due_us = 0;
  g_stage = Stage::Scanning;
  printf("[SCAN] Starting. Put the A7R III into Bluetooth Pairing BEFORE this scan.\n");
  const esp_err_t err = esp_ble_gap_start_scanning(15);
  if (err != ESP_OK) {
    printf("[SCAN] start failed: %s\n", esp_err_to_name(err));
    scheduleRescan(2000);
  }
}

void openCamera(const esp_ble_gap_cb_param_t::ble_scan_result_evt_param& r) {
  memcpy(g_camera_bda, r.bda, ESP_BD_ADDR_LEN);
  g_camera_addr_type = r.ble_addr_type;
  g_stage = Stage::Connecting;
  esp_ble_gap_stop_scanning();

  printf("[BLE] Connecting to ");
  printAddr(g_camera_bda);
  printf(" using Bluedroid\n");

  esp_ble_gatt_creat_conn_params_t cp{};
  memcpy(cp.remote_bda, g_camera_bda, ESP_BD_ADDR_LEN);
  cp.remote_addr_type = g_camera_addr_type;
  cp.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
  cp.is_direct = true;
  cp.is_aux = false;
  cp.phy_mask = 0;

  const esp_err_t err = esp_ble_gattc_enh_open(g_gattc_if, &cp);
  if (err != ESP_OK) {
    printf("[BLE] open failed immediately: %s\n", esp_err_to_name(err));
    resetConnectionState();
    g_stage = Stage::Boot;
    scheduleRescan(2500);
  }
}

void startBondNow() {
  if (!g_connected) return;
  g_stage = Stage::Bonding;
  ++g_pair_attempt;

  printf("[PAIR] BOND-FIRST v6 A/B attempt #%" PRIu32 " profile=%s\n",
         g_pair_attempt,
         g_security_profile == SecurityProfile::LegacyBond ?
           "A:GATTS+LEGACY_BOND" : "B:GATTS+SC_CAPABLE_BOND");
  printf("[PAIR] No MTU exchange, Sony service discovery, EE01, DD21 or DD11 has been attempted yet.\n");
  printf("[PAIR] GATT Server support is ENABLED so Sony can read this C6's Generic Access / Device Name.\n");
  printf("[PAIR] local bond before request=%d\n", isCameraBonded() ? 1 : 0);
  printf("[PAIR] Watch the A7R III screen and press OK if it shows 'SonyGPS-C6-v6'.\n");

  const esp_err_t err = esp_ble_set_encryption(g_camera_bda, ESP_BLE_SEC_ENCRYPT);
  printf("[PAIR] esp_ble_set_encryption(ESP_BLE_SEC_ENCRYPT) -> %s\n",
         esp_err_to_name(err));
  if (err != ESP_OK) {
    closeAndRescan("[PAIR] could not start bond/encryption", 3000);
  }
}

void beginPostBond() {
  if (!g_connected || g_post_bond_started) return;
  g_post_bond_started = true;
  g_stage = Stage::Mtu;

  printf("[PAIR] Bond/encryption established. local bond now=%d\n",
         isCameraBonded() ? 1 : 0);
  printf("[GATT] Now requesting MTU -- this is the FIRST post-bond GATT setup step.\n");

  const esp_err_t err = esp_ble_gattc_send_mtu_req(g_gattc_if, g_conn_id);
  if (err != ESP_OK) {
    printf("[GATT] MTU request could not be submitted: %s\n", esp_err_to_name(err));
    printf("[GATT] Continuing directly to Sony Location service discovery.\n");
    g_stage = Stage::Discovering;
    esp_ble_gattc_search_service(g_gattc_if, g_conn_id, nullptr);
  }
}

void readDd21Diagnostic() {
  if (!g_connected || g_dd21 == 0 || g_gatt_op_inflight) {
    markLocationReady();
    return;
  }

  g_stage = Stage::ReadConfig;
  g_gatt_op_inflight = true;
  printf("[GEO] Optional DD21 diagnostic read after bonding (AUTH_NONE).\n");
  const esp_err_t err = esp_ble_gattc_read_char(
      g_gattc_if, g_conn_id, g_dd21, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    g_gatt_op_inflight = false;
    printf("[GEO] DD21 read submission failed: %s -- ignored for v5.\n",
           esp_err_to_name(err));
    markLocationReady();
  }
}

void markLocationReady() {
  g_stage = Stage::Ready;
  g_ready = true;
  g_last_tx_us = 0;
  printf("[GEO] Sony Location path READY. DD11=0x%04x; fixed packet=95 bytes.\n", g_dd11);
  printf("[GEO] Sending fake Antarctica coordinates every 1 second.\n");
}

void sendFakeLocation() {
  if (!g_connected || !g_ready || g_dd11 == 0 ||
      g_tx_inflight || g_gatt_op_inflight) {
    return;
  }

  SonyGeo95 geo = makeGeoPacket();
  g_tx_inflight = true;
  const esp_err_t err = esp_ble_gattc_write_char(
      g_gattc_if, g_conn_id, g_dd11, sizeof(geo),
      reinterpret_cast<uint8_t*>(&geo),
      ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    g_tx_inflight = false;
    printf("[TX] submit failed: %s\n", esp_err_to_name(err));
  }
}

void handleDiscoveryComplete() {
  if (g_service_start == 0 || g_service_end == 0) {
    closeAndRescan("[GATT] Sony Location service DD00 not found");
    return;
  }

  const bool has11 = resolveGeoChar(UUID_DD11, &g_dd11);
  const bool has21 = resolveGeoChar(UUID_DD21, &g_dd21);

  printf("[GATT] DD11=%s(0x%04x) DD21=%s(0x%04x)\n",
         has11 ? "FOUND" : "missing", g_dd11,
         has21 ? "FOUND" : "missing", g_dd21);

  if (!has11) {
    closeAndRescan("[GATT] Required Sony DD11 location characteristic missing");
    return;
  }

  printf("[GATT] v5 intentionally ignores EE01/DD30/DD31. DD21 is optional.\n");
  if (has21) {
    readDd21Diagnostic();
  } else {
    markLocationReady();
  }
}

void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
        startScan();
      } else {
        printf("[SCAN] parameter setup failed, status=%d\n",
               param->scan_param_cmpl.status);
        scheduleRescan(2000);
      }
      break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      printf("[SCAN] start %s (status=%d)\n",
             param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS ? "OK" : "FAIL",
             param->scan_start_cmpl.status);
      break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      auto& r = param->scan_rst;
      if (r.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT && g_stage == Stage::Scanning) {
        const uint16_t total_len = static_cast<uint16_t>(r.adv_data_len + r.scan_rsp_len);
        uint8_t mfg_len = 0;
        uint8_t* mfg = esp_ble_resolve_adv_data_by_type(
            r.ble_adv, total_len, ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &mfg_len);
        if (mfg && mfg_len >= sizeof(SonyAdv)) {
          SonyAdv adv{};
          memcpy(&adv, mfg, sizeof(adv));
          if (adv.company_id == SONY_COMPANY_ID && adv.type == SONY_CAMERA_TYPE) {
            uint8_t name_len = 0;
            uint8_t* name = esp_ble_resolve_adv_data_by_type(
                r.ble_adv, total_len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
            char name_buf[40] = {};
            if (name && name_len) {
              const size_t n = name_len < sizeof(name_buf) - 1 ?
                               name_len : sizeof(name_buf) - 1;
              memcpy(name_buf, name, n);
            }

            g_protocol_version = adv.protocol_version;
            g_mode22 = adv.mode22;
            printf("[SCAN] Sony camera found: name='%s' addr=", name_buf);
            printAddr(r.bda);
            printf(" proto=%u(0x%02x) mode22=0x%02x bit0x40=%d model=0x%04x RSSI=%d\n",
                   static_cast<unsigned>(adv.protocol_version), adv.protocol_version,
                   adv.mode22, (adv.mode22 & 0x40) ? 1 : 0,
                   adv.model, r.rssi);
            printf("[SCAN] NOTE: v5 logs bit0x40 raw; it does NOT assume this bit means a completed C6 bond.\n");
            openCamera(r);
          }
        }
      } else if (r.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT &&
                 g_stage == Stage::Scanning) {
        printf("[SCAN] ended without connection. Retrying in 3 seconds.\n");
        g_stage = Stage::Boot;
        scheduleRescan(3000);
      }
      break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
      printf("[SCAN] stop status=%d\n", param->scan_stop_cmpl.status);
      break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
      printf("[PAIR] ESP_GAP_BLE_SEC_REQ_EVT from Sony -> ACCEPT\n");
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;

    case ESP_GAP_BLE_NC_REQ_EVT:
      printf("[PAIR] Numeric comparison: %06" PRIu32 " -> YES\n",
             param->ble_security.key_notif.passkey);
      esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
      break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      printf("[PAIR] Passkey notify: %06" PRIu32 "\n",
             param->ble_security.key_notif.passkey);
      break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
      printf("[PAIR] PASSKEY_REQ received. NoInputNoOutput was configured; not supplying a passkey.\n");
      break;

    case ESP_GAP_BLE_KEY_EVT:
      printf("[PAIR] Key exchanged, type=%d\n",
             param->ble_security.ble_key.key_type);
      break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      const auto& a = param->ble_security.auth_cmpl;
      printf("[PAIR] Authentication complete: success=%d addr=", a.success ? 1 : 0);
      printAddr(a.bd_addr);
      printf(" auth_mode=0x%02x", static_cast<unsigned>(a.auth_mode));
      if (!a.success) {
        printf(" fail_reason=0x%02x (%s)\n",
               static_cast<unsigned>(a.fail_reason),
               authFailToString(a.fail_reason));
        const SecurityProfile previous = g_security_profile;
        g_security_profile = (g_security_profile == SecurityProfile::LegacyBond)
            ? SecurityProfile::ScCapableBond
            : SecurityProfile::LegacyBond;
        printf("[PAIR] A/B result: %s FAILED. Next connection will try %s.\n",
               previous == SecurityProfile::LegacyBond ? "A:GATTS+LEGACY_BOND" : "B:GATTS+SC_CAPABLE_BOND",
               g_security_profile == SecurityProfile::LegacyBond ? "A:GATTS+LEGACY_BOND" : "B:GATTS+SC_CAPABLE_BOND");
        applySecurityProfile();
        closeAndRescan("[PAIR] bond-first SMP failed", 3500);
      } else {
        printf(" -> BONDED/ENCRYPTED\n");
        beginPostBond();
      }
      break;
    }

    default:
      break;
  }
}

void gattcCallback(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                   esp_ble_gattc_cb_param_t* param) {
  if (event == ESP_GATTC_REG_EVT) {
    if (param->reg.status != ESP_GATT_OK) {
      printf("[FATAL] GATTC app registration failed, status=%d\n",
             param->reg.status);
      return;
    }
    g_gattc_if = gattc_if;
    printf("[BLE] GATTC registered, interface=%u\n",
           static_cast<unsigned>(gattc_if));
    setScanParamsAndStart();
    return;
  }

  switch (event) {
    case ESP_GATTC_CONNECT_EVT:
      printf("[BLE] Connected event: conn_id=%u remote=",
             param->connect.conn_id);
      printAddr(param->connect.remote_bda);
      printf("\n");
      break;

    case ESP_GATTC_OPEN_EVT:
      if (param->open.status != ESP_GATT_OK) {
        printf("[BLE] Open failed, status=0x%x\n", param->open.status);
        resetConnectionState();
        g_stage = Stage::Boot;
        scheduleRescan(2500);
        break;
      }

      g_connected = true;
      g_conn_id = param->open.conn_id;
      memcpy(g_camera_bda, param->open.remote_bda, ESP_BD_ADDR_LEN);
      printf("[BLE] Open OK. conn_id=%u MTU(initial)=%u\n",
             g_conn_id, param->open.mtu);
      startBondNow();
      break;

    case ESP_GATTC_CFG_MTU_EVT:
      printf("[GATT] MTU exchange: status=%d MTU=%u\n",
             param->cfg_mtu.status, param->cfg_mtu.mtu);
      printf("[GATT] Discovering Sony Location service AFTER bond...\n");
      g_stage = Stage::Discovering;
      esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, nullptr);
      break;

    case ESP_GATTC_SEARCH_RES_EVT:
      if (isGeoServiceUuid(param->search_res.srvc_id.uuid)) {
        g_service_start = param->search_res.start_handle;
        g_service_end = param->search_res.end_handle;
        printf("[GATT] Sony Location service DD00 FOUND: handles 0x%04x..0x%04x uuid_raw=",
               g_service_start, g_service_end);
        printUuid(param->search_res.srvc_id.uuid);
        printf("\n");
      }
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
      printf("[GATT] Service discovery complete: status=0x%x\n",
             param->search_cmpl.status);
      if (param->search_cmpl.status != ESP_GATT_OK) {
        closeAndRescan("[GATT] service discovery failed");
      } else {
        handleDiscoveryComplete();
      }
      break;

    case ESP_GATTC_READ_CHAR_EVT:
      g_gatt_op_inflight = false;
      printf("[GATT] READ handle=0x%04x status=0x%x len=%u\n",
             param->read.handle, param->read.status, param->read.value_len);
      if (param->read.handle == g_dd21 && g_stage == Stage::ReadConfig) {
        if (param->read.status == ESP_GATT_OK) {
          printf("[GEO] DD21 diagnostic:");
          for (uint16_t i = 0; i < param->read.value_len; ++i) {
            printf(" %02x", param->read.value[i]);
          }
          printf("\n");
        } else {
          printf("[GEO] DD21 diagnostic read failed status=0x%x -- ignored in v5.\n",
                 param->read.status);
        }
        markLocationReady();
      }
      break;

    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.handle == g_dd11) {
        g_tx_inflight = false;
        printf("[TX] lat=%.6f lon=%.6f packet=95 bytes status=0x%x -> %s\n",
               FAKE_LAT, FAKE_LON, param->write.status,
               param->write.status == ESP_GATT_OK ? "OK" : "FAIL");
        if (param->write.status == ESP_GATT_INSUF_AUTHENTICATION ||
            param->write.status == ESP_GATT_INSUF_ENCRYPTION) {
          closeAndRescan("[TX] Sony says DD11 link security is insufficient", 3000);
        }
      }
      break;

    case ESP_GATTC_ENC_CMPL_CB_EVT:
      printf("[PAIR] GATT encryption complete callback. local bond=%d\n",
             isCameraBonded() ? 1 : 0);
      // On reconnect with an existing key, Bluedroid may report encryption
      // without producing a fresh AUTH_CMPL event. Continue only when the
      // local bond database confirms the peer.
      if (isCameraBonded()) beginPostBond();
      break;

    case ESP_GATTC_DISCONNECT_EVT:
      printf("[BLE] Disconnected from ");
      printAddr(param->disconnect.remote_bda);
      printf(" reason=0x%02x\n", param->disconnect.reason);
      resetConnectionState();
      g_stage = Stage::Boot;
      scheduleRescan(2200);
      break;

    default:
      break;
  }
}

void printBondedDevices() {
  const int n = esp_ble_get_bond_device_num();
  printf("[PAIR] Bluedroid bond database: %d device(s)\n", n);
  if (n <= 0) return;

  auto* list = static_cast<esp_ble_bond_dev_t*>(
      calloc(static_cast<size_t>(n), sizeof(esp_ble_bond_dev_t)));
  if (!list) return;

  int count = n;
  if (esp_ble_get_bond_device_list(&count, list) == ESP_OK) {
    for (int i = 0; i < count; ++i) {
      printf("[PAIR] bonded[%d]=", i);
      printAddr(list[i].bd_addr);
      printf("\n");
    }
  }
  free(list);
}

void applySecurityProfile() {
  esp_ble_auth_req_t auth_req =
      g_security_profile == SecurityProfile::LegacyBond
          ? ESP_LE_AUTH_BOND
          : ESP_LE_AUTH_REQ_SC_BOND;
  esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
  uint8_t key_size = 16;
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t oob_support = ESP_BLE_OOB_DISABLE;
  uint8_t only_accept = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_DISABLE;

  ESP_ERROR_CHECK(esp_ble_gap_set_security_param(
      ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req)));
  ESP_ERROR_CHECK(esp_ble_gap_set_security_param(
      ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap)));
  ESP_ERROR_CHECK(esp_ble_gap_set_security_param(
      ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)));
  ESP_ERROR_CHECK(esp_ble_gap_set_security_param(
      ESP_BLE_SM_OOB_SUPPORT, &oob_support, sizeof(oob_support)));
  ESP_ERROR_CHECK(esp_ble_gap_set_security_param(
      ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key)));
  ESP_ERROR_CHECK(esp_ble_gap_set_security_param(
      ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key)));
  ESP_ERROR_CHECK(esp_ble_gap_set_security_param(
      ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &only_accept, sizeof(only_accept)));

  printf("[PAIR] Security profile applied: %s auth_req=0x%02x IO=NONE key=16 ENC+ID only_accept=DISABLED\n",
         g_security_profile == SecurityProfile::LegacyBond ?
           "A:GATTS+LEGACY_BOND" : "B:GATTS+SC_CAPABLE_BOND",
         static_cast<unsigned>(auth_req));
  if (g_security_profile == SecurityProfile::ScCapableBond) {
    printf("[PAIR] SC capability advertised; SC-only enforcement remains disabled, so legacy fallback is allowed.\n");
  }
}

bool initBluetooth() {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  if (ret != ESP_OK) {
    printf("[FATAL] NVS init failed: %s\n", esp_err_to_name(ret));
    return false;
  }

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ret = esp_bt_controller_init(&bt_cfg);
  if (ret != ESP_OK) {
    printf("[FATAL] controller init failed: %s\n", esp_err_to_name(ret));
    return false;
  }
  ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (ret != ESP_OK) {
    printf("[FATAL] controller enable failed: %s\n", esp_err_to_name(ret));
    return false;
  }
  ret = esp_bluedroid_init();
  if (ret != ESP_OK) {
    printf("[FATAL] Bluedroid init failed: %s\n", esp_err_to_name(ret));
    return false;
  }
  ret = esp_bluedroid_enable();
  if (ret != ESP_OK) {
    printf("[FATAL] Bluedroid enable failed: %s\n", esp_err_to_name(ret));
    return false;
  }

  ESP_ERROR_CHECK(esp_ble_gap_register_callback(gapCallback));
  ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattcCallback));
  ESP_ERROR_CHECK(esp_ble_gap_set_device_name("SonyGPS-C6-v6"));

  // v6 A/B starts with profile A; profile B is selected automatically after a failed attempt.
  applySecurityProfile();

  // Python reference exchanges MTU 158. The A7R III previously negotiated 128
  // successfully, so request 158 locally and accept whatever the camera returns.
  ret = esp_ble_gatt_set_local_mtu(158);
  if (ret != ESP_OK) {
    printf("[BLE] Warning: set local MTU 158 failed: %s\n", esp_err_to_name(ret));
  }

  printBondedDevices();
  printf("[PAIR] v6 A/B ready: A=legacy BOND, B=SC-capable BOND; both IO=NONE; GATTS enabled.\n");

  ret = esp_ble_gattc_app_register(APP_ID);
  if (ret != ESP_OK) {
    printf("[FATAL] GATTC app register failed: %s\n", esp_err_to_name(ret));
    return false;
  }
  return true;
}

} // namespace

extern "C" void app_main(void) {
  printf("\n=== Sony A7R III fake GPS BLE PoC v6 / SMP A-B / XIAO ESP32-C6 / BLUEDROID ===\n");
  printf("Reference flow: connect -> SMP A/B -> bond -> MTU -> DD00 -> DD11 (95 bytes)\n");
  printf("EE01/DD30/DD31 are intentionally NOT used in this build.\n");
  printf("Fake coordinate: McMurdo Station, Antarctica %.6f, %.6f\n", FAKE_LAT, FAKE_LON);
  printf("Device name: SonyGPS-C6-v6\n");

  if (!initBluetooth()) return;

  for (;;) {
    const uint64_t now = esp_timer_get_time();

    if (g_ready && g_connected && now - g_last_tx_us >= 1000000ULL) {
      if (!g_tx_inflight && !g_gatt_op_inflight) {
        g_last_tx_us = now;
        sendFakeLocation();
      }
    }

    if (!g_connected && g_rescan_due_us != 0 && now >= g_rescan_due_us &&
        g_stage != Stage::Scanning && g_stage != Stage::Connecting) {
      startScan();
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
