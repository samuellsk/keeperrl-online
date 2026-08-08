/* Copyright (C) 2013-2014 Michal Brzozowski (rusolis@poczta.fm)

   This file is part of KeeperRL.

   KeeperRL is free software; you can redistribute it and/or modify it under the terms of the
   GNU General Public License as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   KeeperRL is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
   even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along with this program.
   If not, see http://www.gnu.org/licenses/ . */

#pragma once

#include "enums.h"
#include "util.h"
#include "debug.h"
#include "animation_id.h"
#include "gender.h"
#include "fx_info.h"
#include "creature_experience_info.h"
#include "enum_variant.h"
#include "unique_entity.h"
#include "view_id.h"
#include "campaign_menu_index.h"
#include "keybinding.h"

class CreatureView;
class Level;
class Jukebox;
class ProgressMeter;
class PlayerInfo;
struct ItemInfo;
struct CreatureInfo;
class Sound;
class Campaign;
class Options;
class RetiredGames;
class ScrollPosition;
class FilePath;
class ModInfo;
class UserInput;
struct Color;
struct ScriptedUIData;
struct ScriptedUIState;
namespace fx {
  class FXRenderer;
}
class FXViewManager;

enum class CampaignActionId {
  CANCEL,
  REROLL_MAP,
  UPDATE_MAP,
  CONFIRM,
  UPDATE_OPTION,
  SET_POSITION,
  CHANGE_WORLD_MAP
};

enum class PassableInfo {
  PASSABLE,
  NON_PASSABLE,
  STOPS_HERE,
  UNKNOWN
};

class CampaignAction : public EnumVariant<CampaignActionId, TYPES(OptionId, string, Vec2, int),
  ASSIGN(Vec2, CampaignActionId::SET_POSITION),
  ASSIGN(OptionId, CampaignActionId::UPDATE_OPTION),
  ASSIGN(int, CampaignActionId::CHANGE_WORLD_MAP)> {
    using EnumVariant::EnumVariant;
};

namespace RetiredChoices {
using Confirm = EmptyStruct<struct ConfirmTag>;
using Cancel = EmptyStruct<struct CancelTag>;
using Search = string;
using RetiredChoice = variant<Confirm, Cancel, Search>;
}

using RetiredChoices::RetiredChoice;

struct ModAction {
  int index;
  int actionId;
};

class View {
  public:
  View();
  virtual ~View();

  /** Does all the library specific init.*/
  virtual void initialize(unique_ptr<fx::FXRenderer>, unique_ptr<FXViewManager>) = 0;

  /** Resets the view before a new game.*/
  virtual void reset() = 0;

  /** Displays a splash screen in an active loop until \paramname{ready} is set to true in another thread.*/
  virtual void displaySplash(const ProgressMeter*, const TString& text,
      function<void()> cancelFun = nullptr) = 0;

  virtual void clearSplash() = 0;

  virtual void playVideo(const string& path) = 0;

  void doWithSplash(const TString& text, int totalProgress,
      function<void(ProgressMeter&)> fun, function<void()> cancelFun = nullptr);

  /** Shutdown routine.*/
  virtual void close() = 0;

  /** Reads the game state from \paramname{creatureView} and refreshes the display.*/
  virtual void refreshView() = 0;

  /** Returns real-time game mode speed measured in turns per millisecond. **/
  virtual double getGameSpeed() = 0;

  /** Reads the game state from \paramname{creatureView}. If \paramname{noRefresh} is set,
      won't trigger screen to refresh.*/
  virtual void updateView(CreatureView*, bool noRefresh) = 0;

  virtual void setScrollPos(Position) = 0;

  // RAR live PvP: make SPECTATOR mode INTERACTIVE (scroll instead of exit-on-any-key; only ESC exits). Used by
  // the invader's battlefield view. Default no-op for non-window views.
  virtual void rarSetInteractiveSpectator(bool) {}
  // RAR live PvP: return + clear the last command key pressed in interactive-spectator mode (0 if none). Lets the
  // invader's observer loop read command keys (stop / don't-chase) without the full input system.
  virtual int rarConsumeSpectatorKey() { return 0; }
  // RAR live PvP: queue keypresses instead of flushing/locking them per turn (real-time control mode).
  virtual void rarSetLivePvpInput(bool) {}
  // RAR live PvP: block game-speed changes (1/2/3/4) for the duration of a live battle -- the two games must
  // advance at the same rate, for the same reason pausing is blocked.
  virtual void rarSetSpeedLocked(bool) {}
  // RAR live PvP: pump input events WITHOUT drawing. refreshView() does both, so throttling it for performance
  // also throttled input (a click went unseen for several turns). This is the cheap half, safe to call per turn.
  virtual void rarPumpEvents() {}
  // RAR live PvP diagnostic: real-time ms when the last map click was pushed into the input queue (0 = none).
  // Consuming it lets the player code report how long the click actually took to reach the creature's turn.
  virtual int rarConsumeClickMs() { return 0; }
  // RAR live PvP: is a player order waiting in the queue? Lets the game loop hand the controlled creature its turn
  // immediately instead of making the order wait for the slow real-time turn cadence (~2.8 turns/sec).
  virtual bool rarHasPendingInput() { return false; }
  virtual int rarGetActionCalls() { return 0; } // diagnostic: getAction() calls since the last map click

  /** Scrolls back to the center of the view on next refresh.*/
  virtual void resetCenter() = 0;

  /** Reads input in a non-blocking manner.*/
  virtual UserInput getAction() = 0;

  /** Returns whether a travel interrupt key is pressed at a given moment.*/
  virtual bool travelInterrupt() = 0;

  /** Lets the player choose a direction from the main 8. Returns none if the player cancelled the choice.*/
  virtual optional<Vec2> chooseDirection(Vec2 playerPos, const TString& message) = 0;

  using TargetResult = variant<none_t, Vec2, Keybinding>;
  /** Lets the player choose a target position. Returns none if the player cancelled the choice.*/
  virtual TargetResult chooseTarget(Vec2 playerPos, TargetType, Table<PassableInfo> passable,
      const TString& message, optional<Keybinding> cycleKey) = 0;

  /** Asks the player a yer-or-no question.*/
  bool yesOrNoPrompt(const TString& message, optional<ViewIdList> = none, bool defaultNo = false,
      ScriptedUIId = "yes_or_no");
  optional<int> multiChoice(const TString& message, const vector<TString>&);

  void windowedMessage(ViewIdList, const TString& message);

  /** Draws a window with some text. The text is formatted to fit the window.*/
  void presentText(const optional<TString>& title, const TString& text);
  void presentTextCenter(const TString& text); // like presentText but center-aligned, no title
  void presentTextBelow(const optional<TString>& title, const TString& text);

  virtual void scriptedUI(ScriptedUIId, const ScriptedUIData&, ScriptedUIState&) = 0;
  void scriptedUI(ScriptedUIId, const ScriptedUIData&);

  /** Lets the player choose a number. Returns none if the player cancelled the choice.*/
  virtual optional<int> getNumber(const TString& title, Range range, int initial) = 0;

  /** Lets the player input a string. Returns none if the player cancelled the choice.*/
  virtual optional<string> getText(const TString& title, const string& value, int maxLength) = 0;

  virtual optional<int> chooseAtMouse(const vector<TString>& elems) = 0;

  virtual void dungeonScreenshot(Vec2 size) = 0;

  using BugReportSaveCallback = function<void(FilePath)>;

  bool confirmConflictingItems(const ContentFactory*, const vector<Item*>&);

  virtual void setBugReportSaveCallback(BugReportSaveCallback) = 0;

  struct CampaignMenuState {
    bool helpText;
    CampaignMenuIndex index;
  };
  struct CampaignOptions {
    const Campaign& campaign;
    optional<RetiredGames&> retired;
    vector<OptionId> options;
    TString introText;
    TString currentBiome;
    vector<TString> worldMapNames;
    int currentWorldMap;
    vector<pair<Vec2, TString>> claimedSites; // RAR online: other players' claimed sites (pos -> keeper name)
  };

  virtual CampaignAction prepareCampaign(CampaignOptions, CampaignMenuState&) = 0;

  virtual optional<UniqueEntity<Creature>::Id> chooseCreature(const TString& title, const vector<PlayerInfo>&,
      const TString& cancelText) = 0;

  // A visual grid of items (sprite + name tooltip); returns the index of the chosen one, or none if cancelled.
  // details: extra hover lines under the name -- the item's stat modifiers (defense/damage/...) and its
  // description/effects. Empty for an item that has neither; the caller decides what is worth showing.
  struct ItemChoiceInfo { ViewIdList viewId; TString name; vector<TString> details; };
  virtual optional<int> chooseItem(const TString& title, const vector<ItemChoiceInfo>&,
      const TString& cancelText) = 0;

  //virtual vector<UniqueEntity<Creature>::Id> chooseTeamLeader(const string& title, const vector<CreatureInfo>&) = 0;

  virtual bool creatureInfo(const TString& title, bool prompt, const vector<PlayerInfo>&) = 0;

  virtual optional<Vec2> chooseSite(const TString& message, const Campaign&, Vec2 current,
      const vector<pair<Vec2, TString>>& invadeable = {}) = 0;

  // preview=true shows the WHOLE map regardless of travel influence (used by the offline map-generator tool).
  virtual void presentWorldmap(const Campaign&, Vec2 current, bool preview = false) = 0;

  // Interactive offline layout-generator: a map preview on the left + a control panel on the right
  // (type, X/Y size, scrollable layout list). Returns one action; the caller regenerates and re-shows.
  struct LayoutPreviewInfo {
    vector<string> layouts; // all layout names from random_layouts.txt (sorted)
    string current;         // selected layout
    int sizeX = 80;
    int sizeY = 40;
    bool worldMap = true;   // type: world map (vs dungeon)
    int zoom = 1;           // render zoom (tile-size multiplier); mouse wheel adjusts it, reset per type
    vector<string> mappings;// all layout_mapping.txt mapping ids (dungeon token -> furniture)
    string mapping;         // selected mapping (dungeon only)
    int seed = 0;           // RNG seed this map was generated from -- shown so a good roll can be reproduced
  };
  enum class LayoutPreviewActionId { CLOSE, REROLL, EDIT_X, EDIT_Y, TOGGLE_TYPE, SELECT_LAYOUT, ZOOM_IN, ZOOM_OUT, SELECT_MAPPING, EDIT_SEED };
  struct LayoutPreviewAction { LayoutPreviewActionId id; string layoutName; };
  // campaign == nullptr -> show a placeholder instead of a map (used for the not-yet-implemented dungeon type).
  virtual LayoutPreviewAction previewLayoutMenu(const Campaign*, const LayoutPreviewInfo&) = 0;

  /** Draws an animation of an object between two locations on a map.*/
  virtual void animateObject(Vec2 begin, Vec2 end, optional<ViewId> object, optional<FXInfo> fx) = 0;

  /** Draws an special animation on the map.*/
  virtual void animation(Vec2 pos, AnimationId, Dir orientation = Dir::N) = 0;
  virtual void animation(const FXSpawnInfo&) = 0;

  /** Returns the current real time in milliseconds. The clock is stopped on blocking keyboard input,
      so it can be used to sync game time in real-time mode.*/
  virtual milliseconds getTimeMilli() = 0;

  /** Returns the absolute time, doesn't take pausing into account.*/
  virtual milliseconds getTimeMilliAbsolute() = 0;

  /** Stops the real time clock.*/
  virtual void stopClock() = 0;

  /** Continues the real time clock after it had been stopped.*/
  virtual void continueClock() = 0;

  /** Returns whether the real time clock is currently stopped.*/
  virtual bool isClockStopped() = 0;

  virtual void addSound(const Sound&) = 0;

  virtual void logMessage(const string&) = 0;

  virtual bool zoomUIAvailable() const = 0;

  virtual string translate(const TString&) const = 0;
};
