#pragma once
#include "esp_http_server.h"

// GET  /api/v2/profiles          all profiles + id of the last activated one
// PUT  /api/v2/profiles/<1-3>    store one profile (all values required)
//                                optional "activate": true also activates it (one request, one OTP check)
// Activating a profile with another pool mode restarts the device automatically, because the pool
// manager is created at start-up. "autoRestart": false (POST set-profile, PUT) suppresses that and
// only reports "restartRequired", the web UI uses it to offer a restart button instead.
// DELETE /api/v2/profiles/<1-3>  remove a profile (values and name)
// POST /api/system/set-profile   {"profile": 1..3} activate a stored profile
esp_err_t GET_V2_profiles(httpd_req_t *req);
esp_err_t PUT_V2_profile(httpd_req_t *req);
esp_err_t DELETE_V2_profile(httpd_req_t *req);
esp_err_t POST_set_profile(httpd_req_t *req);
