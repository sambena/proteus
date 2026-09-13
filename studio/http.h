// SPDX-License-Identifier: LGPL-2.1-or-later
// Small blocking HTTPS client for Proteus Studio's worker threads (WinINet on Windows).
#pragma once

#include <string>

// GET when `post_data` is empty, otherwise a form POST. True on HTTP 200.
bool http_fetch(const std::string &url, const std::string &post_data, std::string &response, std::string &error);
// Percent-encodes a query parameter value.
std::string url_encode(const std::string &text);
