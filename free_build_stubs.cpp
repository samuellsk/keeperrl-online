// Stub implementations of the Steamworks-dependent classes for builds made
// without the proprietary Steam SDK (i.e. without STEAMWORKS=true). The rest of
// the engine references these classes unconditionally, so the free build needs
// no-op definitions to link. Guarded so a real Steam build (which compiles
// steam_input.cpp / steam_achievements.cpp) is unaffected.
#ifndef USE_STEAMWORKS

#include "stdafx.h"
#include "steam_input.h"
#include "steam_achievements.h"
#include "achievement_id.h"

SteamAchievements::SteamAchievements() {}
void SteamAchievements::achieve(AchievementId) {}

void MySteamInput::init() {}
void MySteamInput::detectControllers() {}
void MySteamInput::showBindingScreen() {}
void MySteamInput::showFloatingKeyboard(Rectangle) {}
void MySteamInput::dismissFloatingKeyboard() {}
optional<ControllerKey> MySteamInput::getEvent() { return none; }
void MySteamInput::setGameActionLayer(GameActionLayer) {}
pair<double, double> MySteamInput::getJoyPos(ControllerJoy) { return {0.0, 0.0}; }
void MySteamInput::runFrame() {}
bool MySteamInput::isPressed(ControllerKey) { return false; }
bool MySteamInput::isRunningOnDeck() { return false; }
optional<FilePath> MySteamInput::getGlyph(ControllerKey) { return none; }

#endif
