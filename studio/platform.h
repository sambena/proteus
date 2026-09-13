// SPDX-License-Identifier: LGPL-2.1-or-later
// File dialogs, folders and small file helpers for Proteus Studio.
#pragma once

#include <string>
#include <vector>

// Native open dialog; `filter` is pairs of "Label", "*.a;*.b". Empty on cancel.
std::string open_file_dialog(const char *title, const std::vector<std::pair<std::string, std::string>> &filter,
      const std::string &start_dir);
std::string pick_folder_dialog(const char *title);
// %APPDATA%/ProteusStudio (created on demand).
std::string app_data_dir();
bool make_dirs(const std::string &path);
bool file_exists(const std::string &path);
bool dir_exists(const std::string &path);
std::vector<std::string> list_files(const std::string &dir);
std::string read_text(const std::string &path);
bool write_text(const std::string &path, const std::string &text);
std::string dir_of(const std::string &path);
std::string file_name(const std::string &path);
std::string stem_of(const std::string &path);
// Path of `target` relative to `base_dir` when it lies inside it, else `target`.
std::string relative_to(const std::string &base_dir, const std::string &target);
