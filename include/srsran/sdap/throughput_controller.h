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
  struct config {
    /// Target throughput in Mbps (e.g., 5.0 for 5Mbps)
    double target_throughput_mbps = 5.0;
    
    /// PID controller parameters
    double kp = 1.0;  ///< Proportional gain
    double ki = 0.1;  ///< Integral gain
    double kd = 0.01; ///< Derivative gain
    
    /// Control period in milliseconds (how often to adjust DSCP)
    std::chrono::milliseconds control_period_ms{1000};
    
    /// Minimum DSCP value (default: 0)
    uint8_t min_dscp = 0;
    
    /// Maximum DSCP value (default: 63)
    uint8_t max_dscp = 63;
    
    /// Initial DSCP value (default: 32)
    uint8_t initial_dscp = 32;
    
    /// Enable/disable throughput control
    bool enabled = true;
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
  /// Example: {{0, 5.0}, {1, 10.0}, {2, 3.0}} for UE0=5Mbps, UE1=10Mbps, UE2=3Mbps
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

private:
  throughput_controller()  = default;
  ~throughput_controller() = default;
  throughput_controller(const throughput_controller&) = delete;
  throughput_controller& operator=(const throughput_controller&) = delete;

  /// \brief PID controller state for each UE
  struct pid_state {
    double target_mbps = 5.0;
    double current_mbps = 0.0;
    double error = 0.0;
    double integral = 0.0;
    double last_error = 0.0;
    uint8_t current_dscp = 32;
    std::chrono::steady_clock::time_point last_update_time;
    bool enabled = true;
  };

  /// \brief Compute new DSCP value using PID controller
  uint8_t compute_dscp_adjustment(pid_state& state, double current_mbps, double target_mbps);

  /// \brief Clamp DSCP value to valid range
  uint8_t clamp_dscp(uint8_t dscp) const;

  mutable std::mutex                              mutex;
  config                                          cfg;
  std::unordered_map<du_ue_index_t, pid_state>    ue_states;
  std::unordered_map<du_ue_index_t, double>      ue_target_throughput_map;  ///< Pre-configured UE target throughput mapping
};

} // namespace srsran


