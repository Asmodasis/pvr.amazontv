/*
 *  Amazon Live TV PVR Client for Kodi (unofficial)
 *  Adapted from pvr.plutotv by flubshi / Team Kodi
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "kodi/addon-instance/PVR.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <vector>
#include <string>

static const std::string AMAZON_USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                             "AppleWebKit/537.36 (KHTML, like Gecko) "
                                             "Chrome/148.0.0.0 Safari/537.36 OPR/132.0.0.0";

class ATTR_DLL_LOCAL AmazonLiveData : public kodi::addon::CAddonBase,
                                      public kodi::addon::CInstancePVRClient
{
public:
  AmazonLiveData() = default;
  AmazonLiveData(const AmazonLiveData&) = delete;
  AmazonLiveData(AmazonLiveData&&) = delete;
  AmazonLiveData& operator=(const AmazonLiveData&) = delete;
  AmazonLiveData& operator=(AmazonLiveData&&) = delete;

  ADDON_STATUS Create() override;
  ADDON_STATUS SetSetting(const std::string& settingName,
                          const kodi::addon::CSettingValue& settingValue) override;

  PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& capabilities) override;
  PVR_ERROR GetBackendName(std::string& name) override;
  PVR_ERROR GetBackendVersion(std::string& version) override;

  PVR_ERROR GetChannelsAmount(int& amount) override;
  PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results) override;

  PVR_ERROR GetChannelGroupsAmount(int& amount) override;
  PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results) override;
  PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group,
                                   kodi::addon::PVRChannelGroupMembersResultSet& results) override;

  PVR_ERROR GetChannelStreamProperties(
      const kodi::addon::PVRChannel& channel,
      PVR_SOURCE source,
      std::vector<kodi::addon::PVRStreamProperty>& properties) override;

  PVR_ERROR GetEPGForChannel(int channelUid,
                             time_t start,
                             time_t end,
                             kodi::addon::PVREPGTagsResultSet& results) override;

private:
  struct AmazonChannel
  {
    int iUniqueId = 0;
    std::string titleId;           // e.g. amzn1.dv.gti.21cc7811-...
    std::string consumptionId;
    std::string channelName;
    std::string iconPath;
    std::string manifestUrl;       // DASH MPD
    std::string token;             // short-lived signed token
  };

  std::vector<AmazonChannel> m_channels;
  bool m_bChannelsLoaded = false;

  // Settings helpers (add more in settings.xml later)
  std::string GetSettingsDeviceId() const;
  bool GetSettingsUseTelemetry() const;

  void SetStreamProperties(std::vector<kodi::addon::PVRStreamProperty>& properties,
                           const std::string& manifestUrl,
                           bool realtime);

  bool LoadChannelsData();
  std::string GetChannelStreamURL(int uniqueId);

  // API Helpers from HAR
  std::string FetchManifest(const std::string& titleId, const std::string& consumptionId);
  bool SendTelemetry(const std::string& playhead, const std::string& consumptionId);

  std::string m_deviceId;
  std::chrono::time_point<std::chrono::steady_clock> m_lastTokenTime;
};