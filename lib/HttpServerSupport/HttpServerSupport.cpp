#include "http_server_support.h"

#include <cstdio>
#include <cstring>
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool http_read_body(httpd_req_t* request, std::string& body, size_t maxLength) {
    if (!request || request->content_len > maxLength) return false;
    body.assign(request->content_len, '\0');
    size_t received = 0;
    while (received < request->content_len) {
        const int result = httpd_req_recv(request, body.data() + received,
                                          request->content_len - received);
        if (result <= 0) return false;
        received += static_cast<size_t>(result);
    }
    return true;
}

std::string http_request_header(httpd_req_t* request, const char* name) {
    const size_t length = httpd_req_get_hdr_value_len(request, name);
    if (length == 0 || length > 2048) return {};
    std::string value(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(request, name, value.data(), value.size()) != ESP_OK) return {};
    value.resize(std::strlen(value.c_str()));
    return value;
}

std::string http_client_ip(httpd_req_t* request) {
    int socket = httpd_req_to_sockfd(request);
    sockaddr_in address = {};
    socklen_t length = sizeof(address);
    if (getpeername(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) return "unknown";
    char value[64] = {};
    if (address.sin_family == AF_INET) inet_ntoa_r(address.sin_addr, value, sizeof(value));
    else std::strncpy(value, "unknown", sizeof(value) - 1);
    return value;
}

void http_send(httpd_req_t* request, const char* contentType, const std::string& body, int status) {
    httpd_resp_set_type(request, contentType);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (status == 302) httpd_resp_set_status(request, "302 Found");
    else if (status == 301) httpd_resp_set_status(request, "301 Moved Permanently");
    else if (status == 400) httpd_resp_set_status(request, "400 Bad Request");
    else if (status == 401) httpd_resp_set_status(request, "401 Unauthorized");
    else if (status == 403) httpd_resp_set_status(request, "403 Forbidden");
    else if (status == 404) httpd_resp_set_status(request, "404 Not Found");
    else if (status == 409) httpd_resp_set_status(request, "409 Conflict");
    else if (status == 429) httpd_resp_set_status(request, "429 Too Many Requests");
    else if (status == 500) httpd_resp_set_status(request, "500 Internal Server Error");
    else if (status == 503) httpd_resp_set_status(request, "503 Service Unavailable");
    else if (status == 504) httpd_resp_set_status(request, "504 Gateway Timeout");
    httpd_resp_send(request, body.c_str(), body.size());
}

bool http_send_file(httpd_req_t* request, const char* path, const char* contentType) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    httpd_resp_set_type(request, contentType);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char buffer[1024];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(request, buffer, count) != ESP_OK) { std::fclose(file); return false; }
    }
    std::fclose(file);
    return httpd_resp_send_chunk(request, nullptr, 0) == ESP_OK;
}

void http_restart_after_response() {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
