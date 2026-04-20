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

#include "srsran/ran/du_types.h"
#include "srsran/scheduler/scheduler_metrics.h"
#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace srsran {

/// \brief Controls DSCP values dynamically to achieve target throughput
/// This class monitors current throughput and adjusts DSCP values using a PID controller
/// to maintain the target throughput for each UE
class throughput_controller
{
public:
  /// \brief Configuration for throughput control
  /// Default values are set in the constructor, but can be overridden via set_config()
  struct config {
    /// Target throughput in Mbps (fallback when UE-specific target is not set)
    double target_throughput_mbps;
    
    /// PID controller parameters
    double kp;  ///< Proportional gain
    double ki;  ///< Integral gain
    double kd;  ///< Derivative gain
    
    /// Priority scaling factor: PID output × priority_scale = priority_change
    /// Larger value means more aggressive priority changes
    double priority_scale;
    
    /// Control period in milliseconds (how often to adjust DSCP)
    std::chrono::milliseconds control_period_ms;
    
    /// Minimum DSCP value
    uint8_t min_dscp;
    
    /// Maximum DSCP value
    uint8_t max_dscp;
    
    /// Initial DSCP value
    uint8_t initial_dscp;
    
    /// Enable/disable throughput control
    bool enabled;
  };

  /// \brief Get singleton instance
  static throughput_controller& get_instance()
  {
    static throughput_controller instance;
    return instance;
  }

  /// \brief Set configuration for throughput control
  void set_config(const config& cfg);

  /// \brief Set target throughput for a specific UE
  void set_target_throughput(du_ue_index_t ue_index, double target_mbps);

  /// \brief Set target throughput mapping for multiple UEs at once
  /// \param ue_target_map Map of UE index to target throughput (Mbps)
  /// Example: {{0, 1.5}, {1, 2.0}, {2, 1.0}} for UE0=1.5Mbps, UE1=2.0Mbps, UE2=1.0Mbps
  void set_target_throughput_map(const std::unordered_map<du_ue_index_t, double>& ue_target_map);

  /// \brief Update current throughput from scheduler metrics
  /// This should be called periodically with the latest UE metrics
  void update_throughput(const scheduler_ue_metrics& metrics);

  /// \brief Get current DSCP value for a UE (adjusted by controller)
  std::optional<uint8_t> get_controlled_dscp(du_ue_index_t ue_index) const;

  /// \brief Enable/disable throughput control for a specific UE
  void enable_control(du_ue_index_t ue_index, bool enable);

  /// \brief Remove UE from control (when UE is deleted)
  void remove_ue(du_ue_index_t ue_index);

  /// \brief Check if control is enabled for a UE
  bool is_control_enabled(du_ue_index_t ue_index) const;

  /// \brief Get target priority calculated by throughput controller for a UE
  /// Returns the target priority value that should be used in scheduler
  /// Returns empty if control is not enabled or UE not found
  std::optional<uint8_t> get_target_priority(du_ue_index_t ue_index) const;

private:
  // Default: disabled. Config values are only used when enabled=true (after set_target_throughput_map() or set_config())
  // When disabled, original Priority-based scheduling is used
  throughput_controller() = default;
  ~throughput_controller() = default;
  throughput_controller(const throughput_controller&) = delete;
  throughput_controller& operator=(const throughput_controller&) = delete;

  /// \brief PID controller state for each UE
  /// All members are explicitly initialized in update_throughput(), so no default values needed
  struct pid_state {
    double target_mbps;
    double current_mbps;
    double error;
    double integral;
    double last_error;
    uint8_t current_dscp;
    uint8_t base_priority;     // Target throughput에 따른 base priority (높은 throughput = 낮은 priority)
    uint8_t target_priority;   // 최근 계산된 target priority 값 저장 (base_priority + PID adjustment)
    std::chrono::steady_clock::time_point last_update_time;
    bool enabled;
  };

  /// \brief Compute new DSCP value using PID controller
  uint8_t compute_dscp_adjustment(pid_state& state, double current_mbps, double target_mbps);

  /// \brief Clamp DSCP value to valid range
  uint8_t clamp_dscp(uint8_t dscp) const;

  mutable std::mutex                              mutex;
  config                                          cfg{};  // Zero-initialized (enabled=false, other values unused until set_config() or set_target_throughput_map())
  std::unordered_map<du_ue_index_t, pid_state>    ue_states;
  std::unordered_map<du_ue_index_t, double>      ue_target_throughput_map;  ///< Pre-configured UE target throughput mapping
};

} // namespace srsran


