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

#include "srsran/adt/byte_buffer_view.h"
#include <cstdint>
#include <optional>

namespace srsran {
namespace srs_cu_up {

/// Extract DSCP value (6 bits) from IPv4 ToS; empty if not IPv4 or header too short.
inline std::optional<uint8_t> extract_dscp_from_ipv4(byte_buffer_view sdu)
{
  if (sdu.empty()) {
    return {};
  }
  if (sdu.length() < 20) {
    return {};
  }
  const uint8_t version_ihl = sdu[0];
  if ((version_ihl >> 4) != 4) {
    return {};
  }
  const uint8_t tos  = sdu[1];
  const uint8_t dscp = (tos >> 2) & 0x3F;
  return dscp;
}

} // namespace srs_cu_up
} // namespace srsran

