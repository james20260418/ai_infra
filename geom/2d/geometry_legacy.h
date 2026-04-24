#pragma once

// AABox2 has been moved to geom/2d/aabox2.h under namespace geom.
// This legacy header remains for backward compatibility.
// New code should include "geom/2d/aabox2.h" and use geom::AABox2 instead.
#include "geom/2d/aabox2.h"

#include <optional> 

#include "geom/common/check.h"
#include "geom/common/vec.h"

namespace geom_legacy {

// Backward-compatible alias to the new location.
using geom::AABox2;
using geom::AABox2d;

}  // namespace geom_legacy
