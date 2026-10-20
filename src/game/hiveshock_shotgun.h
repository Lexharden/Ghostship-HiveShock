#ifndef HIVESHOCK_SHOTGUN_H
#define HIVESHOCK_SHOTGUN_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void bhv_shotgun_pickup_loop(void);
void bhv_shotgun_held_loop(void);

// A viewer throws a shotgun in front of Mario (same trajectory as the banana peel). Mario equips it by walking over it.
void hiveshock_spawn_shotgun_from_viewer(void);
// Equips Mario directly with `shells` shells.
void hiveshock_shotgun_give(int shells);
// Per-frame: keeps the held model attached, handles the fire button and delayed kills.
void hiveshock_shotgun_update(void);
// Fires one shot in front of Mario (also bound to L). Returns 1 if a shell was used.
int hiveshock_shotgun_fire(void);
int hiveshock_shotgun_shells(void);

#ifdef __cplusplus
}
#endif

#endif // HIVESHOCK_SHOTGUN_H
