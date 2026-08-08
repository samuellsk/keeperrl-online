#pragma once

#include "util.h"
#include "my_containers.h"
#include "cost_info.h"
#include "enum_variant.h"
#include "zones.h"
#include "avatar_info.h"
#include "view_id.h"
#include "furniture_type.h"
#include "tech_id.h"
#include "pretty_archive.h"
#include "furniture_layer.h"
#include "tutorial_highlight.h"

namespace BuildInfoTypes {
  struct Furniture {
    vector<FurnitureType> SERIAL(types);
    CostInfo SERIAL(cost);
    bool SERIAL(noCredit) = false;
    optional<int> SERIAL(limit);
    SERIALIZE_ALL(NAMED(types), OPTION(cost), OPTION(noCredit), NAMED(limit))
  };
  using DestroyLayers = vector<FurnitureLayer>;
  using ImmediateDig = EmptyStruct<struct ImmediateDigTag>;
  using Dig = EmptyStruct<struct DigTag>;
  using CutTree = EmptyStruct<struct CutTreeTag>;
  using FillPit = EmptyStruct<struct FillPitTag>;
  using ClaimTile = EmptyStruct<struct ClaimTileTag>;
  using UnclaimTile = EmptyStruct<struct UnclaimTileTag>;
  using Dispatch = EmptyStruct<struct DispatchTag>;
  using ForbidZone = EmptyStruct<struct ForbidZoneTag>;
  using Zone = ZoneId;
  using PlaceMinion = EmptyStruct<struct PlaceMinionTag>;
  // Like PlaceMinion, but also grants the WORKER trait to creatures that can dig (so placed imps actually
  // dig + haul, which the plain PlaceMinion doesn't set up).
  using PlaceMinionNew = EmptyStruct<struct PlaceMinionNewTag>;
  using PlaceItem = EmptyStruct<struct PlaceItemTag>;
  // Like PlaceItem, but pick from a VISUAL grid of every item type (from items.txt) + a quantity prompt,
  // instead of typing the item type by hand.
  using PlaceItemNew = EmptyStruct<struct PlaceItemNewTag>;
  struct BuildType;
  using Chain = vector<BuildType>;
  #define VARIANT_TYPES_LIST\
    X(Furniture, 0)\
    X(ClaimTile, 1)\
    X(UnclaimTile, 2)\
    X(DestroyLayers, 3)\
    X(Dig, 4)\
    X(CutTree, 5)\
    X(Dispatch, 6)\
    X(ForbidZone, 7)\
    X(PlaceMinion, 8)\
    X(ImmediateDig, 9)\
    X(PlaceItem, 10)\
    X(Zone, 11)\
    X(Chain, 12)\
    X(FillPit, 13)\
    X(PlaceMinionNew, 14)\
    X(PlaceItemNew, 15)

  #define VARIANT_NAME BuildType

  #include "gen_variant.h"
  #include "gen_variant_serialize.h"
  #define DEFAULT_ELEM "Chain"
  inline
  #include "gen_variant_serialize_pretty.h"
  #undef DEFAULT_ELEM
  #undef VARIANT_TYPES_LIST
  #undef VARIANT_NAME

}

struct BuildInfo {
  using DungeonLevel = int;
  MAKE_VARIANT(Requirement, TechId, DungeonLevel);

  static TString getRequirementText(Requirement, const ContentFactory*);
  static bool meetsRequirement(const Collective*, Requirement);
  bool canSelectRectangle() const;

  BuildInfoTypes::BuildType SERIAL(type);
  private:
  mutable TString SERIAL(name);
  public:
  const TString& getName(const ContentFactory*) const;
  TString SERIAL(groupName);
  TString SERIAL(help);
  optional<Keybinding> SERIAL(key) = none;
  vector<Requirement> SERIAL(requirements);
  bool SERIAL(hotkeyOpensGroup) = false;
  optional<TutorialHighlight> SERIAL(tutorialHighlight);
  bool SERIAL(isBuilding) = false;
  // RAR: only a DEVELOPER account gets this entry in its build menu (the dev placement tools). The mod stays
  // ACTIVE for everyone -- whatever it places has to exist for all players -- only the menu entry is hidden.
  bool SERIAL(developerOnly) = false;
  template <class Archive>
  void serializeImpl(Archive& ar, const unsigned int) {
    ar(NAMED(type), NAMED(name), NAMED(groupName), OPTION(help), NAMED(key), OPTION(requirements), OPTION(hotkeyOpensGroup), NAMED(tutorialHighlight), OPTION(isBuilding));
  }
  // BuildInfo is serialized INTO EVERY SAVE (PlayerControl keeps its keeper's build menu), so developerOnly
  // cannot live in serializeImpl: the binary archive would then expect a field that no existing save contains
  // and every keeper made before this build would fail to load. Version-gate the binary side; the config side
  // always reads it, since game_config files are not versioned.
  template <class Archive>
  void serialize(Archive& ar1, const unsigned int v) {
    serializeImpl(ar1, v);
    if (v >= 1)
      ar1(OPTION(developerOnly));
  }

  void serialize(PrettyInputArchive& ar1, const unsigned int v) {
    serializeImpl(ar1, v);
    ar1(OPTION(developerOnly));
    ar1(endInput());
    if (groupName.empty())
      ar1.error("Group name for \""_s + name.data() + "\" is empty.");
  }
};

CEREAL_CLASS_VERSION(BuildInfo, 1)   // v1 added developerOnly

static_assert(std::is_nothrow_move_constructible<BuildInfo>::value, "T should be noexcept MoveConstructible");
