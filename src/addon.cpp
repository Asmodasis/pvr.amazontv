/*
 *  Copyright (C) 2024 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE for more information.
 *
 *  addon.cpp
 *  ─────────
 *  Kodi addon entry point.  Creates CClientInstance when Kodi requests a
 *  PVR addon instance.
 */

#include "ClientInstance.h"
#include <kodi/AddonBase.h>

class ATTR_DLL_LOCAL CMyAddon : public kodi::addon::CAddonBase
{
public:
  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override
  {
    hdl = new CClientInstance(instance);
    return ADDON_STATUS_OK;
  }
};

ADDONCREATOR(CMyAddon)
