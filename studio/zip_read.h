// SPDX-License-Identifier: LGPL-2.1-or-later
// Reads files out of zip archives held in memory (stored or deflated entries).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Calls `want` with each file's name; for those it accepts, `take` receives the
// file's contents. Stops early when `take` returns false. False on a broken archive.
bool zip_read(const std::vector<uint8_t> &zip, const std::function<bool(const std::string &name)> &want,
      const std::function<bool(const std::string &name, std::vector<uint8_t> &data)> &take,
      std::string &error);
// True when the data starts like a zip archive.
bool is_zip(const std::vector<uint8_t> &data);
