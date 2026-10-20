
#include "sm64.h"
#include "types.h"
#include "geo_commands.h"
#include "object_fields.h"
#include "object_constants.h"
#include "behavior_data.h"
#include "sounds.h"
#include "engine/geo_layout.h"
#include "engine/graph_node.h"
#include "engine/math_util.h"
#include "game/area.h"
#include "game/camera.h"
#include "game/level_update.h"
#include "game/mario.h"
#include "game/memory.h"
#include "game/object_helpers.h"
#include "game/object_list_processor.h"
#include "game/hiveshock_banana.h"

#include "audio/external.h"
#include "game/spawn_sound.h"

#define o gCurrentObject

// Fast64 export, compiled directly into the executable (no OTR/model ID needed).
#include "actors/BananaPeel_N64/model.inc.c"
#include "actors/BananaPeel_N64/geo.inc.c"

#define BANANA_LAND_DISTANCE 450.0f // extra distance ahead of Mario, on top of his predicted movement
#define BANANA_FLIGHT_FRAMES 24
#define BANANA_GRAVITY       -3.0f
#define BANANA_TRIGGER_DIST  90.0f
#define BANANA_TRIGGER_DY    80.0f
#define BANANA_LIFETIME      450
#define BANANA_SLIDE_SPEED   80.0f
#define BANANA_SCALE         0.025f // Fast64 export is ~6000 units wide

enum BananaPeelAction {
    BANANA_ACT_FLYING,
    BANANA_ACT_TRAP,
};

// Geo layout nodes are allocated once from a static pool (the level pool is
// resized/freed on level changes, so it can't hold a persistent node).
static u8 sBananaPoolMem[1024] __attribute__((aligned(16)));
static struct AllocOnlyPool sBananaPool;
static struct GraphNode *sBananaGraphNode = NULL;

static struct GraphNode *banana_get_graph_node(void) {
    if (sBananaGraphNode == NULL) {
        sBananaPool.totalSpace = sizeof(sBananaPoolMem);
        sBananaPool.usedSpace = 0;
        sBananaPool.startPtr = sBananaPoolMem;
        sBananaPool.freePtr = sBananaPoolMem;
        sBananaGraphNode = process_geo_layout(&sBananaPool, (void *) BananaPeel_N64_geo);
    }
    return sBananaGraphNode;
}

static void banana_peel_act_flying(void) {
    o->oFaceAngleYaw += 0x800;
    cur_obj_update_floor_and_walls();
    cur_obj_move_standard(-78);

    if (o->oMoveFlags & OBJ_MOVE_ON_GROUND) {
        o->oForwardVel = 0.0f;
        o->oVelY = 0.0f;
        cur_obj_play_sound_2(SOUND_GENERAL_SOFT_LANDING);
        o->oAction = BANANA_ACT_TRAP;
        o->oTimer = 0;
    }
}

static void banana_peel_act_trap(void) {
    cur_obj_update_floor_and_walls();
    cur_obj_move_standard(-78);

    if (dist_between_objects(o, gMarioObject) < BANANA_TRIGGER_DIST
        && absf(o->oPosY - gMarioState->pos[1]) < BANANA_TRIGGER_DY) {
        set_mario_action(gMarioState, ACT_STOMACH_SLIDE, 0);
        mario_set_forward_vel(gMarioState, BANANA_SLIDE_SPEED);
        play_sound(SOUND_MARIO_OOOF2, gMarioState->marioObj->header.gfx.cameraToObject);
        spawn_mist_particles();
        obj_mark_for_deletion(o);
    } else if (o->oTimer > BANANA_LIFETIME) {
        obj_mark_for_deletion(o);
    }
}

void bhv_banana_peel_loop(void) {
    switch (o->oAction) {
        case BANANA_ACT_FLYING:
            banana_peel_act_flying();
            break;
        case BANANA_ACT_TRAP:
            banana_peel_act_trap();
            break;
    }
}

// Throws `prop` from the camera so it lands in front of Mario after BANANA_FLIGHT_FRAMES (shared by banana and shotgun).
void hiveshock_launch_prop_from_viewer(struct Object *prop) {
    f32 targetX, targetZ, targetY, dx, dz, dy, leadDist;
    const f32 T = BANANA_FLIGHT_FRAMES;

    // Lead the target by how far Mario will travel during the flight so it lands in front of a running Mario.
    leadDist = BANANA_LAND_DISTANCE + (gMarioState->forwardVel > 0.0f ? gMarioState->forwardVel * T : 0.0f);
    targetX = gMarioState->pos[0] + sins(gMarioState->faceAngle[1]) * leadDist;
    targetZ = gMarioState->pos[2] + coss(gMarioState->faceAngle[1]) * leadDist;
    targetY = gMarioState->pos[1];

    prop->oPosX = gLakituState.pos[0];
    prop->oPosY = gLakituState.pos[1];
    prop->oPosZ = gLakituState.pos[2];

    dx = targetX - prop->oPosX;
    dz = targetZ - prop->oPosZ;
    dy = targetY - prop->oPosY;

    prop->oMoveAngleYaw = atan2s(dz, dx);
    prop->oForwardVel = sqrtf(dx * dx + dz * dz) / T;
    // y(T) = y0 + T*vy0 + g*T*(T+1)/2
    prop->oVelY = (dy - BANANA_GRAVITY * T * (T + 1.0f) / 2.0f) / T;
    prop->oGravity = BANANA_GRAVITY;
    prop->oFriction = 0.8f;
    prop->oBuoyancy = 1.0f;

    play_sound(SOUND_OBJ_MRI_SHOOT, gGlobalSoundSource);
}

void hiveshock_spawn_banana_from_viewer(void) {
    struct Object *banana;
    struct GraphNode *node;

    if (gMarioObject == NULL || gMarioState == NULL || gCurrentArea == NULL
        || sCurrPlayMode != 0 /* PLAY_MODE_NORMAL */) {
        return;
    }

    node = banana_get_graph_node();
    if (node == NULL) {
        return;
    }

    banana = spawn_object(gMarioObject, MODEL_NONE, bhvBananaPeel);
    if (banana == NULL) {
        return;
    }
    banana->header.gfx.sharedChild = node;
    obj_scale(banana, BANANA_SCALE);
    hiveshock_launch_prop_from_viewer(banana);
}
