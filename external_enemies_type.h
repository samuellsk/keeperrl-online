#pragma once

#include "util.h"

// RAR: the endless-wave trigger. AFTER_WINNING was dropped -- this game has no win condition, so it meant
// "never". The remaining values are start-turn thresholds; the mapping to actual turns lives in the
// ExternalEnemies constructor.
enum class ExternalEnemiesType {
  FROM_START,
  AFTER_25K,
  AFTER_50K,
  AFTER_100K
};
