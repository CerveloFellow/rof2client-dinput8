/**
 * @file config.h
 * @brief Thin wrappers around Win32 INI file APIs.
 * @date 2026-02-08
 *
 * @copyright Copyright (c) 2026
 */

#pragma once
#include <string>

namespace Config
{
    bool        GetBool(const char* section, const char* key, bool defaultVal, const char* iniFile);
    int         GetInt(const char* section, const char* key, int defaultVal, const char* iniFile);
    float       GetFloat(const char* section, const char* key, float defaultVal, const char* iniFile);
    std::string GetString(const char* section, const char* key, const char* defaultVal, const char* iniFile);

    void WriteBool(const char* section, const char* key, bool value, const char* iniFile);
    void WriteInt(const char* section, const char* key, int value, const char* iniFile);
    void WriteFloat(const char* section, const char* key, float value, const char* iniFile);
    void WriteString(const char* section, const char* key, const char* value, const char* iniFile);
}
