/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
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

#include "gtpu_pdu.h"
#include "gtpu_tunnel_base_tx.h"
#include "srsran/adt/byte_buffer.h"
#include "srsran/adt/byte_buffer_view.h"
#include "srsran/gtpu/gtpu_config.h"
#include "srsran/gtpu/gtpu_tunnel_ngu_tx.h"
#include "srsran/support/bit_encoding.h"
#include <array>
#include <chrono>
#include <optional>

namespace srsran {

/// Extract DSCP (6 bits) from IPv4 ToS in inner SDU; empty if not IPv4 or too short.
static inline std::optional<uint8_t> gtpu_extract_dscp_from_ipv4_ul(byte_buffer_view sdu)
{
  if (sdu.length() < 2) {
    return {};
  }
  uint8_t version_ihl = sdu[0];
  if ((version_ihl >> 4) != 4) {
    return {};
  }
  uint8_t tos  = sdu[1];
  uint8_t dscp = (tos >> 2) & 0x3F;
  return dscp;
}

/// Class used for transmitting GTP-U NGU bearers, e.g. on N3 interface.
class gtpu_tunnel_ngu_tx_impl final : public gtpu_tunnel_base_tx, public gtpu_tunnel_ngu_tx_lower_layer_interface
{
public:
  gtpu_tunnel_ngu_tx_impl(srs_cu_up::ue_index_t                                    ue_index_,
                          const gtpu_tunnel_ngu_config::gtpu_tunnel_ngu_tx_config& cfg_,
                          dlt_pcap&                                                gtpu_pcap_,
                          gtpu_tunnel_common_tx_upper_layer_notifier&              upper_dn_) :
    gtpu_tunnel_base_tx(gtpu_tunnel_log_prefix{ue_index_, cfg_.peer_teid, "UL"}, gtpu_pcap_, upper_dn_),
    ue_index(ue_index_),
    cfg(cfg_)
  {
    to_sockaddr(peer_sockaddr, cfg.peer_addr.c_str(), cfg.peer_port);
    logger.log_info("GTPU NGU Tx configured. {}", cfg);
  }

  void stop() { stopped = true; }

  /*
   * SDU/PDU handlers
   */

  void handle_sdu(byte_buffer buf, qos_flow_id_t qfi) override
  {
    if (stopped) {
      return;
    }

    const auto t_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();

    // Log-only here: inner IPv4 DSCP for grepping / scripts. DU path uses SDAP RX + dscp_qos_mapper (DL uses SDAP TX).
    std::optional<uint8_t> inner_dscp = gtpu_extract_dscp_from_ipv4_ul(byte_buffer_view(buf));
    if (inner_dscp.has_value()) {
      const unsigned qfi_val = static_cast<unsigned>(qfi);
      if (qfi_val < last_dscp_per_qfi.size() &&
          (!last_dscp_per_qfi[qfi_val].has_value() || last_dscp_per_qfi[qfi_val].value() != inner_dscp.value())) {
        const int prev =
            last_dscp_per_qfi[qfi_val].has_value() ? static_cast<int>(last_dscp_per_qfi[qfi_val].value()) : -1;
        logger.log_info("ue={}: [GTPU] UL SDU DSCP changed to {} (IPv4 ToS) QFI={} sdu_len={} previous_dscp={}",
                        static_cast<unsigned>(ue_index),
                        inner_dscp.value(),
                        qfi,
                        buf.length(),
                        prev);
        logger.log_info("[GTPU][UL] t_us={} ue={} DSCP changed to {} (inner IPv4 ToS) qfi={} sdu_len={} prev_dscp={}",
                        t_us,
                        static_cast<unsigned>(ue_index),
                        inner_dscp.value(),
                        qfi,
                        buf.length(),
                        prev);
        last_dscp_per_qfi[qfi_val] = inner_dscp.value();
      }
    }

    gtpu_header hdr         = {};
    hdr.flags.version       = GTPU_FLAGS_VERSION_V1;
    hdr.flags.protocol_type = GTPU_FLAGS_GTP_PROTOCOL;
    hdr.flags.ext_hdr       = true;
    hdr.message_type        = GTPU_MSG_DATA_PDU;
    hdr.length              = buf.length() + 4 + 4;
    hdr.teid                = cfg.peer_teid;
    hdr.next_ext_hdr_type   = gtpu_extension_header_type::pdu_session_container;

    byte_buffer ext_buf;
    bit_encoder encoder{ext_buf};
    bool        pack_ok = true;
    pack_ok &= encoder.pack(1, 4);                        // PDU type
    pack_ok &= encoder.pack(0, 4);                        // unused options
    pack_ok &= encoder.pack(0, 1);                        // spare
    pack_ok &= encoder.pack(qos_flow_id_to_uint(qfi), 7); // QFI

    if (!pack_ok) {
      logger.log_error(
          "Dropped SDU, error writing GTP-U extension header. teid={} ext_len={}", hdr.teid, ext_buf.length());
      return;
    }

    gtpu_extension_header ext;
    ext.extension_header_type = gtpu_extension_header_type::pdu_session_container;
    ext.container             = ext_buf;

    hdr.ext_list.push_back(ext);

    bool write_ok = gtpu_write_header(buf, hdr, logger);

    if (!write_ok) {
      logger.log_error("Dropped SDU, error writing GTP-U header. teid={}", hdr.teid);
      return;
    }
    logger.log_info(buf.begin(),
                    buf.end(),
                    "[GTPU][UL-TX] t_us={} TX PDU. pdu_len={} teid={} qfi={} inner_dscp={}",
                    t_us,
                    buf.length(),
                    hdr.teid,
                    qfi,
                    inner_dscp.has_value() ? static_cast<int>(inner_dscp.value()) : -1);
    send_pdu(std::move(buf), peer_sockaddr);
  }

private:
  const srs_cu_up::ue_index_t ue_index;
  const gtpu_tunnel_ngu_config::gtpu_tunnel_ngu_tx_config cfg;
  sockaddr_storage                                        peer_sockaddr = {};
  bool                                                    stopped       = false;

  static constexpr unsigned                    max_qfi = 64;
  std::array<std::optional<uint8_t>, max_qfi> last_dscp_per_qfi = {};
};
} // namespace srsran

