#include "stdafx.h"
#include "visibility_map.h"
#include "creature.h"
#include "vision.h"

SERIALIZE_DEF(VisibilityMap, lastUpdates, visibilityCount, eyeballs)

void VisibilityMap::addPositions(const vector<Position>& positions) {
  for (Position v : positions)
    if (++visibilityCount.getOrInit(v) == 1)
      v.setNeedsRenderUpdate(true);
}

void VisibilityMap::removePositions(const vector<Position>& positions) {
  for (Position v : positions)
    if (--visibilityCount.getOrFail(v) == 0)
      v.setNeedsRenderUpdate(true);
}

void VisibilityMap::update(const Creature* c, const vector<Position>& visibleTiles) {
  PROFILE;
  remove(c);
  lastUpdates.set(c, visibleTiles);
  addPositions(visibleTiles);
}

// RAR live PvP: drop EVERYTHING without inspecting a single Position. When a live battle ends the battlefield
// model is freed, but lastUpdates still holds the tiles its creatures could see -- Positions carrying a raw
// Level* into that dead model -- and visibilityCount still holds its tables. The next time one of those
// creatures moves, update() calls remove(), which walks those stale tiles and dereferences freed memory: the
// invader's game vanished with no message the moment her returned squad moved again in a LATER battle.
// This is transient render state and rebuilds itself as creatures move, so clearing it wholesale is safe --
// and clearing is the only option, since deciding WHICH entries to keep means dereferencing them.
void VisibilityMap::rarClearAll() {
  lastUpdates.clear();
  eyeballs.clear();
  visibilityCount.clear();
}

void VisibilityMap::remove(const Creature* c) {
  if (auto positions = lastUpdates.getMaybe(c))
    removePositions(*positions);
  lastUpdates.erase(c);
}

const static Vision eyeballVision;

void VisibilityMap::updateEyeball(Position pos) {
  removeEyeball(pos);
  auto visibleTiles = pos.getVisibleTiles(eyeballVision);
  eyeballs.set(pos, visibleTiles);
  addPositions(visibleTiles);
}

void VisibilityMap::removeEyeball(Position pos) {
  if (auto positions = eyeballs.getReferenceMaybe(pos))
    removePositions(*positions);
  eyeballs.erase(pos);
}

void VisibilityMap::onVisibilityChanged(Position pos) {
  if (auto c = pos.getCreature())
    if (lastUpdates.hasKey(c))
      update(c, c->getVisibleTiles());
  if (eyeballs.contains(pos))
    updateEyeball(pos);
}

bool VisibilityMap::isVisible(Position pos) const {
  return visibilityCount.getValueMaybe(pos).value_or(0) > 0;
}

