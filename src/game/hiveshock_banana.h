#ifndef HIVESHOCK_BANANA_H
#define HIVESHOCK_BANANA_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void bhv_banana_peel_loop(void);
void hiveshock_spawn_banana_from_viewer(void);
struct Object;
void hiveshock_launch_prop_from_viewer(struct Object *prop);

#ifdef __cplusplus
}
#endif

#endif // HIVESHOCK_BANANA_H
