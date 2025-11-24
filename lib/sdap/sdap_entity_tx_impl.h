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
    // ============================================================
    // [단계 1] SDAP TX: IP 패킷에서 DSCP 추출
    // ============================================================
    // DL 트래픽(네트워크 → UE)이 SDAP를 통과할 때 IPv4 헤더에서 DSCP 값을 추출
    // iperf3로 보낸 트래픽의 DSCP 값(예: 46, 34, 0)을 여기서 추출
    std::optional<uint8_t> dscp = extract_dscp_from_ipv4(byte_buffer_view(sdu));
    if (dscp.has_value()) {
      logger.log_info("[STEP1-SDAP] DSCP 추출 성공 - UE={} PSI={} QFI={} DRB={} DSCP={} (0x{:02x}) pdu_len={}",
                      ue_index, psi, qfi, drb_id, dscp.value(), dscp.value(), sdu.length());
      
      // Store DSCP for potential use in 5QI mapping
      last_dscp = dscp.value();

      // ============================================================
      // [단계 2] DSCP 등록: UE별 DSCP 값 저장
      // ============================================================
      // dscp_qos_mapper 싱글톤에 UE 인덱스와 DSCP 값을 매핑하여 저장
      // 이후 DU나 스케줄러에서 이 값을 조회할 수 있음
      auto& mapper = dscp_qos_mapper::get_instance();
      mapper.register_dscp_for_ue(ue_index, dscp.value());
      logger.log_info("[STEP2-MAPPER] DSCP 등록 완료 - UE={} DSCP={} -> dscp_qos_mapper에 저장됨",
                      ue_index, dscp.value());

      // ============================================================
      // [단계 3] 자동 5QI 매핑: 첫 관찰 시 DSCP → 5QI 자동 매핑
      // ============================================================
      // 이 DSCP 값을 처음 보는 경우, 표준 5QI 목록에서 적절한 5QI를 자동으로 선택
      // DSCP 값이 높을수록(우선순위 높음) 더 높은 우선순위의 5QI를 선택
      bool was_new_dscp = (mapper.map_dscp_to_5qi(dscp.value()).has_value() == false);
      mapper.auto_map_dscp_on_first_observation(dscp.value(), ue_index);
      
      if (was_new_dscp) {
        std::optional<five_qi_t> new_mapped_5qi = mapper.map_dscp_to_5qi(dscp.value());
        if (new_mapped_5qi.has_value()) {
          logger.log_info("[STEP3-AUTO-MAP] 새로운 DSCP 자동 매핑 - UE={} DSCP={} -> 5QI={} (표준 5QI 목록에서 자동 선택)",
                          ue_index, dscp.value(), five_qi_to_uint(new_mapped_5qi.value()));
        }
      }

      // ============================================================
      // [단계 4] 5QI 조회: 매핑된 5QI 확인
      // ============================================================
      // 등록된 DSCP → 5QI 매핑을 조회하여 어떤 5QI가 할당되었는지 확인
      std::optional<five_qi_t> mapped_5qi = mapper.map_dscp_to_5qi(dscp.value());
      
      if (mapped_5qi.has_value()) {
        logger.log_info("[STEP4-MAPPING] DSCP→5QI 매핑 확인 - UE={} PSI={} QFI={} DRB={} DSCP={} -> 5QI={}",
                        ue_index, psi, qfi, drb_id, dscp.value(), five_qi_to_uint(mapped_5qi.value()));
      } else {
        logger.log_warning("[STEP4-MAPPING] DSCP→5QI 매핑 없음 - UE={} DSCP={} (아직 매핑되지 않음, 표준 매핑 시도 예정)",
                          ue_index, dscp.value());
      }
    } else {
      logger.log_debug("[STEP1-SDAP] DSCP 추출 실패 - UE={} QFI={} (IPv4 패킷이 아니거나 헤더 파싱 실패) pdu_len={}",
                      ue_index, qfi, sdu.length());
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

