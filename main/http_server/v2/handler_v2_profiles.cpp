#include "handler_v2_profiles.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "ArduinoJson.h"
#include "global_state.h"
#include "http_cors.h"
#include "http_utils.h"
#include "nvs_config.h"
#include "profiles.h"
#include "psram_allocator.h"

static const char *TAG = "http_v2_profiles";

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static bool readU16(JsonVariantConst v, uint16_t &out)
{
    if (!v.is<uint16_t>()) {
        return false;
    }
    out = v.as<uint16_t>();
    return true;
}

// PID values come as float (like in /api/v2/settings) and are stored x100
static bool readPid(JsonVariantConst v, uint16_t &out)
{
    if (!v.is<float>()) {
        return false;
    }
    float f = v.as<float>();
    if (f < 0.0f || f > 600.0f) {
        return false;
    }
    out = (uint16_t) roundf(f * 100.0f);
    return true;
}

// Returns nullptr on success, otherwise an error text. All values are required,
// only the name is optional.
static const char *parseProfile(JsonDocument &doc, Profiles::Profile &p)
{
    memset(&p, 0, sizeof(p));

    if (!doc["name"].isNull()) {
        if (!doc["name"].is<const char *>()) {
            return "name must be a string";
        }
        const char *name = doc["name"].as<const char *>();
        if (strlen(name) > Profiles::NAME_MAX_LEN) {
            return "name too long";
        }
        strncpy(p.name, name, Profiles::NAME_MAX_LEN);
    }

    if (!readU16(doc["frequency"], p.frequency)) {
        return "frequency missing or invalid";
    }
    if (!readU16(doc["coreVoltage"], p.coreVoltage)) {
        return "coreVoltage missing or invalid";
    }
    if (!readU16(doc["poolMode"], p.poolMode)) {
        return "poolMode missing or invalid";
    }
    if (!readU16(doc["poolBalance"], p.poolBalance)) {
        return "poolBalance missing or invalid";
    }

    JsonArray fans = doc["fans"].as<JsonArray>();
    if (fans.isNull() || fans.size() != 2) {
        return "fans must contain exactly 2 entries";
    }

    for (int ch = 0; ch < 2; ch++) {
        JsonObject fan = fans[ch].as<JsonObject>();
        if (fan.isNull()) {
            return "fans entry must be an object";
        }
        if (!readU16(fan["mode"], p.fan[ch].mode)) {
            return "fan mode missing or invalid";
        }
        if (!readU16(fan["manualSpeed"], p.fan[ch].manualSpeed)) {
            return "fan manualSpeed missing or invalid";
        }

        JsonObject pid = fan["pid"].as<JsonObject>();
        if (pid.isNull()) {
            return "fan pid missing";
        }
        if (!readU16(pid["targetTemp"], p.fan[ch].targetTemp)) {
            return "fan pid targetTemp missing or invalid";
        }
        if (!readPid(pid["p"], p.fan[ch].p) || !readPid(pid["i"], p.fan[ch].i) || !readPid(pid["d"], p.fan[ch].d)) {
            return "fan pid p/i/d missing or invalid";
        }
    }
    return nullptr;
}

static void writeProfile(JsonObject o, int id)
{
    o["id"] = id;

    Profiles::Profile p;
    if (!Profiles::load(id, p)) {
        o["complete"] = false;
        return;
    }

    o["complete"] = true;
    o["name"] = p.name; // may be empty, the UI shows a default then
    o["frequency"] = p.frequency;
    o["coreVoltage"] = p.coreVoltage;
    o["poolMode"] = p.poolMode;
    o["poolBalance"] = p.poolBalance;

    JsonArray fans = o["fans"].to<JsonArray>();
    for (int ch = 0; ch < 2; ch++) {
        JsonObject fan = fans.add<JsonObject>();
        fan["mode"] = p.fan[ch].mode;
        fan["manualSpeed"] = p.fan[ch].manualSpeed;
        JsonObject pid = fan["pid"].to<JsonObject>();
        pid["targetTemp"] = p.fan[ch].targetTemp;
        pid["p"] = p.fan[ch].p / 100.0f;
        pid["i"] = p.fan[ch].i / 100.0f;
        pid["d"] = p.fan[ch].d / 100.0f;
    }
}

// "/api/v2/profiles/2" -> 2, otherwise 0
static int idFromUri(const httpd_req_t *req)
{
    const char *prefix = "/api/v2/profiles/";
    size_t n = strlen(prefix);
    if (strncmp(req->uri, prefix, n) != 0) {
        return 0;
    }
    const char *s = req->uri + n;
    char *end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s || (*end != '\0' && *end != '?')) {
        return 0;
    }
    return Profiles::isValidId((int) v) ? (int) v : 0;
}

// Same sequence as at the end of PATCH_V2_settings: the live values changed,
// so every subsystem re-reads them. ASIC frequency/voltage are picked up by the
// power management task, a changed poolBalance reconnects the pools.
static void reloadAll()
{
    Board *board = SYSTEM_MODULE.getBoard();
    board->loadSettings();
    POWER_MANAGEMENT_MODULE.getFanController().loadSettings();
    SYSTEM_MODULE.loadSettings();
    if (STRATUM_MANAGER) {
        STRATUM_MANAGER->loadSettings();
    }
}

// A different pool mode only takes effect after a restart. Same sequence as POST_restart: the
// answer is already sent, wait so that the config flush task can write the changes (it batches for
// 100 ms), then shut down ASICs and LDOs and reboot. Does not return.
static void restartDevice()
{
    ESP_LOGW(TAG, "Pool mode changed by a profile, restarting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    POWER_MANAGEMENT_MODULE.restart();
}

// ---------------------------------------------------------------------------
// GET /api/v2/profiles
// ---------------------------------------------------------------------------

esp_err_t GET_V2_profiles(httpd_req_t *req)
{
    ConGuard g(http_server, req);

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    httpd_resp_set_type(req, "application/json");
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    PSRAMAllocator allocator;
    JsonDocument doc(&allocator);

    doc["active"] = Profiles::getActive();
    JsonArray profiles = doc["profiles"].to<JsonArray>();
    for (int id = 1; id <= Profiles::COUNT; id++) {
        writeProfile(profiles.add<JsonObject>(), id);
    }

    return sendJsonResponse(req, doc);
}

// ---------------------------------------------------------------------------
// PUT /api/v2/profiles/<1-3>
// ---------------------------------------------------------------------------

esp_err_t PUT_V2_profile(httpd_req_t *req)
{
    ConGuard g(http_server, req);

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (validateOTP(req) != ESP_OK) {
        return ESP_FAIL;
    }

    int id = idFromUri(req);
    if (id == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid profile id (1-3)");
    }

    PSRAMAllocator allocator;
    JsonDocument doc(&allocator);

    esp_err_t err = getJsonData(req, doc);
    if (err != ESP_OK) {
        return err;
    }

    Profiles::Profile p;
    const char *msg = parseProfile(doc, p);
    if (!msg) {
        msg = Profiles::validate(p, SYSTEM_MODULE.getBoard());
    }
    if (msg) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
    }

    // optional: activate the profile right away, the web UI does this for "save current values".
    // One request, because a one-shot OTP code can only be used once.
    bool activateNow = doc["activate"].as<bool>();
    // restart automatically if the pool mode changes; the web UI sets false and offers a button instead
    bool autoRestart = doc["autoRestart"] | true;

    Profiles::store(id, p);
    ESP_LOGI(TAG, "Profile %d stored", id);

    bool restartRequired = false;
    if (activateNow) {
        msg = Profiles::activate(id, SYSTEM_MODULE.getBoard(), &restartRequired);
        if (msg) {
            // already stored, but it could not be activated
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        }
        ESP_LOGI(TAG, "Profile %d activated%s", id, restartRequired ? " (pool mode changed, restart required)" : "");
    }

    doc.clear();
    doc["id"] = id;
    doc["saved"] = true;
    if (activateNow) {
        doc["activated"] = true;
        doc["restartRequired"] = restartRequired;
        doc["restarting"] = restartRequired && autoRestart;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t res = sendJsonResponse(req, doc);

    // answer first, then apply (a changed poolBalance reconnects the pools)
    if (activateNow) {
        if (restartRequired && autoRestart) {
            restartDevice();
        } else {
            reloadAll();
        }
    }
    return res;
}

// ---------------------------------------------------------------------------
// DELETE /api/v2/profiles/<1-3>
// ---------------------------------------------------------------------------

esp_err_t DELETE_V2_profile(httpd_req_t *req)
{
    ConGuard g(http_server, req);

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (validateOTP(req) != ESP_OK) {
        return ESP_FAIL;
    }

    int id = idFromUri(req);
    if (id == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid profile id (1-3)");
    }

    // deleting an empty profile is fine, the result is the same
    Profiles::erase(id);
    ESP_LOGI(TAG, "Profile %d deleted", id);

    PSRAMAllocator allocator;
    JsonDocument doc(&allocator);
    doc["id"] = id;
    doc["deleted"] = true;

    httpd_resp_set_type(req, "application/json");
    return sendJsonResponse(req, doc);
}

// ---------------------------------------------------------------------------
// POST /api/system/set-profile    {"profile": 1..3}
// ---------------------------------------------------------------------------

esp_err_t POST_set_profile(httpd_req_t *req)
{
    ConGuard g(http_server, req);

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (validateOTP(req) != ESP_OK) {
        return ESP_FAIL;
    }

    PSRAMAllocator allocator;
    JsonDocument doc(&allocator);

    esp_err_t err = getJsonData(req, doc);
    if (err != ESP_OK) {
        return err;
    }

    if (!doc["profile"].is<int>()) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "profile missing or invalid");
    }
    int id = doc["profile"].as<int>();
    // restart automatically if the pool mode changes; the web UI sets false and offers a button instead
    bool autoRestart = doc["autoRestart"] | true;

    bool restartRequired = false;
    const char *msg = Profiles::activate(id, SYSTEM_MODULE.getBoard(), &restartRequired);
    if (msg) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
    }
    ESP_LOGI(TAG, "Profile %d activated%s", id, restartRequired ? " (pool mode changed, restart required)" : "");

    bool restartNow = restartRequired && autoRestart;

    doc.clear();
    doc["active"] = id;
    doc["restartRequired"] = restartRequired;
    doc["restarting"] = restartNow;

    httpd_resp_set_type(req, "application/json");
    esp_err_t res = sendJsonResponse(req, doc);

    // answer first, then apply (a changed poolBalance reconnects the pools)
    if (restartNow) {
        restartDevice();
    } else {
        reloadAll();
    }
    return res;
}
