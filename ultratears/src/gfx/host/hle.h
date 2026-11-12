// [Windows port] Stands in for MacNativeGames d2r_gfx/host/hle.h: the graphics front end's host services (dm.h) plus the
// runtime's guest-symbol binding (HLE_FUNC / HLE_NATIVE, hle_register) and the Horizon host (runtime_fail ...).
#pragma once
#include "../../runtime/hle.h"
#include "../../host/host.h"
#undef LOG
#include "dm.h"
