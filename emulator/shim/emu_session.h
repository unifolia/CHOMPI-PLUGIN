// emu::Session value formats. Fixed: older TAPE projects store them as JUCE
// XML attributes. Bools "1"/"0" (any of 1/t/y reads true), lists
// comma-separated.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "chompi/Machine.h"

namespace emu::session
{
/** A parsed number as T, clamped to T's range, NaN as 0: from a damaged
 *  state, a plain conversion would be undefined. */
template <typename T>
T ToInt(double v)
{
    using Limits = std::numeric_limits<T>;
    if(!(v == v))
        return T(0);
    return v <= double(Limits::lowest()) ? Limits::lowest() : v >= double(Limits::max()) ? Limits::max() : T(v);
}

inline void PutInt(Session& s, const char* name, int v) { s[name] = std::to_string(v); }
inline void PutBool(Session& s, const char* name, bool v) { s[name] = v ? "1" : "0"; }
inline void PutFloat(Session& s, const char* name, float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", double(v));
    s[name] = buf;
}
/** decimals < 0: round-trip precision. */
inline void PutFloats(Session& s, const char* name, const float* v, int n, int decimals = -1)
{
    std::string out;
    char        buf[32];
    for(int i = 0; i < n; i++)
    {
        if(decimals < 0)
            std::snprintf(buf, sizeof(buf), "%.9g", double(v[i]));
        else
            std::snprintf(buf, sizeof(buf), "%.*f", decimals, double(v[i]));
        out += (i ? "," : "") + std::string(buf);
    }
    s[name] = out;
}

inline const std::string* Find(const Session& s, const char* name)
{
    const auto it = s.find(name);
    return it == s.end() ? nullptr : &it->second;
}
inline int GetInt(const Session& s, const char* name, int fallback = 0)
{
    const auto* v = Find(s, name);
    return v ? int(std::strtol(v->c_str(), nullptr, 10)) : fallback;
}
inline bool GetBool(const Session& s, const char* name, bool fallback)
{
    const auto* v = Find(s, name);
    if(v == nullptr || v->empty())
        return fallback;
    const char c = (*v)[0];
    return c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y';
}
inline float GetFloat(const Session& s, const char* name, float fallback = 0.f)
{
    const auto* v = Find(s, name);
    return v ? float(std::strtod(v->c_str(), nullptr)) : fallback;
}
/** The comma-separated values, or empty if missing. */
inline std::vector<float> GetFloats(const Session& s, const char* name)
{
    std::vector<float> out;
    const auto*        v = Find(s, name);
    if(v == nullptr || v->empty())
        return out;
    const char* p = v->c_str();
    while(true)
    {
        char* end;
        out.push_back(float(std::strtod(p, &end)));
        if(*end != ',')
            break;
        p = end + 1;
    }
    return out;
}
} // namespace emu::session
