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

#include "sdap_session_logger.h"
#include "srsran/ran/qos/five_qi.h"
#include "srsran/sdap/dscp_qos_mapper.h"
#include "srsran/sdap/sdap_ipv4_dscp_extract.h"
#include "srsran/sdap/sdap.h"
#include <optional>

namespace srsran {

namespace srs_cu_up {

class sdap_entity_rx_impl : public sdap_rx_pdu_handler
{
public:
  sdap_entity_rx_impl(uint32_t              ue_index_,
                      pdu_session_id_t      psi_,
                      qos_flow_id_t         qfi_,
                      drb_id_t              drb_id_,
                      sdap_rx_sdu_notifier& sdu_notifier_) :
    logger("SDAP", {ue_index_, psi_, qfi_, drb_id_, "UL"}),
    ue_index(ue_index_),
    psi(psi_),
    qfi(qfi_),
    drb_id(drb_id_),
    sdu_notifier(sdu_notifier_)
  {
  }

  void handle_pdu(byte_buffer pdu) final
  {
    // Mirror sdap_entity_tx_impl::handle_sdu (DL): IPv4 DSCP -> dscp_qos_mapper -> DU scheduler runtime_qos.
    std::optional<uint8_t> dscp = extract_dscp_from_ipv4(byte_buffer_view(pdu));
    if (dscp.has_value()) {
      logger.log_info("[STEP1-SDAP] DSCP 추출 성공 - UE={} PSI={} QFI={} DRB={} DSCP={} (0x{:02x}) pdu_len={}",
                      ue_index,
                      psi,
                      qfi,
                      drb_id,
                      dscp.value(),
                      dscp.value(),
                      pdu.length());

      auto& mapper = dscp_qos_mapper::get_instance();
      mapper.register_dscp_for_ue(ue_index, dscp.value(), pdu.length());
      logger.log_info("[STEP2-MAPPER] DSCP 등록 완료 - UE={} DSCP={} -> dscp_qos_mapper에 저장됨", ue_index, dscp.value());

      bool was_new_dscp = (mapper.map_dscp_to_5qi(dscp.value()).has_value() == false);
      mapper.auto_map_dscp_on_first_observation(dscp.value(), ue_index);

      if (was_new_dscp) {
        std::optional<five_qi_t> new_mapped_5qi = mapper.map_dscp_to_5qi(dscp.value());
        if (new_mapped_5qi.has_value()) {
          logger.log_info("[STEP3-AUTO-MAP] 새로운 DSCP 자동 매핑 - UE={} DSCP={} -> 5QI={} (표준 5QI 목록에서 자동 선택)",
                          ue_index,
                          dscp.value(),
                          five_qi_to_uint(new_mapped_5qi.value()));
        }
      }

      std::optional<five_qi_t> mapped_5qi = mapper.map_dscp_to_5qi(dscp.value());

      if (mapped_5qi.has_value()) {
        logger.log_info("[STEP4-MAPPING] DSCP→5QI 매핑 확인 - UE={} PSI={} QFI={} DRB={} DSCP={} -> 5QI={}",
                        ue_index,
                        psi,
                        qfi,
                        drb_id,
                        dscp.value(),
                        five_qi_to_uint(mapped_5qi.value()));
      } else {
        logger.log_warning("[STEP4-MAPPING] DSCP→5QI 매핑 없음 - UE={} DSCP={} (아직 매핑되지 않음, 표준 매핑 시도 예정)",
                           ue_index,
                           dscp.value());
      }

      const bool dscp_changed = (!last_dscp.has_value() || last_dscp.value() != dscp.value());
      if (dscp_changed) {
        const int prev_dscp = last_dscp.has_value() ? static_cast<int>(last_dscp.value()) : -1;
        logger.log_info("ue={}: UL SDU DSCP changed to {} (SDAP-RX) QFI={} sdu_len={} previous_dscp={}",
                        ue_index,
                        dscp.value(),
                        qfi,
                        pdu.length(),
                        prev_dscp);
      }
      last_dscp = dscp.value();
    } else {
      logger.log_debug("[STEP1-SDAP] DSCP 추출 실패 - UE={} QFI={} (IPv4 패킷이 아니거나 헤더 파싱 실패) pdu_len={}",
                       ue_index,
                       qfi,
                       pdu.length());
    }

    logger.log_debug("RX SDU. {} sdu_len={}", qfi, pdu.length());
    sdu_notifier.on_new_sdu(std::move(pdu), qfi);
  }

  drb_id_t get_drb_id() const { return drb_id; }

  std::optional<uint8_t> get_last_dscp() const { return last_dscp; }

private:
  sdap_session_trx_logger  logger;
  uint32_t                 ue_index;
  pdu_session_id_t         psi;
  qos_flow_id_t            qfi;
  drb_id_t                 drb_id;
  sdap_rx_sdu_notifier&    sdu_notifier;
  std::optional<uint8_t>   last_dscp;
};

} // namespace srs_cu_up

} // namespace srsran


