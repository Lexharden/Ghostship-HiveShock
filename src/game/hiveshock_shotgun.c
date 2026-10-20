#include "sm64.h"
#include "types.h"
#include "geo_commands.h"
#include "object_fields.h"
#include "object_constants.h"
#include "behavior_data.h"
#include "model_ids.h"
#include "sounds.h"
#include "engine/behavior_script.h"
#include "engine/geo_layout.h"
#include "engine/graph_node.h"
#include "engine/math_util.h"
#include "game/area.h"
#include "game/camera.h"
#include "game/game_init.h"
#include "game/interaction.h"
#include "game/level_update.h"
#include "game/mario.h"
#include "game/memory.h"
#include "game/object_helpers.h"
#include "game/object_list_processor.h"
#include "game/hiveshock_banana.h"
#include "game/hiveshock_shotgun.h"
#include "game/spawn_sound.h"

#include "audio/external.h"

// Fast64 export, compiled directly into the executable (no OTR/model ID needed).
#include "actors/shotgun/model.inc.c"
#include "actors/shotgun/geo.inc.c"

#define o gCurrentObject

#define SHOTGUN_SCALE        0.0195f // export is ~14000 units long (double the previous size); muzzle points to -Z
#define HOLD_FORWARD         45.0f   // grip offset from Mario: stock near his body, barrel pointing ahead
#define HOLD_RIGHT           28.0f
#define HOLD_UP              56.0f
#define MUZZLE_FORWARD       (HOLD_FORWARD + 7840.0f * SHOTGUN_SCALE)
#define SHOTGUN_SHELLS       8
#define SHOTGUN_MAX_SHELLS   24
#define SHOTGUN_RANGE        2400.0f
#define SHOTGUN_SPREAD_BASE  130.0f // cone half-width at the muzzle
#define SHOTGUN_SPREAD_RATE  0.10f  // cone widening per unit of distance
#define SHOTGUN_COOLDOWN     14     // frames between shots
#define SHOTGUN_PICKUP_LIFE  (60 * 30)
#define SHOTGUN_FORCE_DELAY  3      // frames to wait for an object's own death logic before forcing it
#define MAX_PENDING          48

enum ShotgunPickupAction {
    PICKUP_ACT_FLYING,
    PICKUP_ACT_LANDED,
};

static u8 sPoolMem[1024] __attribute__((aligned(16)));
static struct AllocOnlyPool sPool;
static struct GraphNode *sNode = NULL;

static struct GraphNode *shotgun_get_graph_node(void) {
    if (sNode == NULL) {
        sPool.totalSpace = sizeof(sPoolMem);
        sPool.usedSpace = 0;
        sPool.startPtr = sPoolMem;
        sPool.freePtr = sPoolMem;
        sNode = process_geo_layout(&sPool, (void *) shotgun_geo);
    }
    return sNode;
}

static int sShells = 0;
static int sCooldown = 0;
static struct Object *sHeld = NULL;

static struct {
    struct Object *obj;
    const BehaviorScript *bhv;
    int framesLeft;
} sPending[MAX_PENDING];

// ---- Pickup (thrown by a viewer) ----------------------------------------------------------------------

void bhv_shotgun_pickup_loop(void) {
    cur_obj_update_floor_and_walls();
    cur_obj_move_standard(-78);

    if (o->oAction == PICKUP_ACT_FLYING) {
        o->oFaceAngleYaw += 0x800;
        if (o->oMoveFlags & OBJ_MOVE_ON_GROUND) {
            o->oForwardVel = 0.0f;
            o->oVelY = 0.0f;
            cur_obj_play_sound_2(SOUND_GENERAL_BOX_LANDING);
            o->oAction = PICKUP_ACT_LANDED;
            o->oTimer = 0;
        }
        return;
    }

    // Landed: slow spin so it is easy to spot, equip when Mario walks over it.
    o->oFaceAngleYaw += 0x100;
    if (dist_between_objects(o, gMarioObject) < 120.0f && absf(o->oPosY - gMarioState->pos[1]) < 120.0f) {
        hiveshock_shotgun_give(SHOTGUN_SHELLS);
        spawn_mist_particles();
        obj_mark_for_deletion(o);
    } else if (o->oTimer > SHOTGUN_PICKUP_LIFE) {
        obj_mark_for_deletion(o);
    }
}

void hiveshock_spawn_shotgun_from_viewer(void) {
    struct Object *pickup;
    struct GraphNode *node;

    if (gMarioObject == NULL || gMarioState == NULL || gCurrentArea == NULL || sCurrPlayMode != 0) {
        return;
    }
    node = shotgun_get_graph_node();
    if (node == NULL) {
        return;
    }
    pickup = spawn_object(gMarioObject, MODEL_NONE, bhvShotgunPickup);
    if (pickup == NULL) {
        return;
    }
    pickup->header.gfx.sharedChild = node;
    obj_scale(pickup, SHOTGUN_SCALE);
    hiveshock_launch_prop_from_viewer(pickup);
}

// ---- Held shotgun --------------------------------------------------------------------------------------

static void mario_relative(f32 forward, f32 right, f32 up, Vec3f out) {
    s16 yaw = gMarioState->faceAngle[1];
    out[0] = gMarioState->pos[0] + sins(yaw) * forward + sins(yaw + 0x4000) * right;
    out[1] = gMarioState->pos[1] + up;
    out[2] = gMarioState->pos[2] + coss(yaw) * forward + coss(yaw + 0x4000) * right;
}

void bhv_shotgun_held_loop(void) {
    Vec3f pos;

    cur_obj_scale(SHOTGUN_SCALE);
    mario_relative(HOLD_FORWARD, HOLD_RIGHT, HOLD_UP, pos);
    o->oPosX = pos[0];
    o->oPosY = pos[1];
    o->oPosZ = pos[2];
    o->oFaceAngleYaw = gMarioState->faceAngle[1] + 0x8000; // model muzzle is -Z
    o->oMoveAngleYaw = o->oFaceAngleYaw;

    if (sShells <= 0) {
        obj_mark_for_deletion(o);
    }
}

void hiveshock_shotgun_give(int shells) {
    sShells += shells;
    if (sShells > SHOTGUN_MAX_SHELLS) {
        sShells = SHOTGUN_MAX_SHELLS;
    }
    play_sound(SOUND_MENU_STAR_SOUND, gGlobalSoundSource);
}

int hiveshock_shotgun_shells(void) {
    return sShells;
}

// ---- Shooting ------------------------------------------------------------------------------------------

// Interaction types that can never be a target (pickups, doors, warps, hazards that are not "destroyable").
#define IGNORED_TYPES                                                                                           \
    (INTERACT_COIN | INTERACT_CAP | INTERACT_DOOR | INTERACT_WARP_DOOR | INTERACT_WARP | INTERACT_STAR_OR_KEY | \
     INTERACT_POLE | INTERACT_TEXT | INTERACT_HOOT | INTERACT_STRONG_WIND | INTERACT_TORNADO |                   \
     INTERACT_WHIRLPOOL | INTERACT_WATER_RING | INTERACT_BBH_ENTRANCE | INTERACT_IGLOO_BARRIER |                  \
     INTERACT_CANNON_BASE | INTERACT_FLAME)

// Types that are enemies/destructibles; if their own logic did not react, they are removed after a few frames.
#define FORCE_KILL_TYPES                                                                                      \
    (INTERACT_BOUNCE_TOP | INTERACT_BOUNCE_TOP2 | INTERACT_KOOPA | INTERACT_SNUFIT_BULLET |                  \
     INTERACT_MR_BLIZZARD | INTERACT_BULLY | INTERACT_HIT_FROM_BELOW | INTERACT_BREAKABLE |      \
     INTERACT_GRABBABLE /* Chuckya, Bob-omb: their own logic ignores shots */)

static int is_boss(const BehaviorScript *bhv) {
    return bhv == segmented_to_virtual(bhvBowser) || bhv == segmented_to_virtual(bhvKingBobomb)
           || bhv == segmented_to_virtual(bhvWhompKingBoss) || bhv == segmented_to_virtual(bhvEyerokBoss);
}

static void with_current_object(struct Object *obj, void (*fn)(void)) {
    struct Object *saved = gCurrentObject;
    gCurrentObject = obj;
    fn();
    gCurrentObject = saved;
}

static void fx_hit_puff(void) {
    spawn_mist_particles();
}

static void fx_kill(void) {
    // Killed while it was carrying Mario: let go of him so he is not stuck in the grab.
    if (gMarioState->action == ACT_GRABBED && (o->oInteractStatus & INT_STATUS_GRABBED_MARIO || o->oAction == 1)) {
        set_mario_action(gMarioState, ACT_THROWN_FORWARD, 0);
        gMarioState->forwardVel = 20.0f;
        gMarioState->vel[1] = 20.0f;
    }
    spawn_mist_particles_with_sound(SOUND_OBJ_DEFAULT_DEATH);
    obj_spawn_loot_yellow_coins(o, o->oNumLootCoins, 20.0f);
    obj_mark_for_deletion(o);
}

static void add_pending(struct Object *obj) {
    int i;
    for (i = 0; i < MAX_PENDING; i++) {
        if (sPending[i].obj == NULL) {
            sPending[i].obj = obj;
            sPending[i].bhv = obj->behavior;
            sPending[i].framesLeft = SHOTGUN_FORCE_DELAY;
            return;
        }
    }
}

static int is_valid_target(struct Object *obj, const BehaviorScript *shotBhv[]) {
    u32 type = obj->oInteractType;
    int i;

    if (obj == gMarioObject || obj->activeFlags == ACTIVE_FLAG_DEACTIVATED || type == 0 || (type & IGNORED_TYPES)) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        if (obj->behavior == shotBhv[i]) {
            return 0;
        }
    }
    return 1;
}

int hiveshock_shotgun_fire(void) {
    struct MarioState *m = gMarioState;
    const BehaviorScript *own[3];
    Vec3f muzzle;
    f32 fx, fz;
    s16 yaw;
    int list;
    int hits = 0;

    if (sShells <= 0 || sCooldown > 0 || m == NULL || sCurrPlayMode != 0) {
        return 0;
    }
    sShells--;
    sCooldown = SHOTGUN_COOLDOWN;

    own[0] = segmented_to_virtual(bhvShotgunPickup);
    own[1] = segmented_to_virtual(bhvShotgunHeld);
    own[2] = segmented_to_virtual(bhvBananaPeel);

    yaw = m->faceAngle[1];
    fx = sins(yaw);
    fz = coss(yaw);
    mario_relative(MUZZLE_FORWARD, HOLD_RIGHT, HOLD_UP + 46.0f /* barrel height: ~2400 * scale */, muzzle);

    for (list = 0; list < NUM_OBJ_LISTS; list++) {
        struct ObjectNode *head = &gObjectLists[list];
        struct Object *obj = (struct Object *) head->next;
        while (obj != (struct Object *) head) {
            struct Object *next = (struct Object *) obj->header.next;
            if (is_valid_target(obj, own)) {
                f32 dx = obj->oPosX - muzzle[0];
                f32 dy = (obj->oPosY + 40.0f) - muzzle[1];
                f32 dz = obj->oPosZ - muzzle[2];
                f32 along = dx * fx + dz * fz;
                f32 lateral = absf(dx * fz - dz * fx);
                f32 width = SHOTGUN_SPREAD_BASE + along * SHOTGUN_SPREAD_RATE + obj->hitboxRadius * 0.5f;
                if (along > -50.0f && along < SHOTGUN_RANGE && lateral < width && absf(dy) < width + 80.0f) {
                    obj->oInteractStatus = ATTACK_FAST_ATTACK + (INT_STATUS_INTERACTED | INT_STATUS_WAS_ATTACKED);
                    with_current_object(obj, fx_hit_puff);
                    if ((obj->oInteractType & FORCE_KILL_TYPES) && !is_boss(obj->behavior)) {
                        add_pending(obj);
                    }
                    hits++;
                }
            }
            obj = next;
        }
    }

    // Custom effect: blast + smoke at the muzzle, camera-independent.
    // Layered blast so it sounds like a real shotgun: sharp crack + deep boom + wall-breaking rumble, and a kick.
    play_sound(SOUND_GENERAL2_BOBOMB_EXPLOSION, gGlobalSoundSource);
    play_sound(SOUND_GENERAL_BOWSER_BOMB_EXPLOSION, gGlobalSoundSource);
    play_sound(SOUND_GENERAL_WALL_EXPLOSION, gGlobalSoundSource);
    set_camera_shake_from_hit(SHAKE_ATTACK);
    set_environmental_camera_shake(SHAKE_ENV_EXPLOSION);
    if (m->action & ACT_FLAG_AIR) {
        m->vel[1] += 4.0f; // small recoil lift in mid-air
    }
    {
        int i;
        for (i = 0; i < 5; i++) {
            struct Object *smoke = spawn_object(m->marioObj, MODEL_BURN_SMOKE, bhvBlackSmokeUpward);
            if (smoke != NULL) {
                smoke->oPosX = muzzle[0] + fx * 40.0f * i;
                smoke->oPosY = muzzle[1];
                smoke->oPosZ = muzzle[2] + fz * 40.0f * i;
            }
        }
    }
    with_current_object(m->marioObj, fx_hit_puff);
    if (hits > 0) {
        play_sound(SOUND_ACTION_HIT, gGlobalSoundSource);
    }
    return 1;
}

// ---- Per-frame -----------------------------------------------------------------------------------------

void hiveshock_shotgun_update(void) {
    int i;

    if (gMarioState == NULL || gMarioObject == NULL || sCurrPlayMode != 0) {
        return;
    }

    if (sCooldown > 0) {
        sCooldown--;
    }

    // Keep the held model alive (objects are wiped on level changes).
    if (sShells > 0) {
        if (sHeld == NULL || sHeld->activeFlags == ACTIVE_FLAG_DEACTIVATED
            || sHeld->behavior != segmented_to_virtual(bhvShotgunHeld)) {
            struct GraphNode *node = shotgun_get_graph_node();
            sHeld = node != NULL ? spawn_object(gMarioObject, MODEL_NONE, bhvShotgunHeld) : NULL;
            if (sHeld != NULL) {
                sHeld->header.gfx.sharedChild = node;
            }
        }
        if (gPlayer1Controller != NULL && (gPlayer1Controller->buttonPressed & L_TRIG)) {
            hiveshock_shotgun_fire();
        }
    }

    // Enemies whose own logic ignored the shot get removed after a short delay.
    for (i = 0; i < MAX_PENDING; i++) {
        struct Object *obj = sPending[i].obj;
        if (obj == NULL) {
            continue;
        }
        if (obj->activeFlags == ACTIVE_FLAG_DEACTIVATED || obj->behavior != sPending[i].bhv) {
            sPending[i].obj = NULL; // already dead (or the slot was reused)
        } else if (--sPending[i].framesLeft <= 0) {
            with_current_object(obj, fx_kill);
            sPending[i].obj = NULL;
        }
    }
}
