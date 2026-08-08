#pragma once

#include "stdafx.h"
#include "util.h"
#include "game_time.h"
#include "unique_entity.h"
#include "color.h"

struct MovementInfo {
  enum Type { MOVE, ATTACK, WORK };
  MovementInfo(Vec2 direction, LocalTime tBegin, LocalTime tEnd, int moveCounter, Type);
  MovementInfo();
  MovementInfo& setDirection(Vec2);
  MovementInfo& setType(Type);
  MovementInfo& setMaxLength(TimeInterval);
  MovementInfo& setVictim(UniqueEntity<Creature>::Id);
  MovementInfo& setFX(optional<FXVariantName>, optional<Color> = none);
  Vec2 getDir() const;
  std::int8_t dirX = 0;
  std::int8_t dirY = 0;
  float tBegin;
  float tEnd;
  Type type = MOVE;
  int moveCounter = 0;
  GenericId victim = 0;
  optional<FXVariantName> fx;
  optional<Color> fxColor;   // overrides the variant's built-in colour when set
};
