/**
 * @file config.cpp
 * @brief INI file read/write helpers using Win32 Private Profile APIs.
 * @date 2026-02-08
 *
 * @copyright Copyright (c) 2026
 */

#include "pch.h"
#include "config.h"

#include <cstdio>
#include <cstdlib>

namespace Config
{

static constexpr int BUFFER_SIZE = 512;

bool GetBool(const char* section, const char* key, bool defaultVal, const char* iniFile)
{
    char buf[BUFFER_SIZE];
    GetPrivateProfileStringA(section, key, defaultVal ? "1" : "0", buf, BUFFER_SIZE, iniFile);
    return (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' ||
            buf[0] == 'y' || buf[0] == 'Y');
}

int GetInt(const char* section, const char* key, int defaultVal, const char* iniFile)
{
    char def[32];
    snprintf(def, sizeof(def), "%d", defaultVal);
    char buf[BUFFER_SIZE];
    GetPrivateProfileStringA(section, key, def, buf, BUFFER_SIZE, iniFile);
    return atoi(buf);
}

float GetFloat(const char* section, const char* key, float defaultVal, const char* iniFile)
{
    char def[32];
    snprintf(def, sizeof(def), "%f", defaultVal);
    char buf[BUFFER_SIZE];
    GetPrivateProfileStringA(section, key, def, buf, BUFFER_SIZE, iniFile);
    return static_cast<float>(atof(buf));
}

std::string GetString(const char* section, const char* key, const char* defaultVal, const char* iniFile)
{
    char buf[BUFFER_SIZE];
    GetPrivateProfileStringA(section, key, defaultVal, buf, BUFFER_SIZE, iniFile);
    return std::string(buf);
}

void WriteBool(const char* section, const char* key, bool value, const char* iniFile)
{
    WritePrivateProfileStringA(section, key, value ? "1" : "0", iniFile);
}

void WriteInt(const char* section, const char* key, int value, const char* iniFile)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(section, key, buf, iniFile);
}

void WriteFloat(const char* section, const char* key, float value, const char* iniFile)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6f", value);
    WritePrivateProfileStringA(section, key, buf, iniFile);
}

void WriteString(const char* section, const char* key, const char* value, const char* iniFile)
{
    WritePrivateProfileStringA(section, key, value, iniFile);
}

} // namespace Config
