#pragma once

#include "stdafx.h"
#include "util.h"

using Token = string;

// A layout that asks for an area outside the map (or is misconfigured) is normally a FATAL content error:
// USER_CHECK -> UserErrorLog -> DebugOutput::exitProgram(), i.e. the message box and then exit(0). That is
// right for a real game -- a broken layout must not quietly produce a half-generated level -- but it makes
// the --gen_preview dev tool unusable: picking a size a layout can't handle killed the whole program instead
// of letting you try another one.
//
// So inside the previewer (and ONLY there) these become a catchable exception. previewLayout sets the flag
// via LayoutErrorsThrowScope, catches this, shows the message and keeps its menu loop running, so you can
// change X/Y and generate again. Everywhere else the flag is false and the behaviour is exactly as before.
struct LayoutGenerationError {
  string message;
};
extern bool layoutErrorsThrow;   // defined in layout_generator.cpp

struct LayoutErrorsThrowScope {
  LayoutErrorsThrowScope() { layoutErrorsThrow = true; }
  ~LayoutErrorsThrowScope() { layoutErrorsThrow = false; }
};

// Report a layout problem: throws inside the previewer, fatal everywhere else. Takes the message ready-made
// so both paths print exactly the same text.
inline void layoutGenerationError(const string& message) {
  if (layoutErrorsThrow)
    throw LayoutGenerationError{message};
  USER_FATAL << message;
}

#define LAYOUT_CHECK(exp, message) do { if (!(exp)) layoutGenerationError(message); } while (0)

struct LayoutCanvas {
  struct Map {
    Table<vector<Token>> elems;
  };
  LayoutCanvas with(Rectangle area) const {
    LAYOUT_CHECK(map->elems.getBounds().contains(area), "Level generator exceeded map bounds. "_s
        + toString(map->elems.getBounds()) + " and " + toString(area));
    return LayoutCanvas{area, map};
  }
  Rectangle area;
  Map* map;
};
