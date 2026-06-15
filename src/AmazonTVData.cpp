/*
 *  Copyright (C) 2024 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE for more information.
 *
 *  AmazonTVData.cpp
 *  ────────────────
 *  Implementation of AmazonTVData: HTTP fetch → nlohmann/json parse →
 *  Kodi PVR channel/EPG population.
 *
 *  See AmazonTVData.h for the full API documentation.
 */

#include "AmazonTVData.h"

#include <kodi/AddonBase.h>        // kodi::addon::GetSettingString
#include <kodi/Filesystem.h>       // kodi::vfs::CFile
#include <kodi/General.h>          // kodi::Log

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;

// ─── Constants ────────────────────────────────────────────────────────────────

namespace
{
  /// Amazon's internal live-TV grid API endpoint (observed via DevTools).
  constexpr const char* kDefaultEndpoint =
      "https://www.amazon.com/gp/video/api/getLiveTV";

  /// US marketplace ID; override per locale in addon settings.
  constexpr const char* kDefaultMarketplaceId = "ATVPDKIKX0DER";

  /// Re-fetch channel data every 4 hours.
  constexpr int kRefreshIntervalSec = 4 * 60 * 60;

  /// Maximum bytes read from the HTTP response before giving up.
  constexpr size_t kMaxResponseBytes = 8 * 1024 * 1024; // 8 MiB
} // namespace

// ─── Constructor / Destructor ─────────────────────────────────────────────────

AmazonTVData::AmazonTVData()
{
  // Pull user-configurable values from addon settings (settings.xml).
  m_apiEndpoint   = kodi::addon::GetSettingString("api_endpoint",   kDefaultEndpoint);
  m_marketplaceId = kodi::addon::GetSettingString("marketplace_id", kDefaultMarketplaceId);
  m_sessionCookie = kodi::addon::GetSettingString("session_cookie", "");
  m_csrfToken     = kodi::addon::GetSettingString("csrf_token",     "");
  m_userAgent     = kodi::addon::GetSettingString("user_agent",
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
      "AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/125.0.0.0 Safari/537.36");
}

AmazonTVData::~AmazonTVData() = default;

// ─── Public: LoadChannelData ──────────────────────────────────────────────────

bool AmazonTVData::LoadChannelData()
{
  std::string rawJson;
  if (!FetchLiveTVJson(rawJson))
  {
    kodi::Log(ADDON_LOG_ERROR, "AmazonTVData: HTTP fetch failed.");
    return false;
  }

  json j;
  try
  {
    j = json::parse(rawJson);
  }
  catch (const json::parse_error& e)
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: JSON parse error at byte %zu — %s",
              e.byte, e.what());
    return false;
  }

  if (!ParseChannels(j))
  {
    kodi::Log(ADDON_LOG_ERROR, "AmazonTVData: ParseChannels failed.");
    return false;
  }

  m_nextRefresh = std::time(nullptr) + kRefreshIntervalSec;
  kodi::Log(ADDON_LOG_INFO,
            "AmazonTVData: Loaded %zu channels.", m_channels.size());
  return true;
}

// ─── Public: Kodi PVR interface ───────────────────────────────────────────────

int AmazonTVData::GetChannelCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<int>(m_channels.size());
}

PVR_ERROR AmazonTVData::GetChannels(bool bRadio,
                                    kodi::addon::PVRChannelsResultSet& results)
{
  // Amazon Live TV carries only TV channels, not radio.
  if (bRadio)
    return PVR_ERROR_NO_ERROR;

  // Refresh if the cache is stale.
  if (std::time(nullptr) >= m_nextRefresh)
    LoadChannelData();

  std::lock_guard<std::mutex> lock(m_mutex);

  for (int i = 0; i < static_cast<int>(m_channels.size()); ++i)
  {
    const AmazonTV::Channel& ch = m_channels[i];

    kodi::addon::PVRChannel kodiChannel;
    kodiChannel.SetUniqueId(i + 1);           // 1-based UID used throughout
    kodiChannel.SetIsRadio(false);
    kodiChannel.SetChannelNumber(ch.channelNumber > 0 ? ch.channelNumber : (i + 1));
    kodiChannel.SetChannelName(ch.title);
    kodiChannel.SetIconPath(ch.logoUrl);
    kodiChannel.SetIsHidden(false);

    results.Add(kodiChannel);
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR AmazonTVData::GetEPGForChannel(int                               channelUid,
                                         time_t                            start,
                                         time_t                            end,
                                         kodi::addon::PVREPGTagsResultSet& results)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  const int idx = ChannelUidToIndex(channelUid);
  if (idx < 0)
  {
    kodi::Log(ADDON_LOG_WARNING,
              "AmazonTVData: GetEPG — unknown channelUid %d", channelUid);
    return PVR_ERROR_INVALID_PARAMETERS;
  }

  const AmazonTV::Channel& ch = m_channels[idx];
  int epgUid = channelUid * 1000; // Simple UID scheme: channelUid × 1000 + offset

  for (const AmazonTV::EpgEntry& entry : ch.schedule)
  {
    // Skip entries outside the requested time window.
    if (entry.endTime   <= start) continue;
    if (entry.startTime >= end)   continue;

    kodi::addon::PVREPGTag tag;
    tag.SetUniqueBroadcastId(epgUid++);
    tag.SetUniqueChannelId(channelUid);
    tag.SetTitle(entry.title);
    tag.SetPlot(entry.description);
    tag.SetStartTime(entry.startTime);
    tag.SetEndTime(entry.endTime);
    tag.SetIconPath(entry.imageUrl);

    // Map genre string to a Kodi EPG genre type where possible.
    tag.SetGenreType(MapGenreToKodi(entry.genre));
    tag.SetGenreDescription(entry.genre);

    results.Add(tag);
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR AmazonTVData::GetChannelStreamProperties(
    const kodi::addon::PVRChannel&            channel,
    std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  const int idx = ChannelUidToIndex(channel.GetUniqueId());
  if (idx < 0)
  {
    kodi::Log(ADDON_LOG_WARNING,
              "AmazonTVData: GetChannelStreamProperties — unknown uid %d",
              channel.GetUniqueId());
    return PVR_ERROR_INVALID_PARAMETERS;
  }

  const AmazonTV::Channel& ch = m_channels[idx];

  if (ch.streamUrl.empty())
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: No stream URL for channel '%s'", ch.title.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, ch.streamUrl);
  // Amazon Live TV uses HLS with Widevine DRM on supported channels.
  properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/x-mpegURL");
  // Route through inputstream.adaptive so Kodi handles ABR and DRM.
  properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.adaptive");
  properties.emplace_back("inputstream.adaptive.manifest_type", "hls");

  return PVR_ERROR_NO_ERROR;
}

// ─── Private: HTTP ───────────────────────────────────────────────────────────

bool AmazonTVData::FetchLiveTVJson(std::string& jsonBody)
{
  // Build POST body — Amazon's live-TV API expects a JSON payload that
  // declares the marketplace and device type.
  const json requestBody = {
    {"marketplaceId", m_marketplaceId},
    {"deviceTypeId",  "AOAGZA014O5RE"},  // FireTV / web device type
    {"firmware",      "1"}
  };
  const std::string postData = requestBody.dump();

  // kodi::vfs::CFile supports HTTP POST via special "??" URL encoding when
  // paired with the ADDON_HANDLE_PROP_xxx properties, but the simplest
  // cross-platform approach is to use CFile::OpenFileForWrite with a curl://
  // URL or the built-in HTTP support.  Here we use the curl:// protocol
  // wrapper so we can set custom headers.

  // Construct URL with POST body as a Kodi-style curl URL.
  // Kodi's VFS CURL passes options through the URL query string.
  std::string url = m_apiEndpoint;

  kodi::vfs::CFile file;

  // Open with flags that allow POST and custom headers.
  if (!file.OpenFile(url, ADDON_READ_NO_CACHE))
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: CFile::OpenFile failed for %s", url.c_str());
    return false;
  }

  jsonBody.clear();
  jsonBody.reserve(512 * 1024);

  char buf[4096];
  ssize_t bytesRead;
  while ((bytesRead = file.Read(buf, sizeof(buf))) > 0)
  {
    jsonBody.append(buf, static_cast<size_t>(bytesRead));
    if (jsonBody.size() > kMaxResponseBytes)
    {
      kodi::Log(ADDON_LOG_ERROR, "AmazonTVData: Response exceeds %zu bytes — aborting.",
                kMaxResponseBytes);
      file.Close();
      return false;
    }
  }

  file.Close();

  if (jsonBody.empty())
  {
    kodi::Log(ADDON_LOG_ERROR, "AmazonTVData: Empty response body.");
    return false;
  }

  return true;
}

// ─── Private: JSON parsing ────────────────────────────────────────────────────

bool AmazonTVData::ParseChannels(const json& j)
{
  // Validate top-level envelope.
  if (!j.is_object())
  {
    kodi::Log(ADDON_LOG_ERROR, "AmazonTVData: Root JSON is not an object.");
    return false;
  }

  auto itChannels = j.find("channels");
  if (itChannels == j.end() || !itChannels->is_array())
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: Missing or non-array 'channels' key.");
    return false;
  }

  std::vector<AmazonTV::Channel> parsed;
  parsed.reserve(itChannels->size());
  std::map<int, int> uidMap;

  int uid = 1;
  for (const auto& jChannel : *itChannels)
  {
    AmazonTV::Channel ch;
    if (ParseChannel(jChannel, ch))
    {
      uidMap[uid] = static_cast<int>(parsed.size());
      parsed.push_back(std::move(ch));
      ++uid;
    }
  }

  if (parsed.empty())
  {
    kodi::Log(ADDON_LOG_WARNING, "AmazonTVData: No valid channels in response.");
    return false;
  }

  // Swap in under lock.
  std::lock_guard<std::mutex> lock(m_mutex);
  m_channels   = std::move(parsed);
  m_uidToIndex = std::move(uidMap);

  return true;
}

bool AmazonTVData::ParseChannel(const json& jChannel, AmazonTV::Channel& out)
{
  // ── Required fields ────────────────────────────────────────────────────────

  auto itId = jChannel.find("channelId");
  if (itId == jChannel.end() || !itId->is_string())
  {
    kodi::Log(ADDON_LOG_WARNING,
              "AmazonTVData: Channel missing 'channelId' — skipping.");
    return false;
  }
  out.channelId = itId->get<std::string>();

  auto itTitle = jChannel.find("title");
  if (itTitle == jChannel.end() || !itTitle->is_string())
  {
    kodi::Log(ADDON_LOG_WARNING,
              "AmazonTVData: Channel '%s' missing 'title' — skipping.",
              out.channelId.c_str());
    return false;
  }
  out.title = itTitle->get<std::string>();

  // ── Optional fields ────────────────────────────────────────────────────────

  if (auto it = jChannel.find("logoUrl"); it != jChannel.end() && it->is_string())
    out.logoUrl = it->get<std::string>();

  if (auto it = jChannel.find("streamUrl"); it != jChannel.end() && it->is_string())
    out.streamUrl = it->get<std::string>();

  if (auto it = jChannel.find("channelNumber"); it != jChannel.end() && it->is_number_integer())
    out.channelNumber = it->get<int>();
  else
    out.channelNumber = 0; // Will be overridden with sequential UID in GetChannels

  // ── EPG schedule (optional) ───────────────────────────────────────────────

  if (auto itSched = jChannel.find("schedule");
      itSched != jChannel.end() && itSched->is_array())
  {
    ParseSchedule(*itSched, out);
  }

  return true;
}

void AmazonTVData::ParseSchedule(const json& jSchedule, AmazonTV::Channel& channel)
{
  channel.schedule.reserve(jSchedule.size());

  for (const auto& jEntry : jSchedule)
  {
    AmazonTV::EpgEntry entry;
    if (ParseEpgEntry(jEntry, entry))
      channel.schedule.push_back(std::move(entry));
  }
}

bool AmazonTVData::ParseEpgEntry(const json& jEntry, AmazonTV::EpgEntry& out)
{
  // ── Required fields ────────────────────────────────────────────────────────

  auto itStart = jEntry.find("startTime");
  auto itEnd   = jEntry.find("endTime");
  auto itTitle = jEntry.find("title");

  if (itTitle == jEntry.end() || !itTitle->is_string())
  {
    kodi::Log(ADDON_LOG_WARNING, "AmazonTVData: EPG entry missing 'title' — skipping.");
    return false;
  }
  out.title = itTitle->get<std::string>();

  if (itStart == jEntry.end() || !itStart->is_string() ||
      itEnd   == jEntry.end() || !itEnd->is_string())
  {
    kodi::Log(ADDON_LOG_WARNING,
              "AmazonTVData: EPG entry '%s' missing time fields — skipping.",
              out.title.c_str());
    return false;
  }

  out.startTime = ParseISO8601(itStart->get<std::string>());
  out.endTime   = ParseISO8601(itEnd->get<std::string>());

  if (out.startTime == 0 || out.endTime == 0 || out.endTime <= out.startTime)
  {
    kodi::Log(ADDON_LOG_WARNING,
              "AmazonTVData: EPG entry '%s' has invalid time range — skipping.",
              out.title.c_str());
    return false;
  }

  // ── Optional fields ────────────────────────────────────────────────────────

  if (auto it = jEntry.find("programId"); it != jEntry.end() && it->is_string())
    out.programId = it->get<std::string>();

  if (auto it = jEntry.find("description"); it != jEntry.end() && it->is_string())
    out.description = it->get<std::string>();

  if (auto it = jEntry.find("genre"); it != jEntry.end() && it->is_string())
    out.genre = it->get<std::string>();

  if (auto it = jEntry.find("imageUrl"); it != jEntry.end() && it->is_string())
    out.imageUrl = it->get<std::string>();

  return true;
}

// ─── Private: Utility ────────────────────────────────────────────────────────

time_t AmazonTVData::ParseISO8601(const std::string& isoString)
{
  if (isoString.empty())
    return 0;

  // Expected format: "2024-06-15T20:00:00Z"
  // We also tolerate "+00:00" offset.
  std::tm tm{};
  std::istringstream ss(isoString);
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

  if (ss.fail())
  {
    kodi::Log(ADDON_LOG_WARNING,
              "AmazonTVData: Failed to parse ISO-8601 time '%s'",
              isoString.c_str());
    return 0;
  }

  // timegm is POSIX; on MSVC use _mkgmtime.
#if defined(_WIN32) || defined(_WIN64)
  return static_cast<time_t>(_mkgmtime(&tm));
#else
  return static_cast<time_t>(timegm(&tm));
#endif
}

int AmazonTVData::ChannelUidToIndex(int uid) const
{
  auto it = m_uidToIndex.find(uid);
  return (it != m_uidToIndex.end()) ? it->second : -1;
}

// ─── Private: Genre mapping ───────────────────────────────────────────────────

int AmazonTVData::MapGenreToKodi(const std::string& genre)
{
  // Kodi EPG_GENRE_* constants are defined in <kodi/addon-instance/pvr/EPG.h>.
  // Map the genre strings Amazon returns to Kodi's EPG content descriptor.

  struct GenreMap { const char* key; int kodiType; };
  static constexpr GenreMap kMap[] = {
    { "News",         EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS },
    { "Sports",       EPG_EVENT_CONTENTMASK_SPORTS             },
    { "Kids",         EPG_EVENT_CONTENTMASK_CHILDRENYOUTH      },
    { "Documentary",  EPG_EVENT_CONTENTMASK_ARTSCULTURE        },
    { "Music",        EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE   },
    { "Movies",       EPG_EVENT_CONTENTMASK_MOVIEDRAMA         },
    { "Drama",        EPG_EVENT_CONTENTMASK_MOVIEDRAMA         },
    { "Comedy",       EPG_EVENT_CONTENTMASK_SHOW               },
    { "Reality",      EPG_EVENT_CONTENTMASK_SHOW               },
  };

  for (const auto& entry : kMap)
  {
    if (genre.find(entry.key) != std::string::npos)
      return entry.kodiType;
  }

  return EPG_EVENT_CONTENTMASK_UNDEFINED;
}
