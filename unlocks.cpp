#include "stdafx.h"
#include "unlocks.h"
#include "parse_game.h"
#include "options.h"

Unlocks::Unlocks(Options* options, FilePath p) : options(options), path(p) {}
Unlocks Unlocks::allUnlocked() {
  return Unlocks();
}

Unlocks::Unlocks() {}

using UnlocksSet = set<string>;

static UnlocksSet read(FilePath path) {
  if (path.exists()) {
    CompressedInput input(path.getPath());
    UnlocksSet s;
    input.getArchive() >> s;
    return s;
  } else
    return UnlocksSet{};
}

static void write(FilePath path, const UnlocksSet& s) {
  CompressedOutput(path.getPath()).getArchive() << s;
}

bool Unlocks::isUnlocked(UnlockId id) const {
  // RAR: every keeper / content unlock is available from the start -- the puzzle + easter-egg gating is gone,
  // and its settings toggle is hidden (see optionSets in options.cpp; the option itself is kept so existing
  // saved settings files stay index-stable).
  // ACHIEVEMENTS are deliberately NOT blanket-unlocked: isAchieved() calls through here with an "ACH_" id, so
  // returning true for those would report every achievement as already earned.
  if (id.compare(0, 4, "ACH_") != 0)
    return true;
  return !options || options->getBoolValue(OptionId::UNLOCK_ALL) || (path && read(*path).count(id));
}

void Unlocks::unlock(UnlockId id) {
  if (path) {
    auto s = read(*path);
    s.insert(id);
    write(*path, s);
  }
}

void Unlocks::achieve(AchievementId id) {
  unlock("ACH_"_s + id.data());
}

bool Unlocks::isAchieved(AchievementId id) const {
  return isUnlocked("ACH_"_s + id.data());
}