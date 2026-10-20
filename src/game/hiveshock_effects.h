#ifndef HIVESHOCK_EFFECTS_H
#define HIVESHOCK_EFFECTS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runs a HiveShock action by name. `value` is action-specific (wedges, coins, count...),
// `duration` is in seconds for timed effects. Returns 1 if the action is known.
// Must be called from the game thread, during gameplay.
int hiveshock_run_action(const char *action, double value, int hasValue, double duration);

void bhv_hive_poison_mushroom_loop(void);

// Action registry (used by the Network > HiveShock menu and the bridge).
int hiveshock_action_count(void);
const char *hiveshock_action_name(int index);
const char *hiveshock_action_category(int index); // NULL for aliases (hidden)
int hiveshock_action_exists(const char *action);
int hiveshock_can_run(void); // true while Mario exists and the game is in normal play

// Per-frame update for timed effects.
void hiveshock_effects_update(void);

#ifdef __cplusplus
}
#endif

#endif // HIVESHOCK_EFFECTS_H
