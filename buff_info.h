#pragma once

#include "lasting_effect.h"
#include "attr_type.h"
#include "effect.h"
#include "msg_type.h"
#include "fx_variant_name.h"
#include "color.h"
#include "special_attr.h"
#include "t_string.h"

struct YouMessage {
  MsgType SERIAL(type);
  TString SERIAL(message);
  SERIALIZE_ALL(type, message)
};

struct VerbMessage {
  TString SERIAL(secondPerson);
  TString SERIAL(thirdPerson);
  TString SERIAL(message);
  SERIALIZE_ALL(secondPerson, thirdPerson, message)
};

using BuffMessageInfo = variant<YouMessage, VerbMessage>;

void applyMessage(const BuffMessageInfo&, const Creature*);
void serialize(PrettyInputArchive&, BuffMessageInfo&, const unsigned int);

struct BuffInfo {
  TString SERIAL(name);
  optional<BuffMessageInfo> SERIAL(addedMessage);
  optional<BuffMessageInfo> SERIAL(removedMessage);
  optional<Effect> SERIAL(startEffect);
  optional<Effect> SERIAL(tickEffect);
  optional<Effect> SERIAL(endEffect);
  TString SERIAL(description);
  TString SERIAL(adjective);
  bool SERIAL(consideredBad) = false;
  bool SERIAL(combatConsumable) = false;
  bool SERIAL(stacks) = false;
  bool SERIAL(canAbsorb) = true;
  bool SERIAL(canWishFor) = true;
  bool SERIAL(inheritsFromSteed) = false;
  optional<double> SERIAL(efficiencyMultiplier);
  int SERIAL(price) = 50;
  Color SERIAL(color);
  optional<TString> SERIAL(hatedGroupName);
  double SERIAL(defenseMultiplier) = 1.0;
  optional<AttrType> SERIAL(defenseMultiplierAttr);
  // The attacking mirror of the pair above. Scales the damage a creature DEALS at the moment the blow is
  // rolled, leaving the displayed attribute untouched -- a creature with DAMAGE 100 under a 1.3 buff still
  // reads 100 on its sheet and hits for 130. Optional attr restricts it to one damage type, exactly as
  // defenseMultiplierAttr does; unset means every type.
  double SERIAL(damageMultiplier) = 1.0;
  optional<AttrType> SERIAL(damageMultiplierAttr);
  FXVariantName SERIAL(fx) = FXVariantName::BUFF_RED;
  optional<pair<AttrType, AttrType>> SERIAL(modifyDamageAttr);
  optional<SpecialAttr> SERIAL(specialAttr);
  optional<CreaturePredicate> SERIAL(hiddenPredicate);
  template <class Archive>
  void serialize(Archive& ar1, const unsigned int);
};

CEREAL_CLASS_VERSION(BuffInfo, 3)