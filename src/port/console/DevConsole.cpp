#include "DevConsole.h"
#include "port/ShipCompat.h"

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/window/gui/ConsoleWindow.h>
#include <ship/debug/Console.h>

#define CMD_REGISTER ShipCompat::GetConsole()->AddCommand
// TODO: Commands should be using the output passed in.
#define ERROR_MESSAGE                                                                                              \
    std::reinterpret_pointer_cast<Ship::ConsoleWindow>(ShipCompat::GetWindow()->GetGui()->GetGuiWindow("Console")) \
        ->SendErrorMessage
#define INFO_MESSAGE                                                                                               \
    std::reinterpret_pointer_cast<Ship::ConsoleWindow>(ShipCompat::GetWindow()->GetGui()->GetGuiWindow("Console")) \
        ->SendInfoMessage

extern "C" {
void warp_special(int32_t arg);
void game_request_reset(void);
void hiveshock_spawn_banana_from_viewer(void);
int hiveshock_run_action(const char* action, double value, int hasValue, double duration);
extern int16_t sCurrPlayMode; // level_update.h
}

static bool ResetHandler(std::shared_ptr<Ship::Console> Console, std::vector<std::string> args, std::string* output) {
    if (args.size() > 1 && args[1] == "soft") {
        // Do not reset unless currently playing a level
        if (sCurrPlayMode != 0) { // PLAY_MODE_NORMAL
            ERROR_MESSAGE("sCurrPlayMode != PLAY_MODE_NORMAL");
            return 1;
        }
        warp_special(-8);
        return 0;
    }

    game_request_reset();
    return 0;
}

// Entry point for HiveShock gift events: "banana" / "banana_peel".
static bool BananaHandler(std::shared_ptr<Ship::Console> Console, std::vector<std::string> args, std::string* output) {
    hiveshock_spawn_banana_from_viewer();
    return 0;
}

static bool HsHandler(std::shared_ptr<Ship::Console> Console, std::vector<std::string> args, std::string* output) {
    if (args.size() < 2) {
        ERROR_MESSAGE("usage: hs <action> [value] [duration]");
        return 1;
    }
    const bool hasValue = args.size() > 2;
    const double value = hasValue ? atof(args[2].c_str()) : 0.0;
    const double duration = args.size() > 3 ? atof(args[3].c_str()) : 0.0;
    if (!hiveshock_run_action(args[1].c_str(), value, hasValue, duration)) {
        ERROR_MESSAGE("unknown HiveShock action");
        return 1;
    }
    return 0;
}

void DevConsole_Init(void) {
    CMD_REGISTER("hs", { HsHandler, "HiveShock test: `hs <action> [value] [duration]`, e.g. `hs goomba_army`." });
    CMD_REGISTER("banana", { BananaHandler, "HiveShock: a viewer throws a banana peel in front of Mario." });
    CMD_REGISTER("banana_peel", { BananaHandler, "Alias of `banana`." });
    CMD_REGISTER("reset", { ResetHandler,
                            "Reboots the game like a console reset. `reset soft` only returns to the title screen." });
}