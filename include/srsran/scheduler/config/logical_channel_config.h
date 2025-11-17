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

#include "srsran/ran/logical_channel/lcid.h"
#include "srsran/ran/qos/arp_prio_level.h"
#include "srsran/ran/qos/five_qi_qos_mapping.h"
#include "srsran/ran/qos/qos_parameters.h"
#include "srsran/ran/rrm.h"
#include "srsran/ran/sr_configuration.h"
#include "srsran/scheduler/config/logical_channel_group.h"

namespace srsran {

/// \c LogicalChannelConfig, TS 38.331.
/// Information relative to the scheduling of a logical channel in the MAC scheduler.
struct logical_channel_config {
  /// QoS specific features associated with a logical channel (only used by DRBs).
  struct qos_info {
    /// QoS characteristics associated with the logical channel.
    standardized_qos_characteristics qos;
    /// The ARP Priority Level indicates a priority in scheduling resources among QoS Flows. The lowest Priority Level
    /// value corresponds to the highest priority. See TS 23.501, clause 5.7.2.2.
    arp_prio_level_t arp_priority;
    /// QoS information present only for GBR QoS flows.
    std::optional<gbr_qos_flow_information> gbr_qos_info;

    /// Runtime QoS characteristics that can be modified during operation.
    /// Initially set to match the original qos, but can be overridden for runtime adjustments.
    mutable standardized_qos_characteristics runtime_qos;
    /// Runtime ARP Priority Level that can be modified during operation.
    /// Initially set to match the original arp_priority, but can be overridden for runtime adjustments.
    mutable arp_prio_level_t runtime_arp_priority;
    /// Runtime GBR QoS information that can be modified during operation.
    /// Initially set to match the original gbr_qos_info, but can be overridden for runtime adjustments.
    mutable std::optional<gbr_qos_flow_information> runtime_gbr_qos_info;

    /// Synchronizes runtime QoS values with original values.
    void sync_runtime_with_original()
    {
      runtime_qos          = qos;
      runtime_arp_priority = arp_priority;
      runtime_gbr_qos_info = gbr_qos_info;
    }

    /// Sets the runtime QoS characteristics.
    void set_runtime_qos(const standardized_qos_characteristics& value) const { runtime_qos = value; }

    /// Sets the runtime ARP priority.
    void set_runtime_arp_priority(const arp_prio_level_t& value) const { runtime_arp_priority = value; }

    bool operator==(const qos_info& rhs) const
    {
      return qos == rhs.qos && arp_priority == rhs.arp_priority && gbr_qos_info == rhs.gbr_qos_info;
    }
  };

  lcid_t   lcid;
  lcg_id_t lc_group;
  /// Slice associated with this Bearer.
  rrm_policy_member rrm_policy;
  /// QoS information associated with this logical channel.
  std::optional<qos_info> qos;
  // TODO: add remaining fields;
  std::optional<scheduling_request_id> sr_id;
  bool                                 lc_sr_mask;
  bool                                 lc_sr_delay_timer_applied;

  bool operator==(const logical_channel_config& rhs) const
  {
    return lcid == rhs.lcid and lc_group == rhs.lc_group and rrm_policy == rhs.rrm_policy and qos == rhs.qos and
           sr_id == rhs.sr_id and lc_sr_mask == rhs.lc_sr_mask and
           lc_sr_delay_timer_applied == rhs.lc_sr_delay_timer_applied;
  }
};

} // namespace srsran

