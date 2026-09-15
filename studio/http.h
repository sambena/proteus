// SPDX-License-Identifier: GPL-3.0-or-later
// Small blocking HTTPS client for Proteus Studio's worker threads (WinINet on Windows).
#pragma once

#include <atomic>
#include <functional>
#include <string>

// GET when `post_data` is empty, otherwise a form POST. True on HTTP 200.
bool http_fetch(const std::string &url, const std::string &post_data, std::string &response, std::string &error);
// A GET that reports progress (bytes so far, total or 0 when unknown) and stops when `cancel` is set.
bool http_download(const std::string &url, std::string &response, std::string &error,
      const std::function<void(size_t done, size_t total)> &progress, const std::atomic<bool> *cancel = nullptr);
// Percent-encodes a query parameter value.
std::string url_encode(const std::string &text);
