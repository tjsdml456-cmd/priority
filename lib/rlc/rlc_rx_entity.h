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

#include "rlc_bearer_logger.h"
#include "rlc_bearer_metrics_collector.h"
#include "rlc_rx_metrics_container.h"
#include "srsran/adt/byte_buffer.h"
#include "srsran/adt/byte_buffer_chain.h"
#include "srsran/adt/span.h"
#include "srsran/pcap/rlc_pcap.h"
#include "srsran/pdcp/pdcp_sn_util.h"
#include "srsran/rlc/rlc_rx.h"
#include <cstdint>
#include <optional>

namespace srsran {

/// Base class used for receiving RLC bearers.
/// It provides interfaces for the RLC bearers, for the lower layers
class rlc_rx_entity : public rlc_rx_lower_layer_interface
{
protected:
  rlc_rx_entity(gnb_du_id_t                       gnb_du_id,
                du_ue_index_t                     ue_index,
                rb_id_t                           rb_id,
                rlc_rx_upper_layer_data_notifier& upper_dn_,
                rlc_bearer_metrics_collector&     metrics_coll_,
                rlc_pcap&                         pcap_,
                task_executor&                    ue_executor_,
                timer_manager&                    timers) :
    logger("RLC", {gnb_du_id, ue_index, rb_id, "UL"}),
    upper_dn(upper_dn_),
    metrics(metrics_coll_.get_metrics_period().count()),
    pcap(pcap_),
    ue_timer_factory{timers, ue_executor_},
    high_metrics_timer(ue_timer_factory.create_timer()),
    metrics_coll(metrics_coll_)
  {
    if (metrics_coll.get_metrics_period().count()) {
      high_metrics_timer.set(std::chrono::milliseconds(metrics_coll.get_metrics_period().count()),
                             [this](timer_id_t tid) {
                               metrics_coll.push_rx_high_metrics(metrics.get_and_reset_metrics());
                               high_metrics_timer.run();
                             });
      high_metrics_timer.run();
    }
  }

  rlc_bearer_logger                 logger;
  rlc_rx_upper_layer_data_notifier& upper_dn;
  rlc_rx_metrics_container          metrics;
  rlc_pcap&                         pcap;
  timer_factory                     ue_timer_factory;

  unique_timer high_metrics_timer;

  /// Peek PDCP SN from the start of an RLC SDU / full-or-first segment payload (DRB 18/12-bit).
  std::optional<uint32_t> peek_pdcp_sn(byte_buffer_view payload)
  {
    if (payload.length() < 2) {
      return {};
    }
    uint8_t hdr[3];
    size_t  n = 0;
    for (auto it = payload.begin(); it != payload.end() && n < 3; ++it) {
      hdr[n++] = *it;
    }
    expected<byte_buffer> tmp = byte_buffer::create(span<const uint8_t>{hdr, n});
    if (not tmp.has_value()) {
      return {};
    }
    auto pdcp_sn = get_pdcp_sn(*tmp, pdcp_sn_size::size18bits, /*is_srb=*/false, logger.get_basic_logger());
    if (not pdcp_sn.has_value()) {
      pdcp_sn = get_pdcp_sn(*tmp, pdcp_sn_size::size12bits, /*is_srb=*/false, logger.get_basic_logger());
    }
    return pdcp_sn;
  }

  /// Log when a UL RLC PDU payload arrives (before reassembly / upper delivery).
  void log_qrt_prof_rx_pdu(byte_buffer_view          payload,
                           std::optional<uint32_t>   rlc_sn,
                           const char*               si,
                           bool                      peek_pdcp)
  {
    std::optional<uint32_t> pdcp_sn = peek_pdcp ? peek_pdcp_sn(payload) : std::nullopt;
    if (rlc_sn.has_value()) {
      logger.log_info("QRT-PROF RLC_RX_PDU pdcp_sn={} rlc_sn={} si={} payload_len={}",
                      pdcp_sn,
                      rlc_sn.value(),
                      si,
                      payload.length());
    } else {
      logger.log_info(
          "QRT-PROF RLC_RX_PDU pdcp_sn={} si={} payload_len={}", pdcp_sn, si, payload.length());
    }
  }

  /// Log complete UL RLC SDU with peeked PDCP SN (matches UE PDCP_SN / gNB PDCP count when HFN=0).
  void log_qrt_prof_rx_sdu(const byte_buffer_chain& sdu, std::optional<uint32_t> rlc_sn)
  {
    // First bytes of chain == PDCP header of the RLC SDU.
    uint8_t hdr[3];
    size_t  n = 0;
    for (auto it = sdu.begin(); it != sdu.end() && n < 3; ++it) {
      hdr[n++] = *it;
    }
    std::optional<uint32_t> pdcp_sn;
    if (n >= 2) {
      expected<byte_buffer> tmp = byte_buffer::create(span<const uint8_t>{hdr, n});
      if (tmp.has_value()) {
        pdcp_sn = get_pdcp_sn(*tmp, pdcp_sn_size::size18bits, /*is_srb=*/false, logger.get_basic_logger());
        if (not pdcp_sn.has_value()) {
          pdcp_sn = get_pdcp_sn(*tmp, pdcp_sn_size::size12bits, /*is_srb=*/false, logger.get_basic_logger());
        }
      }
    }
    if (rlc_sn.has_value()) {
      logger.log_info(
          "QRT-PROF RLC_RX_SDU pdcp_sn={} rlc_sn={} sdu_len={}", pdcp_sn, rlc_sn.value(), sdu.length());
    } else {
      logger.log_info("QRT-PROF RLC_RX_SDU pdcp_sn={} sdu_len={}", pdcp_sn, sdu.length());
    }
  }

private:
  rlc_bearer_metrics_collector& metrics_coll;

public:
  /// \brief Stops all internal timers.
  ///
  /// This function is inteded to be called upon removal of the bearer before destroying it.
  /// It stops all timers with handlers that may delegate tasks to another executor that could face a deleted object at
  /// a later execution time.
  /// Before this function is called, the adjacent layers should already be disconnected so that no timer is restarted.
  ///
  /// Note: This function shall only be called from ue_executor.
  virtual void stop() = 0;

  rlc_rx_metrics get_metrics() { return metrics.get_metrics(); }
};

} // namespace srsran

