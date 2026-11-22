#pragma once

#include <stddef.h>
#include <stdint.h>

class Board;

// Three user profiles with ASIC and fan settings plus pool balance.
//
// The live settings (Config::getAsicFrequency(), ...) stay as they are and the
// rest of the firmware keeps using them. A profile is only a stored set of
// values next to them:
//   - store():    writes the values of one profile (flat keys "p<id>_...")
//   - activate(): copies the values of a profile into the live settings
//
// Not part of a profile (on purpose): overheat temperature and pool credentials.
namespace Profiles
{

constexpr int COUNT = 3;
constexpr size_t NAME_MAX_LEN = 20;

struct Fan {
    uint16_t mode;
    uint16_t manualSpeed;
    uint16_t targetTemp;
    uint16_t p; // PID values are stored x100, like the live settings
    uint16_t i;
    uint16_t d;
};

struct Profile {
    char name[NAME_MAX_LEN + 1]; // may be empty
    uint16_t frequency;
    uint16_t coreVoltage;
    uint16_t poolMode;    // 0 = failover (primary/fallback), 1 = dual pool
    uint16_t poolBalance; // share of the primary pool in %, only has an effect in dual pool mode
    Fan fan[2];
};

bool isValidId(int id); // 1..COUNT

// 0 = no profile activated yet, otherwise the id that was activated last.
// Manual changes to the live settings afterwards are not tracked.
uint16_t getActive();

// true if every value of the profile is stored (the name is optional)
bool isComplete(int id);

// false if the profile is not complete
bool load(int id, Profile &out);

// Returns nullptr if the profile is valid, otherwise a short error text.
const char *validate(const Profile &p, Board *board);

// Caller must have called validate() before. Writes the profile and flushes.
void store(int id, const Profile &p);

// Removes all values of a profile (including its name). If it was the profile activated
// last, that marker is reset too. The live settings are not touched. Returns false for an invalid id.
bool erase(int id);

// Copies the profile into the live settings and flushes.
// Returns nullptr on success, otherwise a short error text.
// *restartRequired (optional) is set if the pool mode changed: the pool manager is created at
// start-up, so the device has to be restarted for that. The pool balance is only copied in dual pool mode.
// The caller has to reload the subsystems afterwards (see handler_v2_profiles.cpp).
const char *activate(int id, Board *board, bool *restartRequired = nullptr);

} // namespace Profiles
