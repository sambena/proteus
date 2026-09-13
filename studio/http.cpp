// SPDX-License-Identifier: LGPL-2.1-or-later
#include "http.h"

#include <cctype>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#endif

std::string url_encode(const std::string &text)
{
   std::string out;
   for (unsigned char c : text)
   {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
         out += (char)c;
      else
      {
         char buf[4];
         snprintf(buf, sizeof(buf), "%%%02X", c);
         out += buf;
      }
   }
   return out;
}

bool http_fetch(const std::string &url, const std::string &post_data, std::string &response, std::string &error)
{
#ifdef _WIN32
   const std::string scheme = "https://";
   if (url.compare(0, scheme.size(), scheme) != 0)
   {
      error = "only https addresses are supported: " + url;
      return false;
   }
   size_t slash = url.find('/', scheme.size());
   std::string host = url.substr(scheme.size(), slash == std::string::npos ? std::string::npos : slash - scheme.size());
   std::string path = slash == std::string::npos ? "/" : url.substr(slash);

   HINTERNET inet = InternetOpenA("ProteusStudio/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
   if (!inet)
   {
      error = "Failed to initialize WinINet (error " + std::to_string(GetLastError()) + ")";
      return false;
   }
   HINTERNET conn = InternetConnectA(inet, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL,
         INTERNET_SERVICE_HTTP, 0, 0);
   if (!conn)
   {
      error = "Could not connect to " + host + " (error " + std::to_string(GetLastError()) + ")";
      InternetCloseHandle(inet);
      return false;
   }
   DWORD flags = INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE;
   bool post = !post_data.empty();
   HINTERNET req = HttpOpenRequestA(conn, post ? "POST" : "GET", path.c_str(), NULL, NULL, NULL, flags, 0);
   bool ok = false;
   if (!req)
      error = "Failed to create HTTP request (error " + std::to_string(GetLastError()) + ")";
   else
   {
      DWORD timeout = 15000;
      InternetSetOptionA(req, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
      InternetSetOptionA(req, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
      std::string headers = post ? "Content-Type: application/x-www-form-urlencoded\r\n" : "";
      BOOL sent = HttpSendRequestA(req, headers.empty() ? NULL : headers.c_str(), (DWORD)headers.size(),
            post ? (void*)post_data.c_str() : NULL, (DWORD)post_data.size());
      DWORD status = 0, size = sizeof(status);
      if (!sent)
         error = "HTTP request to " + host + " failed (error " + std::to_string(GetLastError()) + ")";
      else if (!HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL) || status != 200)
         error = "Server returned HTTP " + std::to_string(status);
      else
      {
         response.clear();
         char buf[16384];
         DWORD read = 0;
         while (InternetReadFile(req, buf, sizeof(buf), &read) && read > 0)
            response.append(buf, read);
         ok = true;
      }
      InternetCloseHandle(req);
   }
   InternetCloseHandle(conn);
   InternetCloseHandle(inet);
   return ok;
#else
   (void)url; (void)post_data; (void)response;
   error = "Network access is currently supported on Windows";
   return false;
#endif
}
