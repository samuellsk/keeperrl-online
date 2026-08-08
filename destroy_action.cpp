#include "stdafx.h"
#include "destroy_action.h"
#include "sound.h"
#include "creature.h"
#include "creature_attributes.h"
#include "tribe_def.h"
#include "content_factory.h"
#include "game.h"
#include "collective.h"

SERIALIZE_DEF(DestroyAction, type)
SERIALIZATION_CONSTRUCTOR_IMPL(DestroyAction)

DestroyAction::DestroyAction(Type t) : type(t) {
}

TStringId DestroyAction::getTaskDescription() const {
  switch (type) {
    case Type::CUT: return TStringId("CUT_TREE_TASK");
    case Type::DIG: return TStringId("DIG_TASK");
    case Type::FILL: return TStringId("FILL_TASK");
    default:
      return TStringId("DESTROY_TASK");
  }
}

TStringId DestroyAction::getVerbSecondPerson() const {
  switch (type) {
    case Type::BASH: return TStringId("YOU_BASH_FURNITURE");
    case Type::BOULDER: return TStringId("YOU_DESTROY_FURNITURE");
    case Type::CUT: return TStringId("YOU_CUT_FURNITURE");
    case Type::HOSTILE_DIG:
    case Type::DIG: return TStringId("YOU_DIG_INTO_FURNITURE");
    case Type::FILL: return TStringId("YOU_FILL_FURNITURE");
  }
}

TStringId DestroyAction::getVerbThirdPerson() const {
  switch (type) {
    case Type::BASH: return TStringId("BASHES_FURNITURE");
    case Type::BOULDER: return TStringId("DESTROYS_FURNITURE");
    case Type::CUT: return TStringId("CUTS_FURNITURE");
    case Type::HOSTILE_DIG:
    case Type::DIG: return TStringId("DIGS_INTO_FURNITURE");
    case Type::FILL: return TStringId("FILLS_FURNITURE");
  }
}

TStringId DestroyAction::getIsDestroyed() const {
  switch (type) {
    case Type::BASH:
    case Type::BOULDER: return TStringId("FURNITURE_IS_DESTROYED");
    case Type::CUT: return TStringId("FURNITURE_FALLS");
    case Type::HOSTILE_DIG:
    case Type::DIG: return TStringId("FURNITURE_IS_DUG_OUT");
    case Type::FILL: return TStringId("FURNITURE_IS_FILLED");;
  }
}

optional<TStringId> DestroyAction::getSoundText() const {
  switch (type) {
    case Type::BASH: return TStringId("FURNITURE_BANG");
    case Type::BOULDER:
    case Type::CUT: return TStringId("FURNITURE_CRASH");;
    case Type::HOSTILE_DIG:
    case Type::FILL:
    case Type::DIG: return none;
  }
}

optional<Sound> DestroyAction::getSound() const {
  switch (type) {
    case Type::FILL: return none;
    case Type::BASH:
    case Type::BOULDER: return Sound(SoundId("BANG_DOOR"));
    case Type::CUT: return Sound(SoundId("TREE_CUTTING"));
    case Type::HOSTILE_DIG:
    case Type::DIG: return Sound(SoundId("DIGGING"));
   }
}

DestroyAction::Type DestroyAction::getType() const {
  return type;
}

bool DestroyAction::canDestroyFriendly() const {
  switch (type) {
    case Type::BASH:
    case Type::HOSTILE_DIG:
      return false;
    default:
      return true;
  }
}

bool DestroyAction::destroyAnimation() const {
  switch (type) {
    case Type::FILL: return false;
    default: return true;
  }
}

bool DestroyAction::canNavigate(const Creature* c) const {
  switch (type) {
    case Type::HOSTILE_DIG: {
      // Keeper minions dig only where their collective orders them, NOT via this shortcut-through-walls
      // pathing (that's how ENEMIES break into a dungeon).
      // The built-in keeper tribes always qualify; a CUSTOM keeper tribe qualifies via the tribes.txt
      // keeperTribe flag. Checking the flag (rather than just "is this the local player's tribe") matters
      // when INVADING: the defender's minions are in our game but on THEIR own tribe, so a rival on
      // UNDEAD_KEEPER would otherwise tunnel through his own walls.
      if (isOneOf(c->getTribeId(), TribeId::getDarkKeeper(), TribeId::getWhiteKeeper(),
          TribeId::getRetiredKeeper()))
        return false;
      if (auto game = c->getGame()) {
        auto& tribes = game->getContentFactory()->tribes;
        auto it = tribes.find(c->getTribeId());
        if (it != tribes.end() && it->second.keeperTribe)
          return false;
      }
      return true;
    }
    case Type::BASH:
      return true;
    default:
      return false;
  }
}

MinionActivity DestroyAction::getMinionActivity() const {
  switch (type) {
    case Type::DIG:
      return MinionActivity::DIGGING;
    default:
      return MinionActivity::WORKING;
  }
}

double DestroyAction::getDamage(Creature* c, const ContentFactory* factory) const {
  PROFILE;
  switch (type) {
    case Type::BASH:
      return max(c->getBestAttack(factory).value, 15);
    case Type::BOULDER:
    case Type::CUT:
      return max(c->getAttr(AttrType("DAMAGE")), 15);
    case Type::HOSTILE_DIG:
      return max(c->getAttr(AttrType("DIGGING")), 15) / 5.0;
    case Type::DIG:
    case Type::FILL:
      return c->getAttr(AttrType("DIGGING")) / 5.0;
   }
}