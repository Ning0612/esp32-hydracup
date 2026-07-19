#pragma once

#include <cstdint>
#include <string>

#include "esp_http_server.h"

bool http_read_body(httpd_req_t* request, std::string& body, size_t maxLength = 4096);
std::string http_request_header(httpd_req_t* request, const char* name);
std::string http_client_ip(httpd_req_t* request);
void http_send(httpd_req_t* request, const char* contentType, const std::string& body,
               int status = 200);
bool http_send_file(httpd_req_t* request, const char* path, const char* contentType);
void http_restart_after_response();
