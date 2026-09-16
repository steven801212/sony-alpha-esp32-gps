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

// Sony Alpha ESP32 GPS multi-camera development firmware.
// Seeed XIAO ESP32-C6 / ESP-IDF Bluedroid.
//
// This branch refactors the hardware-validated v7 single-camera state machine
// into independent per-camera sessions. The GNSS/location source remains
// shared: one fix can be transmitted to up to MAX_CAMERAS Sony bodies.
//
// v7 remains the hardware-validated release. This development branch must not
// be described as dual-camera verified until it is tested with two cameras
// connected at the same time.

namespace {

// Synthetic/public test fix near Taipei 101. This is intentionally unchanged
// from v7 so single-camera regression testing can compare identical metadata.
constexpr int32_t TEST_LAT_E7 = 250339687;
constexpr int32_t TEST_LON_E7 = 1215644687;
constexpr time_t BASE_UTC_EPOCH = 1789516800; // 2026-09-16 00:00:00 UTC

constexpr size_t MAX_CAMERAS = 2;
constexpr uint16_t INVALID_CONN_ID = 0xffff;
constexpr uint64_t LOCATION_UPDATE_INTERVAL_US = 5000000ULL; // 5 seconds
constexpr uint64_t INTER_CAMERA_TX_GAP_US = 30000ULL;         // 30 ms
constexpr bool ENABLE_CAMERA_TIME_SYNC = false;

constexpr uint16_t SONY_COMPANY_ID = 0x012d;
constexpr uint16_t SONY_CAMERA_TYPE = 0x0003;
constexpr uint16_t APP_ID = 0;
constexpr uint16_t UUID_DD11 = 0xdd11;
constexpr uint16_t UUID_DD21 = 0xdd21;
constexpr uint16_t UUID_DD30 = 0xdd30;
constexpr uint16_t UUID_DD31 = 0xdd31;
constexpr uint16_t UUID_CC13 = 0xcc13;

// Canonical UUID: 8000dd00-dd00-ffff-ffff-ffffffffffff
constexpr uint8_t GEO_SVC_UUID_LE[16] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x00, 0xdd, 0x00, 0xdd, 0x00, 0x80,
};
constexpr uint8_t GEO_SVC_UUID_BE[16] = {
    0x80, 0x00, 0xdd, 0x00, 0xdd, 0x00, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

// Canonical UUID: 8000cc00-cc00-ffff-ffff-ffffffffffff
constexpr uint8_t CONTROL_SVC_UUID_LE[16] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x00, 0xcc, 0x00, 0xcc, 0x00, 0x80,
};
constexpr uint8_t CONTROL_SVC_UUID_BE[16] = {
    0x80, 0x00, 0xcc, 0x00, 0xcc, 0x00, 0xff, 0xff,
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
  uint16_t dst_offset;
};
static_assert(sizeof(SonyGeo95) == 95, "Sony GPS packet must be 95 bytes");

enum class Stage : uint8_t {
  Boot,
  Connecting,
  Bonding,
  Mtu,
  Discovering,
  ReadConfig,
  EnableUnlock,
  EnableLock,
  SyncTime,
  Ready,
  Closing,
};

struct CameraSession {
  bool allocated = false;
  bool connected = false;
  bool post_bond_started = false;
  bool ready = false;
  bool gatt_op_inflight = false;
  bool tx_inflight = false;

  Stage stage = Stage::Boot;
  esp_bd_addr_t bda = {};
  esp_ble_addr_type_t addr_type = BLE_ADDR_TYPE_PUBLIC;
  uint16_t conn_id = INVALID_CONN_ID;

  uint8_t protocol_version = 0;
  uint8_t mode22 = 0;
  uint16_t model = 0;
  char name[40] = {};

  uint32_t pair_attempt = 0;

  uint16_t service_start = 0;
  uint16_t service_end = 0;
  uint16_t control_service_start = 0;
  uint16_t control_service_end = 0;
  uint16_t dd11 = 0;
  uint16_t dd21 = 0;
  uint16_t dd30 = 0;
  uint16_t dd31 = 0;
  uint16_t cc13 = 0;

  bool send_timezone_dst = true;
  uint64_t retry_due_us = 0;
  uint64_t last_tx_us = 0;
};

CameraSession g_cameras[MAX_CAMERAS];

esp_gatt_if_t g_gattc_if = ESP_GATT_IF_NONE;
esp_ble_scan_params_t g_scan_params{};

bool g_scan_active = false;
int g_opening_slot = -1;
uint64_t g_scan_due_us = 0;

int g_global_tx_slot = -1;
uint64_t g_next_tx_allowed_us = 0;
size_t g_tx_round_robin = 0;

void startScan();
void scheduleScan(uint32_t delay_ms = 0);
void beginPostBond(size_t slot);
void beginSonyFeatureHandshake(size_t slot);
void sendTimeSyncOrReady(size_t slot);
void markLocationReady(size_t slot);
void closeAndRetry(size_t slot, const char* why, uint32_t delay_ms = 2500);
void applySecurityProfile();

void printAddr(const esp_bd_addr_t bda) {
  printf("%02X:%02X:%02X:%02X:%02X:%02X",
         bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

void printCameraPrefix(size_t slot, const char* domain) {
  printf("[C%u][%s] ", static_cast<unsigned>(slot + 1), domain);
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

bool uuid128Matches(const esp_bt_uuid_t& uuid, const uint8_t* le, const uint8_t* be) {
  if (uuid.len != ESP_UUID_LEN_128) return false;
  return memcmp(uuid.uuid.uuid128, le, 16) == 0 ||
         memcmp(uuid.uuid.uuid128, be, 16) == 0;
}

bool isGeoServiceUuid(const esp_bt_uuid_t& uuid) {
  return uuid128Matches(uuid, GEO_SVC_UUID_LE, GEO_SVC_UUID_BE);
}

bool isControlServiceUuid(const esp_bt_uuid_t& uuid) {
  return uuid128Matches(uuid, CONTROL_SVC_UUID_LE, CONTROL_SVC_UUID_BE);
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

int findSessionByBda(const esp_bd_addr_t bda) {
  for (size_t i = 0; i < MAX_CAMERAS; ++i) {
    if (g_cameras[i].allocated &&
        memcmp(g_cameras[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int findSessionByConnId(uint16_t conn_id) {
  if (conn_id == INVALID_CONN_ID) return -1;
  for (size_t i = 0; i < MAX_CAMERAS; ++i) {
    if (g_cameras[i].allocated && g_cameras[i].connected &&
        g_cameras[i].conn_id == conn_id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int findFreeSlot() {
  for (size_t i = 0; i < MAX_CAMERAS; ++i) {
    if (!g_cameras[i].allocated) return static_cast<int>(i);
  }
  return -1;
}

size_t connectedCameraCount() {
  size_t count = 0;
  for (size_t i = 0; i < MAX_CAMERAS; ++i) {
    if (g_cameras[i].allocated && g_cameras[i].connected) ++count;
  }
  return count;
}

bool scanStillUseful() {
  if (connectedCameraCount() < MAX_CAMERAS) return true;
  for (size_t i = 0; i < MAX_CAMERAS; ++i) {
    if (g_cameras[i].allocated && !g_cameras[i].connected) return true;
  }
  return false;
}

bool isCameraBonded(const CameraSession& camera) {
  if (!camera.allocated) return false;

  const int n = esp_ble_get_bond_device_num();
  if (n <= 0) return false;

  auto* list = static_cast<esp_ble_bond_dev_t*>(
      calloc(static_cast<size_t>(n), sizeof(esp_ble_bond_dev_t)));
  if (!list) return false;

  int count = n;
  bool found = false;
  if (esp_ble_get_bond_device_list(&count, list) == ESP_OK) {
    for (int i = 0; i < count; ++i) {
      if (memcmp(list[i].bd_addr, camera.bda, ESP_BD_ADDR_LEN) == 0) {
        found = true;
        break;
      }
    }
  }
  free(list);
  return found;
}

bool resolveChar(const CameraSession& camera,
                 uint16_t service_start, uint16_t service_end,
                 uint16_t uuid16, uint16_t* out_handle) {
  if (!out_handle || g_gattc_if == ESP_GATT_IF_NONE || !camera.connected ||
      camera.conn_id == INVALID_CONN_ID || service_start == 0 || service_end == 0) {
    return false;
  }

  esp_bt_uuid_t uuid{};
  uuid.len = ESP_UUID_LEN_16;
  uuid.uuid.uuid16 = uuid16;

  esp_gattc_char_elem_t elem{};
  uint16_t count = 1;
  const esp_gatt_status_t st = esp_ble_gattc_get_char_by_uuid(
      g_gattc_if, camera.conn_id, service_start, service_end,
      uuid, &elem, &count);
  if (st != ESP_GATT_OK || count == 0) {
    *out_handle = 0;
    return false;
  }
  *out_handle = elem.char_handle;
  return true;
}

enum class DstRule : uint8_t { None, Us, Eu, Nz, Aus };

struct TimeZoneInfo {
  const char* name;
  int16_t standard_offset_minutes;
  int16_t dst_offset_minutes;
  bool approximate;
};

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const int adjusted_month = static_cast<int>(month) + (month > 2 ? -3 : 9);
  const unsigned doy =
      (153u * static_cast<unsigned>(adjusted_month) + 2u) / 5u + day - 1u;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

time_t makeUtcEpoch(int year, unsigned month, unsigned day,
                    unsigned hour, unsigned minute, unsigned second) {
  return static_cast<time_t>(daysFromCivil(year, month, day) * 86400LL +
                             static_cast<int64_t>(hour) * 3600 +
                             static_cast<int64_t>(minute) * 60 + second);
}

int weekdaySundayZero(int year, unsigned month, unsigned day) {
  int64_t w = (daysFromCivil(year, month, day) + 4) % 7;
  if (w < 0) w += 7;
  return static_cast<int>(w);
}

unsigned daysInMonth(int year, unsigned month) {
  static const uint8_t kDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return kDays[month - 1];
}

unsigned nthSunday(int year, unsigned month, unsigned nth) {
  const int first = weekdaySundayZero(year, month, 1);
  return 1u + static_cast<unsigned>((7 - first) % 7) + (nth - 1u) * 7u;
}

unsigned lastSunday(int year, unsigned month) {
  const unsigned last = daysInMonth(year, month);
  const int w = weekdaySundayZero(year, month, last);
  return last - static_cast<unsigned>(w);
}

bool dstActive(DstRule rule, int16_t standard_offset_minutes, time_t utc_epoch) {
  if (rule == DstRule::None) return false;

  struct tm utc{};
  gmtime_r(&utc_epoch, &utc);
  const int y = utc.tm_year + 1900;

  if (rule == DstRule::Us) {
    const unsigned start_day = nthSunday(y, 3, 2);
    const unsigned end_day = nthSunday(y, 11, 1);
    const time_t start = makeUtcEpoch(y, 3, start_day, 2, 0, 0) -
                         static_cast<time_t>(standard_offset_minutes) * 60;
    const time_t end = makeUtcEpoch(y, 11, end_day, 2, 0, 0) -
                       static_cast<time_t>(standard_offset_minutes + 60) * 60;
    return utc_epoch >= start && utc_epoch < end;
  }

  if (rule == DstRule::Eu) {
    const time_t start = makeUtcEpoch(y, 3, lastSunday(y, 3), 1, 0, 0);
    const time_t end = makeUtcEpoch(y, 10, lastSunday(y, 10), 1, 0, 0);
    return utc_epoch >= start && utc_epoch < end;
  }

  if (rule == DstRule::Nz) {
    const time_t end = makeUtcEpoch(y, 4, nthSunday(y, 4, 1), 3, 0, 0) -
                       static_cast<time_t>(standard_offset_minutes + 60) * 60;
    const time_t start = makeUtcEpoch(y, 9, lastSunday(y, 9), 2, 0, 0) -
                         static_cast<time_t>(standard_offset_minutes) * 60;
    return utc_epoch < end || utc_epoch >= start;
  }

  if (rule == DstRule::Aus) {
    const time_t end = makeUtcEpoch(y, 4, nthSunday(y, 4, 1), 3, 0, 0) -
                       static_cast<time_t>(standard_offset_minutes + 60) * 60;
    const time_t start = makeUtcEpoch(y, 10, nthSunday(y, 10, 1), 2, 0, 0) -
                         static_cast<time_t>(standard_offset_minutes) * 60;
    return utc_epoch < end || utc_epoch >= start;
  }

  return false;
}

TimeZoneInfo makeTz(const char* name, int16_t standard, DstRule rule,
                    time_t utc_epoch, bool approximate = false) {
  return {name, standard,
          static_cast<int16_t>(dstActive(rule, standard, utc_epoch) ? 60 : 0),
          approximate};
}

bool inside(double lat, double lon, double south, double north,
            double west, double east) {
  return lat >= south && lat <= north && lon >= west && lon <= east;
}

TimeZoneInfo resolveTimeZone(double lat, double lon, time_t utc_epoch) {
  if (lat < -60.0 && lon >= 155.0 && lon <= 180.0)
    return makeTz("Antarctica/McMurdo", 720, DstRule::Nz, utc_epoch);

  if (inside(lat, lon, 21.5, 25.6, 119.0, 122.5))
    return makeTz("Asia/Taipei", 480, DstRule::None, utc_epoch);
  if (inside(lat, lon, 22.0, 23.0, 113.6, 114.6))
    return makeTz("Asia/Hong_Kong", 480, DstRule::None, utc_epoch);
  if (inside(lat, lon, 24.0, 46.5, 122.0, 146.5))
    return makeTz("Asia/Tokyo-Seoul", 540, DstRule::None, utc_epoch, true);
  if (inside(lat, lon, 18.0, 54.5, 73.0, 135.0))
    return makeTz("Asia/Shanghai", 480, DstRule::None, utc_epoch, true);
  if (inside(lat, lon, 0.5, 7.8, 99.0, 120.0))
    return makeTz("Asia/Singapore-Kuala_Lumpur", 480, DstRule::None, utc_epoch, true);
  if (inside(lat, lon, 5.0, 24.0, 97.0, 109.5))
    return makeTz("SE-Asia-UTC+7", 420, DstRule::None, utc_epoch, true);
  if (inside(lat, lon, 26.0, 31.0, 80.0, 89.0))
    return makeTz("Asia/Kathmandu", 345, DstRule::None, utc_epoch, true);
  if (inside(lat, lon, 6.0, 37.5, 68.0, 97.5))
    return makeTz("Asia/Kolkata", 330, DstRule::None, utc_epoch, true);

  if (inside(lat, lon, -48.5, -33.0, 165.0, 180.0))
    return makeTz("Pacific/Auckland", 720, DstRule::Nz, utc_epoch);

  if (inside(lat, lon, -44.5, -10.0, 112.0, 129.0))
    return makeTz("Australia/Perth", 480, DstRule::None, utc_epoch, true);
  if (inside(lat, lon, -26.5, -10.0, 129.0, 138.5))
    return makeTz("Australia/Darwin", 570, DstRule::None, utc_epoch, true);
  if (inside(lat, lon, -29.5, -10.0, 138.0, 154.0))
    return makeTz("Australia/Brisbane", 600, DstRule::None, utc_epoch, true);
  if (inside(lat, lon, -39.5, -25.5, 129.0, 141.5))
    return makeTz("Australia/Adelaide", 570, DstRule::Aus, utc_epoch, true);
  if (inside(lat, lon, -44.5, -27.0, 140.5, 154.5))
    return makeTz("Australia/Sydney", 600, DstRule::Aus, utc_epoch, true);

  if (inside(lat, lon, 49.0, 61.5, -11.0, 2.5))
    return makeTz("Europe/London-Dublin", 0, DstRule::Eu, utc_epoch, true);
  if (inside(lat, lon, 36.0, 43.0, -10.0, -6.0))
    return makeTz("Europe/Lisbon", 0, DstRule::Eu, utc_epoch, true);
  if (inside(lat, lon, 35.0, 43.0, 26.0, 45.0))
    return makeTz("Europe/Istanbul", 180, DstRule::None, utc_epoch, true);
  if (inside(lat, lon, 35.0, 71.5, -6.0, 22.5))
    return makeTz("Europe/Central", 60, DstRule::Eu, utc_epoch, true);
  if (inside(lat, lon, 34.0, 71.5, 22.5, 40.0))
    return makeTz("Europe/Eastern", 120, DstRule::Eu, utc_epoch, true);

  if (inside(lat, lon, 18.5, 22.5, -161.0, -154.0))
    return makeTz("Pacific/Honolulu", -600, DstRule::None, utc_epoch);
  if (inside(lat, lon, 51.0, 72.0, -170.0, -129.0))
    return makeTz("America/Anchorage", -540, DstRule::Us, utc_epoch, true);
  if (inside(lat, lon, 31.0, 37.5, -115.0, -109.0))
    return makeTz("America/Phoenix", -420, DstRule::None, utc_epoch, true);
  if (inside(lat, lon, 24.0, 50.0, -125.0, -66.0)) {
    if (lon < -114.0) return makeTz("US/Pacific", -480, DstRule::Us, utc_epoch, true);
    if (lon < -101.0) return makeTz("US/Mountain", -420, DstRule::Us, utc_epoch, true);
    if (lon < -86.0) return makeTz("US/Central", -360, DstRule::Us, utc_epoch, true);
    return makeTz("US/Eastern", -300, DstRule::Us, utc_epoch, true);
  }

  int offset_hours = static_cast<int>((lon >= 0.0 ? lon + 7.5 : lon - 7.5) / 15.0);
  if (offset_hours < -12) offset_hours = -12;
  if (offset_hours > 14) offset_hours = 14;
  return makeTz("Etc/LongitudeApprox", static_cast<int16_t>(offset_hours * 60),
                DstRule::None, utc_epoch, true);
}

time_t currentTestUtc() {
  return BASE_UTC_EPOCH + static_cast<time_t>(esp_timer_get_time() / 1000000ULL);
}

TimeZoneInfo currentTimeZone(time_t utc_epoch) {
  return resolveTimeZone(static_cast<double>(TEST_LAT_E7) / 1.0E7,
                         static_cast<double>(TEST_LON_E7) / 1.0E7,
                         utc_epoch);
}

SonyGeo95 makeGeoPacket(bool send_timezone_dst,
                        time_t utc_epoch, const TimeZoneInfo& tz) {
  SonyGeo95 geo{};
  const uint8_t prefix95[11] = {
      0x00, 0x5d, 0x08, 0x02, 0xfc, 0x03, 0x00, 0x00, 0x10, 0x10, 0x10};
  const uint8_t prefix91[11] = {
      0x00, 0x59, 0x08, 0x02, 0xfc, 0x00, 0x00, 0x00, 0x10, 0x10, 0x10};
  memcpy(geo.prefix, send_timezone_dst ? prefix95 : prefix91, sizeof(geo.prefix));

  geo.latitude = static_cast<int32_t>(
      __builtin_bswap32(static_cast<uint32_t>(TEST_LAT_E7)));
  geo.longitude = static_cast<int32_t>(
      __builtin_bswap32(static_cast<uint32_t>(TEST_LON_E7)));

  struct tm utc{};
  gmtime_r(&utc_epoch, &utc);
  geo.year = __builtin_bswap16(static_cast<uint16_t>(utc.tm_year + 1900));
  geo.month = static_cast<uint8_t>(utc.tm_mon + 1);
  geo.day = static_cast<uint8_t>(utc.tm_mday);
  geo.hour = static_cast<uint8_t>(utc.tm_hour);
  geo.minute = static_cast<uint8_t>(utc.tm_min);
  geo.second = static_cast<uint8_t>(utc.tm_sec);

  geo.timezone_offset = __builtin_bswap16(
      static_cast<uint16_t>(static_cast<int16_t>(tz.standard_offset_minutes)));
  geo.dst_offset = __builtin_bswap16(
      static_cast<uint16_t>(static_cast<int16_t>(tz.dst_offset_minutes)));
  return geo;
}

void buildTimeSyncPacket(uint8_t out[13], time_t utc_epoch,
                         const TimeZoneInfo& tz) {
  memset(out, 0, 13);
  const int32_t total_offset_seconds =
      static_cast<int32_t>(tz.standard_offset_minutes + tz.dst_offset_minutes) * 60;
  const time_t local_epoch = utc_epoch + total_offset_seconds;
  struct tm local{};
  gmtime_r(&local_epoch, &local);

  const uint16_t year = static_cast<uint16_t>(local.tm_year + 1900);
  const int standard = tz.standard_offset_minutes;
  const int abs_standard = standard < 0 ? -standard : standard;
  int hours = abs_standard / 60;
  if (standard < 0) hours = -hours;

  out[0] = 12;
  out[1] = 0;
  out[2] = 0;
  out[3] = static_cast<uint8_t>(year >> 8);
  out[4] = static_cast<uint8_t>(year & 0xff);
  out[5] = static_cast<uint8_t>(local.tm_mon + 1);
  out[6] = static_cast<uint8_t>(local.tm_mday);
  out[7] = static_cast<uint8_t>(local.tm_hour);
  out[8] = static_cast<uint8_t>(local.tm_min);
  out[9] = static_cast<uint8_t>(local.tm_sec);
  out[10] = tz.dst_offset_minutes != 0 ? 1 : 0;
  out[11] = static_cast<uint8_t>(static_cast<int8_t>(hours));
  out[12] = static_cast<uint8_t>(abs_standard % 60);
}

void resetConnectionRuntime(size_t slot) {
  CameraSession& camera = g_cameras[slot];

  if (g_global_tx_slot == static_cast<int>(slot)) {
    g_global_tx_slot = -1;
    g_next_tx_allowed_us = esp_timer_get_time() + INTER_CAMERA_TX_GAP_US;
  }
  if (g_opening_slot == static_cast<int>(slot)) {
    g_opening_slot = -1;
  }

  camera.connected = false;
  camera.post_bond_started = false;
  camera.ready = false;
  camera.gatt_op_inflight = false;
  camera.tx_inflight = false;
  camera.stage = Stage::Boot;
  camera.conn_id = INVALID_CONN_ID;

  camera.service_start = 0;
  camera.service_end = 0;
  camera.control_service_start = 0;
  camera.control_service_end = 0;
  camera.dd11 = 0;
  camera.dd21 = 0;
  camera.dd30 = 0;
  camera.dd31 = 0;
  camera.cc13 = 0;
  camera.send_timezone_dst = true;
  camera.last_tx_us = 0;
}

void scheduleScan(uint32_t delay_ms) {
  if (!scanStillUseful()) {
    g_scan_due_us = 0;
    return;
  }

  const uint64_t due = esp_timer_get_time() +
                       static_cast<uint64_t>(delay_ms) * 1000ULL;
  if (g_scan_due_us == 0 || due < g_scan_due_us) g_scan_due_us = due;
}

void closeAndRetry(size_t slot, const char* why, uint32_t delay_ms) {
  CameraSession& camera = g_cameras[slot];
  printCameraPrefix(slot, "BLE");
  printf("%s\n", why ? why : "Closing connection");

  camera.ready = false;
  camera.stage = Stage::Closing;
  camera.retry_due_us =
      esp_timer_get_time() + static_cast<uint64_t>(delay_ms) * 1000ULL;

  if (camera.connected && camera.conn_id != INVALID_CONN_ID &&
      g_gattc_if != ESP_GATT_IF_NONE) {
    const esp_err_t err = esp_ble_gattc_close(g_gattc_if, camera.conn_id);
    if (err != ESP_OK) {
      printCameraPrefix(slot, "BLE");
      printf("esp_ble_gattc_close failed: %s\n", esp_err_to_name(err));
      resetConnectionRuntime(slot);
      scheduleScan(delay_ms);
    }
  } else {
    resetConnectionRuntime(slot);
    scheduleScan(delay_ms);
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
    scheduleScan(2000);
  }
}

void startScan() {
  if (!scanStillUseful() || g_scan_active || g_opening_slot >= 0) return;

  const uint64_t now = esp_timer_get_time();

  // If all known disconnected cameras are in retry backoff and no free slot
  // exists, wait for the earliest backoff instead of spinning the scanner.
  bool can_find_something_now = findFreeSlot() >= 0;
  uint64_t earliest_retry = 0;
  for (size_t i = 0; i < MAX_CAMERAS; ++i) {
    const CameraSession& camera = g_cameras[i];
    if (camera.allocated && !camera.connected) {
      if (camera.retry_due_us == 0 || now >= camera.retry_due_us) {
        can_find_something_now = true;
      } else if (earliest_retry == 0 || camera.retry_due_us < earliest_retry) {
        earliest_retry = camera.retry_due_us;
      }
    }
  }
  if (!can_find_something_now) {
    g_scan_due_us = earliest_retry;
    return;
  }

  g_scan_due_us = 0;
  const esp_err_t err = esp_ble_gap_start_scanning(15);
  if (err == ESP_OK) {
    g_scan_active = true;
    printf("[SCAN] Starting multi-camera discovery (%u/%u connected).\n",
           static_cast<unsigned>(connectedCameraCount()),
           static_cast<unsigned>(MAX_CAMERAS));
  } else {
    printf("[SCAN] start failed: %s\n", esp_err_to_name(err));
    scheduleScan(2000);
  }
}

void populateSessionFromAdvertisement(size_t slot,
                                      const esp_ble_gap_cb_param_t::ble_scan_result_evt_param& r,
                                      const SonyAdv& adv,
                                      const char* name) {
  CameraSession& camera = g_cameras[slot];
  if (!camera.allocated) {
    camera.allocated = true;
    memcpy(camera.bda, r.bda, ESP_BD_ADDR_LEN);
  }
  camera.addr_type = r.ble_addr_type;
  camera.protocol_version = adv.protocol_version;
  camera.mode22 = adv.mode22;
  camera.model = adv.model;
  if (name) {
    strncpy(camera.name, name, sizeof(camera.name) - 1);
    camera.name[sizeof(camera.name) - 1] = '\0';
  }
}

void openCamera(size_t slot,
                const esp_ble_gap_cb_param_t::ble_scan_result_evt_param& r) {
  CameraSession& camera = g_cameras[slot];
  camera.stage = Stage::Connecting;
  g_opening_slot = static_cast<int>(slot);

  if (g_scan_active) {
    esp_ble_gap_stop_scanning();
  }

  printCameraPrefix(slot, "BLE");
  printf("Connecting to ");
  printAddr(camera.bda);
  printf(" using Bluedroid\n");

  esp_ble_gatt_creat_conn_params_t cp{};
  memcpy(cp.remote_bda, camera.bda, ESP_BD_ADDR_LEN);
  cp.remote_addr_type = camera.addr_type;
  cp.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
  cp.is_direct = true;
  cp.is_aux = false;
  cp.phy_mask = 0;

  const esp_err_t err = esp_ble_gattc_enh_open(g_gattc_if, &cp);
  if (err != ESP_OK) {
    printCameraPrefix(slot, "BLE");
    printf("open failed immediately: %s\n", esp_err_to_name(err));
    g_opening_slot = -1;
    resetConnectionRuntime(slot);
    camera.retry_due_us = esp_timer_get_time() + 2500000ULL;
    scheduleScan(2500);
  }
}

void startBondNow(size_t slot) {
  CameraSession& camera = g_cameras[slot];
  if (!camera.connected) return;

  camera.stage = Stage::Bonding;
  ++camera.pair_attempt;

  printCameraPrefix(slot, "PAIR");
  printf("BOND-FIRST multi-camera attempt #%" PRIu32
         " profile=GATTS+SC_CAPABLE_BOND\n", camera.pair_attempt);
  printCameraPrefix(slot, "PAIR");
  printf("local bond before request=%d\n", isCameraBonded(camera) ? 1 : 0);

  const esp_err_t err =
      esp_ble_set_encryption(camera.bda, ESP_BLE_SEC_ENCRYPT);
  printCameraPrefix(slot, "PAIR");
  printf("esp_ble_set_encryption(ESP_BLE_SEC_ENCRYPT) -> %s\n",
         esp_err_to_name(err));
  if (err != ESP_OK) {
    closeAndRetry(slot, "[PAIR] could not start bond/encryption", 3000);
  }
}

void beginPostBond(size_t slot) {
  CameraSession& camera = g_cameras[slot];
  if (!camera.connected || camera.post_bond_started) return;

  camera.post_bond_started = true;
  camera.stage = Stage::Mtu;

  printCameraPrefix(slot, "PAIR");
  printf("Bond/encryption established. local bond now=%d\n",
         isCameraBonded(camera) ? 1 : 0);
  printCameraPrefix(slot, "GATT");
  printf("Requesting MTU after bond.\n");

  const esp_err_t err =
      esp_ble_gattc_send_mtu_req(g_gattc_if, camera.conn_id);
  if (err != ESP_OK) {
    printCameraPrefix(slot, "GATT");
    printf("MTU request submit failed: %s; continuing to service discovery.\n",
           esp_err_to_name(err));
    camera.stage = Stage::Discovering;
    esp_ble_gattc_search_service(g_gattc_if, camera.conn_id, nullptr);
  }
}

void readDd21Config(size_t slot) {
  CameraSession& camera = g_cameras[slot];
  if (!camera.connected || camera.dd21 == 0 || camera.gatt_op_inflight) {
    beginSonyFeatureHandshake(slot);
    return;
  }

  camera.stage = Stage::ReadConfig;
  camera.gatt_op_inflight = true;
  printCameraPrefix(slot, "GEO");
  printf("Reading DD21 location configuration.\n");

  const esp_err_t err = esp_ble_gattc_read_char(
      g_gattc_if, camera.conn_id, camera.dd21, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    camera.gatt_op_inflight = false;
    printCameraPrefix(slot, "GEO");
    printf("DD21 read submit failed: %s; keeping 95-byte default.\n",
           esp_err_to_name(err));
    beginSonyFeatureHandshake(slot);
  }
}

bool submitHandshakeWrite(size_t slot, uint16_t handle,
                          const uint8_t* data, uint16_t len,
                          Stage stage, const char* label) {
  CameraSession& camera = g_cameras[slot];
  if (!camera.connected || handle == 0 || camera.gatt_op_inflight) return false;

  camera.stage = stage;
  camera.gatt_op_inflight = true;
  const esp_err_t err = esp_ble_gattc_write_char(
      g_gattc_if, camera.conn_id, handle, len, const_cast<uint8_t*>(data),
      ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    camera.gatt_op_inflight = false;
    printCameraPrefix(slot, "GATT");
    printf("%s submit failed: %s\n", label, esp_err_to_name(err));
    return false;
  }

  printCameraPrefix(slot, "GATT");
  printf("%s submitted to handle 0x%04x.\n", label, handle);
  return true;
}

void sendTimeSyncOrReady(size_t slot) {
  CameraSession& camera = g_cameras[slot];
  if (!ENABLE_CAMERA_TIME_SYNC || camera.cc13 == 0) {
    if (ENABLE_CAMERA_TIME_SYNC && camera.cc13 == 0) {
      printCameraPrefix(slot, "TIME");
      printf("CC13 not exposed; camera clock sync skipped.\n");
    }
    markLocationReady(slot);
    return;
  }

  const time_t utc = currentTestUtc();
  const TimeZoneInfo tz = currentTimeZone(utc);
  uint8_t packet[13]{};
  buildTimeSyncPacket(packet, utc, tz);

  printCameraPrefix(slot, "TIME");
  printf("timezone=%s standard=%dmin dst=%dmin%s\n",
         tz.name, tz.standard_offset_minutes, tz.dst_offset_minutes,
         tz.approximate ? " (approx boundary resolver)" : "");

  if (!submitHandshakeWrite(slot, camera.cc13, packet, sizeof(packet),
                            Stage::SyncTime, "CC13 camera time sync")) {
    markLocationReady(slot);
  }
}

void beginSonyFeatureHandshake(size_t slot) {
  CameraSession& camera = g_cameras[slot];
  static const uint8_t enable = 0x01;

  if (camera.dd30 != 0) {
    if (submitHandshakeWrite(slot, camera.dd30, &enable, 1,
                             Stage::EnableUnlock,
                             "DD30 GPS enable/unlock")) {
      return;
    }
  }
  if (camera.dd31 != 0) {
    if (submitHandshakeWrite(slot, camera.dd31, &enable, 1,
                             Stage::EnableLock,
                             "DD31 GPS enable/lock")) {
      return;
    }
  }
  sendTimeSyncOrReady(slot);
}

void markLocationReady(size_t slot) {
  CameraSession& camera = g_cameras[slot];
  camera.stage = Stage::Ready;
  camera.ready = true;
  camera.last_tx_us = 0;

  printCameraPrefix(slot, "GEO");
  printf("Sony Location path READY. DD11=0x%04x; packet=%u bytes.\n",
         camera.dd11, camera.send_timezone_dst ? 95u : 91u);

  // Once one camera is stable, discover the next one while keeping this
  // connection alive. Discovery/setup is serialized one camera at a time.
  scheduleScan(250);
}

void sendStaticLocation(size_t slot) {
  CameraSession& camera = g_cameras[slot];
  if (!camera.connected || !camera.ready || camera.dd11 == 0 ||
      camera.tx_inflight || camera.gatt_op_inflight ||
      g_global_tx_slot >= 0) {
    return;
  }

  const time_t utc = currentTestUtc();
  const TimeZoneInfo tz = currentTimeZone(utc);
  SonyGeo95 geo = makeGeoPacket(camera.send_timezone_dst, utc, tz);
  const uint16_t packet_len = camera.send_timezone_dst ? 95 : 91;

  camera.tx_inflight = true;
  g_global_tx_slot = static_cast<int>(slot);

  const esp_err_t err = esp_ble_gattc_write_char(
      g_gattc_if, camera.conn_id, camera.dd11, packet_len,
      reinterpret_cast<uint8_t*>(&geo),
      ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    camera.tx_inflight = false;
    g_global_tx_slot = -1;
    g_next_tx_allowed_us = esp_timer_get_time() + INTER_CAMERA_TX_GAP_US;
    printCameraPrefix(slot, "TX");
    printf("submit failed: %s\n", esp_err_to_name(err));
  }
}

void handleDiscoveryComplete(size_t slot) {
  CameraSession& camera = g_cameras[slot];

  if (camera.service_start == 0 || camera.service_end == 0) {
    closeAndRetry(slot, "[GATT] Sony Location service DD00 not found", 2500);
    return;
  }

  const bool has11 = resolveChar(
      camera, camera.service_start, camera.service_end, UUID_DD11, &camera.dd11);
  const bool has21 = resolveChar(
      camera, camera.service_start, camera.service_end, UUID_DD21, &camera.dd21);
  const bool has30 = resolveChar(
      camera, camera.service_start, camera.service_end, UUID_DD30, &camera.dd30);
  const bool has31 = resolveChar(
      camera, camera.service_start, camera.service_end, UUID_DD31, &camera.dd31);
  const bool has13 = resolveChar(
      camera, camera.control_service_start, camera.control_service_end,
      UUID_CC13, &camera.cc13);

  printCameraPrefix(slot, "GATT");
  printf("DD11=%s(0x%04x) DD21=%s(0x%04x) DD30=%s DD31=%s CC13=%s\n",
         has11 ? "FOUND" : "missing", camera.dd11,
         has21 ? "FOUND" : "missing", camera.dd21,
         has30 ? "FOUND" : "missing",
         has31 ? "FOUND" : "missing",
         has13 ? "FOUND" : "missing");

  if (!has11) {
    closeAndRetry(slot, "[GATT] Required Sony DD11 characteristic missing", 2500);
    return;
  }

  if (has21) {
    camera.send_timezone_dst = true;
    readDd21Config(slot);
  } else {
    camera.send_timezone_dst = false;
    printCameraPrefix(slot, "GEO");
    printf("DD21 absent: using 91-byte packet without timezone/DST fields.\n");
    beginSonyFeatureHandshake(slot);
  }
}

void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
        scheduleScan(0);
      } else {
        printf("[SCAN] parameter setup failed, status=%d\n",
               param->scan_param_cmpl.status);
        scheduleScan(2000);
      }
      break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      printf("[SCAN] start %s (status=%d)\n",
             param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS ? "OK" : "FAIL",
             param->scan_start_cmpl.status);
      if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        g_scan_active = false;
        scheduleScan(2000);
      }
      break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      auto& r = param->scan_rst;

      if (r.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
        const uint16_t total_len =
            static_cast<uint16_t>(r.adv_data_len + r.scan_rsp_len);
        uint8_t mfg_len = 0;
        uint8_t* mfg = esp_ble_resolve_adv_data_by_type(
            r.ble_adv, total_len,
            ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &mfg_len);

        if (mfg && mfg_len >= sizeof(SonyAdv)) {
          SonyAdv adv{};
          memcpy(&adv, mfg, sizeof(adv));
          if (adv.company_id == SONY_COMPANY_ID &&
              adv.type == SONY_CAMERA_TYPE) {
            uint8_t name_len = 0;
            uint8_t* name = esp_ble_resolve_adv_data_by_type(
                r.ble_adv, total_len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
            char name_buf[40] = {};
            if (name && name_len) {
              const size_t n = name_len < sizeof(name_buf) - 1
                                   ? name_len
                                   : sizeof(name_buf) - 1;
              memcpy(name_buf, name, n);
            }

            int slot = findSessionByBda(r.bda);
            if (slot >= 0) {
              CameraSession& camera = g_cameras[slot];
              if (camera.connected ||
                  camera.stage == Stage::Connecting ||
                  camera.stage == Stage::Bonding ||
                  esp_timer_get_time() < camera.retry_due_us) {
                break;
              }
            } else {
              slot = findFreeSlot();
              if (slot < 0) break;
            }

            populateSessionFromAdvertisement(
                static_cast<size_t>(slot), r, adv, name_buf);

            printCameraPrefix(static_cast<size_t>(slot), "SCAN");
            printf("Sony camera found: name='%s' addr=", name_buf);
            printAddr(r.bda);
            printf(" proto=%u(0x%02x) mode22=0x%02x bit0x40=%d "
                   "model=0x%04x RSSI=%d\n",
                   static_cast<unsigned>(adv.protocol_version),
                   adv.protocol_version, adv.mode22,
                   (adv.mode22 & 0x40) ? 1 : 0, adv.model, r.rssi);
            printCameraPrefix(static_cast<size_t>(slot), "SCAN");
            printf("mode22 is logged raw; no single bit is treated as proof of bond state.\n");

            openCamera(static_cast<size_t>(slot), r);
          }
        }
      } else if (r.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
        g_scan_active = false;
        printf("[SCAN] discovery window ended.\n");
        scheduleScan(3000);
      }
      break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
      g_scan_active = false;
      printf("[SCAN] stop status=%d\n", param->scan_stop_cmpl.status);
      break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
      printf("[PAIR] ESP_GAP_BLE_SEC_REQ_EVT -> ACCEPT\n");
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
      printf("[PAIR] PASSKEY_REQ received; NoInputNoOutput configured.\n");
      break;

    case ESP_GAP_BLE_KEY_EVT:
      printf("[PAIR] Key exchanged, type=%d\n",
             param->ble_security.ble_key.key_type);
      break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      const auto& auth = param->ble_security.auth_cmpl;
      const int slot = findSessionByBda(auth.bd_addr);

      printf("[PAIR] Authentication complete: success=%d addr=",
             auth.success ? 1 : 0);
      printAddr(auth.bd_addr);
      printf(" auth_mode=0x%02x", static_cast<unsigned>(auth.auth_mode));

      if (slot < 0) {
        printf(" -> no active camera session\n");
        break;
      }

      if (!auth.success) {
        printf(" fail_reason=0x%02x (%s)\n",
               static_cast<unsigned>(auth.fail_reason),
               authFailToString(auth.fail_reason));
        closeAndRetry(static_cast<size_t>(slot),
                      "[PAIR] SMP failed; retrying same profile", 3500);
      } else {
        printf(" -> BONDED/ENCRYPTED\n");
        beginPostBond(static_cast<size_t>(slot));
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
    case ESP_GATTC_CONNECT_EVT: {
      const int slot = findSessionByBda(param->connect.remote_bda);
      if (slot >= 0) printCameraPrefix(static_cast<size_t>(slot), "BLE");
      else printf("[BLE] ");
      printf("Connected event: conn_id=%u remote=", param->connect.conn_id);
      printAddr(param->connect.remote_bda);
      printf("\n");
      break;
    }

    case ESP_GATTC_OPEN_EVT: {
      int slot = findSessionByBda(param->open.remote_bda);
      if (slot < 0) slot = g_opening_slot;

      if (slot < 0) {
        printf("[BLE] Open event for unknown camera, status=0x%x\n",
               param->open.status);
        break;
      }

      CameraSession& camera = g_cameras[slot];
      g_opening_slot = -1;

      if (param->open.status != ESP_GATT_OK) {
        printCameraPrefix(static_cast<size_t>(slot), "BLE");
        printf("Open failed, status=0x%x\n", param->open.status);
        resetConnectionRuntime(static_cast<size_t>(slot));
        camera.retry_due_us = esp_timer_get_time() + 2500000ULL;
        scheduleScan(2500);
        break;
      }

      camera.connected = true;
      camera.conn_id = param->open.conn_id;
      memcpy(camera.bda, param->open.remote_bda, ESP_BD_ADDR_LEN);
      camera.retry_due_us = 0;

      printCameraPrefix(static_cast<size_t>(slot), "BLE");
      printf("Open OK. conn_id=%u MTU(initial)=%u\n",
             camera.conn_id, param->open.mtu);

      startBondNow(static_cast<size_t>(slot));
      break;
    }

    case ESP_GATTC_CFG_MTU_EVT: {
      const int slot = findSessionByConnId(param->cfg_mtu.conn_id);
      if (slot < 0) break;
      CameraSession& camera = g_cameras[slot];

      printCameraPrefix(static_cast<size_t>(slot), "GATT");
      printf("MTU exchange: status=%d MTU=%u\n",
             param->cfg_mtu.status, param->cfg_mtu.mtu);
      camera.stage = Stage::Discovering;
      esp_ble_gattc_search_service(
          gattc_if, camera.conn_id, nullptr);
      break;
    }

    case ESP_GATTC_SEARCH_RES_EVT: {
      const int slot = findSessionByConnId(param->search_res.conn_id);
      if (slot < 0) break;
      CameraSession& camera = g_cameras[slot];

      if (isGeoServiceUuid(param->search_res.srvc_id.uuid)) {
        camera.service_start = param->search_res.start_handle;
        camera.service_end = param->search_res.end_handle;
        printCameraPrefix(static_cast<size_t>(slot), "GATT");
        printf("Sony Location service DD00 FOUND: handles 0x%04x..0x%04x uuid_raw=",
               camera.service_start, camera.service_end);
        printUuid(param->search_res.srvc_id.uuid);
        printf("\n");
      } else if (isControlServiceUuid(param->search_res.srvc_id.uuid)) {
        camera.control_service_start = param->search_res.start_handle;
        camera.control_service_end = param->search_res.end_handle;
        printCameraPrefix(static_cast<size_t>(slot), "GATT");
        printf("Sony Control service CC00 FOUND: handles 0x%04x..0x%04x uuid_raw=",
               camera.control_service_start, camera.control_service_end);
        printUuid(param->search_res.srvc_id.uuid);
        printf("\n");
      }
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      const int slot = findSessionByConnId(param->search_cmpl.conn_id);
      if (slot < 0) break;

      printCameraPrefix(static_cast<size_t>(slot), "GATT");
      printf("Service discovery complete: status=0x%x\n",
             param->search_cmpl.status);
      if (param->search_cmpl.status != ESP_GATT_OK) {
        closeAndRetry(static_cast<size_t>(slot),
                      "[GATT] service discovery failed", 2500);
      } else {
        handleDiscoveryComplete(static_cast<size_t>(slot));
      }
      break;
    }

    case ESP_GATTC_READ_CHAR_EVT: {
      const int slot = findSessionByConnId(param->read.conn_id);
      if (slot < 0) break;
      CameraSession& camera = g_cameras[slot];

      camera.gatt_op_inflight = false;
      printCameraPrefix(static_cast<size_t>(slot), "GATT");
      printf("READ handle=0x%04x status=0x%x len=%u\n",
             param->read.handle, param->read.status, param->read.value_len);

      if (param->read.handle == camera.dd21 &&
          camera.stage == Stage::ReadConfig) {
        if (param->read.status == ESP_GATT_OK) {
          printCameraPrefix(static_cast<size_t>(slot), "GEO");
          printf("DD21 config:");
          for (uint16_t i = 0; i < param->read.value_len; ++i) {
            printf(" %02x", param->read.value[i]);
          }
          printf("\n");

          camera.send_timezone_dst =
              param->read.value_len >= 5 &&
              (param->read.value[4] & 0x02) != 0;

          printCameraPrefix(static_cast<size_t>(slot), "GEO");
          printf("DD21 timezone/DST flag=%d -> packet=%u bytes.\n",
                 camera.send_timezone_dst ? 1 : 0,
                 camera.send_timezone_dst ? 95u : 91u);
        } else {
          printCameraPrefix(static_cast<size_t>(slot), "GEO");
          printf("DD21 read failed status=0x%x; keeping 95-byte default.\n",
                 param->read.status);
        }
        beginSonyFeatureHandshake(static_cast<size_t>(slot));
      }
      break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT: {
      const int slot = findSessionByConnId(param->write.conn_id);
      if (slot < 0) break;
      CameraSession& camera = g_cameras[slot];

      if (param->write.handle == camera.dd11) {
        camera.tx_inflight = false;
        if (g_global_tx_slot == slot) {
          g_global_tx_slot = -1;
          g_next_tx_allowed_us =
              esp_timer_get_time() + INTER_CAMERA_TX_GAP_US;
        }

        printCameraPrefix(static_cast<size_t>(slot), "TX");
        printf("lat=%.7f lon=%.7f packet=%u bytes status=0x%x -> %s\n",
               static_cast<double>(TEST_LAT_E7) / 1.0E7,
               static_cast<double>(TEST_LON_E7) / 1.0E7,
               camera.send_timezone_dst ? 95u : 91u,
               param->write.status,
               param->write.status == ESP_GATT_OK ? "OK" : "FAIL");

        if (param->write.status == ESP_GATT_INSUF_AUTHENTICATION ||
            param->write.status == ESP_GATT_INSUF_ENCRYPTION) {
          closeAndRetry(static_cast<size_t>(slot),
                        "[TX] Sony reports insufficient link security", 3000);
        }
      } else if (param->write.handle == camera.dd30 &&
                 camera.stage == Stage::EnableUnlock) {
        camera.gatt_op_inflight = false;
        if (param->write.status == ESP_GATT_OK && camera.dd31 != 0) {
          static const uint8_t enable = 0x01;
          if (submitHandshakeWrite(static_cast<size_t>(slot),
                                   camera.dd31, &enable, 1,
                                   Stage::EnableLock,
                                   "DD31 GPS enable/lock")) {
            break;
          }
        }
        sendTimeSyncOrReady(static_cast<size_t>(slot));
      } else if (param->write.handle == camera.dd31 &&
                 camera.stage == Stage::EnableLock) {
        camera.gatt_op_inflight = false;
        sendTimeSyncOrReady(static_cast<size_t>(slot));
      } else if (param->write.handle == camera.cc13 &&
                 camera.stage == Stage::SyncTime) {
        camera.gatt_op_inflight = false;
        printCameraPrefix(static_cast<size_t>(slot), "TIME");
        printf("CC13 write status=0x%x -> %s\n",
               param->write.status,
               param->write.status == ESP_GATT_OK ? "OK" : "FAIL (continuing)");
        markLocationReady(static_cast<size_t>(slot));
      }
      break;
    }

    case ESP_GATTC_ENC_CMPL_CB_EVT: {
      // This event does not provide a useful per-link payload in the API used
      // by the v7 code. Connection setup is intentionally serialized, so at
      // most one camera should be waiting in Bonding at this moment.
      for (size_t i = 0; i < MAX_CAMERAS; ++i) {
        CameraSession& camera = g_cameras[i];
        if (camera.allocated && camera.connected &&
            camera.stage == Stage::Bonding &&
            isCameraBonded(camera)) {
          printCameraPrefix(i, "PAIR");
          printf("GATT encryption complete callback; local bond=1\n");
          beginPostBond(i);
          break;
        }
      }
      break;
    }

    case ESP_GATTC_DISCONNECT_EVT: {
      int slot = findSessionByConnId(param->disconnect.conn_id);
      if (slot < 0) slot = findSessionByBda(param->disconnect.remote_bda);

      if (slot >= 0) {
        printCameraPrefix(static_cast<size_t>(slot), "BLE");
        printf("Disconnected from ");
        printAddr(param->disconnect.remote_bda);
        printf(" reason=0x%02x\n", param->disconnect.reason);

        CameraSession& camera = g_cameras[slot];
        const uint64_t retry_due = camera.retry_due_us;
        resetConnectionRuntime(static_cast<size_t>(slot));
        camera.retry_due_us =
            retry_due != 0 ? retry_due : esp_timer_get_time() + 2200000ULL;
        scheduleScan(2200);
      } else {
        printf("[BLE] Disconnected unknown peer reason=0x%02x\n",
               param->disconnect.reason);
      }
      break;
    }

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
  esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
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
      ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH,
      &only_accept, sizeof(only_accept)));

  printf("[PAIR] Security: GATTS+SC_CAPABLE_BOND auth_req=0x%02x "
         "IO=NONE key=16 ENC+ID; SC-only disabled.\n",
         static_cast<unsigned>(auth_req));
}

bool initBluetooth() {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
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
  ESP_ERROR_CHECK(esp_ble_gap_set_device_name("SonyGPS-C6-multi-dev"));

  applySecurityProfile();

  ret = esp_ble_gatt_set_local_mtu(158);
  if (ret != ESP_OK) {
    printf("[BLE] Warning: set local MTU 158 failed: %s\n",
           esp_err_to_name(ret));
  }

  printBondedDevices();
  printf("[BLE] Multi-camera session manager enabled: max=%u.\n",
         static_cast<unsigned>(MAX_CAMERAS));

  ret = esp_ble_gattc_app_register(APP_ID);
  if (ret != ESP_OK) {
    printf("[FATAL] GATTC app register failed: %s\n",
           esp_err_to_name(ret));
    return false;
  }
  return true;
}

void serviceLocationTransmit(uint64_t now) {
  if (g_global_tx_slot >= 0 || now < g_next_tx_allowed_us) return;

  for (size_t offset = 0; offset < MAX_CAMERAS; ++offset) {
    const size_t slot = (g_tx_round_robin + offset) % MAX_CAMERAS;
    CameraSession& camera = g_cameras[slot];

    if (!camera.allocated || !camera.connected || !camera.ready ||
        camera.tx_inflight || camera.gatt_op_inflight) {
      continue;
    }

    if (camera.last_tx_us != 0 &&
        now - camera.last_tx_us < LOCATION_UPDATE_INTERVAL_US) {
      continue;
    }

    camera.last_tx_us = now;
    g_tx_round_robin = (slot + 1) % MAX_CAMERAS;
    sendStaticLocation(slot);
    break;
  }
}

} // namespace

extern "C" void app_main(void) {
  printf("\n=== Sony Alpha ESP32 GPS multi-camera development / XIAO ESP32-C6 ===\n");
  printf("Max simultaneous Sony sessions: %u\n",
         static_cast<unsigned>(MAX_CAMERAS));
  printf("Flow per camera: connect -> bond -> MTU -> DD00/CC00 -> DD21 -> "
         "optional DD30/DD31/CC13 -> DD11\n");
  printf("Static shared fix: %.7f, %.7f\n",
         static_cast<double>(TEST_LAT_E7) / 1.0E7,
         static_cast<double>(TEST_LON_E7) / 1.0E7);

  const TimeZoneInfo boot_tz = currentTimeZone(BASE_UTC_EPOCH);
  printf("Timezone resolver: %s standard=%dmin dst=%dmin%s\n",
         boot_tz.name, boot_tz.standard_offset_minutes,
         boot_tz.dst_offset_minutes,
         boot_tz.approximate ? " (approx boundary resolver)" : "");
  printf("Device name: SonyGPS-C6-multi-dev\n");

  if (!initBluetooth()) return;

  for (;;) {
    const uint64_t now = esp_timer_get_time();

    serviceLocationTransmit(now);

    if (g_scan_due_us != 0 && now >= g_scan_due_us &&
        !g_scan_active && g_opening_slot < 0) {
      startScan();
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
