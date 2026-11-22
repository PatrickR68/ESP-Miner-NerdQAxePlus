#include "profiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boards/board.h"
#include "nvs_config.h"

namespace Profiles
{

// The config document is flat, so a profile is stored as flat keys:
//   p<id>_name, p<id>_freq, p<id>_volt, p<id>_bal,
//   p<id>_f<ch>_mode, _spd, _tmp, _p, _i, _d     (ch = 0 or 1)
// plus one key for the profile that was activated last.
static const char *KEY_ACTIVE = "profile_act";

static void makeKey(char *buf, size_t size, int id, const char *suffix)
{
    snprintf(buf, size, "p%d_%s", id, suffix);
}

static void makeFanKey(char *buf, size_t size, int id, int ch, const char *suffix)
{
    snprintf(buf, size, "p%d_f%d_%s", id, ch, suffix);
}

static const char *const FAN_SUFFIXES[] = {"mode", "spd", "tmp", "p", "i", "d"};
static const int FAN_SUFFIX_COUNT = sizeof(FAN_SUFFIXES) / sizeof(FAN_SUFFIXES[0]);

bool isValidId(int id)
{
    return id >= 1 && id <= COUNT;
}

uint16_t getActive()
{
    uint16_t id = Config::cfgGetU16(KEY_ACTIVE, 0);
    return isValidId(id) ? id : 0;
}

static bool hasU16(int id, const char *suffix)
{
    char key[24];
    makeKey(key, sizeof(key), id, suffix);
    return Config::cfgHasKey(key);
}

static uint16_t getU16(int id, const char *suffix)
{
    char key[24];
    makeKey(key, sizeof(key), id, suffix);
    return Config::cfgGetU16(key, 0);
}

static void setU16(int id, const char *suffix, uint16_t value)
{
    char key[24];
    makeKey(key, sizeof(key), id, suffix);
    Config::cfgSetU16(key, value);
}

static uint16_t getFanU16(int id, int ch, const char *suffix)
{
    char key[24];
    makeFanKey(key, sizeof(key), id, ch, suffix);
    return Config::cfgGetU16(key, 0);
}

static void setFanU16(int id, int ch, const char *suffix, uint16_t value)
{
    char key[24];
    makeFanKey(key, sizeof(key), id, ch, suffix);
    Config::cfgSetU16(key, value);
}

bool isComplete(int id)
{
    if (!isValidId(id)) {
        return false;
    }

    if (!hasU16(id, "freq") || !hasU16(id, "volt") || !hasU16(id, "pmode") || !hasU16(id, "bal")) {
        return false;
    }

    for (int ch = 0; ch < 2; ch++) {
        for (int s = 0; s < FAN_SUFFIX_COUNT; s++) {
            char key[24];
            makeFanKey(key, sizeof(key), id, ch, FAN_SUFFIXES[s]);
            if (!Config::cfgHasKey(key)) {
                return false;
            }
        }
    }
    return true;
}

bool load(int id, Profile &out)
{
    if (!isComplete(id)) {
        return false;
    }

    memset(&out, 0, sizeof(out));

    char key[24];
    makeKey(key, sizeof(key), id, "name");
    char *name = Config::cfgGetStrAlloc(key, "");
    if (name) {
        strncpy(out.name, name, NAME_MAX_LEN);
        out.name[NAME_MAX_LEN] = '\0';
        free(name);
    }

    out.frequency = getU16(id, "freq");
    out.coreVoltage = getU16(id, "volt");
    out.poolMode = getU16(id, "pmode");
    out.poolBalance = getU16(id, "bal");

    for (int ch = 0; ch < 2; ch++) {
        out.fan[ch].mode = getFanU16(id, ch, "mode");
        out.fan[ch].manualSpeed = getFanU16(id, ch, "spd");
        out.fan[ch].targetTemp = getFanU16(id, ch, "tmp");
        out.fan[ch].p = getFanU16(id, ch, "p");
        out.fan[ch].i = getFanU16(id, ch, "i");
        out.fan[ch].d = getFanU16(id, ch, "d");
    }
    return true;
}

static bool validNameChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
           c == '_';
}

const char *validate(const Profile &p, Board *board)
{
    // name: optional, limited characters
    size_t len = strnlen(p.name, NAME_MAX_LEN + 1);
    if (len > NAME_MAX_LEN) {
        return "name too long";
    }
    for (size_t i = 0; i < len; i++) {
        if (!validNameChar(p.name[i])) {
            return "name contains invalid characters (allowed: A-Z a-z 0-9 space - _)";
        }
    }

    // ASIC frequency and voltage: stay inside the limits of this board
    if (p.frequency == 0) {
        return "frequency missing";
    }
    int absMaxFreq = board->getAbsMaxAsicFrequency();
    if (absMaxFreq > 0 && p.frequency > absMaxFreq) {
        return "frequency above board limit";
    }

    if (p.coreVoltage == 0) {
        return "coreVoltage missing";
    }
    int absMinVolt = board->getAbsMinAsicVoltageMillis();
    int absMaxVolt = board->getAbsMaxAsicVoltageMillis();
    if (absMinVolt > 0 && p.coreVoltage < absMinVolt) {
        return "coreVoltage below board limit";
    }
    if (absMaxVolt > 0 && p.coreVoltage > absMaxVolt) {
        return "coreVoltage above board limit";
    }

    // pool mode: 0 = failover, 1 = dual pool
    if (p.poolMode > 1) {
        return "poolMode must be 0 (failover) or 1 (dual pool)";
    }

    // pool balance: share of the primary pool in percent, the web UI allows 1-99
    if (p.poolBalance < 1 || p.poolBalance > 99) {
        return "poolBalance must be 1-99";
    }

    // fans: only plausibility checks, the same ranges as the normal settings are not enforced further
    for (int ch = 0; ch < 2; ch++) {
        // modes: 0 = manual, 2 = PID, 3 = linked (channel 1 follows channel 0,
        // only possible for channel 1 because channel 0 is processed first)
        bool modeOk = (p.fan[ch].mode == 0 || p.fan[ch].mode == 2 || (ch == 1 && p.fan[ch].mode == 3));
        if (!modeOk) {
            return ch == 0 ? "fan 0 mode must be 0 (manual) or 2 (PID)" : "fan 1 mode must be 0, 2 or 3 (linked)";
        }
        if (p.fan[ch].manualSpeed > 100) {
            return "fan manualSpeed must be 0-100";
        }
    }

    return nullptr;
}

void store(int id, const Profile &p)
{
    char key[24];
    makeKey(key, sizeof(key), id, "name");
    Config::cfgSetStr(key, p.name);

    setU16(id, "freq", p.frequency);
    setU16(id, "volt", p.coreVoltage);
    setU16(id, "pmode", p.poolMode);
    setU16(id, "bal", p.poolBalance);

    for (int ch = 0; ch < 2; ch++) {
        setFanU16(id, ch, "mode", p.fan[ch].mode);
        setFanU16(id, ch, "spd", p.fan[ch].manualSpeed);
        setFanU16(id, ch, "tmp", p.fan[ch].targetTemp);
        setFanU16(id, ch, "p", p.fan[ch].p);
        setFanU16(id, ch, "i", p.fan[ch].i);
        setFanU16(id, ch, "d", p.fan[ch].d);
    }

    Config::flush();
}

bool erase(int id)
{
    if (!isValidId(id)) {
        return false;
    }

    char key[24];
    static const char *const BASE_SUFFIXES[] = {"name", "freq", "volt", "pmode", "bal"};
    for (const char *suffix : BASE_SUFFIXES) {
        makeKey(key, sizeof(key), id, suffix);
        Config::cfgRemoveKey(key);
    }
    for (int ch = 0; ch < 2; ch++) {
        for (int s = 0; s < FAN_SUFFIX_COUNT; s++) {
            makeFanKey(key, sizeof(key), id, ch, FAN_SUFFIXES[s]);
            Config::cfgRemoveKey(key);
        }
    }

    if (getActive() == id) {
        Config::cfgRemoveKey(KEY_ACTIVE);
    }

    Config::flush();
    return true;
}

const char *activate(int id, Board *board, bool *restartRequired)
{
    if (restartRequired) {
        *restartRequired = false;
    }

    if (!isValidId(id)) {
        return "invalid profile";
    }

    Profile p;
    if (!load(id, p)) {
        return "profile is not complete";
    }

    // the limits may differ from the time the profile was stored (other firmware)
    const char *err = validate(p, board);
    if (err) {
        return err;
    }

    Config::setAsicFrequency(p.frequency);
    Config::setAsicVoltage(p.coreVoltage);
    // The pool manager is created at start-up, a different pool mode needs a restart.
    if (restartRequired) {
        *restartRequired = (Config::getPoolMode() != p.poolMode);
    }
    Config::setPoolMode(p.poolMode);
    // The balance only has an effect in dual pool mode, in failover mode it is left alone.
    if (p.poolMode == 1) {
        Config::setPoolBalance(p.poolBalance);
    }

    for (int ch = 0; ch < 2; ch++) {
        Config::setFanMode(ch, p.fan[ch].mode);
        Config::setFanManualSpeed(ch, p.fan[ch].manualSpeed);
        Config::setFanPidTargetTemp(ch, p.fan[ch].targetTemp);
        Config::setFanPidP(ch, p.fan[ch].p);
        Config::setFanPidI(ch, p.fan[ch].i);
        Config::setFanPidD(ch, p.fan[ch].d);
    }

    Config::cfgSetU16(KEY_ACTIVE, (uint16_t) id);
    Config::flush();
    return nullptr;
}

} // namespace Profiles
