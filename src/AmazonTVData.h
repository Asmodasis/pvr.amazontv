/*
 *  Copyright (C) 2024 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  AmazonTVData.h
 *  ──────────────
 *  Data-layer class for the Amazon Prime Video Live TV PVR addon.
 *  Mirrors the role of PlutotvData in pvr.plutotv: owns the channel cache,
 *  talks to the backend, and hands populated Kodi result-sets back to
 *  ClientInstance. Uses nlohmann/json instead of RapidJSON.
 *
 *  ─── CONFIRMED REAL API (captured via HAR on 2026-06-20) ─────────────────────
 *
 *  Unlike Pluto TV, Amazon does not expose one tidy "all channels + full
 *  guide" JSON document. Two separate, real, currently-working unofficial
 *  endpoints were captured from genuine browser network traffic on
 *  https://www.amazon.com/gp/video/livetv while a channel was playing:
 *
 *  1) EPG / schedule for ONE channel (per-channel call, ~12h rolling window):
 *
 *     GET https://atv-ps.amazon.com/cdp/lumina/playerChromeResources/v1
 *         ?deviceID=<uuid>
 *         &deviceTypeID=AOAGZA014O5RE
 *         &gascEnabled=false
 *         &marketplaceID=ATVPDKIKX0DER
 *         &uxLocale=en_US
 *         &desiredResources=stationAiringsAndRestrictions
 *         &entityId=<channel GTI, e.g. amzn1.dv.gti.21cc7811-...>
 *         &firmware=1
 *         &widgetScheme=pvplayer-web-v3
 *         &nerid=<client-generated nonce>
 *
 *     Confirmed real response shape (abridged):
 *     {
 *       "resources": {
 *         "stationAiringsAndRestrictions": {
 *           "now": 1781918351016,
 *           "queryWindow": { "startTime": 1781896750999, "endTime": 1781939950999 },
 *           "station": {
 *             "stationName": { "value": "Ancient Aliens", "locale": "en_US" },
 *             "images": [
 *               { "key": "LOGO", "value": [{ "localizedImage": { "imageUrl": "https://..." } }] }
 *             ]
 *           },
 *           "linearAirings": [
 *             {
 *               "airingId": "amzn1.pv.linear.airing.sa-<gti>::<startMs>::pb-<uuid>",
 *               "program": {
 *                 "programId": "amzn1.pv.linear.program.<uuid>",
 *                 "title":       { "value": "Aliens and Sacred Places", "locale": "en" },
 *                 "description": { "value": "Temple Mount; shrine at Mecca.", "locale": "en" },
 *                 "images": [
 *                   { "key": "BACKGROUND", "value": [{ "localizedImage": { "imageUrl": "https://..." } }] }
 *                 ]
 *               },
 *               "startTime": 1781893980000,   // epoch MILLISECONDS
 *               "endTime":   1781896980000,   // epoch MILLISECONDS
 *               "airingAttributes": ["NOW"]
 *             }
 *           ]
 *         }
 *       },
 *       "failedResources": {}
 *     }
 *
 *  2) Live playback URL resolution (per-channel, called on tune):
 *
 *     POST https://atv-ps.amazon.com/playback/prs/GetLiveLinearPlaybackResources
 *          ?deviceID=<uuid>&deviceTypeID=AOAGZA014O5RE&gascEnabled=false
 *          &marketplaceID=ATVPDKIKX0DER&uxLocale=en_US&firmware=1
 *          &titleId=<channel GTI>&nerid=<nonce>
 *     Content-Type: text/plain
 *     Body: JSON envelope (globalParameters, liveLinearPlaybackUrlsRequest, ...)
 *
 *     Confirmed real response (abridged):
 *     {
 *       "liveLinearPlaybackUrls": {
 *         "result": {
 *           "urlSets": [
 *             { "urls": { "manifest": { "url": "https://....mpd?...", "drm": "CENC",
 *                                        "streamingTechnology": "DASH" } } }
 *           ]
 *         }
 *       },
 *       "widevineServiceCertificate": { "result": { "encodedServiceCertificate": "..." } }
 *     }
 *
 *  WHAT WE DID NOT CAPTURE: a bulk "all channels" grid endpoint. The HAR began
 *  mid-playback on an already-selected channel, so channel discovery (the
 *  list of GTIs that make up the live TV grid) is NOT confirmed from a live
 *  capture. m_seedChannels below is a configurable seed list (GTI + display
 *  name) the user populates via addon settings or a companion file; each GTI
 *  is then enriched with real logo/title/schedule data via endpoint (1).
 *  This is analogous to how pvr.plutotv seeds from Pluto's public channel
 *  list endpoint — except Amazon's equivalent bulk endpoint is unconfirmed,
 *  so we substitute a configurable seed list pending further capture.
 * ───────────────────────────────────────────────────────────────────────────
 */

#pragma once

#include <kodi/addon-instance/PVR.h>
#include <nlohmann/json.hpp>

#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace AmazonTV
{

// ─── EPG entry (one "linearAirings[]" item from stationAiringsAndRestrictions) ─
struct EpgEntry
{
  std::string airingId;     ///< amzn1.pv.linear.airing.sa-<gti>::<ms>::pb-<uuid>
  std::string programId;    ///< amzn1.pv.linear.program.<uuid>
  std::string title;
  std::string description;
  std::string imageUrl;     ///< BACKGROUND image, falls back to CAROUSEL
  time_t      startTime{0}; ///< Converted from epoch milliseconds to time_t (seconds)
  time_t      endTime{0};
  bool        isNowAiring{false}; ///< true if "airingAttributes" contains "NOW"
};

// ─── Channel (one live-TV "station") ─────────────────────────────────────────
struct Channel
{
  std::string           gti;        ///< amzn1.dv.gti.<uuid> — the channel's titleId/entityId
  std::string           title;      ///< stationName.value, e.g. "Ancient Aliens"
  std::string           logoUrl;    ///< station LOGO image
  int                   channelNumber{0};
  std::vector<EpgEntry> schedule;
};

} // namespace AmazonTV


class AmazonTVData
{
public:
  AmazonTVData();
  ~AmazonTVData();

  /**
   * @brief Refresh channel metadata + EPG for every seeded channel.
   *
   * For each GTI in the seed list, calls playerChromeResources with
   * desiredResources=stationAiringsAndRestrictions, which conveniently
   * returns BOTH the station's display info (name/logo) AND its schedule
   * in a single request — so this one call populates AmazonTV::Channel
   * fully without a separate "catalog" lookup.
   *
   * @return true if at least one channel was loaded successfully.
   */
  bool LoadChannelData();

  int        GetChannelCount() const;
  PVR_ERROR  GetChannels(bool bRadio, kodi::addon::PVRChannelsResultSet& results);
  PVR_ERROR  GetEPGForChannel(int channelUid, time_t start, time_t end,
                              kodi::addon::PVREPGTagsResultSet& results);
  PVR_ERROR  GetChannelStreamProperties(const kodi::addon::PVRChannel& channel,
                                        std::vector<kodi::addon::PVRStreamProperty>& props);

private:
  // ── HTTP: endpoint (1) — stationAiringsAndRestrictions ───────────────────

  /**
   * @brief Build the playerChromeResources URL for a given channel GTI and
   *        fetch its raw JSON body.
   *
   * @param gti      Channel GTI (e.g. "amzn1.dv.gti.21cc7811-...").
   * @param jsonOut  Output: raw JSON response text.
   * @return true on HTTP 200 with a non-empty body.
   */
  bool FetchStationAirings(const std::string& gti, std::string& jsonOut);

  /**
   * @brief Parse one playerChromeResources response into a populated Channel.
   *
   * Walks: j["resources"]["stationAiringsAndRestrictions"]
   *          -> "station"        (name + logo)
   *          -> "linearAirings"  (schedule array)
   *
   * @param j    Parsed JSON root.
   * @param gti  The GTI this response is for (carried into the result, since
   *             the response itself does not echo the request's entityId).
   * @param out  Populated channel on success.
   * @return true if at least the station name was present.
   */
  bool ParseStationAiringsResponse(const nlohmann::json& j, const std::string& gti,
                                   AmazonTV::Channel& out);

  /**
   * @brief Parse the "station" sub-object (name + images) into a Channel.
   */
  void ParseStationInfo(const nlohmann::json& jStation, AmazonTV::Channel& out);

  /**
   * @brief Parse the "linearAirings" array into channel.schedule.
   */
  void ParseLinearAirings(const nlohmann::json& jAirings, AmazonTV::Channel& ch);

  /**
   * @brief Parse a single linearAirings[] entry.
   *
   * Reads: airingId, program.{programId,title.value,description.value,images},
   *        startTime / endTime (epoch milliseconds), airingAttributes[].
   */
  bool ParseAiringEntry(const nlohmann::json& jAiring, AmazonTV::EpgEntry& out);

  // ── HTTP: endpoint (2) — GetLiveLinearPlaybackResources ──────────────────

  /**
   * @brief Build and send the playback-resolution POST for a channel GTI.
   *
   * @param gti      Channel GTI.
   * @param jsonOut  Output: raw JSON response text.
   */
  bool FetchPlaybackResources(const std::string& gti, std::string& jsonOut);

  /**
   * @brief Pull the signed DASH manifest URL out of a playback response.
   *
   * Walks: j["liveLinearPlaybackUrls"]["result"]["urlSets"][0]["urls"]["manifest"]["url"]
   *
   * @return Manifest URL, or empty string if not found.
   */
  std::string ExtractManifestUrl(const nlohmann::json& j);

  // ── Localized-value / image helpers (nlohmann/json) ──────────────────────

  /**
   * @brief Amazon wraps most display strings as { "value": "...", "locale": "..." }.
   *        This pulls the "value" field safely.
   */
  static std::string LocalizedValue(const nlohmann::json& j, const std::string& key,
                                    const std::string& defaultVal = "");

  /**
   * @brief Images are arrays of { "key": "LOGO"|"BACKGROUND"|"CAROUSEL"|"HERO",
   *        "value": [ { "localizedImage": { "imageUrl": "..." } } ] }.
   *        Finds the first imageUrl matching one of the given keys, in order
   *        of preference.
   */
  static std::string FindImageUrl(const nlohmann::json& jImages,
                                  const std::vector<std::string>& preferredKeys);

  /**
   * @brief Amazon timestamps in this API are epoch MILLISECONDS, not seconds
   *        and not ISO-8601. Converts safely to time_t (seconds).
   */
  static time_t EpochMsToTimeT(const nlohmann::json& j);

  static int MapAiringToKodiGenre(const std::string& title, const std::string& description);

  int ChannelUidToIndex(int uid) const;

  // ── State ─────────────────────────────────────────────────────────────────
  mutable std::mutex              m_mutex;
  std::vector<AmazonTV::Channel>  m_channels;
  std::map<int, int>              m_uidToIndex; ///< uid -> index into m_channels

  /// Seed list of channel GTIs to enrich on each LoadChannelData() pass.
  /// Populated from addon settings (a delimited string of GTIs), since no
  /// confirmed bulk "list all channels" endpoint exists yet — see header
  /// comment above. Each entry is just the GTI; display name/logo/schedule
  /// are all fetched live from stationAiringsAndRestrictions.
  std::vector<std::string> m_seedGtis;

  // Device / session identifiers — read from addon settings.
  std::string m_deviceId;       ///< Random per-install UUID, mirrors browser's deviceID
  std::string m_marketplaceId;  ///< e.g. "ATVPDKIKX0DER" for US
  std::string m_uxLocale;       ///< e.g. "en_US"
  std::string m_sessionCookie;  ///< Full Cookie header for authenticated requests
  std::string m_userAgent;

  time_t m_nextRefresh{0};

  static constexpr const char* kDeviceTypeId = "AOAGZA014O5RE";
  static constexpr int    kRefreshIntervalSec = 30 * 60; // EPG window is ~12h; refresh often
  static constexpr size_t kMaxResponseBytes   = 8 * 1024 * 1024;
};
