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

#include "scheduler_time_qos.h"
#include "../slicing/slice_ue_repository.h"
#include "../support/csi_report_helpers.h"
#include "../ue_scheduling/grant_params_selector.h"
#include "srsran/ran/qos/five_qi_qos_mapping.h"
#include "srsran/ran/qos/five_qi.h"
#include "srsran/ran/qos/qos_parameters.h"
#include "srsran/sdap/dscp_qos_mapper.h"
#include "srsran/srslog/srslog.h"
#include "fmt/format.h"
#include <algorithm>

using namespace srsran;

// [Implementation-defined] Limit for the coefficient of the proportional fair metric to avoid issues with double
// imprecision.
static constexpr unsigned MAX_PF_COEFF = 10;

// [Implementation-defined] Maximum number of slots skipped between scheduling opportunities.
static constexpr unsigned MAX_SLOT_SKIPPED = 20;

static bool qos_res_type_uses_gbr_rate_target(qos_flow_resource_type res_type)
{
  return res_type == qos_flow_resource_type::gbr or res_type == qos_flow_resource_type::delay_critical_gbr;
}

scheduler_time_qos::scheduler_time_qos(const scheduler_ue_expert_config& expert_cfg_, du_cell_index_t cell_index_) :
  params(std::get<time_qos_scheduler_config>(expert_cfg_.policy_cfg)), cell_index(cell_index_)
{
}

void scheduler_time_qos::add_ue(du_ue_index_t ue_index)
{
  srsran_assert(not ue_history_db.contains(ue_index), "UE was already added to this slice");
  ue_history_db.emplace(ue_index, ue_ctxt{ue_index, cell_index, this});
}

void scheduler_time_qos::rem_ue(du_ue_index_t ue_index)
{
  ue_history_db.erase(ue_index);
}

void scheduler_time_qos::compute_ue_dl_priorities(slot_point               pdcch_slot,
                                                  slot_point               pdsch_slot,
                                                  span<ue_newtx_candidate> ue_candidates)
{
  unsigned nof_slots_elapsed = std::min(last_pdsch_slot.valid() ? pdsch_slot - last_pdsch_slot : 1U, MAX_SLOT_SKIPPED);
  last_pdsch_slot            = pdsch_slot;

  // Compute UE candidate priorities.
  for (auto& u : ue_candidates) {
    ue_ctxt& uectxt = ue_history_db[u.ue->ue_index()];
    uectxt.compute_dl_prio(*u.ue, pdcch_slot, pdsch_slot, nof_slots_elapsed);
    u.priority = uectxt.dl_prio;
  }
}

void scheduler_time_qos::compute_ue_ul_priorities(slot_point               pdcch_slot,
                                                  slot_point               pusch_slot,
                                                  span<ue_newtx_candidate> ue_candidates)
{
  unsigned nof_slots_elapsed = std::min(last_pusch_slot.valid() ? pusch_slot - last_pusch_slot : 1U, MAX_SLOT_SKIPPED);
  last_pusch_slot            = pusch_slot;

  // Compute UE candidate priorities.
  for (auto& u : ue_candidates) {
    ue_ctxt& uectxt = ue_history_db[u.ue->ue_index()];
    uectxt.compute_ul_prio(*u.ue, pdcch_slot, pusch_slot, nof_slots_elapsed);
    u.priority = uectxt.ul_prio;
  }
}

void scheduler_time_qos::save_dl_newtx_grants(span<const dl_msg_alloc> dl_grants)
{
  // Save result of DL grants in UE history.
  for (const dl_msg_alloc& grant : dl_grants) {
    ue_history_db[grant.context.ue_index].save_dl_alloc(grant.pdsch_cfg.codewords[0].tb_size_bytes, grant.tb_list[0]);
  }
}

void scheduler_time_qos::save_ul_newtx_grants(span<const ul_sched_info> ul_grants)
{
  // Save result of UL grants in UE history.
  for (const ul_sched_info& grant : ul_grants) {
    ue_history_db[grant.context.ue_index].save_ul_alloc(grant.pusch_cfg.tb_size_bytes);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// [Implementation-defined] Helper value to set a maximum metric weight that is low enough to avoid overflows during
// the final QoS weight computation.
static constexpr double max_metric_weight = 1.0e12;

// [Implementation-defined] Averaging window for GBR rate weights (matches dl_logical_channel_manager).
static constexpr unsigned QOS_RATE_AVG_WINDOW_MS = 2000;

// GBR 7M/9M, DC-GBR 4M/6M: MFBR policed at MAC SDU allocation. non-GBR has no rate cap.

static double compute_pf_metric(double estim_rate, double avg_rate, double fairness_coeff)
{
  double pf_weight = 0.0;
  if (estim_rate > 0) {
    if (avg_rate != 0) {
      if (fairness_coeff >= MAX_PF_COEFF) {
        // For very high coefficients, the pow(.) will be very high, leading to pf_weight of 0 due to lack of precision.
        // In such scenarios, we change the way to compute the PF weight. Instead, we completely disregard the estimated
        // rate, as its impact is minimal.
        pf_weight = 1 / avg_rate;
      } else {
        pf_weight = estim_rate / std::pow(avg_rate, fairness_coeff);
      }
    } else {
      // In case the avg rate is zero, the division would be inf. Instead, we give the highest priority to the UE.
      pf_weight = max_metric_weight;
    }
  }
  return pf_weight;
}

static double combine_qos_metrics(double                           pf_weight,
                                  double                           gbr_weight,
                                  double                           prio_weight,
                                  double                           delay_weight,
                                  const time_qos_scheduler_config& policy_params)
{
  if (policy_params.combine_function == time_qos_scheduler_config::combine_function_type::gbr_prioritized and
      gbr_weight > 1.0) {
    // GBR target has not been met and we prioritize GBR over PF.
    pf_weight = std::max(1.0, pf_weight);
  }

  // Log QoS metrics for debugging
  static auto& logger = srslog::fetch_basic_logger("SCHED", false);
  logger.info("QoS Metrics - pf_weight={:.6f}, gbr_weight={:.6f}, prio_weight={:.6f}, delay_weight={:.6f}, "
              "combined={:.6f}",
              pf_weight,
              gbr_weight,
              prio_weight,
              delay_weight,
              gbr_weight * pf_weight * prio_weight * delay_weight);
  // The return is a combination of QoS priority, ARP priority, GBR and PF weight functions.
  return gbr_weight * pf_weight * prio_weight * delay_weight;
}

/// \brief Computes DL rate weight used in computation of DL priority value for a UE in a slot.
static double compute_dl_qos_weights(const slice_ue&                  u,
                                     double                           estim_dl_rate,
                                     double                           avg_dl_rate,
                                     slot_point                       slot_tx,
                                     const time_qos_scheduler_config& policy_params)
{
  static constexpr uint16_t max_combined_prio_level = qos_prio_level_t::max() * arp_prio_level_t::max();
  uint16_t                  min_combined_prio       = max_combined_prio_level;
  double                    gbr_weight              = 0;
  double                    delay_weight            = 0;
  static auto&              logger                  = srslog::fetch_basic_logger("SCHED", false);
  if (policy_params.gbr_enabled or policy_params.priority_enabled or policy_params.pdb_enabled) {
    for (logical_channel_config_ptr lc : *u.logical_channels()) {
      if (not u.contains(lc->lcid) or not lc->qos.has_value() or u.pending_dl_newtx_bytes(lc->lcid) == 0) {
        continue;
      }

      if (policy_params.priority_enabled) {
        min_combined_prio = std::min(static_cast<uint16_t>(lc->qos->runtime_qos.priority.value() *
                                                           lc->qos->runtime_arp_priority.value()),
                                     min_combined_prio);
      }

      slot_point hol_toa = u.dl_hol_toa(lc->lcid);
      if (hol_toa.valid() and slot_tx >= hol_toa) {
        const unsigned diff_slots       = slot_tx - hol_toa;
        const double   slot_duration_ms = 1.0 / slot_tx.nof_slots_per_subframe();
        const double   hol_delay_ms     = static_cast<double>(diff_slots) * slot_duration_ms;
        const unsigned pdb              = lc->qos->runtime_qos.packet_delay_budget_ms;
        double         delay_contrib    = hol_delay_ms / static_cast<double>(pdb);
        
        delay_weight += delay_contrib;

         if ( static_cast<double>(pdb) == 300 ) {

            delay_weight = 1.0;
          
        }

        logger.info("[DELAY-WEIGHT] UE{} LCID{} hol_toa={} slot_tx={} hol_delay_ms={:.3f} PDB={}ms delay_contrib={:.3f} "
                    "delay_weight={:.3f}",
                    u.ue_index(),
                    fmt::underlying(lc->lcid),
                    hol_toa.to_uint(),
                    slot_tx.to_uint(),
                    hol_delay_ms,
                    pdb,
                    delay_contrib,
                    delay_weight);
      }

      if (not qos_res_type_uses_gbr_rate_target(lc->qos->runtime_qos.res_type)) {
        continue;
      }

      const std::optional<dscp_qos_rate_target> rates =
          dscp_qos_mapper::get_instance().get_qos_rates_for_ue(static_cast<uint32_t>(u.ue_index()));
      if (not rates.has_value()) {
        continue;
      }

      const double gbr_bps     = static_cast<double>(rates->gbr_bps);
      const double dl_avg_rate = u.dl_avg_bit_rate(lc->lcid);
      if (dl_avg_rate != 0) {
        gbr_weight += std::min(gbr_bps / dl_avg_rate, max_metric_weight);

        logger.info("GBR weight calculation: LCID={}, GBR_DL={} bps, delivered_avg_rate={} bps, gbr_weight={:.6f}",
                    fmt::underlying(lc->lcid),
                    rates->gbr_bps,
                    dl_avg_rate,
                    gbr_weight);
      } else {
        gbr_weight += max_metric_weight;
      }
    }
  }

  gbr_weight = (policy_params.gbr_enabled and gbr_weight != 0) ? std::max(gbr_weight, 1.0) : 1.0;

  if (avg_dl_rate == 0) {
    return std::numeric_limits<double>::max();
  }

  const double delay_weight_before = delay_weight;
  delay_weight                     = policy_params.pdb_enabled and delay_weight != 0 ? delay_weight : 1.0;
  logger.info("[DELAY-WEIGHT-FINAL] UE{} delay_weight_before={:.3f} pdb_enabled={} delay_weight_after={:.3f} (reason: {})",
              u.ue_index(),
              delay_weight_before,
              policy_params.pdb_enabled,
              delay_weight,
              (policy_params.pdb_enabled and delay_weight_before != 0) ? "calculated"
              : (not policy_params.pdb_enabled)                          ? "pdb_disabled"
                                                                         : "delay_weight_was_zero");

  double pf_weight = compute_pf_metric(estim_dl_rate, avg_dl_rate, policy_params.pf_fairness_coeff);

  double prio_weight = policy_params.priority_enabled ? (max_combined_prio_level + 1 - min_combined_prio) /
                                                            static_cast<double>(max_combined_prio_level + 1)
                                                      : 1.0;

  // If GBR or PDB is unsatisfied (>1), keep both weights at least 1.0 so neither
  // can dilute the combined priority while the other metric is urgent.
//  if (gbr_weight < 1.0 or delay_weight < 1.0) {
//   gbr_weight   = std::max(gbr_weight, 1.0);
  //  delay_weight = std::max(delay_weight, 1.0);
  //}

  double final_priority = combine_qos_metrics(pf_weight, gbr_weight, prio_weight, delay_weight, policy_params);

  logger.info("DL Priority calc: UE{} min_combined_prio={}, prio_weight={:.3f}, pf_weight={:.3f}, gbr_weight={:.3f}, "
              "delay_weight={:.3f}, final_priority={:.3f}",
              u.ue_index(),
              min_combined_prio,
              prio_weight,
              pf_weight,
              gbr_weight,
              delay_weight,
              final_priority);

  for (logical_channel_config_ptr lc : *u.logical_channels()) {
    if (not u.contains(lc->lcid) or not lc->qos.has_value() or u.pending_dl_newtx_bytes(lc->lcid) == 0) {
      continue;
    }
    const auto& runtime_qos  = lc->qos->runtime_qos;
    const char* res_type_str = runtime_qos.res_type == qos_flow_resource_type::gbr              ? "GBR"
                               : runtime_qos.res_type == qos_flow_resource_type::delay_critical_gbr ? "DelayCriticalGBR"
                                                                                                    : "non-GBR";
    if (lc->qos->runtime_gbr_qos_info.has_value()) {
      logger.info("[SCHED-QoS] UE{} LCID{} PDB={}ms GBR_DL={}bps MBR_DL={}bps Type={} (used in scheduling)",
                  u.ue_index(),
                  static_cast<unsigned>(lc->lcid),
                  runtime_qos.packet_delay_budget_ms,
                  lc->qos->runtime_gbr_qos_info->gbr_dl,
                  lc->qos->runtime_gbr_qos_info->max_br_dl,
                  res_type_str);
    } else {
      logger.info("[SCHED-QoS] UE{} LCID{} PDB={}ms GBR=None Type={}",
                  u.ue_index(),
                  static_cast<unsigned>(lc->lcid),
                  runtime_qos.packet_delay_budget_ms,
                  res_type_str);
    }
  }

  return final_priority;
}

/// \brief Computes UL weights used in computation of UL priority value for a UE in a slot.
static double compute_ul_qos_weights(const slice_ue&                  u,
                                     double                           estim_ul_rate,
                                     double                           avg_ul_rate,
                                     const time_qos_scheduler_config& policy_params,
                                     slot_point                       pusch_slot)
{
  if (avg_ul_rate == 0) {
    return max_sched_priority;
  }

  if (u.has_pending_sr()) {
    static constexpr double slot_prio_coeff = max_sched_priority * 1e-6;
    const auto              slot_diff       = pusch_slot - u.pending_sr_slot_rx();
    return max_sched_priority - (pusch_slot.nof_slots_per_hyper_system_frame() - slot_diff) * slot_prio_coeff;
  }

  static constexpr uint16_t max_combined_prio_level = qos_prio_level_t::max() * arp_prio_level_t::max();
  uint16_t                  min_combined_prio       = max_combined_prio_level;
  double                    gbr_weight              = 0;
  if (policy_params.gbr_enabled or policy_params.priority_enabled) {
    for (logical_channel_config_ptr lc : *u.logical_channels()) {
      if (not u.contains(lc->lcid) or not lc->qos.has_value() or u.pending_ul_unacked_bytes(lc->lc_group) == 0) {
        continue;
      }

      if (policy_params.priority_enabled) {
        min_combined_prio = std::min(static_cast<uint16_t>(lc->qos->runtime_qos.priority.value() *
                                                           lc->qos->runtime_arp_priority.value()),
                                     min_combined_prio);
      }

      if (not qos_res_type_uses_gbr_rate_target(lc->qos->runtime_qos.res_type)) {
        continue;
      }

      const std::optional<dscp_qos_rate_target> rates =
          dscp_qos_mapper::get_instance().get_qos_rates_for_ue(static_cast<uint32_t>(u.ue_index()));
      if (not rates.has_value()) {
        continue;
      }

      const double gbr_bps = static_cast<double>(rates->gbr_bps);
      lcg_id_t     lcg_id  = u.get_lcg_id(lc->lcid);
      const double ul_rate = u.ul_avg_bit_rate(lcg_id);
      if (ul_rate != 0) {
        gbr_weight += std::min(gbr_bps / ul_rate, max_metric_weight);
      } else {
        gbr_weight = max_metric_weight;
      }
    }
  }

  gbr_weight = (policy_params.gbr_enabled and gbr_weight != 0) ? gbr_weight : 1.0;
  double prio_weight = policy_params.priority_enabled ? (max_combined_prio_level + 1 - min_combined_prio) /
                                                            static_cast<double>(max_combined_prio_level + 1)
                                                      : 1.0;
  double pf_weight   = compute_pf_metric(estim_ul_rate, avg_ul_rate, policy_params.pf_fairness_coeff);

  static auto& logger = srslog::fetch_basic_logger("SCHED", false);
  logger.info("UL QoS Weights - ue={}, pf_weight={:.6f}, gbr_weight={:.6f}, prio_weight={:.6f}, "
              "delay_weight=1.0, estim_rate={:.2f}, avg_rate={:.2f}",
              u.ue_index(),
              pf_weight,
              gbr_weight,
              prio_weight,
              estim_ul_rate,
              avg_ul_rate);

  return combine_qos_metrics(pf_weight, gbr_weight, prio_weight, 1.0, policy_params);
}

void scheduler_time_qos::ue_ctxt::apply_5qi_based_runtime_overrides(const slice_ue& u)
{
  auto&                       mapper = dscp_qos_mapper::get_instance();
  static srslog::basic_logger& logger = srslog::fetch_basic_logger("SCHED");

  for (logical_channel_config_ptr lc : *u.logical_channels()) {
    if (not u.contains(lc->lcid) || not lc->qos.has_value()) {
      continue;
    }

    if (lc->qos->five_qi == five_qi_t::invalid) {
      continue;
    }

    five_qi_t              effective_5qi = lc->qos->five_qi;
    std::optional<uint8_t> ue_dscp       = mapper.get_dscp_for_ue(static_cast<uint32_t>(ue_index));

    if (ue_dscp.has_value()) {
      std::optional<five_qi_t> dscp_mapped_5qi = mapper.map_dscp_to_5qi(ue_dscp.value());
      if (dscp_mapped_5qi.has_value()) {
        effective_5qi = dscp_mapped_5qi.value();
      } else {
        std::optional<five_qi_t> std_mapped_5qi = mapper.map_dscp_to_5qi_using_standard_mapping(ue_dscp.value());
        if (std_mapped_5qi.has_value()) {
          effective_5qi = std_mapped_5qi.value();
        }
      }
    }

    const standardized_qos_characteristics* qos_chars = get_5qi_to_qos_characteristics_mapping(effective_5qi);
    qos_prio_level_t                        effective_priority;
    unsigned                                effective_pdb;
    if (qos_chars != nullptr) {
      effective_priority = qos_chars->priority;
      effective_pdb      = qos_chars->packet_delay_budget_ms;
    } else {
      effective_priority = lc->qos->qos.priority;
      effective_pdb      = lc->qos->qos.packet_delay_budget_ms;
    }

    auto                   runtime_qos    = lc->qos->runtime_qos;
    qos_prio_level_t       old_priority   = runtime_qos.priority;
    unsigned               old_pdb        = runtime_qos.packet_delay_budget_ms;
    qos_flow_resource_type old_res_type   = runtime_qos.res_type;
    runtime_qos.priority                  = effective_priority;
    runtime_qos.packet_delay_budget_ms    = effective_pdb;
    runtime_qos.average_window_ms         = QOS_RATE_AVG_WINDOW_MS;
    if (qos_chars != nullptr) {
      runtime_qos.res_type = qos_chars->res_type;
    }
    lc->qos->set_runtime_qos(runtime_qos);

    if (ue_dscp.has_value()) {
      if (qos_res_type_uses_gbr_rate_target(runtime_qos.res_type)) {
        if (const std::optional<dscp_qos_rate_target> rates = mapper.map_dscp_to_qos_rates(ue_dscp.value())) {
          gbr_qos_flow_information gbr_info{};
          gbr_info.gbr_dl    = rates->gbr_bps;
          gbr_info.max_br_dl = rates->mbr_bps;
          gbr_info.gbr_ul    = rates->gbr_bps;
          gbr_info.max_br_ul = rates->mbr_bps;
          lc->qos->runtime_gbr_qos_info = gbr_info;
        } else {
          lc->qos->runtime_gbr_qos_info.reset();
        }
      } else {
        // non-GBR: DSCP→5QI/PDB/priority only — no runtime GBR.
        lc->qos->runtime_gbr_qos_info.reset();
      }
      u.apply_dl_lc_rate_avg_window(lc->lcid);
    }
    // No DSCP for this UE yet: keep prior runtime_gbr_qos_info.

    qos_flow_resource_type new_res_type = runtime_qos.res_type;
    const bool             qos_profile_changed = old_priority != effective_priority or old_pdb != effective_pdb or
                                     old_res_type != new_res_type;

    const char* old_res_type_str = old_res_type == qos_flow_resource_type::gbr              ? "GBR"
                                   : old_res_type == qos_flow_resource_type::delay_critical_gbr ? "DelayCriticalGBR"
                                                                                                : "non-GBR";
    const char* new_res_type_str = (qos_chars != nullptr)
                                       ? (qos_chars->res_type == qos_flow_resource_type::gbr ? "GBR"
                                          : qos_chars->res_type == qos_flow_resource_type::delay_critical_gbr
                                                ? "DelayCriticalGBR"
                                                : "non-GBR")
                                       : old_res_type_str;
    if (qos_profile_changed or effective_5qi != lc->qos->five_qi) {
      logger.info("[QOS-RECONFIG] path=dscp UE{} LCID{} five_qi={}->{} priority={}->{} pdb_ms={}->{}",
                  ue_index,
                  static_cast<unsigned>(lc->lcid),
                  five_qi_to_uint(lc->qos->five_qi),
                  five_qi_to_uint(effective_5qi),
                  old_priority.value(),
                  effective_priority.value(),
                  old_pdb,
                  effective_pdb);
      logger.info("[STEP6-SCHED] QoS 업데이트 (DSCP 기반) - UE{} LCID{} 5QI={}->{} Priority={}->{} PDB={}->{}ms "
                  "Type={}->{} (ARP={})",
                  ue_index,
                  static_cast<unsigned>(lc->lcid),
                  lc->qos->five_qi,
                  effective_5qi,
                  old_priority.value(),
                  effective_priority.value(),
                  old_pdb,
                  effective_pdb,
                  old_res_type_str,
                  new_res_type_str,
                  lc->qos->runtime_arp_priority.value());
    } else {
      logger.debug("[STEP6-SCHED] QoS 업데이트 - UE{} LCID{} 5QI={} Priority={}->{} PDB={}->{}ms Type={}->{} (ARP={})",
                   ue_index,
                   static_cast<unsigned>(lc->lcid),
                   effective_5qi,
                   old_priority.value(),
                   effective_priority.value(),
                   old_pdb,
                   effective_pdb,
                   old_res_type_str,
                   new_res_type_str,
                   lc->qos->runtime_arp_priority.value());
    }

    if (qos_profile_changed) {
      const auto& updated_runtime_qos = lc->qos->runtime_qos;
      if (lc->qos->runtime_gbr_qos_info.has_value()) {
        logger.info("[STEP6-SCHED] QoS Info - UE{} LCID{} 5QI={} PDB={}ms GBR_DL={}bps MBR_DL={}bps GBR_UL={}bps "
                    "MBR_UL={}bps Type={} DSCP={}",
                    ue_index,
                    static_cast<unsigned>(lc->lcid),
                    effective_5qi,
                    updated_runtime_qos.packet_delay_budget_ms,
                    lc->qos->runtime_gbr_qos_info->gbr_dl,
                    lc->qos->runtime_gbr_qos_info->max_br_dl,
                    lc->qos->runtime_gbr_qos_info->gbr_ul,
                    lc->qos->runtime_gbr_qos_info->max_br_ul,
                    new_res_type_str,
                    ue_dscp.has_value() ? static_cast<unsigned>(ue_dscp.value()) : 255U);
      } else {
        logger.info("[STEP6-SCHED] QoS Info - UE{} LCID{} 5QI={} PDB={}ms GBR=None Type={}",
                    ue_index,
                    static_cast<unsigned>(lc->lcid),
                    effective_5qi,
                    updated_runtime_qos.packet_delay_budget_ms,
                    new_res_type_str);
      }
    }
  }
}

scheduler_time_qos::ue_ctxt::ue_ctxt(du_ue_index_t             ue_index_,
                                     du_cell_index_t           cell_index_,
                                     const scheduler_time_qos* parent_) :
  ue_index(ue_index_),
  cell_index(cell_index_),
  parent(parent_),
  total_dl_avg_rate_(parent->exp_avg_alpha),
  total_ul_avg_rate_(parent->exp_avg_alpha)
{
}

void scheduler_time_qos::ue_ctxt::compute_dl_prio(const slice_ue& u,
                                                  slot_point      pdcch_slot,
                                                  slot_point      pdsch_slot,
                                                  unsigned        nof_slots_elapsed)
{
  dl_prio = forbid_prio;

  apply_5qi_based_runtime_overrides(u);
  compute_dl_avg_rate(u, nof_slots_elapsed);

  const ue_cell& ue_cc = u.get_cc();

  srsran_sanity_check(ue_cc.is_pdsch_enabled(pdcch_slot, pdsch_slot) and ue_cc.harqs.has_empty_dl_harqs() and
                          u.has_pending_dl_newtx_bytes(),
                      "Invalid DL UE candidate state");

  const search_space_id ue_ded_ss_id = to_search_space_id(2);
  const auto&           ss_info      = ue_cc.cfg().search_space(ue_ded_ss_id);

  uint8_t                    pdsch_time_res_index = 0;
  const pdsch_config_params& pdsch_cfg =
      ss_info.get_pdsch_config(pdsch_time_res_index, ue_cc.channel_state_manager().get_nof_dl_layers());

  auto mcs = ue_cc.link_adaptation_controller().calculate_dl_mcs(pdsch_cfg.mcs_table);
  if (not mcs.has_value()) {
    return;
  }

  const double estimated_rate         = ue_cc.get_estimated_dl_rate(pdsch_cfg, mcs.value(), ss_info.dl_crb_lims.length());
  const double current_total_avg_rate = total_dl_avg_rate();
  dl_prio = compute_dl_qos_weights(u, estimated_rate, current_total_avg_rate, pdcch_slot, parent->params);
}

void scheduler_time_qos::ue_ctxt::compute_ul_prio(const slice_ue& u,
                                                  slot_point      pdcch_slot,
                                                  slot_point      pusch_slot,
                                                  unsigned        nof_slots_elapsed)
{
  ul_prio = forbid_prio;

  apply_5qi_based_runtime_overrides(u);
  compute_ul_avg_rate(u, nof_slots_elapsed);

  const ue_cell& ue_cc = u.get_cc();
  srsran_sanity_check(not ue_cc.is_in_fallback_mode() and ue_cc.is_pusch_enabled(pdcch_slot, pusch_slot) and
                          ue_cc.harqs.has_empty_ul_harqs() and u.pending_ul_newtx_bytes() > 0,
                      "UE UL candidate in invalid state");

  const search_space_id ue_ded_ss_id = to_search_space_id(2);
  const auto&           ss_info      = ue_cc.cfg().search_space(ue_ded_ss_id);

  span<const pusch_time_domain_resource_allocation> pusch_td_res_list = ss_info.pusch_time_domain_list;
  const pusch_time_domain_resource_allocation&      pusch_td_cfg      = pusch_td_res_list.front();
  constexpr unsigned                                nof_harq_ack_bits = 0;
  const bool is_csi_report_slot = ue_cc.cfg().csi_meas_cfg() != nullptr and
                                  csi_helper::is_csi_reporting_slot(*ue_cc.cfg().csi_meas_cfg(), pusch_slot);

  pusch_config_params pusch_cfg;
  switch (ss_info.get_ul_dci_format()) {
    case dci_ul_format::f0_0:
      pusch_cfg = get_pusch_config_f0_0_c_rnti(ue_cc.cfg().cell_cfg_common,
                                               &ue_cc.cfg(),
                                               ue_cc.cfg().cell_cfg_common.ul_cfg_common.init_ul_bwp,
                                               pusch_td_cfg,
                                               nof_harq_ack_bits,
                                               is_csi_report_slot);
      break;
    case dci_ul_format::f0_1:
      pusch_cfg = get_pusch_config_f0_1_c_rnti(ue_cc.cfg(),
                                               pusch_td_cfg,
                                               ue_cc.channel_state_manager().get_nof_ul_layers(),
                                               nof_harq_ack_bits,
                                               is_csi_report_slot);
      break;
    default:
      report_fatal_error("Unsupported PDCCH DCI UL format");
  }

  sch_mcs_index mcs =
      ue_cc.link_adaptation_controller().calculate_ul_mcs(pusch_cfg.mcs_table, pusch_cfg.use_transform_precoder);

  const double estimated_rate   = ue_cc.get_estimated_ul_rate(pusch_cfg, mcs.value(), ss_info.ul_crb_lims.length());
  const double current_avg_rate = total_ul_avg_rate();

  ul_prio = compute_ul_qos_weights(u, estimated_rate, current_avg_rate, parent->params, pusch_slot);
}

void scheduler_time_qos::ue_ctxt::compute_dl_avg_rate(const slice_ue& u, unsigned nof_slots_elapsed)
{
  if (nof_slots_elapsed > 1) {
    total_dl_avg_rate_.push_zeros(nof_slots_elapsed - 1);
  }

  total_dl_avg_rate_.push(dl_sum_alloc_bytes);
  dl_sum_alloc_bytes = 0;
}

void scheduler_time_qos::ue_ctxt::compute_ul_avg_rate(const slice_ue& u, unsigned nof_slots_elapsed)
{
  if (nof_slots_elapsed > 1) {
    total_ul_avg_rate_.push_zeros(nof_slots_elapsed - 1);
  }

  total_ul_avg_rate_.push(ul_sum_alloc_bytes);
  ul_sum_alloc_bytes = 0;
}

void scheduler_time_qos::ue_ctxt::save_dl_alloc(uint32_t total_alloc_bytes, const dl_msg_tb_info& tb_info)
{
  dl_sum_alloc_bytes += total_alloc_bytes;
}

void scheduler_time_qos::ue_ctxt::save_ul_alloc(unsigned alloc_bytes)
{
  if (alloc_bytes == 0) {
    return;
  }
  ul_sum_alloc_bytes += alloc_bytes;
}






