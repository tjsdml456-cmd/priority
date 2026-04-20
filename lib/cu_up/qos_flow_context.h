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

#include "adapters/sdap_adapters.h"
#include "srsran/e1ap/common/e1ap_types.h"
#include "srsran/pdcp/pdcp_rx.h"
#include "srsran/ran/cu_types.h"
#include "srsran/support/srsran_assert.h"
#include <optional>

namespace srsran {
namespace srs_cu_up {

struct qos_flow_profile {
  five_qi_t                             five_qi = five_qi_t::invalid;
  std::optional<qos_prio_level_t>       qos_prio_level;
  alloc_and_retention_priority          arp{};
  std::optional<gbr_qos_flow_information> gbr; ///< Optional GBR info associated with this flow.
};

struct qos_flow_context {
  explicit qos_flow_context(const e1ap_qos_flow_qos_param_item& flow);

  /// Returns the original QoS profile as provided by the control plane.
  const qos_flow_profile& get_original_profile() const { return original_profile; }

  /// Returns the mutable runtime QoS profile used by scheduling and mapping decisions.
  qos_flow_profile& access_runtime_profile() { return runtime_profile; }
  const qos_flow_profile& get_runtime_profile() const { return runtime_profile; }
  void set_runtime_five_qi(five_qi_t value) { runtime_profile.five_qi = value; }
  void set_runtime_qos_prio(std::optional<qos_prio_level_t> value) { runtime_profile.qos_prio_level = value; }
  void set_runtime_arp(const alloc_and_retention_priority& value) { runtime_profile.arp = value; }
  void set_runtime_gbr(std::optional<gbr_qos_flow_information> value) { runtime_profile.gbr = value; }

  /// Restores the runtime profile to the original control-plane provided values.
  void reset_runtime_profile() { runtime_profile = original_profile; }

  qos_flow_id_t qos_flow_id = qos_flow_id_t::invalid; // The QoS flow ID.
  qos_flow_profile original_profile;
  qos_flow_profile runtime_profile;

  sdap_pdcp_adapter sdap_to_pdcp_adapter;

  std::unique_ptr<pdcp_rx_upper_data_notifier> sdap_rx_notifier;
};

} // namespace srs_cu_up
} // namespace srsran

inline srsran::srs_cu_up::qos_flow_context::qos_flow_context(const e1ap_qos_flow_qos_param_item& flow) :
  qos_flow_id(flow.qos_flow_id)
{
  const auto& qos_params = flow.qos_flow_level_qos_params;
  const auto& qos_desc   = qos_params.qos_desc;

  const five_qi_t derived_five_qi = qos_desc.get_5qi();
  srsran_assert(derived_five_qi != five_qi_t::invalid, "FiveQI must be set.");

  original_profile.five_qi = derived_five_qi;
  if (qos_desc.is_dyn_5qi()) {
    // Dynamic 5QI descriptors always provide a priority level.
    original_profile.qos_prio_level = qos_desc.get_dyn_5qi().qos_prio_level;
  } else {
    const auto& non_dyn = qos_desc.get_nondyn_5qi();
    if (non_dyn.qos_prio_level.has_value()) {
      original_profile.qos_prio_level = non_dyn.qos_prio_level.value();
    }
  }

  original_profile.arp = qos_params.ng_ran_alloc_retention;
  if (qos_params.gbr_qos_flow_info.has_value()) {
    original_profile.gbr = qos_params.gbr_qos_flow_info;
  }

  runtime_profile = original_profile;
}


