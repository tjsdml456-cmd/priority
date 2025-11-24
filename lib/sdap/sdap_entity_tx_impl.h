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

#include "srsran/sdap/dscp_qos_mapper.h"
#include "sdap_session_logger.h"
#include "srsran/ran/qos/five_qi.h"
#include "srsran/sdap/sdap.h"
#include <optional>
#include <map>

namespace srsran {

namespace srs_cu_up {

/// Extract DSCP value from IPv4 header
/// Returns DSCP value (6 bits) if valid IPv4 packet, otherwise returns empty
static std::optional<uint8_t> extract_dscp_from_ipv4(byte_buffer_view sdu)
{
  if (sdu.empty()) {
    return {};
  }

  // Check minimum IPv4 header length (20 bytes)
  if (sdu.length() < 20) {
    return {};
  }

  // Check IP version (first 4 bits should be 0x4 for IPv4)
  uint8_t version_ihl = sdu[0];
  if ((version_ihl >> 4) != 4) {
    return {};
  }

  // Extract DSCP from ToS field (bits 0-5 of byte 1)
  // IPv4 header: [Version(4) IHL(4)] [ToS(8)] ...
  // ToS: [DSCP(6) ECN(2)]
  uint8_t tos = sdu[1];
  uint8_t dscp = (tos >> 2) & 0x3F; // Extract upper 6 bits

  return dscp;
}

// DSCP to 5QI mapping is now handled by dscp_qos_mapper class
// This allows dynamic mapping based on actual DSCP values extracted from IP packets

class sdap_entity_tx_impl
{
public:
  sdap_entity_tx_impl(uint32_t              ue_index_,
                      pdu_session_id_t      psi_,
                      qos_flow_id_t         qfi_,
                      drb_id_t              drb_id_,
                      sdap_tx_pdu_notifier& pdu_notifier_) :
    logger("SDAP", {ue_index_, psi_, qfi_, drb_id_, "DL"}),
    ue_index(ue_index_),
    psi(psi_),
    qfi(qfi_),
    drb_id(drb_id_),
    pdu_notifier(pdu_notifier_)
  {
  }

  void handle_sdu(byte_buffer sdu)
  {
    // Extract DSCP from IP header for QoS mapping
    std::optional<uint8_t> dscp = extract_dscp_from_ipv4(byte_buffer_view(sdu));
    if (dscp.has_value()) {
      // Store DSCP for potential use in 5QI mapping
      last_dscp = dscp.value();

      // Register this DSCP value for the UE (allows tracking actual DSCP values per UE)
      auto& mapper = dscp_qos_mapper::get_instance();
      mapper.register_dscp_for_ue(ue_index, dscp.value());

      // Auto-map DSCP to 5QI if this is the first time we see this DSCP value
      // This allows iperf3 to send any DSCP value and it will be automatically mapped
      mapper.auto_map_dscp_on_first_observation(dscp.value(), ue_index);

      // Map DSCP directly to 5QI using explicit mapping table
      // This uses the actual DSCP value extracted from iperf3 traffic
      std::optional<five_qi_t> mapped_5qi = mapper.map_dscp_to_5qi(dscp.value());
      
      if (mapped_5qi.has_value()) {
        // Log DSCP extraction with 5QI mapping information
        logger.log_info("SDAP TX: Extracted DSCP from IP packet - UE={} PSI={} QFI={} DRB={} DSCP={} (0x{:02x}) -> Mapped 5QI={} pdu_len={}",
                        ue_index,
                        psi,
                        qfi,
                        drb_id,
                        dscp.value(),
                        dscp.value(),
                        five_qi_to_uint(mapped_5qi.value()),
                        sdu.length());
      } else {
        // No mapping exists for this DSCP value
        logger.log_info("SDAP TX: Extracted DSCP from IP packet - UE={} PSI={} QFI={} DRB={} DSCP={} (0x{:02x}) -> No 5QI mapping found pdu_len={}",
                        ue_index,
                        psi,
                        qfi,
                        drb_id,
                        dscp.value(),
                        dscp.value(),
                        sdu.length());
      }
    } else {
      logger.log_debug("TX PDU. {} pdu_len={} (no DSCP - not IPv4 or invalid packet)", qfi, sdu.length());
    }

    // pass through
    pdu_notifier.on_new_pdu(std::move(sdu));
  }

  drb_id_t get_drb_id() const { return drb_id; }

  /// Get last extracted DSCP value
  std::optional<uint8_t> get_last_dscp() const { return last_dscp; }

private:
  sdap_session_trx_logger logger;
  uint32_t                ue_index;
  pdu_session_id_t        psi;
  qos_flow_id_t           qfi;
  drb_id_t                drb_id;
  sdap_tx_pdu_notifier&   pdu_notifier;
  std::optional<uint8_t>  last_dscp; ///< Last extracted DSCP value
};

} // namespace srs_cu_up

} // namespace srsran

