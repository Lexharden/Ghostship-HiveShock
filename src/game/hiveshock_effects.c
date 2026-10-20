#include <string.h>

#include "sm64.h"
#include "types.h"
#include "object_fields.h"
#include "object_constants.h"
#include "behavior_data.h"
#include "model_ids.h"
#include "sounds.h"
#include "engine/behavior_script.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "game/area.h"
#include "game/interaction.h"
#include "game/level_update.h"
#include "game/mario.h"
#include "game/object_helpers.h"
#include "game/object_list_processor.h"
#include "game/behavior_actions.h"
#include "game/hiveshock_banana.h"
#include "game/hiveshock_shotgun.h"
#include "game/hiveshock_effects.h"

#include "audio/external.h"

#define HS_MAX_TRACKED 48
#define HS_FPS         30

#define o gCurrentObject

// Objects spawned by HiveShock, so `clear_enemies` can remove only those.
static struct {
    struct Object *obj;
    const BehaviorScript *bhv;
} sTracked[HS_MAX_TRACKED];

static void track(struct Object *obj, const BehaviorScript *bhv) {
    int i;
    for (i = 0; i < HS_MAX_TRACKED; i++) {
        if (sTracked[i].obj == NULL || sTracked[i].obj->activeFlags == ACTIVE_FLAG_DEACTIVATED
            || sTracked[i].obj->behavior != sTracked[i].bhv) {
            sTracked[i].obj = obj;
            sTracked[i].bhv = bhv;
            return;
        }
    }
}

static void clear_tracked(void) {
    int i;
    for (i = 0; i < HS_MAX_TRACKED; i++) {
        struct Object *obj = sTracked[i].obj;
        if (obj != NULL && obj->activeFlags != ACTIVE_FLAG_DEACTIVATED && obj->behavior == sTracked[i].bhv) {
            obj_mark_for_deletion(obj);
        }
        sTracked[i].obj = NULL;
    }
}

// Spawns `model`/`bhv` `dist` units in front of Mario (+ `side` to his right), `height` above the floor.
// Returns NULL if there is no floor at that spot.
static struct Object *spawn_ahead(u32 model, const BehaviorScript *bhv, f32 dist, f32 side, f32 height, u32 behParam) {
    struct MarioState *m = gMarioState;
    struct Surface *floor;
    struct Object *obj;
    s16 yaw = m->faceAngle[1];
    f32 x = m->pos[0] + sins(yaw) * dist + sins(yaw + 0x4000) * side;
    f32 z = m->pos[2] + coss(yaw) * dist + coss(yaw + 0x4000) * side;
    f32 floorY = find_floor(x, m->pos[1] + 200.0f, z, &floor);

    if (floor == NULL || floorY <= FLOOR_LOWER_LIMIT + 1) {
        return NULL;
    }

    obj = spawn_object(gMarioObject, model, bhv);
    if (obj == NULL) {
        return NULL;
    }
    obj->oPosX = x;
    obj->oPosY = floorY + height;
    obj->oPosZ = z;
    obj->oHomeX = x;
    obj->oHomeY = obj->oPosY;
    obj->oHomeZ = z;
    obj->oMoveAngleYaw = yaw + 0x8000; // face Mario
    obj->oFaceAngleYaw = obj->oMoveAngleYaw;
    obj->parentObj = obj; // like level-script objects: behaviors (goomba, bobomb...) treat parentObj as their spawner
    obj->oBehParams = behParam;
    obj->oBehParams2ndByte = (behParam >> 16) & 0xFF;
    track(obj, bhv);
    return obj;
}

// ---- Chasing enemies ----------------------------------------------------------------------------------

#define POISON_MUSHROOM_SPEED    16.0f
#define POISON_MUSHROOM_LIFETIME (45 * HS_FPS)

void bhv_hive_poison_mushroom_loop(void) {
    struct MarioState *m = gMarioState;

    cur_obj_scale(1.6f);
    cur_obj_update_floor_and_walls();

    o->oAngleToMario = obj_angle_to_object(o, gMarioObject);
    cur_obj_rotate_yaw_toward(o->oAngleToMario, 0x500);
    o->oForwardVel = POISON_MUSHROOM_SPEED;
    cur_obj_move_standard(-78);

    // Hop over obstacles in its way.
    if ((o->oMoveFlags & OBJ_MOVE_HIT_WALL) && (o->oMoveFlags & OBJ_MOVE_ON_GROUND)) {
        o->oVelY = 35.0f;
    }

    // Left far behind: reappear close to Mario so it keeps chasing him.
    if (o->oDistanceToMario > 4000.0f) {
        o->oPosX = m->pos[0] - sins(m->faceAngle[1]) * 1200.0f;
        o->oPosZ = m->pos[2] - coss(m->faceAngle[1]) * 1200.0f;
        o->oPosY = m->pos[1] + 200.0f;
        o->oVelY = 0.0f;
    }

    if (o->oTimer % 6 == 0) {
        spawn_object(o, MODEL_BURN_SMOKE, bhvBlackSmokeUpward);
    }

    if (dist_between_objects(o, gMarioObject) < 90.0f && absf(o->oPosY - m->pos[1]) < 110.0f) {
        if (m->action & ACT_FLAG_ATTACKING) {
            // Mario can destroy it by punching, kicking or ground pounding it.
            spawn_mist_particles();
            obj_mark_for_deletion(o);
        } else {
            m->health = 0xFF;
            spawn_mist_particles();
            obj_mark_for_deletion(o);
        }
        return;
    }

    if (o->oTimer > POISON_MUSHROOM_LIFETIME) {
        spawn_mist_particles();
        obj_mark_for_deletion(o);
    }
}

// ---- Timed effects ------------------------------------------------------------------------------------

enum TimedEffect {
    FX_LOW_GRAVITY,
    FX_HIGH_GRAVITY,
    FX_SPEED_BOOST,
    FX_COUNT,
};

static s32 sTimers[FX_COUNT];

static void start_timed(enum TimedEffect fx, double duration, double fallbackSeconds) {
    double seconds = duration > 0.0 ? duration : fallbackSeconds;
    if (seconds > 120.0) {
        seconds = 120.0;
    }
    sTimers[fx] = (s32) (seconds * HS_FPS);
}

void hiveshock_effects_update(void) {
    struct MarioState *m = gMarioState;
    int i;

    hiveshock_shotgun_update();

    if (m == NULL || sCurrPlayMode != 0) {
        return;
    }

    for (i = 0; i < FX_COUNT; i++) {
        if (sTimers[i] > 0) {
            sTimers[i]--;
        }
    }

    if ((m->action & ACT_FLAG_AIR) && !(m->action & ACT_FLAG_SWIMMING)) {
        if (sTimers[FX_LOW_GRAVITY] > 0 && m->vel[1] < 0.0f) {
            m->vel[1] += 2.5f; // cancel most of gravity while falling, floaty descent
        } else if (sTimers[FX_LOW_GRAVITY] > 0) {
            m->vel[1] += 1.5f;
        }
        if (sTimers[FX_HIGH_GRAVITY] > 0) {
            m->vel[1] -= 3.0f;
            if (m->vel[1] < -90.0f) {
                m->vel[1] = -90.0f;
            }
        }
    }

    if (sTimers[FX_SPEED_BOOST] > 0 && (m->action & ACT_FLAG_MOVING) && m->forwardVel > 8.0f) {
        m->forwardVel *= 1.04f;
        if (m->forwardVel > 70.0f) {
            m->forwardVel = 70.0f;
        }
    }
}

// ---- Actions ------------------------------------------------------------------------------------------
//
// Every action is a row in sActions. To add one: write a `static void act_xxx(const HsArgs *a)` and add a
// row with its name and category. The Network > HiveShock menu, per-category toggles and test buttons are
// generated from this table, so nothing else needs to change.

typedef struct {
    double value;
    int hasValue;
    double duration;
    int count; // value clamped to 1..10 (spawn counts)
    int wedges; // value as health wedges (default 2)
} HsArgs;

typedef void (*HsFn)(const HsArgs *a);

typedef struct {
    const char *name;
    const char *category; // NULL = alias, hidden from the menu
    HsFn fn;
} HsAction;

static void give_cap(u32 flag, double duration, double fallbackSeconds) {
    struct MarioState *m = gMarioState;
    double seconds = duration > 0.0 ? duration : fallbackSeconds;
    m->flags |= flag | MARIO_CAP_ON_HEAD;
    if (m->capTimer < (u16) (seconds * HS_FPS)) {
        m->capTimer = (u16) (seconds * HS_FPS);
    }
    play_sound(SOUND_MENU_STAR_SOUND, gGlobalSoundSource);
}

static void spawn_enemy_group(u32 model, const BehaviorScript *bhv, int count, f32 dist, f32 height, u32 behParam) {
    int i;
    for (i = 0; i < count; i++) {
        f32 side = (i - (count - 1) / 2.0f) * 140.0f;
        spawn_ahead(model, bhv, dist, side, height, behParam);
    }
}

// Gameplay
static void act_banana(const HsArgs *a) { hiveshock_spawn_banana_from_viewer(); }
static void act_heal(const HsArgs *a) { gMarioState->healCounter += 4 * a->wedges; }
static void act_damage(const HsArgs *a) { gMarioState->hurtCounter += 4 * a->wedges; }
static void act_kill(const HsArgs *a) { gMarioState->health = 0xFF; }
static void act_lives_add(const HsArgs *a) {
    int lives = gMarioState->numLives + (a->hasValue ? (int) a->value : 1);
    gMarioState->numLives = (s8) (lives < 0 ? 0 : (lives > 99 ? 99 : lives));
    play_sound(SOUND_GENERAL_COLLECT_1UP, gGlobalSoundSource);
}
static void act_coins_add(const HsArgs *a) {
    gMarioState->numCoins += a->hasValue ? (int) a->value : 10;
    gMarioState->healCounter += 4;
    play_sound(SOUND_GENERAL_COIN, gGlobalSoundSource);
}

// Enemies
static void act_goomba(const HsArgs *a) { spawn_enemy_group(MODEL_GOOMBA, bhvGoomba, a->count, 450.0f, 0.0f, 0); }
static void act_goomba_army(const HsArgs *a) { spawn_enemy_group(MODEL_GOOMBA, bhvGoomba, 5, 500.0f, 0.0f, 0); }
static void act_goomba_huge(const HsArgs *a) { spawn_ahead(MODEL_GOOMBA, bhvGoomba, 600.0f, 0.0f, 0.0f, 1 << 16); }
static void act_bobomb(const HsArgs *a) { spawn_enemy_group(MODEL_BLACK_BOBOMB, bhvBobomb, a->count, 450.0f, 0.0f, 0); }
static void act_chuckya(const HsArgs *a) { spawn_ahead(MODEL_CHUCKYA, bhvHiveChuckya, 700.0f, 0.0f, 0.0f, 0); }
static void act_poison_mushroom(const HsArgs *a) {
    spawn_enemy_group(MODEL_1UP, bhvHivePoisonMushroom, a->count, 900.0f, 0.0f, 0);
}
static void act_flyguy(const HsArgs *a) { spawn_enemy_group(MODEL_FLYGUY, bhvFlyGuy, a->count, 500.0f, 250.0f, 0); }
static void act_amp(const HsArgs *a) { spawn_enemy_group(MODEL_AMP, bhvHomingAmp, a->count, 600.0f, 120.0f, 0); }
static void act_koopa_shell(const HsArgs *a) { spawn_ahead(MODEL_KOOPA_SHELL, bhvKoopaShell, 350.0f, 0.0f, 0.0f, 0); }
static void act_bowling_ball(const HsArgs *a) {
    spawn_ahead(MODEL_BOWLING_BALL, bhvFreeBowlingBall, 500.0f, 0.0f, 1200.0f, 0);
}
static void act_bowling_rain(const HsArgs *a) {
    int i;
    for (i = 0; i < 6; i++) {
        f32 dist = 200.0f + (f32) (random_u16() % 700);
        f32 side = ((f32) (random_u16() % 1000) - 500.0f);
        spawn_ahead(MODEL_BOWLING_BALL, bhvFreeBowlingBall, dist, side, 1200.0f + 250.0f * i, 0);
    }
}
static void act_clear_enemies(const HsArgs *a) { clear_tracked(); }

// Shotgun
static void act_shotgun(const HsArgs *a) { hiveshock_spawn_shotgun_from_viewer(); }
static void act_shotgun_give(const HsArgs *a) { hiveshock_shotgun_give(a->hasValue ? (int) a->value : 8); }
static void act_shotgun_fire(const HsArgs *a) { hiveshock_shotgun_fire(); }

// Mario
static void act_burn(const HsArgs *a) {
    gMarioState->marioObj->oMarioBurnTimer = 0;
    drop_and_set_mario_action(gMarioState, ACT_BURNING_JUMP, 0);
    gMarioState->hurtCounter += 4;
}
static void act_shock(const HsArgs *a) {
    drop_and_set_mario_action(gMarioState, ACT_SHOCKED, 0);
    gMarioState->hurtCounter += 4;
}
static void act_squish(const HsArgs *a) { drop_and_set_mario_action(gMarioState, ACT_SQUISHED, 0); }
static void act_knockback(const HsArgs *a) {
    drop_and_set_mario_action(gMarioState, ACT_THROWN_FORWARD, 0);
    gMarioState->forwardVel = a->hasValue ? (f32) a->value : 40.0f;
    gMarioState->vel[1] = 30.0f;
}
static void act_launch(const HsArgs *a) {
    set_mario_action(gMarioState, ACT_TWIRLING, 0);
    gMarioState->vel[1] = a->hasValue ? (f32) a->value : 90.0f;
    play_sound(SOUND_MARIO_YAHOO, gMarioState->marioObj->header.gfx.cameraToObject);
}
static void act_ground_pound(const HsArgs *a) {
    struct MarioState *m = gMarioState;
    if (m->action & ACT_FLAG_AIR) {
        set_mario_action(m, ACT_GROUND_POUND, 0);
    } else {
        m->vel[1] = 60.0f;
        set_mario_action(m, ACT_TWIRLING, 0);
    }
}
static void act_blow(const HsArgs *a) {
    drop_and_set_mario_action(gMarioState, ACT_GETTING_BLOWN, 0);
    gMarioState->forwardVel = 30.0f;
}
static void act_cap_off(const HsArgs *a) { mario_blow_off_cap(gMarioState, 25.0f); }

// Power-ups / timed
static void act_metal_cap(const HsArgs *a) { give_cap(MARIO_METAL_CAP, a->duration, 20.0); }
static void act_wing_cap(const HsArgs *a) { give_cap(MARIO_WING_CAP, a->duration, 30.0); }
static void act_vanish_cap(const HsArgs *a) { give_cap(MARIO_VANISH_CAP, a->duration, 20.0); }
static void act_low_gravity(const HsArgs *a) { start_timed(FX_LOW_GRAVITY, a->duration, 20.0); }
static void act_high_gravity(const HsArgs *a) { start_timed(FX_HIGH_GRAVITY, a->duration, 15.0); }
static void act_speed_boost(const HsArgs *a) { start_timed(FX_SPEED_BOOST, a->duration, 20.0); }

static const HsAction sActions[] = {
    { "banana", "Gameplay", act_banana },
    { "banana_peel", NULL, act_banana },
    { "heal", "Gameplay", act_heal },
    { "damage", "Gameplay", act_damage },
    { "kill", "Gameplay", act_kill },
    { "instakill", NULL, act_kill },
    { "lives_add", "Gameplay", act_lives_add },
    { "coins_add", "Gameplay", act_coins_add },

    { "goomba", "Enemies", act_goomba },
    { "goomba_army", "Enemies", act_goomba_army },
    { "goomba_huge", "Enemies", act_goomba_huge },
    { "bobomb", "Enemies", act_bobomb },
    { "chuckya", "Enemies", act_chuckya },
    { "poison_mushroom", "Enemies", act_poison_mushroom },
    { "flyguy", "Enemies", act_flyguy },
    { "amp", "Enemies", act_amp },
    { "koopa_shell", "Enemies", act_koopa_shell },
    { "bowling_ball", "Enemies", act_bowling_ball },
    { "bowling_rain", "Enemies", act_bowling_rain },
    { "clear_enemies", "Enemies", act_clear_enemies },

    { "shotgun", "Weapons", act_shotgun },
    { "shotgun_give", "Weapons", act_shotgun_give },
    { "shotgun_fire", "Weapons", act_shotgun_fire },

    { "burn", "Mario", act_burn },
    { "shock", "Mario", act_shock },
    { "squish", "Mario", act_squish },
    { "knockback", "Mario", act_knockback },
    { "push", NULL, act_knockback },
    { "launch", "Mario", act_launch },
    { "ground_pound", "Mario", act_ground_pound },
    { "blow", "Mario", act_blow },
    { "cap_off", "Mario", act_cap_off },

    { "metal_cap", "Power-ups", act_metal_cap },
    { "wing_cap", "Power-ups", act_wing_cap },
    { "vanish_cap", "Power-ups", act_vanish_cap },
    { "low_gravity", "Power-ups", act_low_gravity },
    { "high_gravity", "Power-ups", act_high_gravity },
    { "speed_boost", "Power-ups", act_speed_boost },
};

#define NUM_ACTIONS ((int) (sizeof(sActions) / sizeof(sActions[0])))

int hiveshock_action_count(void) {
    return NUM_ACTIONS;
}

const char *hiveshock_action_name(int index) {
    return (index >= 0 && index < NUM_ACTIONS) ? sActions[index].name : NULL;
}

const char *hiveshock_action_category(int index) {
    return (index >= 0 && index < NUM_ACTIONS) ? sActions[index].category : NULL;
}

int hiveshock_action_exists(const char *action) {
    int i;
    for (i = 0; i < NUM_ACTIONS; i++) {
        if (strcmp(sActions[i].name, action) == 0) {
            return 1;
        }
    }
    return 0;
}

int hiveshock_can_run(void) {
    return gMarioState != NULL && gMarioObject != NULL && gCurrentArea != NULL && sCurrPlayMode == 0;
}

int hiveshock_run_action(const char *action, double value, int hasValue, double duration) {
    HsArgs args;
    int i;

    if (action == NULL) {
        return 0;
    }
    for (i = 0; i < NUM_ACTIONS; i++) {
        if (strcmp(sActions[i].name, action) != 0) {
            continue;
        }
        if (!hiveshock_can_run()) {
            return 1; // known, but nothing to do outside gameplay
        }
        args.value = value;
        args.hasValue = hasValue;
        args.duration = duration;
        args.count = hasValue ? (int) value : 1;
        args.count = args.count < 1 ? 1 : (args.count > 10 ? 10 : args.count);
        args.wedges = hasValue ? (int) value : 2;
        sActions[i].fn(&args);
        return 1;
    }
    return 0;
}
