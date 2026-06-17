/*
 *  Copyright (C) 2024 Amazon Live TV PVR add-on
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "AmazonLiveData.h"

#include "Curl.h"
#include "Utils.h"
#include "kodi/tools/StringUtils.h"

#include <cctype>
#include <ctime>
#include <iomanip>
#include <ios>
#include <sstream>

ADDON_STATUS AmazonLiveData::Create()
{
  kodi::Log(ADDON_LOG_DEBUG, "%s - Creating the Amazon Live TV PVR add-on", __FUNCTION__);
  return ADDON_STATUS_OK;
}

ADDON_STATUS AmazonLiveData::SetSetting(const std::string& settingName,
                                        const kodi::addon::CSettingValue& settingValue)
{
  return ADDON_STATUS_NEED_RESTART;
}

PVR_ERROR AmazonLiveData::GetCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
  capabilities.SetSupportsEPG(true);
  capabilities.SetSupportsTV(true);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR AmazonLiveData::GetBackendName(std::string& name)
{
  name = "Amazon Live TV PVR add-on";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR AmazonLiveData::GetBackendVersion(std::string& version)
{
  version = STR(IPTV_VERSION);
  return PVR_ERROR_NO_ERROR;
}

namespace
{
// http://stackoverflow.com/a/17708801
const std::string UrlEncode(const std::string& value)
{
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;

  for (auto c : value)
  {
    // Keep alphanumeric and other accepted characters intact
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      escaped << c;
      continue;
    }

    // Any other characters are percent-encoded
    escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
  }

  return escaped.str();
}
} // unnamed namespace

void AmazonLiveData::SetStreamProperties(std::vector<kodi::addon::PVRStreamProperty>& properties,
                                         const std::string& url,
                                         bool realtime)
{
  kodi::Log(ADDON_LOG_DEBUG, "[PLAY STREAM] url: %s", url.c_str());

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, url);
  properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.adaptive");
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, realtime ? "true" : "false");
  // HLS
  properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/x-mpegURL");

  const std::string encodedUserAgent{UrlEncode(AMAZON_LIVE_USER_AGENT)};
  properties.emplace_back("inputstream.adaptive.manifest_headers",
                          "User-Agent=" + encodedUserAgent);
  properties.emplace_back("inputstream.adaptive.stream_headers", "User-Agent=" + encodedUserAgent);

  if (GetSettingsWorkaroundBrokenStreams())
    properties.emplace_back("inputstream.adaptive.manifest_config",
                            "{\"hls_ignore_endlist\":true,\"hls_fix_mediasequence\":true,\"hls_fix_"
                            "discsequence\":true}");
}

bool AmazonLiveData::LoadChannelsData()
{
  if (m_bChannelsLoaded)
    return true;

  GetAccessToken();
  if (m_accessToken.empty())
    return false;

  kodi::Log(ADDON_LOG_DEBUG, "[load data] GET CHANNELS");

  const std::string jsonChannels{GetChannelsJson()};

  if (jsonChannels.empty() || jsonChannels == "[]")
  {
    kodi::Log(ADDON_LOG_ERROR, "[channels] ERROR - empty response");
    return false;
  }

  kodi::Log(ADDON_LOG_DEBUG, "[channels] length: %i;", jsonChannels.size());
  kodi::Log(ADDON_LOG_DEBUG, "[channels] %s;", jsonChannels.c_str());
  kodi::Log(ADDON_LOG_DEBUG, "[channels] %s;",
            jsonChannels.substr(jsonChannels.size() - 40).c_str());

  // parse channels
  kodi::Log(ADDON_LOG_DEBUG, "[channels] parse channels");
  nlohmann::json channelsDoc = nlohmann::json::parse(jsonChannels.c_str());
  if (channelsDoc.is_discarded())
  {
    kodi::Log(ADDON_LOG_ERROR, "[LoadChannelData] ERROR: error while parsing json");
    return false;
  }

  kodi::Log(ADDON_LOG_DEBUG, "[channels] iterate channels");

  // Get channels array - adjust key based on actual Amazon API response
  nlohmann::json channelsArray;
  if (channelsDoc.contains("channels"))
  {
    channelsArray = channelsDoc.at("channels");
  }
  else if (channelsDoc.contains("data"))
  {
    channelsArray = channelsDoc.at("data");
  }
  else
  {
    kodi::Log(ADDON_LOG_ERROR, "[channels] ERROR - no channels data found in response");
    return false;
  }

  kodi::Log(ADDON_LOG_DEBUG, "[channels] size: %i;", channelsArray.size());

  // Use configured start channel number to populate the channel list
  int i = GetSettingsStartChannel();
  for (const auto& channel : channelsArray)
  {
    const std::string amazonChannelId{channel.at("id")};

    AmazonChannel amazon_channel;
    amazon_channel.iChannelNumber = i++; // position
    kodi::Log(ADDON_LOG_DEBUG, "[channel] channelnr(pos): %i;", amazon_channel.iChannelNumber);

    amazon_channel.amazonChannelId = amazonChannelId;
    kodi::Log(ADDON_LOG_DEBUG, "[channel] Amazon Channel ID: %s;", amazon_channel.amazonChannelId.c_str());

    const int uniqueId = Utils::Hash(amazonChannelId);
    amazon_channel.iUniqueId = uniqueId;
    kodi::Log(ADDON_LOG_DEBUG, "[channel] id: %i;", uniqueId);

    const std::string displayName = channel.at("name");
    amazon_channel.strChannelName = displayName;
    kodi::Log(ADDON_LOG_DEBUG, "[channel] name: %s;", amazon_channel.strChannelName.c_str());

    std::string logo;

    // Handle logo/image extraction - adjust based on actual Amazon API response
    if (channel.contains("images") && channel.at("images").size() > 0)
    {
      for (const auto& img : channel.at("images"))
      {
        if (!img.contains("type"))
          continue;

        if (GetSettingsColoredChannelLogos())
        {
          if (img.at("type") == "colorLogo" || img.at("type") == "colorLogoPNG")
          {
            logo = img.at("url");
            break;
          }
        }
        else if (img.at("type") == "solidLogo" || img.at("type") == "solidLogoPNG")
        {
          logo = img.at("url");
          break;
        }
        // fallback
        if (img.at("type") == "logo")
        {
          logo = img.at("url");
        }
      }
    }
    else if (channel.contains("thumbnail"))
    {
      logo = channel.at("thumbnail");
    }

    amazon_channel.strIconPath = logo;
    kodi::Log(ADDON_LOG_DEBUG, "[channel] iconpath: %s;", amazon_channel.strIconPath.c_str());

    // Handle stream URL extraction - adjust based on actual Amazon API response
    if (channel.contains("streamUrl"))
    {
      amazon_channel.strStreamURL = channel.at("streamUrl");
      kodi::Log(ADDON_LOG_DEBUG, "[channel] streamURL: %s;", amazon_channel.strStreamURL.c_str());
    }

    m_channels.emplace_back(amazon_channel);
  }

  m_bChannelsLoaded = true;
  return true;
}

PVR_ERROR AmazonLiveData::GetChannelsAmount(int& amount)
{
  kodi::Log(ADDON_LOG_DEBUG, "Amazon Live TV function call: [%s]", __FUNCTION__);

  LoadChannelsData();
  if (!m_bChannelsLoaded)
    return PVR_ERROR_SERVER_ERROR;

  amount = static_cast<int>(m_channels.size());
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR AmazonLiveData::GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results)
{
  kodi::Log(ADDON_LOG_DEBUG, "Amazon Live TV function call: [%s]", __FUNCTION__);

  if (!radio)
  {
    LoadChannelsData();
    if (!m_bChannelsLoaded)
      return PVR_ERROR_SERVER_ERROR;

    for (const auto& channel : m_channels)
    {
      kodi::addon::PVRChannel kodiChannel;

      kodiChannel.SetUniqueId(channel.iUniqueId);
      kodiChannel.SetIsRadio(false);
      kodiChannel.SetChannelNumber(channel.iChannelNumber);
      kodiChannel.SetChannelName(channel.strChannelName);
      kodiChannel.SetIconPath(channel.strIconPath);
      kodiChannel.SetIsHidden(false);

      results.Add(kodiChannel);
    }
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR AmazonLiveData::GetChannelStreamProperties(
    const kodi::addon::PVRChannel& channel,
    PVR_SOURCE source,
    std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  const std::string strUrl = GetChannelStreamURL(channel.GetUniqueId());
  kodi::Log(ADDON_LOG_DEBUG, "Stream URL -> %s", strUrl.c_str());
  PVR_ERROR ret = PVR_ERROR_FAILED;
  if (!strUrl.empty())
  {
    SetStreamProperties(properties, strUrl, true);
    ret = PVR_ERROR_NO_ERROR;
  }
  return ret;
}

std::string AmazonLiveData::GetSettingsUUID(const std::string& setting)
{
  std::string uuid = kodi::addon::GetSettingString(setting);
  if (uuid.empty())
  {
    uuid = Utils::CreateUUID();
    kodi::Log(ADDON_LOG_DEBUG, "uuid (generated): %s", uuid.c_str());
    kodi::addon::SetSettingString(setting, uuid);
  }
  return uuid;
}

int AmazonLiveData::GetSettingsStartChannel() const
{
  return kodi::addon::GetSettingInt("start_channelnum", 1);
}

bool AmazonLiveData::GetSettingsColoredChannelLogos() const
{
  return kodi::addon::GetSettingBoolean("colored_channel_logos", true);
}

bool AmazonLiveData::GetSettingsWorkaroundBrokenStreams() const
{
  return kodi::addon::GetSettingBoolean("workaround_broken_streams", true);
}

std::string AmazonLiveData::GetChannelStreamURL(int uniqueId)
{
  LoadChannelsData();
  if (!m_bChannelsLoaded)
    return {};

  for (const auto& channel : m_channels)
  {
    if (channel.iUniqueId == uniqueId)
    {
      kodi::Log(ADDON_LOG_DEBUG, "Get live url for channel %s", channel.strChannelName.c_str());
      kodi::Log(ADDON_LOG_DEBUG, "stream URL: %s", channel.strStreamURL.c_str());
      return channel.strStreamURL;
    }
  }
  return {};
}

PVR_ERROR AmazonLiveData::GetChannelGroupsAmount(int& amount)
{
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR AmazonLiveData::GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results)
{
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR AmazonLiveData::GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group,
                                                 kodi::addon::PVRChannelGroupMembersResultSet& results)
{
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR AmazonLiveData::GetEPGForChannel(int channelUid,
                                           time_t start,
                                           time_t end,
                                           kodi::addon::PVREPGTagsResultSet& results)
{
  LoadChannelsData();
  if (!m_bChannelsLoaded)
    return PVR_ERROR_SERVER_ERROR;

  // Find channel data
  for (const auto& channel : m_channels)
  {
    if (channel.iUniqueId != channelUid)
      continue;

    // Channel data found
    if (!m_epg_cache_document || m_epg_cache_start == 0 || m_epg_cache_end == 0 ||
        start < m_epg_cache_start || end > m_epg_cache_end)
    {
      const time_t orig_start = start;
      const time_t now = std::time(nullptr);
      if (orig_start < now)
      {
        kodi::Log(ADDON_LOG_DEBUG, "[epg-amazon] adjusting start time to current time");
        start = now;
      }

      const std::string jsonEpg{GetEpgJson(start, end)};
      kodi::Log(ADDON_LOG_DEBUG, "[epg-amazon-all] %s", jsonEpg.c_str());
      if (jsonEpg.empty())
      {
        kodi::Log(ADDON_LOG_ERROR, "[epg-amazon] empty server response");
        return PVR_ERROR_SERVER_ERROR;
      }

      const auto epgDoc{std::make_shared<nlohmann::json>(nlohmann::json::parse(jsonEpg.c_str()))};
      if ((*epgDoc).is_discarded())
      {
        kodi::Log(ADDON_LOG_ERROR, "[GetAmazonEPG] ERROR: error while parsing json");
        return PVR_ERROR_SERVER_ERROR;
      }

      m_epg_cache_document = epgDoc;
      m_epg_cache_start = orig_start;
      m_epg_cache_end = end;
    }

    kodi::Log(ADDON_LOG_DEBUG, "[epg-amazon] iterate entries");

    // Amazon's JSON structure may differ - adjust based on actual API response
    nlohmann::json programsArray;
    if ((*m_epg_cache_document).contains("programs"))
    {
      programsArray = (*m_epg_cache_document).at("programs");
    }
    else if ((*m_epg_cache_document).contains("epgPrograms"))
    {
      programsArray = (*m_epg_cache_document).at("epgPrograms");
    }
    else
    {
      kodi::Log(ADDON_LOG_WARNING, "[epg-amazon] No programs found in response");
      return PVR_ERROR_NO_ERROR;
    }

    kodi::Log(ADDON_LOG_DEBUG, "[epg-amazon] size: %i;", programsArray.size());

    // Parse EPG data
    for (const auto& program : programsArray)
    {
      // Filter by channel
      if (!program.contains("channelId") || program.at("channelId") != channel.amazonChannelId)
        continue;

      kodi::addon::PVREPGTag tag;

      // Generate unique broadcast ID
      const std::string epg_id = program.contains("id") ? program.at("id").get<std::string>() : "";
      const int epg_bid = Utils::Hash(epg_id);
      tag.SetUniqueBroadcastId(epg_bid);

      // Set channel ID
      tag.SetUniqueChannelId(channel.iUniqueId);

      // Set title
      const std::string title{program.at("title")};
      tag.SetTitle(title);
      kodi::Log(ADDON_LOG_DEBUG, "[epg-amazon] title: %s;", title.c_str());

      // Start and end times
      if (program.contains("startTime"))
      {
        tag.SetStartTime(program.at("startTime"));
      }
      if (program.contains("endTime"))
      {
        tag.SetEndTime(program.at("endTime"));
      }

      // Description
      if (program.contains("description") && program.at("description").is_string())
      {
        tag.SetPlot(program.at("description"));
      }

      // Genre
      if (program.contains("genre") && program.at("genre").is_string())
      {
        tag.SetGenreType(EPG_GENRE_USE_STRING);
        tag.SetGenreDescription(program.at("genre"));
      }

      // Thumbnail/Icon
      if (program.contains("thumbnail") && program.at("thumbnail").is_string())
      {
        tag.SetIconPath(program.at("thumbnail"));
      }

      // Series information
      if (program.contains("series"))
      {
        const auto& series = program.at("series");

        if (series.contains("title") && series.at("title").is_string())
        {
          tag.SetTitle(series.at("title"));
        }

        if (program.contains("episodeName") && program.at("episodeName").is_string())
        {
          tag.SetEpisodeName(program.at("episodeName"));
        }

        if (series.contains("seasonNumber") && series.at("seasonNumber").is_number_integer())
        {
          tag.SetSeriesNumber(series.at("seasonNumber"));
        }

        if (series.contains("episodeNumber") && series.at("episodeNumber").is_number_integer())
        {
          tag.SetEpisodeNumber(series.at("episodeNumber"));
        }

        tag.SetFlags(EPG_TAG_FLAG_IS_SERIES);
      }

      // Parental rating
      if (program.contains("rating") && program.at("rating").is_string())
      {
        const std::string ratingString{program.at("rating")};
        kodi::Log(ADDON_LOG_DEBUG, "[epg-amazon] rating: %s", ratingString.c_str());
        tag.SetParentalRatingCode(ratingString);

        const int rating{Utils::StringToInt(ratingString, -1)};
        if (rating > -1)
        {
          tag.SetParentalRating(rating);
        }
      }

      // First aired date
      if (program.contains("releaseDate") && program.at("releaseDate").is_string())
      {
        tag.SetFirstAired(program.at("releaseDate"));
      }

      results.Add(tag);
    }

    return PVR_ERROR_NO_ERROR;
  }

  kodi::Log(ADDON_LOG_ERROR, "[GetAmazonEPG] ERROR: channel not found");
  return PVR_ERROR_INVALID_PARAMETERS;
}

std::string AmazonLiveData::GetAccessToken()
{
  // Access token may expire after a certain period (adjust timing as needed)
  if (m_accessToken.empty() || (std::chrono::steady_clock::now() - m_tokenTimestamp > std::chrono::hours(23)))
  {
    // TODO: Implement Amazon authentication
    // This will depend on Amazon's actual Live TV API authentication mechanism
    // Common options:
    // 1. OAuth 2.0 flow
    // 2. Amazon account authentication
    // 3. Device registration
    // 4. Cookie-based authentication
    
    std::string url{"https://www.amazon.com/gp/video/livetv/api/v1/auth/token"};

    m_accessToken.clear();

    Curl curl;
    curl.AddHeader("User-Agent", AMAZON_LIVE_USER_AGENT);

    int statusCode{500};
    const std::string json{curl.Get(url, statusCode)};
    if (statusCode == 200)
    {
      nlohmann::json doc = nlohmann::json::parse(json.c_str());
      if (doc.is_discarded())
      {
        kodi::Log(ADDON_LOG_ERROR, "[GetAccessToken] ERROR: error while parsing json");
      }
      else
      {
        // Adjust field name based on actual Amazon API response
        if (doc.contains("token"))
        {
          m_accessToken = doc.at("token");
        }
        else if (doc.contains("accessToken"))
        {
          m_accessToken = doc.at("accessToken");
        }

        m_tokenTimestamp = std::chrono::steady_clock::now();
        kodi::Log(ADDON_LOG_DEBUG, "[GetAccessToken]: New Access Token: %s.", m_accessToken.c_str());
      }
    }
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "[GetAccessToken] error. status: %i, body: %s", statusCode, json.c_str());
    }
  }
  return m_accessToken;
}

std::string AmazonLiveData::GetChannelsJson() const
{
  // TODO: Update with actual Amazon Live TV API endpoint
  std::string url{"https://www.amazon.com/gp/video/livetv/api/v1/channels"};
  url += "?limit=1000";

  Curl curl;
  curl.AddHeader("authority", "www.amazon.com");
  curl.AddHeader("accept", "application/json");
  curl.AddHeader("accept-language", "en-US,en;q=0.9");
  curl.AddHeader("authorization", "Bearer " + m_accessToken);
  curl.AddHeader("origin", "https://www.amazon.com");
  curl.AddHeader("referer", "https://www.amazon.com/gp/video/livetv/");
  curl.AddHeader("user-agent", AMAZON_LIVE_USER_AGENT);

  int statusCode{500};
  const std::string json{curl.Get(url, statusCode)};
  if (statusCode == 200)
  {
    kodi::Log(ADDON_LOG_DEBUG, "[GetChannelsJson] Response: %s.", json.c_str());
    return json;
  }

  kodi::Log(ADDON_LOG_ERROR, "[GetChannelsJson] ERROR. status: %i, body: %s", statusCode,
            json.c_str());
  return {};
}

std::string AmazonLiveData::GetEpgJson(time_t start, time_t end) const
{
  // Format timestamps for Amazon API
  const std::tm* pstm_start{std::localtime(&start)};
  char startTime[21] = {};
  std::strftime(startTime, sizeof(startTime), "%Y-%m-%dT%H:%M:%SZ", pstm_start);

  const std::tm* pstm_end{std::localtime(&end)};
  char endTime[21] = {};
  std::strftime(endTime, sizeof(endTime), "%Y-%m-%dT%H:%M:%SZ", pstm_end);

  // TODO: Update with actual Amazon Live TV API endpoint
  std::string url{"https://www.amazon.com/gp/video/livetv/api/v1/epg"};
  url += "?start=" + std::string{startTime};
  url += "&end=" + std::string{endTime};
  url += "&includeMetadata=true";

  Curl curl;
  curl.AddHeader("authority", "www.amazon.com");
  curl.AddHeader("accept", "application/json");
  curl.AddHeader("accept-language", "en-US,en;q=0.9");
  curl.AddHeader("authorization", "Bearer " + m_accessToken);
  curl.AddHeader("origin", "https://www.amazon.com");
  curl.AddHeader("referer", "https://www.amazon.com/gp/video/livetv/");
  curl.AddHeader("user-agent", AMAZON_LIVE_USER_AGENT);

  int statusCode{500};
  const std::string json{curl.Get(url, statusCode)};

  if (statusCode == 200)
  {
    kodi::Log(ADDON_LOG_DEBUG, "[GetEpgJson] Response: %s.", json.c_str());
    return json;
  }

  kodi::Log(ADDON_LOG_ERROR, "[GetEpgJson] ERROR. status: %i, body: %s", statusCode, json.c_str());
  return "";
}

ADDONCREATOR(AmazonLiveData)