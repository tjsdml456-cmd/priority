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
  /// This allows tracking which DSCP values are actually used by each UE
  void register_dscp_for_ue(uint32_t ue_index, uint8_t dscp)
  {
    std::lock_guard<std::mutex> lock(mutex);
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

    // 공통 매핑 테이블 사용
    const auto& mapping_table = get_dscp_to_5qi_mapping_table();
    auto mapping_it = mapping_table.find(dscp);
    if (mapping_it != mapping_table.end()) {    
      five_qi_t selected_5qi = mapping_it->second;
      // Verify the selected 5QI exists in standard mapping
      const auto* qos_chars = get_5qi_to_qos_characteristics_mapping(selected_5qi);
      if (qos_chars != nullptr) {
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

    // 공통 매핑 테이블 사용
    const auto& mapping_table = get_dscp_to_5qi_mapping_table();
    auto mapping_it = mapping_table.find(dscp);
    if (mapping_it != mapping_table.end()) {    
      five_qi_t selected_5qi = mapping_it->second;
      // Verify the selected 5QI exists in standard mapping
      const auto* qos_chars = get_5qi_to_qos_characteristics_mapping(selected_5qi);
      if (qos_chars != nullptr) {
        return selected_5qi;
      }
    }

    return {};
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

private:
  dscp_qos_mapper()  = default;
  ~dscp_qos_mapper() = default;
  dscp_qos_mapper(const dscp_qos_mapper&) = delete;
  dscp_qos_mapper& operator=(const dscp_qos_mapper&) = delete;

  /// \brief 공통 DSCP → 5QI 매핑 테이블 (모든 함수에서 공유)
  /// five_qi_qos_mapping.cpp에 정의된 모든 5QI를 DSCP에 매핑
  /// DSCP 값이 높을수록 더 높은 우선순위의 5QI (낮은 priority 값) 할당
  static const std::map<uint8_t, five_qi_t>& get_dscp_to_5qi_mapping_table()
  {
    static const std::map<uint8_t, five_qi_t> mapping_table = {
      // 최고 우선순위 (DSCP 56-63) -> priority 5-7
      {63, uint_to_five_qi(69)},  // priority=5 (Non-GBR)
      {62, uint_to_five_qi(85)},  // priority=21 (Delay Critical GBR)
      {61, uint_to_five_qi(65)},  // priority=7 (GBR)
      {60, uint_to_five_qi(5)},   // priority=10 (Non-GBR)
      {59, uint_to_five_qi(67)},  // priority=15 (GBR)
      {58, uint_to_five_qi(82)},  // priority=19 (Delay Critical GBR)
      {57, uint_to_five_qi(1)},   // priority=20 (GBR)
      {56, uint_to_five_qi(66)},  // priority=20 (GBR)
      
      // 높은 우선순위 (DSCP 48-55) -> priority 22-30
      {55, uint_to_five_qi(83)},  // priority=22 (Delay Critical GBR)
      {54, uint_to_five_qi(84)},  // priority=24 (Delay Critical GBR)
      {53, uint_to_five_qi(3)},   // priority=30 (GBR)
      {52, uint_to_five_qi(2)},   // priority=40 (GBR)
      {51, uint_to_five_qi(4)},   // priority=50 (GBR)
      {50, uint_to_five_qi(70)},  // priority=55 (Non-GBR)
      {49, uint_to_five_qi(6)},   // priority=60 (Non-GBR)
      {48, uint_to_five_qi(79)},  // priority=65 (Non-GBR)
      
      // 중간 우선순위 (DSCP 40-47) -> priority 68-80
      {47, uint_to_five_qi(80)},  // priority=68 (Non-GBR)
      {46, uint_to_five_qi(7)},   // priority=70 (Non-GBR)
      {45, uint_to_five_qi(8)},   // priority=80 (Non-GBR)
      {44, uint_to_five_qi(9)},   // priority=90 (Non-GBR)
      {43, uint_to_five_qi(69)},  // priority=5 (Non-GBR) - 재사용
      {42, uint_to_five_qi(85)},  // priority=21 (Delay Critical GBR용) - 재사
      {41, uint_to_five_qi(65)},  // priority=7 (GBR) - 재사용
      {40, uint_to_five_qi(5)},   // priority=10 (Non-GBR) - 재사용
      
      // 중간-낮은 우선순위 (DSCP 32-39)
      {39, uint_to_five_qi(67)},  // priority=15 (GBR) - 재사용
      {38, uint_to_five_qi(82)},  // priority=19 (Delay Critical GBR) - 재사용
      {37, uint_to_five_qi(1)},   // priority=20 (GBR) - 재사용
      {36, uint_to_five_qi(66)},  // priority=20 (GBR) - 재사용
      {35, uint_to_five_qi(83)},  // priority=22 (Delay Critical GBR) - 재사용
      {34, uint_to_five_qi(84)},  // priority=24 (Delay Critical GBR) - 재사용
      {33, uint_to_five_qi(3)},   // priority=30 (GBR) - 재사용
      {32, uint_to_five_qi(2)},   // priority=40 (GBR) - 재사용
      
      // 낮은 우선순위 (DSCP 24-31)
      {31, uint_to_five_qi(4)},   // priority=50 (GBR) - 재사용
      {30, uint_to_five_qi(70)},  // priority=55 (Non-GBR) - 재사용
      {29, uint_to_five_qi(6)},   // priority=60 (Non-GBR) - 재사용
      {28, uint_to_five_qi(79)},  // priority=65 (Non-GBR) - 재사용
      {27, uint_to_five_qi(80)},  // priority=68 (Non-GBR) - 재사용
      {26, uint_to_five_qi(7)},   // priority=70 (Non-GBR) - 재사용
      {25, uint_to_five_qi(8)},   // priority=80 (Non-GBR) - 재사용
      {24, uint_to_five_qi(9)},   // priority=90 (Non-GBR) - 재사용
      
      // 낮은 우선순위 (DSCP 16-23)
      {23, uint_to_five_qi(69)},  // priority=5 (Non-GBR) - 재사용
      {22, uint_to_five_qi(85)},  // priority=21 (Delay Critical GBR) - 재사용
      {21, uint_to_five_qi(65)},  // priority=7 (GBR) - 재사용
      {20, uint_to_five_qi(5)},   // priority=10 (Non-GBR) - 재사용
      {19, uint_to_five_qi(67)},  // priority=15 (GBR) - 재사용
      {18, uint_to_five_qi(82)},  // priority=19 (Delay Critical GBR) - 재사용
      {17, uint_to_five_qi(1)},   // priority=20 (GBR) - 재사용
      {16, uint_to_five_qi(66)},  // priority=20 (GBR) - 재사용
      
      // 최저 우선순위 (DSCP 8-15)
      {15, uint_to_five_qi(83)},  // priority=22 (Delay Critical GBR) - 재사용
      {14, uint_to_five_qi(84)},  // priority=24 (Delay Critical GBR) - 재사용
      {13, uint_to_five_qi(3)},   // priority=30 (GBR) - 재사용
      {12, uint_to_five_qi(2)},   // priority=40 (GBR) - 재사용
      {11, uint_to_five_qi(4)},   // priority=50 (GBR) - 재사용
      {10, uint_to_five_qi(70)},  // priority=55 (Non-GBR) - 재사용
      {9,  uint_to_five_qi(6)},   // priority=60 (Non-GBR) - 재사용
      {8,  uint_to_five_qi(79)},  // priority=65 (Non-GBR) - 재사용
      
      // 최저 우선순위 (DSCP 0-7)
      {7,  uint_to_five_qi(80)},  // priority=68 (Non-GBR) - 재사용
      {6,  uint_to_five_qi(7)},   // priority=70 (Non-GBR) - 재사용
      {5,  uint_to_five_qi(8)},   // priority=80 (Non-GBR) - 재사용
      {4,  uint_to_five_qi(9)},   // priority=90 (Non-GBR) - 재사용
      {3,  uint_to_five_qi(9)},   // priority=90 (Non-GBR) - 재사용
      {2,  uint_to_five_qi(9)},   // priority=90 (Non-GBR) - 재사용
      {1,  uint_to_five_qi(9)},   // priority=90 (Non-GBR) - 재사용
      {0,  uint_to_five_qi(9)},   // priority=90 (Non-GBR) - 재사용
    };
    return mapping_table;
  }

  mutable std::mutex                              mutex;
  std::unordered_map<uint32_t, uint8_t>          ue_dscp_map;      ///< UE index -> DSCP mapping
  std::map<uint8_t, five_qi_t>                   dscp_to_5qi_map; ///< Explicit DSCP -> 5QI mapping
};

} // namespace srsran


