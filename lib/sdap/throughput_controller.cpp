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

#include "srsran/sdap/throughput_controller.h"
#include "srsran/sdap/dscp_qos_mapper.h"
#include "srsran/ran/qos/five_qi_qos_mapping.h"
#include "srsran/srslog/srslog.h"
#include <algorithm>
#include <cmath>

using namespace srsran;

// THROUGHPUT_CTRL 로거는 lazy initialization으로 가져옴
// (srslog::init() 후에 로거를 가져와야 올바른 sink를 사용함)
static srslog::basic_logger& get_logger()
{
  static srslog::basic_logger& logger = srslog::fetch_basic_logger("THROUGHPUT_CTRL");
  return logger;
}

void throughput_controller::set_config(const config& cfg_)
{
  std::lock_guard<std::mutex> lock(mutex);
  cfg = cfg_;
  get_logger().info("Throughput controller config updated: target={}Mbps, kp={}, ki={}, kd={}, period={}ms",
              cfg.target_throughput_mbps, cfg.kp, cfg.ki, cfg.kd, cfg.control_period_ms.count());
}

void throughput_controller::set_target_throughput(du_ue_index_t ue_index, double target_mbps)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto& state = ue_states[ue_index];
  state.target_mbps = target_mbps;
  get_logger().info("Target throughput set for UE{}: {}Mbps", static_cast<unsigned>(ue_index), target_mbps);
}

void throughput_controller::set_target_throughput_map(const std::unordered_map<du_ue_index_t, double>& ue_target_map)
{
  std::lock_guard<std::mutex> lock(mutex);
  
  // Enable throughput control and set default config when UE targets are set
  if (not ue_target_map.empty()) {
    cfg.enabled = true;
    // Set default PID parameters if not already configured (only set once to avoid overwriting user config)
    if (cfg.kp == 0.0 and cfg.ki == 0.0 and cfg.kd == 0.0) {
      cfg.kp = 1.0;  // Increased for more aggressive response
      cfg.ki = 0.1;  // Increased for faster integral accumulation
      cfg.kd = 0.01; // Increased for better derivative response
      cfg.priority_scale = 50.0;  // Increased to make priority changes more aggressive (PID output 1.0 = priority 50 change)
      cfg.control_period_ms = std::chrono::milliseconds{1000};
      cfg.min_dscp = 0;
      cfg.max_dscp = 63;
      cfg.initial_dscp = 44;
    }
  }
  
  // Store the mapping for future UE initializations
  this->ue_target_throughput_map = ue_target_map;
  
  // Update existing UE states if they already exist
  for (const auto& [ue_index, target_mbps] : ue_target_map) {
    auto it = ue_states.find(ue_index);
    if (it != ue_states.end()) {
      // UE already exists, update its target
      it->second.target_mbps = target_mbps;
      get_logger().info("Target throughput updated for UE{}: {}Mbps (from map)", static_cast<unsigned>(ue_index), target_mbps);
    } else {
      // UE doesn't exist yet, will be applied when UE is initialized
      get_logger().info("Target throughput pre-configured for UE{}: {}Mbps (will apply on initialization)", 
                  static_cast<unsigned>(ue_index), target_mbps);
    }
  }
  get_logger().info("Target throughput map applied for {} UEs (throughput control {})", 
              ue_target_map.size(), cfg.enabled ? "enabled" : "disabled");
}

void throughput_controller::update_throughput(const scheduler_ue_metrics& metrics)
{
  // Throughput control is disabled by default, enabled when set_target_throughput_map() is called
  if (not cfg.enabled) {
    get_logger().debug("Throughput control disabled, skipping update for UE{} (using original Priority-based scheduling)", 
                static_cast<unsigned>(metrics.ue_index));
    return;
  }

  std::lock_guard<std::mutex> lock(mutex);
  
  auto it = ue_states.find(metrics.ue_index);
  if (it == ue_states.end()) {
    // Only initialize if there's a pre-configured target for this UE
    // If no target is set, throughput control is disabled for this UE (uses original Priority)
    auto preconfigured_it = ue_target_throughput_map.find(metrics.ue_index);
    if (preconfigured_it == ue_target_throughput_map.end()) {
      // No target set for this UE, skip initialization (will use original Priority-based scheduling)
      return;
    }
    
    // Initialize new UE state
    pid_state new_state;
    new_state.target_mbps = preconfigured_it->second;
    get_logger().info("Using pre-configured target throughput for UE{}: {}Mbps", 
                static_cast<unsigned>(metrics.ue_index), preconfigured_it->second);
    new_state.current_dscp = cfg.initial_dscp;
    
    // Calculate base priority based on target throughput difference
    // Higher target throughput = lower priority (higher scheduling priority)
    // Priority range: 40 (highest scheduling priority) to 80 (lowest scheduling priority)
    double max_target = preconfigured_it->second;
    double min_target = preconfigured_it->second;
    for (const auto& [ue_idx, target] : ue_target_throughput_map) {
      max_target = std::max(max_target, target);
      min_target = std::min(min_target, target);
    }
    
    // Calculate base priority: higher target -> lower priority (40) to lower target -> higher priority (80)
    if (std::abs(max_target - min_target) < 0.1) {
      // All targets are similar, use same base priority
      new_state.base_priority = 60;
    } else {
      // Linear interpolation: priority = 40 + (max_target - target) / (max_target - min_target) * 40
      new_state.base_priority = static_cast<uint8_t>(
          40 + (max_target - preconfigured_it->second) / (max_target - min_target) * 40.0);
      new_state.base_priority = std::clamp(new_state.base_priority, uint8_t(40), uint8_t(80));
    }
    
    // Get mapper instance
    auto& mapper = dscp_qos_mapper::get_instance();
    
    // 초기 target_priority는 base_priority로 설정 (PID 조정 전)
    new_state.target_priority = new_state.base_priority;
    new_state.last_update_time = std::chrono::steady_clock::now();
    new_state.enabled = true;
    ue_states[metrics.ue_index] = new_state;
    it = ue_states.find(metrics.ue_index);
    
    // Register initial DSCP with mapper
    mapper.register_dscp_for_ue(static_cast<uint32_t>(metrics.ue_index), cfg.initial_dscp);
    get_logger().info("Initialized throughput control for UE{}: target={}Mbps, initial_dscp={}, base_priority={}",
                static_cast<unsigned>(metrics.ue_index), new_state.target_mbps, cfg.initial_dscp, new_state.base_priority);
  }

  auto& state = it->second;
  if (not state.enabled) {
    return;
  }

  // Convert kbps to Mbps
  double current_mbps = metrics.dl_brate_kbps / 1000.0;
  state.current_mbps = current_mbps;

  // Check if enough time has passed since last update
  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last_update_time);
  
  if (elapsed >= cfg.control_period_ms) {
    // Compute new DSCP value using PID controller
    uint8_t old_dscp = state.current_dscp;  // 변경 전 DSCP 저장
    uint8_t new_dscp = compute_dscp_adjustment(state, current_mbps, state.target_mbps);
    
    if (new_dscp != state.current_dscp) {
      state.last_update_time = now;
      
      // Update DSCP in mapper
      auto& mapper = dscp_qos_mapper::get_instance();
      mapper.register_dscp_for_ue(static_cast<uint32_t>(metrics.ue_index), new_dscp);
      
      // Update state after logging (for correct logging)
      state.current_dscp = new_dscp;
      
      get_logger().info("DSCP adjusted for UE{}: throughput={:.2f}Mbps (target={:.2f}Mbps), DSCP={}->{}",
                  static_cast<unsigned>(metrics.ue_index),
                  current_mbps,
                  state.target_mbps,
                  old_dscp,
                  new_dscp);
    } else {
      state.last_update_time = now;
    }
  }
}

std::optional<uint8_t> throughput_controller::get_controlled_dscp(du_ue_index_t ue_index) const
{
  std::lock_guard<std::mutex> lock(mutex);
  auto it = ue_states.find(ue_index);
  if (it != ue_states.end() && it->second.enabled) {
    return it->second.current_dscp;
  }
  return {};
}

void throughput_controller::enable_control(du_ue_index_t ue_index, bool enable)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto it = ue_states.find(ue_index);
  if (it != ue_states.end()) {
    it->second.enabled = enable;
    get_logger().info("Throughput control {} for UE{}", enable ? "enabled" : "disabled", static_cast<unsigned>(ue_index));
  }
}

void throughput_controller::remove_ue(du_ue_index_t ue_index)
{
  std::lock_guard<std::mutex> lock(mutex);
  ue_states.erase(ue_index);
  get_logger().info("Removed UE{} from throughput control", static_cast<unsigned>(ue_index));
}

bool throughput_controller::is_control_enabled(du_ue_index_t ue_index) const
{
  std::lock_guard<std::mutex> lock(mutex);
  
  // cfg.enabled가 false면 항상 false 반환
  if (not cfg.enabled) {
    return false;
  }
  
  // UE가 아직 초기화되지 않았으면, cfg.enabled를 확인
  // (UE가 pre-configured target을 가지고 있으면 곧 초기화될 예정이므로 true 반환)
  auto it = ue_states.find(ue_index);
  if (it == ue_states.end()) {
    // Pre-configured target이 있으면 곧 활성화될 예정이므로 true
    return ue_target_throughput_map.find(ue_index) != ue_target_throughput_map.end();
  }
  
  // UE가 이미 초기화되었으면 state.enabled 확인
  return it->second.enabled;
}

std::optional<uint8_t> throughput_controller::get_target_priority(du_ue_index_t ue_index) const
{
  std::lock_guard<std::mutex> lock(mutex);
  
  if (not cfg.enabled) {
    return {};
  }
  
  auto it = ue_states.find(ue_index);
  if (it == ue_states.end() or not it->second.enabled) {
    return {};
  }
  
  return it->second.target_priority;
}

uint8_t throughput_controller::compute_dscp_adjustment(pid_state& state, double current_mbps, double target_mbps)
{
  // Calculate error as a **minimum throughput** gap, not a strict target.
  // - If current < target  -> positive error  -> try to increase throughput.
  // - If current >= target -> error = 0      -> do NOT try to reduce throughput.
  //
  // This allows small configured bitrates (e.g. 1 Mbps) to still be reached,
  // while also allowing the traffic to grow above the target without being
  // artificially throttled by lowering the priority.
  state.error = std::max(0.0, target_mbps - current_mbps);
  
  // Proportional term
  double p_term = cfg.kp * state.error;
  
  // Integral term (with anti-windup)
  state.integral += state.error;
  // Limit integral to prevent windup
  const double max_integral = 100.0;
  state.integral = std::clamp(state.integral, -max_integral, max_integral);
  double i_term = cfg.ki * state.integral;
  
  // Derivative term
  double d_term = cfg.kd * (state.error - state.last_error);
  state.last_error = state.error;
  
  // PID output
  double pid_output = p_term + i_term + d_term;
  
  // ============================================================
  // Priority 기반 DSCP 조정
  // ============================================================
  // 중요: DSCP 값 자체가 우선순위를 결정하는 것이 아니라
  // DSCP → 5QI → Priority 매핑을 통해 우선순위가 결정됨
  // Priority 값이 낮을수록 높은 우선순위 (예: priority=5 > priority=90)
  
  auto& mapper = dscp_qos_mapper::get_instance();
  
  // 1. 현재 DSCP의 Priority 확인
  std::optional<five_qi_t> current_5qi = mapper.map_dscp_to_5qi(state.current_dscp);
  if (!current_5qi.has_value()) {
    current_5qi = mapper.map_dscp_to_5qi_using_standard_mapping(state.current_dscp);
  }
  
  uint8_t current_priority = 90; // 기본값 (최저 우선순위)
  if (current_5qi.has_value()) {
    const standardized_qos_characteristics* qos_chars = 
        get_5qi_to_qos_characteristics_mapping(current_5qi.value());
    if (qos_chars != nullptr) {
      current_priority = qos_chars->priority.value();
    }
  }
  
  // 2. PID 출력을 Priority 변화로 변환
  // Positive error (need more throughput) -> priority 감소 (우선순위 증가)
  // Negative error (too much throughput) -> priority 증가 (우선순위 감소)
  // Priority 범위: 1-127 (낮을수록 높은 우선순위)
  // PID 출력을 priority 변화로 스케일링 (cfg.priority_scale 사용)
  int priority_change = static_cast<int>(std::round(pid_output * cfg.priority_scale));
  
  // 3. 목표 Priority 계산
  // base_priority는 target throughput 차이에 따라 설정된 기본값
  // PID output은 base_priority를 중심으로 조정
  // priority가 낮을수록 높은 우선순위이므로, 스루풋 부족 시 priority 감소
  int target_priority = static_cast<int>(state.base_priority) - priority_change;
  target_priority = std::clamp(target_priority, 1, 127); // Priority 범위 제한
  state.target_priority = static_cast<uint8_t>(target_priority);  // 상태에 저장
  
  // 4. 목표 Priority에 가장 가까운 DSCP 찾기
  // DSCP 범위를 탐색하여 목표 priority에 가장 가까운 5QI를 가진 DSCP 찾기
  uint8_t best_dscp = state.current_dscp;
  int min_priority_diff = std::numeric_limits<int>::max();
  
  // 모든 DSCP 값을 탐색하여 목표 priority에 가장 가까운 5QI를 가진 DSCP 찾기
  // PID 출력이 작아도 항상 최적의 DSCP를 찾음 (PID가 작으면 target_priority가 current_priority와 비슷하게 계산됨)
  for (uint8_t test_dscp = cfg.min_dscp; test_dscp <= cfg.max_dscp; ++test_dscp) {
    std::optional<five_qi_t> test_5qi = mapper.map_dscp_to_5qi(test_dscp);
    if (!test_5qi.has_value()) {
      test_5qi = mapper.map_dscp_to_5qi_using_standard_mapping(test_dscp);
    }
    
    if (test_5qi.has_value()) {
      const standardized_qos_characteristics* qos_chars = 
          get_5qi_to_qos_characteristics_mapping(test_5qi.value());
      if (qos_chars != nullptr) {
        uint8_t test_priority = qos_chars->priority.value();
        int priority_diff = std::abs(static_cast<int>(test_priority) - target_priority);
        
        // 목표 priority에 더 가까운 DSCP를 찾으면 선택
        // 같은 차이를 가진 경우 현재 DSCP와 다르면 선택 (진동 방지를 위해 현재와 같은 값은 유지)
        if (priority_diff < min_priority_diff) {
          min_priority_diff = priority_diff;
          best_dscp = test_dscp;
        } else if (priority_diff == min_priority_diff && test_dscp == state.current_dscp) {
          // 현재 DSCP가 이미 최적이면 유지
          best_dscp = test_dscp;
        }
      }
    }
  }
  
  // best_dscp의 실제 Priority 확인
  std::optional<five_qi_t> best_5qi = mapper.map_dscp_to_5qi(best_dscp);
  if (!best_5qi.has_value()) {
    best_5qi = mapper.map_dscp_to_5qi_using_standard_mapping(best_dscp);
  }
  uint8_t best_priority = current_priority; // 기본값
  if (best_5qi.has_value()) {
    const standardized_qos_characteristics* best_qos_chars = 
        get_5qi_to_qos_characteristics_mapping(best_5qi.value());
    if (best_qos_chars != nullptr) {
      best_priority = best_qos_chars->priority.value();
    }
  }
  
  // 로깅: DSCP 변경이 있으면 info, 없으면 debug 레벨로 로깅
  bool dscp_changed = (best_dscp != state.current_dscp);
  // Log dl_brate_kbps for debugging (to verify metric calculation)
  double dl_brate_kbps_for_log = current_mbps * 1000.0;  // Convert back to kbps for logging
  if (dscp_changed) {
    get_logger().info("PID control (Priority-based): current={:.2f}Mbps (dl_brate_kbps={:.2f}), target={:.2f}Mbps, error={:.2f}, "
                "P={:.2f}, I={:.2f}, D={:.2f}, output={:.2f}, "
                "current_dscp={}, base_priority={}, current_priority={}, target_priority={}, priority_change={}, "
                "best_dscp={}, best_priority={}, priority_diff={} [CHANGED]",
                current_mbps,
                dl_brate_kbps_for_log,
                target_mbps,
                state.error,
                p_term,
                i_term,
                d_term,
                pid_output,
                state.current_dscp,
                state.base_priority,
                current_priority,
                target_priority,
                priority_change,
                best_dscp,
                best_priority,
                static_cast<int>(best_priority) - static_cast<int>(current_priority));
  } else {
    get_logger().debug("PID control (Priority-based): current={:.2f}Mbps (dl_brate_kbps={:.2f}), target={:.2f}Mbps, error={:.2f}, "
                 "P={:.2f}, I={:.2f}, D={:.2f}, output={:.2f}, "
                 "current_dscp={}, base_priority={}, current_priority={}, target_priority={}, priority_change={}, "
                 "best_dscp={}, best_priority={}, priority_diff={} [NO CHANGE]",
                 current_mbps,
                 dl_brate_kbps_for_log,
                 target_mbps,
                 state.error,
                 p_term,
                 i_term,
                 d_term,
                 pid_output,
                 state.current_dscp,
                 state.base_priority,
                 current_priority,
                 target_priority,
                 priority_change,
                 best_dscp,
                 best_priority,
                 static_cast<int>(best_priority) - static_cast<int>(current_priority));
  }
  
  return best_dscp;
}

uint8_t throughput_controller::clamp_dscp(uint8_t dscp) const
{
  return std::clamp(dscp, cfg.min_dscp, cfg.max_dscp);
}



