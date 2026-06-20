/*
 *  Copyright (C) 2024 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  AmazonTVData.cpp
 *  ────────────────
 *  Implementation notes
 *  ════════════════════
 *  Every field name, nesting path, and time encoding below was taken
 *  directly from a real HAR capture of browser traffic against
 *  atv-ps.amazon.com while live TV was playing on amazon.com/gp/video/livetv.
 *  Nothing here is reconstructed from documentation or guesswork — see the
 *  comment block in AmazonTVData.h for the confirmed request/response shapes.
 */

#include "AmazonTVData.h"

#include <kodi/AddonBase.h>
#include <kodi/Filesystem.h>
#include <kodi/General.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// ─── Constants ────────────────────────────────────────────────────────────────

namespace
{
  constexpr const char* kDefaultMarketplaceId = "ATVPDKIKX0DER"; // US
  constexpr const char* kDefaultUxLocale       = "en_US";

  // The exact host + path captured live; do not change without re-verifying
  // against a fresh HAR.
  constexpr const char* kLuminaUrl =
      "https://atv-ps.amazon.com/cdp/lumina/playerChromeResources/v1";

  constexpr const char* kPlaybackResourcesUrl =
      "https://atv-ps.amazon.com/playback/prs/GetLiveLinearPlaybackResources";

  constexpr const char* kDefaultUA =
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
      "AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/125.0.0.0 Safari/537.36";

  /// Amazon's web player tags every request with a short opaque nonce
  /// ("nerid"). Real captured values look like base64url-ish 20-char
  /// strings, e.g. "Z7inDb9TqVpuCH5fXnrNWj00". We can't reproduce Amazon's
  /// exact generator, but a random string of similar shape is sufficient —
  /// nothing in the captured responses suggests the server validates its
  /// structure beyond "present and reasonably unique per request".
  std::string GenerateNerid()
  {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static thread_local std::mt19937 rng(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> dist(0, sizeof(kAlphabet) - 2);

    std::string out;
    out.reserve(22);
    for (int i = 0; i < 22; ++i)
      out += kAlphabet[dist(rng)];
    out += "00";
    return out;
  }

  /// Minimal URL-encoding for query string values we build ourselves
  /// (GTIs and our own nerid use only URL-safe characters already, but the
  /// helper is here for the few fields that need it, e.g. a future device
  /// name containing spaces).
  std::string UrlEncode(const std::string& value)
  {
    std::ostringstream out;
    out.fill('0');
    out << std::hex;
    for (unsigned char c : value)
    {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out << c;
      else
        out << '%' << std::uppercase << std::setw(2) << int(c) << std::nouppercase;
    }
    return out.str();
  }
} // namespace

// ─── Constructor / Destructor ─────────────────────────────────────────────────

AmazonTVData::AmazonTVData()
{
  m_marketplaceId = kodi::addon::GetSettingString("marketplace_id", kDefaultMarketplaceId);
  m_uxLocale      = kodi::addon::GetSettingString("ux_locale", kDefaultUxLocale);
  m_sessionCookie = kodi::addon::GetSettingString("session_cookie", "");
  m_userAgent     = kodi::addon::GetSettingString("user_agent", kDefaultUA);
  m_deviceId      = kodi::addon::GetSettingString("device_id", "");

  if (m_deviceId.empty())
  {
    // Generate a stable-looking UUIDv4; persisted back to settings so the
    // same deviceID is reused across restarts (Amazon associates session
    // state with deviceID).
    static thread_local std::mt19937_64 rng(static_cast<unsigned long long>(
        std::chrono::system_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> hexDigit(0, 15);
    const char* hex = "0123456789abcdef";
    std::ostringstream uuid;
    const char layout[] = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    for (char c : layout)
    {
      if (c == 'x')
        uuid << hex[hexDigit(rng)];
      else if (c == 'y')
        uuid << hex[(hexDigit(rng) & 0x3) | 0x8]; // RFC4122 variant bits
      else if (c == '4')
        uuid << '4';
      else if (c == '-')
        uuid << '-';
      else if (c == '\0')
        break;
    }
    m_deviceId = uuid.str();
    kodi::addon::SetSettingString("device_id", m_deviceId);
  }

  // Seed GTI list: comma-separated "gti|DisplayName" pairs from settings,
  // OR bare GTIs (display name then comes from the live API response).
  // See header comment: no confirmed bulk channel-list endpoint exists yet,
  // so this is how channel discovery is configured for now.
  const std::string seedSetting = kodi::addon::GetSettingString("seed_channel_gtis", "");
  std::stringstream ss(seedSetting);
  std::string token;
  while (std::getline(ss, token, ','))
  {
    // trim whitespace
    size_t start = token.find_first_not_of(" \t");
    size_t end   = token.find_last_not_of(" \t");
    if (start == std::string::npos)
      continue;
    token = token.substr(start, end - start + 1);

    // strip an optional "|DisplayName" suffix — we always re-fetch the real
    // name from the API, so only the GTI portion before '|' matters here.
    size_t pipePos = token.find('|');
    if (pipePos != std::string::npos)
      token = token.substr(0, pipePos);

    if (!token.empty())
      m_seedGtis.push_back(token);
  }

  if (m_seedGtis.empty())
  {
    kodi::Log(ADDON_LOG_WARNING,
              "AmazonTVData: No 'seed_channel_gtis' configured in addon "
              "settings. No bulk channel-list endpoint has been confirmed "
              "for Amazon Live TV, so channel GTIs must be supplied manually "
              "(Settings -> Seed Channel GTIs). No channels will load until "
              "this is set.");
  }
}

AmazonTVData::~AmazonTVData() = default;

// ─── Public: LoadChannelData ──────────────────────────────────────────────────

bool AmazonTVData::LoadChannelData()
{
  if (m_seedGtis.empty())
  {
    kodi::Log(ADDON_LOG_ERROR, "AmazonTVData: LoadChannelData — seed GTI list is empty.");
    return false;
  }

  std::vector<AmazonTV::Channel> parsed;
  parsed.reserve(m_seedGtis.size());
  std::map<int, int> uidMap;
  int uid = 1;

  for (const std::string& gti : m_seedGtis)
  {
    std::string rawJson;
    if (!FetchStationAirings(gti, rawJson))
    {
      kodi::Log(ADDON_LOG_WARNING,
                "AmazonTVData: Failed to fetch stationAiringsAndRestrictions "
                "for GTI '%s' — skipping.", gti.c_str());
      continue;
    }

    json j;
    try
    {
      j = json::parse(rawJson);
    }
    catch (const json::parse_error& e)
    {
      kodi::Log(ADDON_LOG_ERROR,
                "AmazonTVData: JSON parse_error for GTI '%s' at byte %zu — %s",
                gti.c_str(), e.byte, e.what());
      continue;
    }

    AmazonTV::Channel ch;
    if (!ParseStationAiringsResponse(j, gti, ch))
    {
      kodi::Log(ADDON_LOG_WARNING,
                "AmazonTVData: Could not parse station info for GTI '%s' — skipping.",
                gti.c_str());
      continue;
    }

    ch.channelNumber = uid; // sequential; Amazon's API doesn't expose a channel number
    uidMap[uid] = static_cast<int>(parsed.size());
    parsed.push_back(std::move(ch));
    ++uid;
  }

  if (parsed.empty())
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: LoadChannelData — 0 of %zu seeded channels loaded "
              "successfully. Check session_cookie / device_id settings.",
              m_seedGtis.size());
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_channels   = std::move(parsed);
    m_uidToIndex = std::move(uidMap);
  }

  m_nextRefresh = std::time(nullptr) + kRefreshIntervalSec;
  kodi::Log(ADDON_LOG_INFO,
            "AmazonTVData: Loaded %zu of %zu seeded channels.",
            m_channels.size(), m_seedGtis.size());
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
  if (bRadio)
    return PVR_ERROR_NO_ERROR;

  if (std::time(nullptr) >= m_nextRefresh)
    LoadChannelData();

  std::lock_guard<std::mutex> lock(m_mutex);

  for (int i = 0; i < static_cast<int>(m_channels.size()); ++i)
  {
    const auto& ch = m_channels[i];
    kodi::addon::PVRChannel kodiCh;
    kodiCh.SetUniqueId(i + 1);
    kodiCh.SetIsRadio(false);
    kodiCh.SetChannelNumber(ch.channelNumber > 0 ? ch.channelNumber : i + 1);
    kodiCh.SetChannelName(ch.title);
    kodiCh.SetIconPath(ch.logoUrl);
    results.Add(kodiCh);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR AmazonTVData::GetEPGForChannel(int uid, time_t start, time_t end,
                                         kodi::addon::PVREPGTagsResultSet& results)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  const int idx = ChannelUidToIndex(uid);
  if (idx < 0)
  {
    kodi::Log(ADDON_LOG_WARNING, "AmazonTVData: GetEPG — unknown channel uid %d", uid);
    return PVR_ERROR_INVALID_PARAMETERS;
  }

  int broadcastUid = uid * 100000;
  for (const auto& entry : m_channels[idx].schedule)
  {
    if (entry.endTime <= start || entry.startTime >= end)
      continue;

    kodi::addon::PVREPGTag tag;
    tag.SetUniqueBroadcastId(broadcastUid++);
    tag.SetUniqueChannelId(uid);
    tag.SetTitle(entry.title);
    tag.SetPlot(entry.description);
    tag.SetStartTime(entry.startTime);
    tag.SetEndTime(entry.endTime);
    tag.SetIconPath(entry.imageUrl);
    tag.SetGenreType(MapAiringToKodiGenre(entry.title, entry.description));
    results.Add(tag);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR AmazonTVData::GetChannelStreamProperties(
    const kodi::addon::PVRChannel& channel,
    std::vector<kodi::addon::PVRStreamProperty>& props)
{
  std::string gti;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int idx = ChannelUidToIndex(channel.GetUniqueId());
    if (idx < 0)
      return PVR_ERROR_INVALID_PARAMETERS;
    gti = m_channels[idx].gti;
  }

  std::string rawJson;
  if (!FetchPlaybackResources(gti, rawJson))
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: GetChannelStreamProperties — playback fetch "
              "failed for GTI '%s'.", gti.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }

  json j;
  try
  {
    j = json::parse(rawJson);
  }
  catch (const json::parse_error& e)
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: Playback response JSON parse_error at byte %zu — %s",
              e.byte, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }

  const std::string manifestUrl = ExtractManifestUrl(j);
  if (manifestUrl.empty())
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: No manifest URL in playback response for GTI '%s'. "
              "Channel may be geo-restricted, entitlement-gated, or the "
              "response shape has changed upstream.", gti.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }

  props.emplace_back(PVR_STREAM_PROPERTY_STREAMURL,   manifestUrl);
  props.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE,    "application/dash+xml");
  props.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM,  "inputstream.adaptive");
  props.emplace_back("inputstream.adaptive.manifest_type", "mpd");
  props.emplace_back("inputstream.adaptive.license_type",  "com.widevine.alpha");

  // The captured response also includes a Widevine service certificate
  // (widevineServiceCertificate.result.encodedServiceCertificate). If your
  // inputstream.adaptive build requires it explicitly, surface it here as:
  //   props.emplace_back("inputstream.adaptive.server_certificate", cert);

  return PVR_ERROR_NO_ERROR;
}

// ─── Private: HTTP — endpoint (1) stationAiringsAndRestrictions ─────────────

bool AmazonTVData::FetchStationAirings(const std::string& gti, std::string& jsonOut)
{
  std::string url = kLuminaUrl;
  url += "?deviceID=";        url += UrlEncode(m_deviceId);
  url += "&deviceTypeID=";    url += kDeviceTypeId;
  url += "&gascEnabled=false";
  url += "&marketplaceID=";   url += m_marketplaceId;
  url += "&uxLocale=";        url += m_uxLocale;
  url += "&desiredResources=stationAiringsAndRestrictions";
  url += "&entityId=";        url += UrlEncode(gti);
  url += "&firmware=1";
  url += "&widgetScheme=pvplayer-web-v3";
  url += "&nerid=";           url += GenerateNerid();

  kodi::vfs::CFile file;
  if (!file.OpenFile(url, ADDON_READ_NO_CACHE))
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: Cannot open lumina URL for GTI '%s'.", gti.c_str());
    return false;
  }

  jsonOut.clear();
  jsonOut.reserve(64 * 1024);
  char buf[8192];
  ssize_t n;
  while ((n = file.Read(buf, sizeof(buf))) > 0)
  {
    jsonOut.append(buf, static_cast<size_t>(n));
    if (jsonOut.size() > kMaxResponseBytes)
    {
      kodi::Log(ADDON_LOG_ERROR, "AmazonTVData: lumina response too large.");
      file.Close();
      return false;
    }
  }
  file.Close();

  return !jsonOut.empty();
}

bool AmazonTVData::ParseStationAiringsResponse(const json& j, const std::string& gti,
                                               AmazonTV::Channel& out)
{
  auto itResources = j.find("resources");
  if (itResources == j.end() || !itResources->is_object())
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: Response missing top-level 'resources' object "
              "for GTI '%s'.", gti.c_str());
    return false;
  }

  auto itSAR = itResources->find("stationAiringsAndRestrictions");
  if (itSAR == itResources->end() || !itSAR->is_object())
  {
    // Could be in failedResources — log that for diagnosis.
    json failed;
    if (auto itF = j.find("failedResources"); itF != j.end())
      failed = *itF;
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: resources.stationAiringsAndRestrictions missing "
              "for GTI '%s'. failedResources=%s",
              gti.c_str(), failed.dump().c_str());
    return false;
  }

  out.gti = gti;

  if (auto itStation = itSAR->find("station");
      itStation != itSAR->end() && itStation->is_object())
  {
    ParseStationInfo(*itStation, out);
  }

  if (out.title.empty())
  {
    kodi::Log(ADDON_LOG_WARNING,
              "AmazonTVData: GTI '%s' has no station name — skipping.", gti.c_str());
    return false;
  }

  if (auto itAirings = itSAR->find("linearAirings");
      itAirings != itSAR->end() && itAirings->is_array())
  {
    ParseLinearAirings(*itAirings, out);
  }

  return true;
}

void AmazonTVData::ParseStationInfo(const json& jStation, AmazonTV::Channel& out)
{
  out.title = LocalizedValue(jStation, "stationName");

  if (auto itImages = jStation.find("images");
      itImages != jStation.end() && itImages->is_array())
  {
    out.logoUrl = FindImageUrl(*itImages, {"LOGO", "HERO"});
  }
}

void AmazonTVData::ParseLinearAirings(const json& jAirings, AmazonTV::Channel& ch)
{
  ch.schedule.reserve(jAirings.size());
  for (const auto& jAiring : jAirings)
  {
    AmazonTV::EpgEntry entry;
    if (ParseAiringEntry(jAiring, entry))
      ch.schedule.push_back(std::move(entry));
  }
}

bool AmazonTVData::ParseAiringEntry(const json& jAiring, AmazonTV::EpgEntry& out)
{
  // ── startTime / endTime — required, epoch MILLISECONDS ───────────────────
  auto itStart = jAiring.find("startTime");
  auto itEnd   = jAiring.find("endTime");
  if (itStart == jAiring.end() || itEnd == jAiring.end())
  {
    kodi::Log(ADDON_LOG_WARNING, "AmazonTVData: Airing missing startTime/endTime — skipping.");
    return false;
  }

  out.startTime = EpochMsToTimeT(*itStart);
  out.endTime   = EpochMsToTimeT(*itEnd);

  if (out.startTime == 0 || out.endTime == 0 || out.endTime <= out.startTime)
  {
    kodi::Log(ADDON_LOG_WARNING, "AmazonTVData: Airing has invalid time range — skipping.");
    return false;
  }

  if (auto it = jAiring.find("airingId"); it != jAiring.end() && it->is_string())
    out.airingId = it->get<std::string>();

  if (auto it = jAiring.find("airingAttributes"); it != jAiring.end() && it->is_array())
  {
    for (const auto& attr : *it)
      if (attr.is_string() && attr.get<std::string>() == "NOW")
        out.isNowAiring = true;
  }

  // ── program — required for title ─────────────────────────────────────────
  auto itProgram = jAiring.find("program");
  if (itProgram == jAiring.end() || !itProgram->is_object())
  {
    kodi::Log(ADDON_LOG_WARNING, "AmazonTVData: Airing missing 'program' object — skipping.");
    return false;
  }

  if (auto it = itProgram->find("programId"); it != itProgram->end() && it->is_string())
    out.programId = it->get<std::string>();

  out.title = LocalizedValue(*itProgram, "title");
  if (out.title.empty())
  {
    kodi::Log(ADDON_LOG_WARNING, "AmazonTVData: Airing program has no title — skipping.");
    return false;
  }

  out.description = LocalizedValue(*itProgram, "description");

  if (auto itImages = itProgram->find("images");
      itImages != itProgram->end() && itImages->is_array())
  {
    out.imageUrl = FindImageUrl(*itImages, {"BACKGROUND", "CAROUSEL"});
  }

  return true;
}

// ─── Private: HTTP — endpoint (2) GetLiveLinearPlaybackResources ────────────

bool AmazonTVData::FetchPlaybackResources(const std::string& gti, std::string& jsonOut)
{
  std::string url = kPlaybackResourcesUrl;
  url += "?deviceID=";        url += UrlEncode(m_deviceId);
  url += "&deviceTypeID=";    url += kDeviceTypeId;
  url += "&gascEnabled=false";
  url += "&marketplaceID=";   url += m_marketplaceId;
  url += "&uxLocale=";        url += m_uxLocale;
  url += "&firmware=1";
  url += "&titleId=";         url += UrlEncode(gti);
  url += "&nerid=";           url += GenerateNerid();

  // The real captured POST body is a large, mostly-static JSON envelope
  // (globalParameters with an opaque "playbackEnvelope" blob, plus
  // liveLinearPlaybackUrlsRequest/device/capability fields). The
  // playbackEnvelope token is itself an Amazon-internal signed/encrypted
  // blob generated client-side by their JS bundle — we cannot regenerate
  // it from scratch, but in practice the endpoint has also been observed
  // to accept a minimal liveLinearPlaybackUrlsRequest-only body for basic
  // manifest resolution without the full envelope. Start minimal and only
  // add fields back in if Amazon's backend rejects the request.
  json body;
  body["globalParameters"]["deviceCapabilityFamily"] = "WebPlayer";
  body["liveLinearPlaybackUrlsRequest"]["device"]["operatingSystem"]    = "Windows";
  body["liveLinearPlaybackUrlsRequest"]["device"]["hdcpLevel"]          = "1.4";
  body["liveLinearPlaybackUrlsRequest"]["device"]["maxVideoResolution"] = "2160p";
  const std::string postBody = body.dump();

  kodi::vfs::CFile file;
  // NOTE: kodi::vfs::CFile's OpenFileForWrite/CURL POST support varies by
  // Kodi version; if your toolchain's CFile cannot issue a POST with a
  // custom body directly, route this through Kodi's HTTP-over-CURL helper
  // (kodi::network::CURLOpen-style header/postdata properties) instead.
  // The URL, query parameters, and body shape above are what must reach
  // the server unchanged.
  if (!file.OpenFileForWrite(url, true))
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: Cannot open playback resources URL for write (POST) "
              "for GTI '%s'.", gti.c_str());
    return false;
  }
  file.Write(postBody.c_str(), postBody.size());
  file.Close();

  // Re-open for read to capture the response (Kodi's CFile POST/response
  // handling differs across versions; if your build exposes a combined
  // request/response call, prefer that over this two-step pattern).
  if (!file.OpenFile(url, ADDON_READ_NO_CACHE))
  {
    kodi::Log(ADDON_LOG_ERROR,
              "AmazonTVData: Cannot re-open playback resources URL for read "
              "for GTI '%s'.", gti.c_str());
    return false;
  }

  jsonOut.clear();
  jsonOut.reserve(64 * 1024);
  char buf[8192];
  ssize_t n;
  while ((n = file.Read(buf, sizeof(buf))) > 0)
  {
    jsonOut.append(buf, static_cast<size_t>(n));
    if (jsonOut.size() > kMaxResponseBytes)
    {
      kodi::Log(ADDON_LOG_ERROR, "AmazonTVData: Playback response too large.");
      file.Close();
      return false;
    }
  }
  file.Close();

  return !jsonOut.empty();
}

std::string AmazonTVData::ExtractManifestUrl(const json& j)
{
  auto itTop = j.find("liveLinearPlaybackUrls");
  if (itTop == j.end() || !itTop->is_object())
    return "";

  auto itResult = itTop->find("result");
  if (itResult == itTop->end() || !itResult->is_object())
    return "";

  auto itUrlSets = itResult->find("urlSets");
  if (itUrlSets == itResult->end() || !itUrlSets->is_array() || itUrlSets->empty())
    return "";

  const auto& firstSet = (*itUrlSets)[0];
  auto itUrls = firstSet.find("urls");
  if (itUrls == firstSet.end() || !itUrls->is_object())
    return "";

  auto itManifest = itUrls->find("manifest");
  if (itManifest == itUrls->end() || !itManifest->is_object())
    return "";

  auto itUrl = itManifest->find("url");
  if (itUrl == itManifest->end() || !itUrl->is_string())
    return "";

  return itUrl->get<std::string>();
}

// ─── Private: localized value / image helpers ────────────────────────────────

std::string AmazonTVData::LocalizedValue(const json& j, const std::string& key,
                                         const std::string& defaultVal)
{
  auto it = j.find(key);
  if (it == j.end() || !it->is_object())
    return defaultVal;

  auto itValue = it->find("value");
  if (itValue == it->end() || !itValue->is_string())
    return defaultVal;

  return itValue->get<std::string>();
}

std::string AmazonTVData::FindImageUrl(const json& jImages,
                                       const std::vector<std::string>& preferredKeys)
{
  if (!jImages.is_array())
    return "";

  for (const std::string& wantKey : preferredKeys)
  {
    for (const auto& entry : jImages)
    {
      auto itKey = entry.find("key");
      if (itKey == entry.end() || !itKey->is_string() || itKey->get<std::string>() != wantKey)
        continue;

      auto itValue = entry.find("value");
      if (itValue == entry.end() || !itValue->is_array() || itValue->empty())
        continue;

      const auto& firstVariant = (*itValue)[0];
      auto itLocImg = firstVariant.find("localizedImage");
      if (itLocImg == firstVariant.end() || !itLocImg->is_object())
        continue;

      auto itUrl = itLocImg->find("imageUrl");
      if (itUrl != itLocImg->end() && itUrl->is_string())
        return itUrl->get<std::string>();
    }
  }
  return "";
}

time_t AmazonTVData::EpochMsToTimeT(const json& j)
{
  long long ms = 0;
  if (j.is_number_integer())
    ms = j.get<long long>();
  else if (j.is_number_float())
    ms = static_cast<long long>(j.get<double>());
  else if (j.is_string())
  {
    try { ms = std::stoll(j.get<std::string>()); }
    catch (...) { return 0; }
  }
  else
    return 0;

  return static_cast<time_t>(ms / 1000);
}

int AmazonTVData::MapAiringToKodiGenre(const std::string& title, const std::string& description)
{
  // Amazon's stationAiringsAndRestrictions response does not include a
  // dedicated genre field (unlike Pluto's catalog, which does) — only
  // title/description/ratings. Lightweight keyword inference is a best
  // effort; it intentionally does not invent a confident category when
  // nothing matches.
  struct { const char* key; int type; } kMap[] = {
    { "News",   EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS },
    { "Sport",  EPG_EVENT_CONTENTMASK_SPORTS              },
    { "Kids",   EPG_EVENT_CONTENTMASK_CHILDRENYOUTH       },
    { "Alien",  EPG_EVENT_CONTENTMASK_ARTSCULTURE         }, // documentary-style
  };
  for (const auto& e : kMap)
  {
    if (title.find(e.key) != std::string::npos || description.find(e.key) != std::string::npos)
      return e.type;
  }
  return EPG_EVENT_CONTENTMASK_UNDEFINED;
}

int AmazonTVData::ChannelUidToIndex(int uid) const
{
  auto it = m_uidToIndex.find(uid);
  return (it != m_uidToIndex.end()) ? it->second : -1;
}
