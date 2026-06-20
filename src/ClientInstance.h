/*
 *  Copyright (C) 2024 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE for more information.
 *
 *  ClientInstance.h
 *  ────────────────
 *  Kodi PVR addon instance class.  Mirrors the pattern used in pvr.plutotv's
 *  addon.cpp / CPlutotvAddon.
 *
 *  This class:
 *    1. Inherits kodi::addon::CInstancePVRClient (Kodi PVR interface).
 *    2. Owns an AmazonTVData object (data layer).
 *    3. Delegates every PVR_ERROR call through to AmazonTVData.
 */

#pragma once

#include "AmazonTVData.h"

#include <kodi/addon-instance/PVR.h>

#include <memory>

class CClientInstance : public kodi::addon::CInstancePVRClient
{
public:
  explicit CClientInstance(const kodi::addon::IInstanceInfo& instance);
  ~CClientInstance() override;

  // ── kodi::addon::CInstancePVRClient overrides ────────────────────────────

  PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& caps) override;

  PVR_ERROR GetBackendName(std::string& name) override;
  PVR_ERROR GetBackendVersion(std::string& version) override;

  // Channels
  PVR_ERROR GetChannelsAmount(int& amount) override;
  PVR_ERROR GetChannels(bool bRadio, kodi::addon::PVRChannelsResultSet& results) override;

  // EPG
  PVR_ERROR GetEPGForChannel(int channelUid, time_t start, time_t end,
                             kodi::addon::PVREPGTagsResultSet& results) override;

  // Streaming
  PVR_ERROR GetChannelStreamProperties(
      const kodi::addon::PVRChannel& channel,
      PVR_SOURCE source,
      std::vector<kodi::addon::PVRStreamProperty>& properties) override;

private:
  std::unique_ptr<AmazonTVData> m_data;
};
