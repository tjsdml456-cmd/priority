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

#include "srsran/ran/du_types.h"
#include "srsran/ran/qos/five_qi_qos_mapping.h"
#include "srsran/rlc/rlc_runtime_pdb_cache.h"
#include "srsran/rlc/rlc_tx.h"
#include "srsran/sdap/dscp_qos_mapper.h"
#include "fmt/format.h"
#include <algorithm>
#include <chrono>
#include <optional>

namespace srsran {

inline std::optional<unsigned> resolve_pdb_ms_from_dscp(uint8_t dscp)
{
  auto& mapper = dscp_qos_mapper::get_instance();
  std::optional<five_qi_t> five_qi = mapper.map_dscp_to_5qi(dscp);
  if (not five_qi.has_value()) {
    five_qi = mapper.map_dscp_to_5qi_using_standard_mapping(dscp);
  }
  if (not five_qi.has_value()) {
    return std::nullopt;
  }
  const standardized_qos_characteristics* qos_chars = get_5qi_to_qos_characteristics_mapping(five_qi.value());
  if (qos_chars == nullptr) {
    return std::nullopt;
  }
  return qos_chars->packet_delay_budget_ms;
}

inline std::optional<unsigned> resolve_rlc_hol_pdb_ms(uint32_t ue_index)
{
  auto& mapper = dscp_qos_mapper::get_instance();

  if (const std::optional<uint8_t> dscp = mapper.get_dscp_for_ue(ue_index); dscp.has_value()) {
    if (const std::optional<unsigned> pdb = resolve_pdb_ms_from_dscp(dscp.value()); pdb.has_value()) {
      cache_rlc_ue_runtime_pdb_ms(ue_index, pdb.value());
      return pdb;
    }
  }

  if (const std::optional<unsigned> runtime_pdb = mapper.get_runtime_pdb_for_ue(ue_index); runtime_pdb.has_value()) {
    cache_rlc_ue_runtime_pdb_ms(ue_index, runtime_pdb.value());
    return runtime_pdb;
  }

  if (ue_index < MAX_NOF_DU_UES) {
    return rlc_ue_runtime_pdb_cache()[ue_index];
  }
  return std::nullopt;
}

/// PDB for AQM drop: tightest of scheduler runtime PDB and SDAP DSCP PDB (phase-shrink).
inline std::optional<unsigned> resolve_rlc_hol_pdb_ms_for_drop(uint32_t ue_index)
{
  auto& mapper = dscp_qos_mapper::get_instance();

  std::optional<unsigned> dscp_pdb;
  if (const std::optional<uint8_t> dscp = mapper.get_dscp_for_ue(ue_index); dscp.has_value()) {
    dscp_pdb = resolve_pdb_ms_from_dscp(dscp.value());
  }
  const std::optional<unsigned> sched_pdb = mapper.get_runtime_pdb_for_ue(ue_index);

  if (dscp_pdb.has_value() && sched_pdb.has_value()) {
    return std::min(dscp_pdb.value(), sched_pdb.value());
  }
  if (sched_pdb.has_value()) {
    return sched_pdb;
  }
  if (dscp_pdb.has_value()) {
    return dscp_pdb;
  }

  if (ue_index < MAX_NOF_DU_UES) {
    return rlc_ue_runtime_pdb_cache()[ue_index];
  }
  return std::nullopt;
}

inline double compute_hol_sojourn_ms(const std::chrono::time_point<std::chrono::steady_clock>& hol_toa)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hol_toa).count();
}

inline void stamp_rlc_sdu_pdb_ms(rlc_sdu& sdu, uint32_t ue_index)
{
  auto& mapper = dscp_qos_mapper::get_instance();
  const std::optional<uint8_t> dscp = mapper.get_dscp_for_ue(ue_index);
  if (not dscp.has_value()) {
    return;
  }
  const std::optional<unsigned> pdb = resolve_pdb_ms_from_dscp(dscp.value());
  if (not pdb.has_value()) {
    return;
  }
  sdu.pdb_ms = pdb;
  cache_rlc_ue_runtime_pdb_ms(ue_index, pdb.value());
}

inline constexpr bool RLC_PDB_AQM_ENABLED = true;

inline constexpr double RLC_PDB_AQM_DROP_FRACTION = 1.0;

inline constexpr unsigned RLC_PDB_AQM_MAX_DROPS_PER_PASS = 16;

inline uint32_t rlc_mapper_ue_index(du_ue_index_t ue_index)
{
  return static_cast<uint32_t>(fmt::underlying(ue_index));
}

inline bool rlc_pdb_phase_shrank(std::optional<unsigned> live_pdb, std::optional<unsigned> stamped_pdb)
{
  return live_pdb.has_value() && stamped_pdb.has_value() && live_pdb.value() < stamped_pdb.value();
}

inline bool rlc_backlog_pdb_expired(const std::chrono::time_point<std::chrono::steady_clock>& hol_toa,
                                    const std::optional<unsigned>&                            stamped_pdb,
                                    uint32_t                                                  ue_index)
{
  const double                  sojourn_ms  = compute_hol_sojourn_ms(hol_toa);
  const std::optional<unsigned> current_pdb = resolve_rlc_hol_pdb_ms_for_drop(ue_index);

  if (current_pdb.has_value()) {
    return sojourn_ms >= static_cast<double>(current_pdb.value()) * RLC_PDB_AQM_DROP_FRACTION;
  }

  if (stamped_pdb.has_value()) {
    return sojourn_ms >= static_cast<double>(stamped_pdb.value()) * RLC_PDB_AQM_DROP_FRACTION;
  }
  return false;
}

inline bool rlc_sdu_pdb_expired(const rlc_sdu& sdu, uint32_t ue_index)
{
  return rlc_backlog_pdb_expired(sdu.time_of_arrival, sdu.pdb_ms, ue_index);
}

} // namespace srsran

