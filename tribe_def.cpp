#include "stdafx.h"
#include "tribe_def.h"

// BINARY layout: exactly the three SERIAL fields, and nothing else. keeperTribe is content-only -- adding it
// here would change the layout of every save (TribeDef lives inside ContentFactory). See tribe_def.h.
template <class Archive>
void TribeDef::serialize(Archive& ar, const unsigned int version) {
  ar(OPTION(enemies), OPTION(enemyOfAll), OPTION(diplomatic));
}

SERIALIZABLE(TribeDef);

#include "pretty_archive.h"

// CONTENT layout (tribes.txt): the three above plus keeperTribe, allies and allyToAll (all content-only).
template <>
void TribeDef::serialize(PrettyInputArchive& ar, const unsigned int version) {
  ar(OPTION(enemies), OPTION(enemyOfAll), OPTION(diplomatic), OPTION(keeperTribe), OPTION(allies),
      OPTION(allyToAll));
}
