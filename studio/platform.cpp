// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "util.h"
}

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

static std::wstring widen(const std::string &s)
{
   int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
   std::wstring w(n > 0 ? n - 1 : 0, L'\0');
   if (n > 1)
      MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
   return w;
}

static std::string narrow(const wchar_t *w)
{
   int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
   std::string s(n > 0 ? n - 1 : 0, '\0');
   if (n > 1)
      WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
   return s;
}

std::string open_file_dialog(const char *title, const std::vector<std::pair<std::string, std::string>> &filter,
      const std::string &start_dir)
{
   std::wstring filters;
   for (auto &f : filter)
   {
      filters += widen(f.first);
      filters.push_back(L'\0');
      filters += widen(f.second);
      filters.push_back(L'\0');
   }
   filters.push_back(L'\0');

   wchar_t file[4096] = L"";
   std::wstring wtitle = widen(title), wdir = widen(start_dir);
   OPENFILENAMEW ofn{};
   ofn.lStructSize     = sizeof(ofn);
   ofn.lpstrFilter     = filters.c_str();
   ofn.lpstrFile       = file;
   ofn.nMaxFile        = 4096;
   ofn.lpstrTitle      = wtitle.c_str();
   ofn.lpstrInitialDir = wdir.empty() ? nullptr : wdir.c_str();
   ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
   return GetOpenFileNameW(&ofn) ? narrow(file) : std::string();
}

std::string pick_folder_dialog(const char *title)
{
   std::wstring wtitle = widen(title);
   BROWSEINFOW bi{};
   bi.lpszTitle = wtitle.c_str();
   bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
   PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
   if (!pidl)
      return std::string();
   wchar_t path[MAX_PATH];
   std::string out = SHGetPathFromIDListW(pidl, path) ? narrow(path) : std::string();
   CoTaskMemFree(pidl);
   return out;
}

std::string app_data_dir()
{
   const char *appdata = getenv("APPDATA");
   std::string dir = std::string(appdata ? appdata : ".") + "\\ProteusStudio";
   make_dirs(dir);
   return dir;
}

bool make_dirs(const std::string &path)
{
   std::wstring w = widen(path);
   for (size_t i = 3; i <= w.size(); i++)
   {
      if (i == w.size() || w[i] == L'\\' || w[i] == L'/')
      {
         std::wstring part = w.substr(0, i);
         CreateDirectoryW(part.c_str(), nullptr);
      }
   }
   DWORD attr = GetFileAttributesW(w.c_str());
   return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::string &path)
{
   DWORD attr = GetFileAttributesW(widen(path).c_str());
   return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}
#else
std::string open_file_dialog(const char *, const std::vector<std::pair<std::string, std::string>> &, const std::string &) { return {}; }
std::string pick_folder_dialog(const char *) { return {}; }
std::string app_data_dir() { std::string d = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.proteus-studio"; make_dirs(d); return d; }
bool make_dirs(const std::string &path) { return system(("mkdir -p '" + path + "'").c_str()) == 0; }
bool dir_exists(const std::string &path) { return px_file_exists((path + "/.").c_str()); }
#endif

bool file_exists(const std::string &path)
{
   return px_file_exists(path.c_str());
}

static void collect(const char *name, void *userdata)
{
   ((std::vector<std::string>*)userdata)->push_back(name);
}

std::vector<std::string> list_files(const std::string &dir)
{
   std::vector<std::string> out;
   px_list_files(dir.c_str(), collect, &out);
   return out;
}

std::string read_text(const std::string &path)
{
   std::string out;
   FILE *f = px_fopen(path.c_str(), "rb");
   if (!f)
      return out;
   char buf[8192];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      out.append(buf, n);
   fclose(f);
   return out;
}

bool write_text(const std::string &path, const std::string &text)
{
   FILE *f = px_fopen(path.c_str(), "wb");
   if (!f)
      return false;
   bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
   return fclose(f) == 0 && ok;
}

std::string dir_of(const std::string &path)
{
   size_t sep = path.find_last_of("/\\");
   return sep == std::string::npos ? "." : path.substr(0, sep);
}

std::string file_name(const std::string &path)
{
   size_t sep = path.find_last_of("/\\");
   return sep == std::string::npos ? path : path.substr(sep + 1);
}

std::string stem_of(const std::string &path)
{
   std::string name = file_name(path);
   size_t dot = name.find_last_of('.');
   return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string normalize_path(const std::string &path)
{
   std::string s = path;
   for (char &c : s)
   {
      if (c == '\\')
         c = '/';
#ifdef _WIN32
      c = (char)tolower((unsigned char)c);
#endif
   }
   return s;
}

std::string relative_to(const std::string &base_dir, const std::string &target)
{
   std::string b = normalize_path(base_dir), t = normalize_path(target);
   if (!b.empty() && b.back() != '/')
      b.push_back('/');
   if (t.compare(0, b.size(), b) == 0)
   {
      std::string rel = target.substr(b.size());
      for (char &c : rel)
         if (c == '\\')
            c = '/';
      return rel;
   }
   return target;
}

bool copy_file_data(const std::string &src, const std::string &dst)
{
   FILE *in = px_fopen(src.c_str(), "rb");
   if (!in)
      return false;
   make_dirs(dir_of(dst));
   FILE *out = px_fopen(dst.c_str(), "wb");
   if (!out)
   {
      fclose(in);
      return false;
   }
   char buf[65536];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
   {
      if (fwrite(buf, 1, n, out) != n)
      {
         fclose(in);
         fclose(out);
         return false;
      }
   }
   fclose(in);
   return fclose(out) == 0;
}

std::string sanitize_filename(const std::string &name)
{
   std::string r;
   for (char c : name)
   {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
         r.push_back(c);
      else if (c == ' ' || c == '(' || c == ')' || c == '[' || c == ']' || c == '/')
         r.push_back('_');
   }
   // Windows drops a trailing dot from file and folder names ("Super Mario Bros." from its ROM name).
   while (!r.empty() && (r.back() == '_' || r.back() == '.'))
      r.pop_back();
   return r.empty() ? "track" : r;
}

bool read_file_bytes(const std::string &path, std::vector<uint8_t> &out)
{
   FILE *f = px_fopen(path.c_str(), "rb");
   if (!f)
      return false;
   fseek(f, 0, SEEK_END);
   long size = ftell(f);
   fseek(f, 0, SEEK_SET);
   out.resize(size > 0 ? (size_t)size : 0);
   bool ok = size <= 0 || fread(out.data(), 1, out.size(), f) == out.size();
   fclose(f);
   return ok;
}

std::string lower_ext(const std::string &name)
{
   size_t dot = name.find_last_of('.');
   std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
   for (char &c : ext)
      c = (char)tolower((unsigned char)c);
   return ext;
}

std::string to_lower(const std::string &s)
{
   std::string r = s;
   for (char &c : r)
      c = (char)tolower((unsigned char)c);
   return r;
}

void open_folder(const std::string &path)
{
#ifdef _WIN32
   ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOW);
#endif
}
