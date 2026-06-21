/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#pragma once

#include "srsran/ran/qos/five_qi.h"
#include "srsran/ran/qos/five_qi_qos_mapping.h"
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace srsran {

/// DSCP-profile GBR targets (bps). MBR equals GBR for each profile.
inline constexpr uint64_t DSCP_PROFILE_GBR_BPS    = 7'000'000;
inline constexpr uint64_t DSCP_PROFILE_DC_GBR_BPS = 5'000'000;
/// GBR/DC-GBR flows at or above this rate use TBS-level air cap (token bucket + per-grant TBS limit).
inline constexpr uint64_t DL_AIR_TBS_CAP_MIN_GBR_BPS = DSCP_PROFILE_DC_GBR_BPS;
/// SDAP-observed IPv4 PDUs below this size must not seed or downgrade an active GBR/DC-GBR profile.
inline constexpr unsigned DSCP_MAPPER_MIN_PDU_LEN = 128;

/// GFBR/MFBR / scheduler rate targets per DSCP profile (GBR 7M, DC-GBR 5M; MBR = GBR).
struct dscp_qos_rate_target {
  uint64_t gbr_bps = 0;
  uint64_t mbr_bps = 0;
};

/// DSCP profile: 5QI mapping plus optional scheduler rate target.
struct dscp_qos_profile {
  five_qi_t                           five_qi;
  std::optional<dscp_qos_rate_target> rates;
};

/// \brief Manages DSCP to 5QI mapping based on actual DSCP values extracted from IP packets
/// This class allows dynamic mapping of DSCP values to 5QI without hardcoding specific values
class dscp_qos_mapper
{
public:
  // 싱글톤 패턴으로 구현
  /// \brief Get singleton instance
  static dscp_qos_mapper& get_instance()
  {
    static dscp_qos_mapper instance;
    return instance;
  }
  
  // UE별 DSCP 값 저장 (SDAP에서 호출)
  /// \brief Register a DSCP value observed for a specific UE
  /// \param pdu_len IPv4 PDU length from SDAP (0 = explicit control path, no size filter).
  /// Small non-GBR packets must not seed the mapper or downgrade GBR/DC-GBR (e.g. 60-byte TCP ACK).
  void register_dscp_for_ue(uint32_t ue_index, uint8_t dscp, unsigned pdu_len = 0)
  {
    std::lock_guard<std::mutex> lock(mutex);

    const bool new_has_rate = dscp_has_qos_rate_target_locked(dscp);

    auto it = ue_dscp_map.find(ue_index);
    if (it == ue_dscp_map.end()) {
      if (pdu_len > 0 && pdu_len < DSCP_MAPPER_MIN_PDU_LEN && !new_has_rate) {
        return;
      }
      ue_dscp_map[ue_index] = dscp;
      return;
    }

    if (it->second == dscp) {
      return;
    }

    const bool cur_has_rate = dscp_has_qos_rate_target_locked(it->second);
    if (pdu_len > 0 && pdu_len < DSCP_MAPPER_MIN_PDU_LEN && cur_has_rate && !new_has_rate) {
      return;
    }

    ue_dscp_map[ue_index] = dscp;
  }
  
  // UE별 DSCP 값 조회 (DU, 스케줄러에서 호출)
  /// \brief Get the DSCP value for a specific UE
  std::optional<uint8_t> get_dscp_for_ue(uint32_t ue_index) const
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = ue_dscp_map.find(ue_index);
    if (it != ue_dscp_map.end()) {
      return it->second;
    }
    return {};
  }
 
  // 명시적 DSCP→5QI 매핑 설정
  /// \brief Set explicit DSCP to 5QI mapping
  /// This allows custom mapping of specific DSCP values to 5QI
  /// Each DSCP value must be individually mapped - no automatic range-based mapping
  void set_dscp_to_5qi_mapping(uint8_t dscp, five_qi_t five_qi)
  {
    std::lock_guard<std::mutex> lock(mutex);
    dscp_to_5qi_map[dscp] = five_qi;
  }
  
  // DSCP→5QI 매핑 조회
  /// \brief Map DSCP value to 5QI
  /// Uses explicit mapping table - each DSCP value must be explicitly mapped to 5QI
  /// Returns empty if no mapping exists for the given DSCP value
  std::optional<five_qi_t> map_dscp_to_5qi(uint8_t dscp) const
  {
    std::lock_guard<std::mutex> lock(mutex);
    
    // Check if explicit mapping exists
    auto explicit_it = dscp_to_5qi_map.find(dscp);
    if (explicit_it != dscp_to_5qi_map.end()) {
      return explicit_it->second;
    }

    // No mapping found - return empty to indicate this DSCP is not mapped
    return {};
  }
  
  // 첫 관찰 시 자동 매핑 (DSCP 값에 따라 표준 5QI 선택)
  /// \brief Auto-map DSCP to 5QI when first observed using fine-grained mapping
  /// Uses predefined DSCP-to-5QI mapping table for more precise mapping
  /// Falls back to standard 5QI selection if DSCP is not in the predefined table
  void auto_map_dscp_on_first_observation(uint8_t dscp, uint32_t ue_index)
  {
    std::lock_guard<std::mutex> lock(mutex);
    
    // Check if mapping already exists
    if (dscp_to_5qi_map.find(dscp) != dscp_to_5qi_map.end()) {
      return; // Mapping already exists, don't overwrite
    }

    const auto& profile_table = get_dscp_qos_profile_table();
    auto        profile_it    = profile_table.find(dscp);
    if (profile_it != profile_table.end()) {
      const five_qi_t selected_5qi = profile_it->second.five_qi;
      if (get_5qi_to_qos_characteristics_mapping(selected_5qi) != nullptr) {
        dscp_to_5qi_map[dscp] = selected_5qi;
        return;
      }
    }
    
    // Fallback: 매핑 테이블에 없거나 유효하지 않은 경우 기본값 사용
    dscp_to_5qi_map[dscp] = five_qi_t(9);
  }

  /// \brief Map DSCP to 5QI using standard 5QI characteristics
  /// Uses the same predefined mapping table as auto_map_dscp_on_first_observation
  std::optional<five_qi_t> map_dscp_to_5qi_using_standard_mapping(uint8_t dscp) const
  {
    std::lock_guard<std::mutex> lock(mutex);
    
    // Check if explicit mapping exists first
    auto explicit_it = dscp_to_5qi_map.find(dscp);
    if (explicit_it != dscp_to_5qi_map.end()) {
      return explicit_it->second;
    }

    return resolve_dscp_to_5qi_locked(dscp);
  }

  /// \brief Get all registered DSCP values
  std::map<uint8_t, five_qi_t> get_all_dscp_mappings() const
  {
    std::lock_guard<std::mutex> lock(mutex);
    std::map<uint8_t, five_qi_t> result;
    for (const auto& [dscp, five_qi] : dscp_to_5qi_map) {
      result[dscp] = five_qi;
    }
    return result;
  }

  /// \brief Map DSCP to scheduler rate target from profile (GBR / DC-GBR only; non-GBR has no rate target).
  std::optional<dscp_qos_rate_target> map_dscp_to_qos_rates(uint8_t dscp) const
  {
    std::optional<dscp_qos_rate_target> explicit_rates;
    std::optional<dscp_qos_profile>     profile;
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto explicit_rate_it = dscp_to_qos_rate_map.find(dscp);
      if (explicit_rate_it != dscp_to_qos_rate_map.end()) {
        explicit_rates = explicit_rate_it->second;
      }
      profile = resolve_dscp_profile_locked(dscp);
    }
    if (explicit_rates.has_value()) {
      return explicit_rates;
    }
    if (not profile.has_value() or not profile->rates.has_value()) {
      return {};
    }
    return profile->rates;
  }

  /// \brief GFBR/MFBR for the DSCP last observed on this UE (from SDAP / iperf3).
  std::optional<dscp_qos_rate_target> get_qos_rates_for_ue(uint32_t ue_index) const
  {
    const std::optional<uint8_t> dscp = get_dscp_for_ue(ue_index);
    if (not dscp.has_value()) {
      return {};
    }
    return map_dscp_to_qos_rates(dscp.value());
  }

  /// \brief Override GFBR/MFBR for a DSCP value (runtime).
  void set_dscp_to_qos_rate_mapping(uint8_t dscp, const dscp_qos_rate_target& rates)
  {
    std::lock_guard<std::mutex> lock(mutex);
    dscp_to_qos_rate_map[dscp] = rates;
  }

private:
  dscp_qos_mapper()  = default;
  ~dscp_qos_mapper() = default;
  dscp_qos_mapper(const dscp_qos_mapper&) = delete;
  dscp_qos_mapper& operator=(const dscp_qos_mapper&) = delete;

  /// \brief DSCP profile table: GBR 7 Mbps, DC-GBR 5 Mbps (MBR = GBR). non-GBR has no rate target.
  static const std::map<uint8_t, dscp_qos_profile>& get_dscp_qos_profile_table()
  {
    static const dscp_qos_rate_target gbr_rates{DSCP_PROFILE_GBR_BPS, DSCP_PROFILE_GBR_BPS};
    static const dscp_qos_rate_target dc_gbr_rates{DSCP_PROFILE_DC_GBR_BPS, DSCP_PROFILE_DC_GBR_BPS};

    static const std::map<uint8_t, dscp_qos_profile> profile_table = {
        // GBR — 7 Mbps
        {44, {uint_to_five_qi(66), gbr_rates}},
        {34, {uint_to_five_qi(2), gbr_rates}},
        {32, {uint_to_five_qi(3), gbr_rates}},
        {28, {uint_to_five_qi(4), gbr_rates}},
        {38, {uint_to_five_qi(67), gbr_rates}},
        // non-GBR — 5QI/PDB/priority only (no token bucket)
        {40, {uint_to_five_qi(5), std::nullopt}},
        {26, {uint_to_five_qi(6), std::nullopt}},
        {22, {uint_to_five_qi(7), std::nullopt}},
        {0, {uint_to_five_qi(9), std::nullopt}},
        {30, {uint_to_five_qi(70), std::nullopt}},
        {24, {uint_to_five_qi(80), std::nullopt}},
        // Delay-critical GBR — 5 Mbps
        {17, {uint_to_five_qi(82), dc_gbr_rates}},
        {16, {uint_to_five_qi(83), dc_gbr_rates}},
        {15, {uint_to_five_qi(84), dc_gbr_rates}},
        {14, {uint_to_five_qi(85), dc_gbr_rates}},
    };
    return profile_table;
  }

  /// \brief Resolve DSCP profile (mutex must be held). Runtime 5QI override updates five_qi only.
  std::optional<dscp_qos_profile> resolve_dscp_profile_locked(uint8_t dscp) const
  {
    const auto& profile_table = get_dscp_qos_profile_table();
    auto        profile_it    = profile_table.find(dscp);
    if (profile_it == profile_table.end()) {
      auto explicit_it = dscp_to_5qi_map.find(dscp);
      if (explicit_it != dscp_to_5qi_map.end()) {
        return dscp_qos_profile{explicit_it->second, std::nullopt};
      }
      return {};
    }
    dscp_qos_profile profile = profile_it->second;
    auto             explicit_it = dscp_to_5qi_map.find(dscp);
    if (explicit_it != dscp_to_5qi_map.end()) {
      profile.five_qi = explicit_it->second;
    }
    if (get_5qi_to_qos_characteristics_mapping(profile.five_qi) == nullptr) {
      return {};
    }
    return profile;
  }

  /// \brief Resolve DSCP to 5QI using explicit and profile tables (mutex must be held).
  std::optional<five_qi_t> resolve_dscp_to_5qi_locked(uint8_t dscp) const
  {
    const std::optional<dscp_qos_profile> profile = resolve_dscp_profile_locked(dscp);
    if (profile.has_value()) {
      return profile->five_qi;
    }
    return {};
  }

  bool dscp_has_qos_rate_target_locked(uint8_t dscp) const
  {
    const auto explicit_rate_it = dscp_to_qos_rate_map.find(dscp);
    if (explicit_rate_it != dscp_to_qos_rate_map.end()) {
      return explicit_rate_it->second.gbr_bps > 0 || explicit_rate_it->second.mbr_bps > 0;
    }
    const std::optional<dscp_qos_profile> profile = resolve_dscp_profile_locked(dscp);
    return profile.has_value() && profile->rates.has_value();
  }

  mutable std::mutex                          mutex;
  std::unordered_map<uint32_t, uint8_t>      ue_dscp_map;          ///< UE index -> DSCP mapping
  std::map<uint8_t, five_qi_t>               dscp_to_5qi_map;      ///< Runtime DSCP -> 5QI override
  std::map<uint8_t, dscp_qos_rate_target>    dscp_to_qos_rate_map; ///< Runtime DSCP -> GBR/MBR override
};

} // namespace srsran







