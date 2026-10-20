#include "GhostshipMenu.h"
#include "port/mods/HiveShock.h"

#include <map>
#include <string>
#include <vector>

extern "C" {
int hiveshock_action_count(void);
const char* hiveshock_action_name(int index);
const char* hiveshock_action_category(int index);
}

namespace GhostshipGui {

extern std::shared_ptr<GhostshipMenu> mGhostshipMenu;
using namespace UIWidgets;

static TextOptions* TextOpts(WidgetInfo& info) {
    return static_cast<TextOptions*>(info.options.get());
}

// Network menu. HiveShock gets its own sidebar; the "Effects" sidebar is generated from the action registry
// in src/game/hiveshock_effects.c, so new actions show up here (toggle + test button) without touching this file.
void GhostshipMenu::AddMenuNetwork() {
    AddMenuEntry("Network", CVAR_SETTING("Menu.NetworkSidebarSection"));

    // --- HiveShock: connection ------------------------------------------------------------------------------
    AddSidebarEntry("Network", "HiveShock", 2);
    WidgetPath path = { "Network", "HiveShock", SECTION_COLUMN_1 };

    AddWidget(path, "HiveShock", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Status", WIDGET_TEXT)
        .HideInSearch(true)
        .Options(TextOptions())
        .PreFunc([](WidgetInfo& info) {
            const HiveShockStatus st = HiveShock_GetStatus();
            switch (st.state) {
                case HiveShockState::Listening:
                    info.name = "Status: listening on port " + std::to_string(st.port) + (st.lan ? " (LAN)" : " (this PC only)");
                    TextOpts(info)->color = Colors::Green;
                    break;
                case HiveShockState::Error:
                    info.name = "Status: error - " + st.lastError;
                    TextOpts(info)->color = Colors::Red;
                    break;
                case HiveShockState::Unsupported:
                    info.name = "Status: not supported on this platform";
                    TextOpts(info)->color = Colors::Orange;
                    break;
                default:
                    info.name = "Status: disabled";
                    TextOpts(info)->color = Colors::Gray;
                    break;
            }
        });
    AddWidget(path, "Enable HiveShock", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_REMOTE("HiveShock.Enabled"))
        .Options(CheckboxOptions().Tooltip(
            "Listens for stream effects from the HiveShock bridge. Starts automatically on launch when enabled."))
        .Callback([](WidgetInfo&) { HiveShock_Apply(); });
    AddWidget(path, "Allow LAN connections", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_REMOTE("HiveShock.AllowLAN"))
        .Options(CheckboxOptions().Tooltip(
            "Accept effects from other devices on your network (phone companion, another PC). "
            "Anyone on the network can trigger effects unless a token is set (CVar gRemote.HiveShock.Token)."))
        .Callback([](WidgetInfo&) { HiveShock_Apply(); });
    AddWidget(path, "Effects port: %d", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_REMOTE("HiveShock.Port"))
        .Options(IntSliderOptions().Min(1024).Max(65535).DefaultValue(43000).Tooltip(
            "TCP port the bridge sends effects to (GAME_PORT in HiveShock, default 43000)."))
        .Callback([](WidgetInfo&) { HiveShock_Apply(); });
    AddWidget(path, "Events port: %d", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_REMOTE("HiveShock.EventPort"))
        .Options(IntSliderOptions().Min(1024).Max(65535).DefaultValue(43002).Tooltip(
            "TCP port on this PC where game events (deaths) are sent back (EVENT_PORT, default 43002)."));
    AddWidget(path, "Minimum gap between effects: %d ms", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_REMOTE("HiveShock.MinGapMs"))
        .Options(IntSliderOptions().Min(0).Max(5000).DefaultValue(0).Tooltip(
            "Extra spacing between queued effects so a burst of gifts does not stack in one frame."));

    // --- HiveShock: live stats ------------------------------------------------------------------------------
    path.column = SECTION_COLUMN_2;
    AddWidget(path, "Activity", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Stats", WIDGET_TEXT)
        .HideInSearch(true)
        .Options(TextOptions())
        .PreFunc([](WidgetInfo& info) {
            const HiveShockStatus st = HiveShock_GetStatus();
            info.name = "Received: " + std::to_string(st.received) + "   Executed: " + std::to_string(st.executed) +
                        "   Rejected: " + std::to_string(st.rejected) + "\nQueued: " + std::to_string(st.queued) +
                        "   Open connections: " + std::to_string(st.clients) + "\nDeaths reported: " +
                        std::to_string(st.deaths);
        });
    AddWidget(path, "Last effect", WIDGET_TEXT)
        .HideInSearch(true)
        .Options(TextOptions())
        .PreFunc([](WidgetInfo& info) {
            const HiveShockStatus st = HiveShock_GetStatus();
            info.name = st.lastAction.empty() ? "Last effect: -" : "Last effect: " + st.lastAction + " <- " + st.lastUser;
        });
    AddWidget(path, "Last rejected", WIDGET_TEXT)
        .HideInSearch(true)
        .Options(TextOptions().Color(Colors::Orange))
        .PreFunc([](WidgetInfo& info) {
            const HiveShockStatus st = HiveShock_GetStatus();
            info.isHidden = st.lastRejected.empty();
            info.name = "Last rejected: " + st.lastRejected;
        });
    AddWidget(path, "Effects are only applied while Mario is playable (not in menus or cutscenes); "
                    "they wait in the queue until then.",
              WIDGET_TEXT)
        .HideInSearch(true)
        .Options(TextOptions().Color(Colors::Gray));

    // --- HiveShock: effects (generated from the registry) --------------------------------------------------
    AddSidebarEntry("Network", "HiveShock Effects", 2);
    WidgetPath fx = { "Network", "HiveShock Effects", SECTION_COLUMN_1 };

    std::vector<std::string> categoryOrder;
    std::map<std::string, std::vector<std::string>> byCategory;
    for (int i = 0; i < hiveshock_action_count(); i++) {
        const char* category = hiveshock_action_category(i);
        if (category == nullptr) {
            continue; // alias
        }
        if (!byCategory.contains(category)) {
            categoryOrder.push_back(category);
        }
        byCategory[category].push_back(hiveshock_action_name(i));
    }

    int index = 0;
    for (const auto& category : categoryOrder) {
        fx.column = (index++ % 2 == 0) ? SECTION_COLUMN_1 : SECTION_COLUMN_2;
        AddWidget(fx, category, WIDGET_SEPARATOR_TEXT);
        AddWidget(fx, "Allow " + category + " effects", WIDGET_CVAR_CHECKBOX)
            .CVar(strdup((std::string(CVAR_REMOTE("HiveShock.Cat.")) + category).c_str()))
            .Options(CheckboxOptions().DefaultValue(true).Tooltip(
                strdup(("When off, the bridge's \"" + category + "\" effects are rejected.").c_str())));
        for (const auto& action : byCategory[category]) {
            AddWidget(fx, "Test: " + action, WIDGET_BUTTON)
                .Options(ButtonOptions().Size(Sizes::Inline).Tooltip("Queues this effect as if a viewer sent it."))
                .Callback([action](WidgetInfo&) { HiveShock_QueueTest(action); });
        }
    }
}

} // namespace GhostshipGui
