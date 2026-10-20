#pragma once

// HiveShock bridge (https://hiveshock.yafel.dev). Mirrors the other HiveShock game integrations:
// the bridge opens a short-lived TCP connection to GAME_PORT (default 43000) and sends one JSON
// line per effect (`{"action":"banana","user":"viewer"}`). Game events (`player_death`) are sent
// back to EVENT_PORT (default 43002).
//
// This module has no dependency on the Satella/USE_NETWORKING options: it uses plain sockets and is
// always built (except on consoles without BSD sockets, where it compiles to a no-op).
//
// Adding an action does not touch this file: register it in src/game/hiveshock_effects.c (sActions).
//
// CVars (all under CVAR_REMOTE("HiveShock.*")):
//   Enabled (0)      Master switch; the listener starts at boot when set.
//   Port (43000)     TCP port for effects.   EventPort (43002)  TCP port for game events.
//   AllowLAN (0)     Listen on 0.0.0.0 instead of 127.0.0.1 (effects are unauthenticated unless Token is set).
//   Token ("")       If non-empty, commands must carry a matching "token" field.
//   MinGapMs (0)     Minimum time between executed effects (extra queueing on top of the bridge's own gap).
//   Cat.<name> (1)   Per-category enable: Gameplay, Enemies, Mario, Power-ups.

#include <cstdint>
#include <string>

enum class HiveShockState { Disabled, Listening, Error, Unsupported };

struct HiveShockStatus {
    HiveShockState state = HiveShockState::Disabled;
    std::string lastError;
    std::string lastAction; // last executed action
    std::string lastUser;
    std::string lastRejected; // reason of the last rejected command
    uint64_t received = 0;    // valid commands accepted into the queue
    uint64_t executed = 0;
    uint64_t rejected = 0; // unknown / disabled category / bad token / queue full / bad JSON
    uint32_t queued = 0;
    uint32_t clients = 0; // connections currently open
    int port = 0;
    bool lan = false;
    int deaths = 0;
};

// Starts, stops or restarts the listener according to the CVars. Safe to call at any time (main thread).
void HiveShock_Apply(void);

// Snapshot of the module state for the UI.
HiveShockStatus HiveShock_GetStatus(void);

// Queues an action as if the bridge had sent it (menu test buttons). Runs on the next game frame.
void HiveShock_QueueTest(const std::string& action);

// Whether the platform supports the listener at all.
bool HiveShock_IsSupported(void);
