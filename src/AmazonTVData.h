/*
 *  Copyright (C) 2024 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE for more information.
 *
 *  AmazonTVData.h
 *  ──────────────
 *  Data-layer class for the Amazon Prime Video Live TV PVR addon.
 *
 *  Architecture mirrors pvr.plutotv (PlutotvData):
 *    • LoadChannelData()   — fetches the Amazon live-TV grid JSON and populates
 *                            m_channels and m_epgEntries.
 *    • GetChannels()       — hands channel list to Kodi's PVR framework.
 *    • GetEPGForChannel()  — hands EPG entries for a specific channel to Kodi.
 *    • GetChannelStreamProperties() — resolves the playback URL for a channel.
 *
 *  JSON parsing uses nlohmann/json (https://github.com/nlohmann/json).
 *  HTTP I/O uses Kodi's built-in kodi::vfs::CFile (CURL-backed).
 *
 *  ─── Amazon Live TV API ─────────────────────────────────────────────────────
 *  Endpoint (observed via browser DevTools on amazon.com/gp/video/livetv):
 *
 *    POST https://www.amazon.com/gp/video/api/getLiveTV
 *         Content-Type: application/json
 *
 *  The response envelope looks like:
 *  {
 *    "channels": [
 *      {
 *        "channelId":      "ABC123",
 *        "title":          "CNN",
 *        "logoUrl":        "https://…/cnn-logo.png",
 *        "channelNumber":  1,
 *        "streamUrl":      "https://…/stream.m3u8",
 *        "schedule": [
 *          {
 *            "programId":   "P001",
 *            "title":       "Anderson Cooper 360",
 *            "description": "In-depth news coverage.",
 *            "startTime":   "2024-06-15T20:00:00Z",
 *            "endTime":     "2024-06-15T21:00:00Z",
 *            "imageUrl":    "https://…/ac360.jpg",
 *            "genre":       "News"
 *          }
 *        ]
 *      }
 *    ]
 *  }
 *
 *  Authentication: the request requires valid Amazon session cookies (session-id,
 *  ubid-main, x-main) and the "anti-csrftoken-a2z" header obtained from the
 *  livetv page's HTML.  Pass them via AmazonTVSettings (see AmazonTVData.cpp).
 * ─────────────────────────────────────────────────────────────────────────────
 */

#pragma once

#include <kodi/addon-instance/PVR.h>
#include <nlohmann/json.hpp>

#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ─── Forward declarations ────────────────────────────────────────────────────

namespace AmazonTV
{

// ─── EPG entry (one programme in the electronic programme guide) ─────────────

struct EpgEntry
{
  std::string programId;    ///< Unique programme ID from Amazon
  std::string title;        ///< Programme title
  std::string description;  ///< Short synopsis
  std::string genre;        ///< Genre string, e.g. "News", "Sports"
  std::string imageUrl;     ///< Artwork URL
  time_t      startTime{0}; ///< UTC epoch start
  time_t      endTime{0};   ///< UTC epoch end
};

// ─── Channel ─────────────────────────────────────────────────────────────────

struct Channel
{
  std::string         channelId;     ///< Opaque string ID from Amazon
  std::string         title;         ///< Display name, e.g. "CNN"
  std::string         logoUrl;       ///< Channel logo URL
  std::string         streamUrl;     ///< HLS/DASH playback URL
  int                 channelNumber; ///< Logical channel number
  std::vector<EpgEntry> schedule;   ///< EPG entries for this channel
};

} // namespace AmazonTV


// ─── AmazonTVData ─────────────────────────────────────────────────────────────

class AmazonTVData
{
public:
  // ── Lifecycle ────────────────────────────────────────────────────────────

  AmazonTVData();
  ~AmazonTVData();

  /**
   * @brief Fetch and parse the Amazon Live TV channel grid.
   *
   * Sends the authenticated POST request, validates the JSON envelope, and
   * populates m_channels.  Should be called once at addon initialisation
   * and then periodically to refresh the EPG.
   *
   * @return true on success, false if the request failed or JSON was malformed.
   */
  bool LoadChannelData();

  // ── Kodi PVR interface ───────────────────────────────────────────────────

  /**
   * @brief Return the total number of cached channels.
   */
  int GetChannelCount() const;

  /**
   * @brief Populate Kodi's channel list from the cached data.
   *
   * @param results  Kodi result set; call results.Add() for each channel.
   * @return PVR_ERROR_NO_ERROR on success.
   */
  PVR_ERROR GetChannels(bool bRadio, kodi::addon::PVRChannelsResultSet& results);

  /**
   * @brief Populate Kodi's EPG for a single channel.
   *
   * @param channelUid  The unique ID previously assigned to the channel.
   * @param start       EPG window start (UTC epoch).
   * @param end         EPG window end   (UTC epoch).
   * @param results     Kodi result set.
   * @return PVR_ERROR_NO_ERROR on success.
   */
  PVR_ERROR GetEPGForChannel(int                             channelUid,
                             time_t                          start,
                             time_t                          end,
                             kodi::addon::PVREPGTagsResultSet& results);

  /**
   * @brief Provide stream properties (URL, MIME type) for a channel.
   *
   * @param channel     The Kodi channel object.
   * @param properties  Output list; append PVRStreamProperty entries.
   * @return PVR_ERROR_NO_ERROR on success.
   */
  PVR_ERROR GetChannelStreamProperties(
      const kodi::addon::PVRChannel&           channel,
      std::vector<kodi::addon::PVRStreamProperty>& properties);

private:
  // ── HTTP helpers ──────────────────────────────────────────────────────────

  /**
   * @brief Send the live-TV POST request and return the raw JSON body.
   *
   * @param jsonBody  Output: full JSON response string.
   * @return true on HTTP 200, false otherwise.
   */
  bool FetchLiveTVJson(std::string& jsonBody);

  // ── JSON parsing ──────────────────────────────────────────────────────────

  /**
   * @brief Parse the top-level "channels" array from the API response.
   *
   * @param j  Parsed JSON object (nlohmann::json).
   * @return true if at least one channel was parsed successfully.
   */
  bool ParseChannels(const nlohmann::json& j);

  /**
   * @brief Parse a single channel object and append it to m_channels.
   *
   * @param jChannel  A single entry from the "channels" JSON array.
   * @param out       Populated AmazonTV::Channel on success.
   * @return true if mandatory fields were present.
   */
  bool ParseChannel(const nlohmann::json& jChannel, AmazonTV::Channel& out);

  /**
   * @brief Parse the "schedule" array inside a channel JSON object.
   *
   * @param jSchedule  The JSON array value of the "schedule" key.
   * @param channel    The channel to append EpgEntry items to.
   */
  void ParseSchedule(const nlohmann::json& jSchedule, AmazonTV::Channel& channel);

  /**
   * @brief Parse a single EPG entry inside a schedule array.
   *
   * @param jEntry  A single schedule item.
   * @param out     Populated AmazonTV::EpgEntry on success.
   * @return true if mandatory fields were present.
   */
  bool ParseEpgEntry(const nlohmann::json& jEntry, AmazonTV::EpgEntry& out);

  // ── Utility helpers ───────────────────────────────────────────────────────

  /**
   * @brief Convert an ISO-8601 UTC string ("2024-06-15T20:00:00Z") to time_t.
   *
   * @param isoString  Input string.
   * @return Parsed UTC epoch, or 0 on failure.
   */
  static time_t ParseISO8601(const std::string& isoString);

  /**
   * @brief Map from channelUid (int) → AmazonTV::Channel index so GetEPG and
   *        GetChannelStreamProperties can look up data in O(1).
   */
  int ChannelUidToIndex(int uid) const;

  int AmazonTVData::MapGenreToKodi(const std::string& genre);

  // ── State ─────────────────────────────────────────────────────────────────

  mutable std::mutex            m_mutex;       ///< Guards m_channels
  std::vector<AmazonTV::Channel> m_channels;   ///< Cached channel+EPG data

  /// Map channelUid → index into m_channels (rebuilt on each LoadChannelData)
  std::map<int, int>            m_uidToIndex;

  // Amazon API configuration (populated from addon settings)
  std::string m_apiEndpoint;   ///< e.g. "https://www.amazon.com/gp/video/api/getLiveTV"
  std::string m_sessionCookie; ///< Full Cookie header value (session-id, ubid-main, x-main)
  std::string m_csrfToken;     ///< anti-csrftoken-a2z header value
  std::string m_marketplaceId; ///< e.g. "ATVPDKIKX0DER" for US
  std::string m_userAgent;     ///< Browser UA string for requests

  // Next scheduled refresh time
  time_t m_nextRefresh{0};
};
