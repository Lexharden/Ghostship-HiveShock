// hive_chuckya.inc.c
// HiveShock Chuckya: chases Mario everywhere, ignoring walls and terrain (it climbs), and after throwing him
// it waits a moment (chuckya_act_3) and goes right back after him. Grab/throw logic is the vanilla Chuckya's.

#define HIVE_CHUCKYA_SPEED     30.0f
#define HIVE_CHUCKYA_MAX_BOOST 40.0f // extra speed when far behind, so it always catches up
#define HIVE_CHUCKYA_CLIMB     22.0f // max vertical speed

static void hive_chuckya_act_chase(void) {
    f32 dx = gMarioState->pos[0] - o->oPosX;
    f32 dz = gMarioState->pos[2] - o->oPosZ;
    f32 dy = gMarioState->pos[1] - o->oPosY;
    f32 lateral = sqrtf(dx * dx + dz * dz);
    f32 boost = lateral / 100.0f;
    f32 speed, climb;
    s16 target = atan2s(dz, dx);

    if (boost > HIVE_CHUCKYA_MAX_BOOST) {
        boost = HIVE_CHUCKYA_MAX_BOOST;
    }
    speed = HIVE_CHUCKYA_SPEED + boost;

    o->oAngleToMario = target;
    cur_obj_rotate_yaw_toward(target, 0x800);

    if (lateral > 60.0f) {
        f32 step = lateral < speed ? lateral : speed;
        o->oPosX += dx / lateral * step;
        o->oPosZ += dz / lateral * step;
    }

    // Climb walls / follow Mario up and down regardless of the geometry in between.
    climb = dy * 0.2f;
    if (climb > HIVE_CHUCKYA_CLIMB) {
        climb = HIVE_CHUCKYA_CLIMB;
    } else if (climb < -HIVE_CHUCKYA_CLIMB) {
        climb = -HIVE_CHUCKYA_CLIMB;
    }
    o->oPosY += climb;

    o->oForwardVel = 0.0f;
    o->oVelY = 0.0f;
    cur_obj_init_animation_with_sound(4);
    if (lateral > 60.0f) {
        cur_obj_play_sound_1(SOUND_AIR_CHUCKYA_MOVE);
    }
}

static void hive_chuckya_move(void) {
    cur_obj_update_floor_and_walls();
    if (o->oAction == 0) {
        hive_chuckya_act_chase();
    } else {
        cur_obj_call_action_function(sChuckyaActions);
        cur_obj_move_standard(-30);
    }
    if (o->oInteractStatus & INT_STATUS_GRABBED_MARIO) {
        o->oAction = 1;
        o->oChuckyaUnk88 = 1;
        cur_obj_play_sound_2(SOUND_OBJ_UNKNOWN3);
    }
}

void bhv_hive_chuckya_loop(void) {
    f32 sp2C = 20.0f;
    f32 sp28 = 50.0f;

    cur_obj_scale(2.0f);
    o->oInteractionSubtype |= INT_SUBTYPE_GRABS_MARIO;

    switch (o->oHeldState) {
        case HELD_FREE:
            hive_chuckya_move();
            break;
        case HELD_HELD:
            cur_obj_unrender_set_action_and_anim(2, 0);
            break;
        case HELD_THROWN:
        case HELD_DROPPED:
            cur_obj_get_thrown_or_placed(sp2C, sp28, 2);
            break;
    }

    o->oInteractStatus = 0;
}
