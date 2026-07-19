#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

std::string http_random_token(size_t byteCount = 16);
bool http_constant_time_equal(const std::string& left, const std::string& right);
std::string http_cookie_value(const std::string& header, const char* name);
std::string http_create_password_hash(const std::string& password);
bool http_verify_password_hash(const std::string& password, const std::string& stored);
