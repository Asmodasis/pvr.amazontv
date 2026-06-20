/*
 *  Copyright (C) 2024 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE for more information.
 *
 *  ClientInstance.cpp
 */

#include "ClientInstance.h"
#include <kodi/General.h>

CClientInstance::CClientInstance(const kodi::addon::IInstanceInfo& instance)
  : CInstancePVRClient(instance),
    m_data(std::make_unique<AmazonTVData>())
{
  // Kick off the initial channel load in the constructor so channels are
  // immediately available when Kodi first calls GetChannels().
  if (!m_data->LoadChannelData())
    kodi::Log(ADDON_LOG_ERROR,
              "CClientInstance: Initial channel load failed. "
              "Check your session cookie and CSRF token in addon settings.");
}

CClientInstance::~CClientInstance() = default;

// ─── Capabilities ─────────────────────────────────────────────────────────────

PVR_ERROR CClientInstance::GetCapabilities(kodi::addon::PVRCapabilities& caps)
{
  caps.SetSupportsTV(true);
  caps.SetSupportsRadio(false);
  caps.SetSupportsChannelGroups(false);
  caps.SetSupportsEPG(true);
  caps.SetSupportsRecordings(false);
  caps.SetSupportsTimers(false);
  return PVR_ERROR_NO_ERROR;
}

// ─── Backend info ─────────────────────────────────────────────────────────────

PVR_ERROR CClientInstance::GetBackendName(std::string& name)
{
  name = "Amazon Prime Video Live TV";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CClientInstance::GetBackendVersion(std::string& version)
{
  version = "1.0.0";
  return PVR_ERROR_NO_ERROR;
}

// ─── Channels ─────────────────────────────────────────────────────────────────

PVR_ERROR CClientInstance::GetChannelsAmount(int& amount)
{
  amount = m_data->GetChannelCount();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR CClientInstance::GetChannels(bool bRadio,
                                       kodi::addon::PVRChannelsResultSet& results)
{
  return m_data->GetChannels(bRadio, results);
}

// ─── EPG ─────────────────────────────────────────────────────────────────────

PVR_ERROR CClientInstance::GetEPGForChannel(int channelUid, time_t start, time_t end,
                                            kodi::addon::PVREPGTagsResultSet& results)
{
  return m_data->GetEPGForChannel(channelUid, start, end, results);
}

// ─── Streaming ────────────────────────────────────────────────────────────────

PVR_ERROR CClientInstance::GetChannelStreamProperties(
    const kodi::addon::PVRChannel& channel,
    PVR_SOURCE /*source*/,
    std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  return m_data->GetChannelStreamProperties(channel, properties);
}
