/*
 *  Amazon Live TV PVR Client for Kodi (unofficial)
 *  Adapted from pvr.plutotv
 */

#include "AmazonLiveTV.h"
#include "Curl.h"
#include "Utils.h"
#include "kodi/tools/StringUtils.h"

#include <ctime>
#include <iomanip>
#include <sstream>

ADDON_STATUS AmazonLiveData::Create()
{
  kodi::Log(ADDON_LOG_DEBUG, "%s - Creating Amazon Live TV PVR add-on", __FUNCTION__);
  m_deviceId = GetSettingsDeviceId();
  if (m_deviceId.empty())
    m_deviceId = Utils::CreateUUID();
  return ADDON_STATUS_OK;
}

ADDON_STATUS AmazonLiveData::SetSetting(const std::string& settingName,
                                        const kodi::addon::CSettingValue& settingValue)
{
  return ADDON_STATUS_NEED_RESTART;
}

PVR_ERROR AmazonLiveData::GetCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
  capabilities.SetSupportsEPG(false);   // TODO: implement later
  capabilities.SetSupportsTV(true);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR AmazonLiveData::GetBackendName(std::string& name)
{
  name = "Amazon Live TV (unofficial)";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR AmazonLiveData::GetBackendVersion(std::string& version)
{
  version = "0.1.0";
  return PVR_ERROR_NO_ERROR;
}

// ==================== CHANNELS ====================

bool AmazonLiveData::LoadChannelsData()
{
  if (m_bChannelsLoaded)
    return true;

  kodi::Log(ADDON_LOG_DEBUG, "[AmazonLive] Loading channels...");

  // TODO: For v0.1 we hardcode one channel from your HAR.
  // Later: scrape https://www.amazon.com/live or reverse more endpoints.
  AmazonChannel ch;
  ch.iUniqueId = 1;
  ch.titleId = "amzn1.dv.gti.21cc7811-42f3-4a96-b819-fe6d59657b7c";
  ch.consumptionId = "20624-354a991c-7f07-43d2-8ee4-7265c130fa2c-09095";
  ch.channelName = "Amazon Live Test Channel (from HAR)";
  ch.iconPath = ""; // TODO: fetch logo
  ch.manifestUrl = "https://live-emt-pv-ta-enf.amazon.fastly-edge.com/IAD/1511b28ddae14f0a8cba31690c27c6e0/v1/dash/487368008139/imdb_wurl_amzn1_dv_live_csid_87f823f4-4041-49b2-bac6-0ca57e821864_us-east-1_iad_dash_h264/live/clients/dash/enc/wde4fcvliw/out/v1/f51bd64f13e6469cb75711a23d80dd9c/cenc.mpd";

  m_channels.emplace_back(ch);
  m_bChannelsLoaded = true;
  return true;
}

PVR_ERROR AmazonLiveData::GetChannelsAmount(int& amount)
{
  LoadChannelsData();
  amount = static_cast<int>(m_channels.size());
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR AmazonLiveData::GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results)
{
  if (radio)
    return PVR_ERROR_NO_ERROR;

  LoadChannelsData();

  for (const auto& channel : m_channels)
  {
    kodi::addon::PVRChannel kodiChannel;
    kodiChannel.SetUniqueId(channel.iUniqueId);
    kodiChannel.SetIsRadio(false);
    kodiChannel.SetChannelNumber(1);
    kodiChannel.SetChannelName(channel.channelName);
    kodiChannel.SetIconPath(channel.iconPath);
    kodiChannel.SetIsHidden(false);
    results.Add(kodiChannel);
  }
  return PVR_ERROR_NO_ERROR;
}

// ==================== STREAM ====================

void AmazonLiveData::SetStreamProperties(std::vector<kodi::addon::PVRStreamProperty>& properties,
                                         const std::string& manifestUrl,
                                         bool realtime)
{
  kodi::Log(ADDON_LOG_DEBUG, "[PLAY] DASH Manifest: %s", manifestUrl.c_str());

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, manifestUrl);
  properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.adaptive");
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, realtime ? "true" : "false");
  properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/dash+xml");

  const std::string encodedUA = /* UrlEncode(AMAZON_USER_AGENT) */;
  properties.emplace_back("inputstream.adaptive.manifest_headers",
                          "User-Agent=" + encodedUA + "|Referer=https://www.amazon.com/|Origin=https://www.amazon.com");
  properties.emplace_back("inputstream.adaptive.stream_headers", "User-Agent=" + encodedUA);
}

PVR_ERROR AmazonLiveData::GetChannelStreamProperties(
    const kodi::addon::PVRChannel& channel,
    PVR_SOURCE source,
    std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  const std::string url = GetChannelStreamURL(channel.GetUniqueId());
  if (!url.empty())
  {
    SetStreamProperties(properties, url, true);
    // Optional: SendTelemetry(...) to keep session alive
    return PVR_ERROR_NO_ERROR;
  }
  return PVR_ERROR_FAILED;
}

std::string AmazonLiveData::GetChannelStreamURL(int uniqueId)
{
  LoadChannelsData();
  for (const auto& ch : m_channels)
  {
    if (ch.iUniqueId == uniqueId)
      return ch.manifestUrl;   // TODO: refresh token if expired
  }
  return {};
}

// ==================== TELEMETRY (from HAR) ====================

bool AmazonLiveData::SendTelemetry(const std::string& playhead, const std::string& consumptionId)
{
  // Implement POST to https://global.telemetry.insights.video.a2z.com/Events/AV20180601
  // with the large JSON payload from your HAR.
  // For now stubbed.
  kodi::Log(ADDON_LOG_DEBUG, "[Telemetry] playhead %s", playhead.c_str());
  return true;
}

// ==================== PLACEHOLDERS ====================

PVR_ERROR AmazonLiveData::GetChannelGroupsAmount(int& amount) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR AmazonLiveData::GetChannelGroups(...) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR AmazonLiveData::GetChannelGroupMembers(...) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR AmazonLiveData::GetEPGForChannel(...) { return PVR_ERROR_NOT_IMPLEMENTED; }

std::string AmazonLiveData::GetSettingsDeviceId() const
{
  return kodi::addon::GetSettingString("device_id");
}

bool AmazonLiveData::GetSettingsUseTelemetry() const
{
  return kodi::addon::GetSettingBoolean("send_telemetry", true);
}

ADDONCREATOR(AmazonLiveData)