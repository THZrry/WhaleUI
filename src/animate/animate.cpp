/* Animation engine: CSS `transition` + `@keyframes`/`animation`.
 *
 * See animate.h for the design. Pure logic (no GPU/window), the clock is
 * caller-supplied so tests drive time deterministically. */

#include "animate/animate.h"

#include "render/render.h" /* whaleui_render_parse_color */

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

namespace {

/* --- value helpers --- */

std::string num_str(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

std::string color_str(unsigned int c)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
                  (c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    return buf;
}

unsigned int lerp_ch(unsigned int f, unsigned int g, float t)
{
    return static_cast<unsigned int>(f + (static_cast<float>(g) - f) * t + 0.5f);
}

unsigned int lerp_color(unsigned int from, unsigned int to, float t)
{
    unsigned int fr = (from >> 16) & 0xFF, fg = (from >> 8) & 0xFF,
                 fb = from & 0xFF, fa = (from >> 24) & 0xFF;
    unsigned int tr = (to >> 16) & 0xFF, tg = (to >> 8) & 0xFF,
                 tb = to & 0xFF, ta = (to >> 24) & 0xFF;
    return (lerp_ch(fa, ta, t) << 24) | (lerp_ch(fr, tr, t) << 16) |
           (lerp_ch(fg, tg, t) << 8) | lerp_ch(fb, tb, t);
}

/* Interpolate two property values at t in [0,1] into out. Colors and numeric
 * lengths (same unit) interpolate; anything else (auto/none/keywords) can't
 * and returns false (caller snaps). */
bool value_lerp(const std::string& from, const std::string& to, float t,
                std::string& out)
{
    unsigned int fc = 0, tc = 0;
    if (whaleui_render_parse_color(from.c_str(), &fc) == 0 &&
        whaleui_render_parse_color(to.c_str(), &tc) == 0) {
        out = color_str(lerp_color(fc, tc, t));
        return true;
    }
    float fn = 0, tn = 0;
    int fu = 0, tu = 0;
    if (whaleui_style_parse_len(from.c_str(), &fn, &fu) == 0 &&
        whaleui_style_parse_len(to.c_str(), &tn, &tu) == 0 && fu == tu &&
        fu != 3 /* auto */) {
        const char* suf = fu == 0 ? "px" : (fu == 1 ? "%" : (fu == 2 ? "em" : ""));
        out = num_str(fn + (tn - fn) * t) + suf;
        return true;
    }
    return false;
}

/* --- timing functions -> progress in [0,1] --- */

float bez(float t, float p1, float p2)
{
    float u = 1.0f - t;
    return 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t;
}

/* solve x(t) = p for t (x1/x2 in [0,1] -> monotonic) via binary search */
float bez_solve(float p, float x1, float x2)
{
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 14; ++i) {
        float t = (lo + hi) * 0.5f;
        if (bez(t, x1, x2) < p) {
            lo = t;
        } else {
            hi = t;
        }
    }
    return (lo + hi) * 0.5f;
}

float bez_y(float p, float x1, float y1, float x2, float y2)
{
    return bez(bez_solve(p, x1, x2), y1, y2);
}

/* "steps(n[,start|end])" stepping */
bool parse_steps(const std::string& s, float p, float* out)
{
    if (s.compare(0, 6, "steps(") != 0) {
        return false;
    }
    const char* c = s.c_str() + 6;
    int n = std::atoi(c);
    if (n <= 0) {
        return false;
    }
    bool start = std::strstr(c, "start") != nullptr;
    float k = p * static_cast<float>(n);
    float v = start ? std::ceil(k) : std::floor(k);
    if (v < 0) {
        v = 0;
    }
    if (v > n) {
        v = static_cast<float>(n);
    }
    *out = v / static_cast<float>(n);
    return true;
}

float ease_value(const std::string& name, float t)
{
    if (name.empty() || name == "linear") {
        return t;
    }
    float v = 0;
    if (parse_steps(name, t, &v)) {
        return v;
    }
    if (name.compare(0, 13, "cubic-bezier(") == 0) {
        const char* c = name.c_str() + 13;
        float x1 = static_cast<float>(std::atof(c));
        while (*c && *c != ',') ++c;
        if (*c) ++c;
        float y1 = static_cast<float>(std::atof(c));
        while (*c && *c != ',') ++c;
        if (*c) ++c;
        float x2 = static_cast<float>(std::atof(c));
        while (*c && *c != ',') ++c;
        if (*c) ++c;
        float y2 = static_cast<float>(std::atof(c));
        return bez_y(t, x1, y1, x2, y2);
    }
    /* standard CSS curves */
    if (name == "ease") {
        return bez_y(t, 0.25f, 0.1f, 0.25f, 1.0f);
    }
    if (name == "ease-in") {
        return bez_y(t, 0.42f, 0.0f, 1.0f, 1.0f);
    }
    if (name == "ease-out") {
        return bez_y(t, 0.0f, 0.0f, 0.58f, 1.0f);
    }
    if (name == "ease-in-out") {
        return bez_y(t, 0.42f, 0.0f, 0.58f, 1.0f);
    }
    return t;
}

/* --- token helpers (animation shorthand) --- */

void split_ws(const std::string& s, std::vector<std::string>& out)
{
    const char* p = s.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        const char* q = p;
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
        out.push_back(std::string(q, static_cast<size_t>(p - q)));
    }
}

bool is_num(const char* s)
{
    if (!*s) {
        return false;
    }
    const char* p = s;
    if (*p == '-' || *p == '+') {
        ++p;
    }
    bool digit = false;
    while (*p >= '0' && *p <= '9') {
        digit = true;
        ++p;
    }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            digit = true;
            ++p;
        }
    }
    return digit && *p == '\0';
}

bool is_time(const char* s)
{
    size_t n = std::strlen(s);
    if (n < 2) {
        return false;
    }
    if (s[n - 1] == 's' && s[n - 2] != 'm') {
        return is_num(std::string(s, n - 1).c_str());
    }
    if (s[n - 1] == 's' && s[n - 2] == 'm') {
        return is_num(std::string(s, n - 2).c_str());
    }
    return false;
}

uint32_t time_ms(const std::string& s)
{
    if (s.size() >= 2 && s[s.size() - 2] == 'm' && s[s.size() - 1] == 's') {
        return static_cast<uint32_t>(std::atof(s.substr(0, s.size() - 2).c_str()));
    }
    return static_cast<uint32_t>(std::atof(s.substr(0, s.size() - 1).c_str()) * 1000.0f);
}

/* Parsed `animation` shorthand. iter: -1 = infinite. dir: 0=normal,
 * 1=reverse, 2=alternate, 3=alternate-reverse. */
struct AnimSpec
{
    std::string name;
    uint32_t dur;
    uint32_t delay;
    int iter;
    bool forwards;
    bool backwards;
    std::string timing;
    int dir;
};

bool parse_anim(const std::string& v, AnimSpec& out)
{
    out = AnimSpec();
    out.dur = 0;
    out.iter = 1;
    std::vector<std::string> toks;
    split_ws(v, toks);
    bool have_dur = false;
    for (size_t i = 0; i < toks.size(); ++i) {
        const std::string& tok = toks[i];
        if (tok == "infinite") {
            out.iter = -1;
            continue;
        }
        if (tok == "forwards") {
            out.forwards = true;
            continue;
        }
        if (tok == "backwards" || tok == "both") {
            out.backwards = true;
            if (tok == "both") {
                out.forwards = true;
            }
            continue;
        }
        if (tok == "normal") {
            out.dir = 0;
            continue;
        }
        if (tok == "reverse") {
            out.dir = 1;
            continue;
        }
        if (tok == "alternate") {
            out.dir = 2;
            continue;
        }
        if (tok == "alternate-reverse") {
            out.dir = 3;
            continue;
        }
        if (tok == "linear" || tok == "ease" || tok == "ease-in" ||
            tok == "ease-out" || tok == "ease-in-out" ||
            tok.compare(0, 13, "cubic-bezier(") == 0 ||
            tok.compare(0, 6, "steps(") == 0) {
            out.timing = tok;
            continue;
        }
        if (is_num(tok.c_str())) { /* iteration count */
            out.iter = std::atoi(tok.c_str());
            continue;
        }
        if (is_time(tok.c_str())) {
            uint32_t ms = time_ms(tok);
            if (!have_dur) {
                out.dur = ms;
                have_dur = true;
            } else {
                out.delay = ms;
            }
            continue;
        }
        if (out.name.empty()) {
            out.name = tok;
        }
    }
    return !out.name.empty() && out.dur > 0;
}

/* Merge the animation longhand properties (animation-name/-duration/...)
 * over the shorthand values. */
void merge_anim_longs(const WhaleUIComputedStyle& style, AnimSpec& spec)
{
    auto get = [&style](const char* k) -> std::string {
        WhaleUIComputedStyle::const_iterator it = style.find(k);
        return it == style.end() ? std::string() : it->second;
    };
    std::string v;
    v = get("animation-name");
    if (!v.empty() && v != "none") {
        spec.name = v;
    }
    v = get("animation-duration");
    if (!v.empty()) {
        spec.dur = time_ms(v); /* "0.5s"/"500ms" -> ms */
    }
    v = get("animation-delay");
    if (!v.empty()) {
        spec.delay = time_ms(v);
    }
    v = get("animation-iteration-count");
    if (!v.empty()) {
        spec.iter = v == "infinite" ? -1 : std::atoi(v.c_str());
    }
    v = get("animation-timing-function");
    if (!v.empty()) {
        spec.timing = v;
    }
    v = get("animation-fill-mode");
    if (!v.empty()) {
        spec.forwards = v == "forwards" || v == "both";
        spec.backwards = v == "backwards" || v == "both";
    }
    v = get("animation-direction");
    if (v == "reverse") {
        spec.dir = 1;
    } else if (v == "alternate") {
        spec.dir = 2;
    } else if (v == "alternate-reverse") {
        spec.dir = 3;
    } else if (v == "normal") {
        spec.dir = 0;
    }
}

/* "250ms"/"0.25s" -> ms (single token) */
uint32_t parse_ms(const std::string& s)
{
    if (s.size() >= 2 && s[s.size() - 2] == 'm' && s[s.size() - 1] == 's') {
        return static_cast<uint32_t>(std::atof(s.substr(0, s.size() - 2).c_str()));
    }
    if (!s.empty() && s[s.size() - 1] == 's') {
        return static_cast<uint32_t>(std::atof(s.substr(0, s.size() - 1).c_str()) * 1000.0f);
    }
    return 0;
}

void split_comma(const std::string& s, std::vector<std::string>& out)
{
    const char* p = s.c_str();
    while (*p) {
        const char* q = std::strchr(p, ',');
        std::string item = q ? std::string(p, static_cast<size_t>(q - p)) : std::string(p);
        size_t b = item.find_first_not_of(" \t");
        size_t e = item.find_last_not_of(" \t");
        if (b != std::string::npos) {
            out.push_back(item.substr(b, e - b + 1));
        }
        if (!q) {
            break;
        }
        p = q + 1;
    }
}

/* Parsed `transition` value: property whitelist + first duration/delay/
 * timing (per-property values collapse to the first; the sample pages use
 * one timing for every listed property). */
struct TransSpec
{
    bool all; /* no property list: every interpolable property transitions */
    std::vector<std::string> props;
    uint32_t dur;
    uint32_t delay;
    std::string timing;
};

bool is_timing_token(const std::string& tok)
{
    return tok == "linear" || tok == "ease" || tok == "ease-in" ||
           tok == "ease-out" || tok == "ease-in-out" ||
           tok.compare(0, 13, "cubic-bezier(") == 0 ||
           tok.compare(0, 6, "steps(") == 0;
}

/* parse the `transition` shorthand ("background-color 0.3s ease", "all
 * 0.1s", "opacity .5s,transform .5s"). Empty property token = all. */
void parse_transition_list(const std::string& v, TransSpec& ts)
{
    std::vector<std::string> items;
    split_comma(v, items);
    for (size_t i = 0; i < items.size(); ++i) {
        std::vector<std::string> toks;
        split_ws(items[i], toks);
        std::string prop;
        for (size_t j = 0; j < toks.size(); ++j) {
            const std::string& tok = toks[j];
            if (is_time(tok.c_str())) {
                uint32_t ms = time_ms(tok);
                if (ts.dur == 0) {
                    ts.dur = ms;
                } else if (ts.delay == 0) {
                    ts.delay = ms;
                }
                continue;
            }
            if (is_timing_token(tok)) {
                if (ts.timing.empty()) {
                    ts.timing = tok;
                }
                continue;
            }
            if (prop.empty()) {
                prop = tok;
            }
        }
        if (prop.empty()) {
            ts.all = true;
        } else if (!ts.all) {
            ts.props.push_back(prop);
        }
    }
}

void build_trans_spec(const WhaleUIComputedStyle& style, TransSpec& ts)
{
    ts = TransSpec();
    std::string sh, pv, dv, lv, tv;
    WhaleUIComputedStyle::const_iterator it;
    if ((it = style.find("transition")) != style.end()) {
        sh = it->second;
    }
    if ((it = style.find("transition-property")) != style.end()) {
        pv = it->second;
    }
    if ((it = style.find("transition-duration")) != style.end()) {
        dv = it->second;
    }
    if ((it = style.find("transition-delay")) != style.end()) {
        lv = it->second;
    }
    if ((it = style.find("transition-timing-function")) != style.end()) {
        tv = it->second;
    }
    if (!sh.empty() && sh != "none") {
        parse_transition_list(sh, ts);
    }
    /* longhand properties override the shorthand */
    if (!pv.empty()) {
        ts.props.clear();
        ts.all = false;
        if (pv == "all") {
            ts.all = true;
        } else {
            split_comma(pv, ts.props);
        }
    }
    if (!dv.empty()) {
        std::vector<std::string> items;
        split_comma(dv, items);
        if (!items.empty()) {
            ts.dur = parse_ms(items[0]);
        }
    }
    if (!lv.empty()) {
        std::vector<std::string> items;
        split_comma(lv, items);
        if (!items.empty()) {
            ts.delay = parse_ms(items[0]);
        }
    }
    if (!tv.empty()) {
        std::vector<std::string> items;
        split_comma(tv, items);
        if (!items.empty()) {
            ts.timing = items[0];
        }
    }
}

bool trans_in_props(const TransSpec& ts, const std::string& prop)
{
    if (ts.all) {
        return true;
    }
    for (size_t i = 0; i < ts.props.size(); ++i) {
        if (ts.props[i] == prop) {
            return true;
        }
    }
    return false;
}

/* --- transform: parse / interpolate / evaluate --- */

struct TransformOp
{
    int op;     /* 0=translate, 1=scale, 2=scaleX, 3=scaleY */
    float a, b; /* translate: x,y; scale: sx,sy */
    int au, bu; /* 0=px/number, 1=% (translate only) */
};

/* skip spaces, parse a number (+ optional %), advance p */
bool parse_num(const char*& p, float* v, int* unit)
{
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    char* end = nullptr;
    float f = std::strtof(p, &end);
    if (end == p) {
        return false;
    }
    *v = f;
    *unit = 0;
    p = end;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '%') {
        *unit = 1;
        ++p;
    }
    return true;
}

void skip_to_close(const char*& p)
{
    while (*p && *p != ')') {
        ++p;
    }
    if (*p == ')') {
        ++p;
    }
}

bool transform_parse(const char* v, std::vector<TransformOp>& out)
{
    const char* p = v;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            ++p;
        }
        if (!*p) {
            break;
        }
        if (std::strncmp(p, "none", 4) == 0) {
            p += 4;
            continue;
        }
        if (std::strncmp(p, "translateX(", 11) == 0) {
            p += 11;
            float a = 0;
            int au = 0;
            if (!parse_num(p, &a, &au)) {
                return false;
            }
            skip_to_close(p);
            TransformOp op = {0, a, 0, au, 0};
            out.push_back(op);
        } else if (std::strncmp(p, "translateY(", 11) == 0) {
            p += 11;
            float b = 0;
            int bu = 0;
            if (!parse_num(p, &b, &bu)) {
                return false;
            }
            skip_to_close(p);
            TransformOp op = {0, 0, b, 0, bu};
            out.push_back(op);
        } else if (std::strncmp(p, "translate(", 10) == 0) {
            p += 10;
            float a = 0, b = 0;
            int au = 0, bu = 0;
            if (!parse_num(p, &a, &au)) {
                return false;
            }
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            if (*p == ',') {
                ++p;
                if (!parse_num(p, &b, &bu)) {
                    return false;
                }
            }
            skip_to_close(p);
            TransformOp op = {0, a, b, au, bu};
            out.push_back(op);
        } else if (std::strncmp(p, "scale(", 6) == 0) {
            p += 6;
            float a = 0, b = 0;
            int au = 0, bu = 0;
            if (!parse_num(p, &a, &au)) {
                return false;
            }
            b = a;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            if (*p == ',') {
                ++p;
                if (!parse_num(p, &b, &bu)) {
                    return false;
                }
            }
            skip_to_close(p);
            TransformOp op = {1, a, b, 0, 0};
            out.push_back(op);
        } else if (std::strncmp(p, "scaleX(", 7) == 0) {
            p += 7;
            float a = 0;
            int au = 0;
            if (!parse_num(p, &a, &au)) {
                return false;
            }
            skip_to_close(p);
            TransformOp op = {2, a, 1.0f, 0, 0};
            out.push_back(op);
        } else if (std::strncmp(p, "scaleY(", 7) == 0) {
            p += 7;
            float a = 0;
            int au = 0;
            if (!parse_num(p, &a, &au)) {
                return false;
            }
            skip_to_close(p);
            TransformOp op = {3, a, 1.0f, 0, 0};
            out.push_back(op);
        } else {
            return false; /* unsupported function */
        }
    }
    return true;
}

/* "none" viewed as the reference ops with identity values */
std::vector<TransformOp> initial_ops(const std::vector<TransformOp>& ref)
{
    std::vector<TransformOp> v;
    v.reserve(ref.size());
    for (size_t i = 0; i < ref.size(); ++i) {
        TransformOp z = ref[i];
        z.a = ref[i].op == 0 ? 0.0f : 1.0f;
        z.b = ref[i].op == 0 ? 0.0f : 1.0f;
        v.push_back(z);
    }
    return v;
}

/* Interpolate two transform values (matching function lists). Non-matching
 * lists snap to the target. */
bool transform_lerp(const std::string& from, const std::string& to, float t,
                    std::string& out)
{
    std::vector<TransformOp> a, b;
    if (!transform_parse(from.c_str(), a) || !transform_parse(to.c_str(), b)) {
        return false;
    }
    if (a.empty() && b.empty()) {
        out = "none";
        return true;
    }
    if (a.empty()) {
        a = initial_ops(b);
    } else if (b.empty()) {
        b = initial_ops(a);
    }
    if (a.size() != b.size()) {
        out = to; /* function lists differ: snap */
        return true;
    }
    std::string res;
    char buf[96];
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].op != b[i].op) {
            out = to; /* different function kinds: snap */
            return true;
        }
        float av = a[i].a + (b[i].a - a[i].a) * t;
        float bv = a[i].b + (b[i].b - a[i].b) * t;
        if (a[i].op == 0) {
            std::snprintf(buf, sizeof(buf), "translate(%g%s, %g%s)", av,
                          a[i].au ? "%" : "px", bv, a[i].bu ? "%" : "px");
        } else if (a[i].op == 1) {
            std::snprintf(buf, sizeof(buf), "scale(%g, %g)", av, bv);
        } else if (a[i].op == 2) {
            std::snprintf(buf, sizeof(buf), "scaleX(%g)", av);
        } else {
            std::snprintf(buf, sizeof(buf), "scaleY(%g)", av);
        }
        res += buf;
        if (i + 1 < a.size()) {
            res += " ";
        }
    }
    out = res;
    return true;
}

/* property-aware interpolation: transform has its own rules */
bool any_lerp(const std::string& prop, const std::string& from,
              const std::string& to, float t, std::string& out)
{
    if (prop == "transform") {
        return transform_lerp(from, to, t, out);
    }
    return value_lerp(from, to, t, out);
}

/* --- keyframes frame interpolation --- */

struct KfFrame
{
    float pct;
    std::vector<std::pair<std::string, std::string>> decls;
};

bool parse_frame(const char* enc, KfFrame& f)
{
    /* "0%=opacity=0;background-color=#000;" - first '=' splits the key */
    const char* eq = std::strchr(enc, '=');
    if (!eq) {
        return false;
    }
    std::string key(enc, static_cast<size_t>(eq - enc));
    if (key == "from") {
        f.pct = 0;
    } else if (key == "to") {
        f.pct = 1;
    } else {
        f.pct = static_cast<float>(std::atof(key.c_str())) / 100.0f; /* 0..1 */
    }
    const char* p = eq + 1;
    while (*p) {
        const char* s = p;
        while (*p && *p != ';') {
            ++p;
        }
        std::string decl(s, static_cast<size_t>(p - s));
        if (*p == ';') {
            ++p;
        }
        const char* dq = std::strchr(decl.c_str(), '=');
        if (dq) {
            std::string prop(decl.c_str(), static_cast<size_t>(dq - decl.c_str()));
            /* trim */
            size_t pb = prop.find_first_not_of(" \t");
            size_t pe = prop.find_last_not_of(" \t");
            if (pb != std::string::npos) {
                prop = prop.substr(pb, pe - pb + 1);
            }
            std::string val(dq + 1);
            size_t vb = val.find_first_not_of(" \t");
            size_t ve = val.find_last_not_of(" \t\r\n");
            if (vb != std::string::npos) {
                val = val.substr(vb, ve - vb + 1);
            }
            if (!prop.empty()) {
                f.decls.push_back(std::pair<std::string, std::string>(prop, val));
            }
        }
    }
    return true;
}

const whaleui_keyframes_t* find_keyframes(const whaleui_css_keyframes_t* kfs,
                                          const char* name)
{
    if (!kfs) {
        return nullptr;
    }
    for (size_t i = 0; i < kfs->count; ++i) {
        if (kfs->items[i].name && std::strcmp(kfs->items[i].name, name) == 0) {
            return &kfs->items[i];
        }
    }
    return nullptr;
}

/* Interpolate the keyframe block at progress p in [0,1] into style. */
void interp_frames(const whaleui_keyframes_t* kf, float p, WhaleUIComputedStyle& style)
{
    if (!kf || kf->frame_count == 0) {
        return;
    }
    std::vector<KfFrame> frames;
    frames.reserve(kf->frame_count);
    for (size_t i = 0; i < kf->frame_count; ++i) {
        KfFrame f;
        if (parse_frame(kf->frames[i], f)) {
            frames.push_back(f);
        }
    }
    if (frames.empty()) {
        return;
    }
    /* sort by percentage (stable; keeps source order for ties) */
    for (size_t i = 1; i < frames.size(); ++i) {
        for (size_t j = i; j > 0 && frames[j].pct < frames[j - 1].pct; --j) {
            std::swap(frames[j], frames[j - 1]);
        }
    }
    /* surrounding frames: lo = last frame <= p, hi = first frame >= p */
    const KfFrame* lo = nullptr;
    const KfFrame* hi = nullptr;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].pct <= p) {
            lo = &frames[i];
        }
        if (frames[i].pct >= p) {
            hi = &frames[i];
            break;
        }
    }
    if (!lo && !hi) {
        return;
    }
    /* a keyframes block may omit 0% and/or 100%: the spec implies those
     * endpoints are the element's base style. "rise{from{opacity:0}}" thus
     * animates opacity 0 -> base (1), not 0 forever. The `style` argument
     * carries the pre-animation values, so lerp toward/away from them. */
    if (!hi) {
        float t = 1.0f - lo->pct > 0.0f
                      ? (p - lo->pct) / (1.0f - lo->pct)
                      : 0.0f;
        for (size_t i = 0; i < lo->decls.size(); ++i) {
            const std::string& prop = lo->decls[i].first;
            WhaleUIComputedStyle::iterator bi = style.find(prop);
            if (bi == style.end()) {
                continue; /* no base value: leave the default */
            }
            std::string v;
            if (any_lerp(prop, lo->decls[i].second, bi->second, t, v)) {
                style[prop] = v;
            }
        }
        return;
    }
    if (!lo) {
        float t = hi->pct > 0.0f ? p / hi->pct : 0.0f;
        for (size_t i = 0; i < hi->decls.size(); ++i) {
            const std::string& prop = hi->decls[i].first;
            WhaleUIComputedStyle::iterator bi = style.find(prop);
            if (bi == style.end()) {
                continue;
            }
            std::string v;
            if (any_lerp(prop, bi->second, hi->decls[i].second, t, v)) {
                style[prop] = v;
            }
        }
        return;
    }
    float t = hi->pct == lo->pct ? 0.0f : (p - lo->pct) / (hi->pct - lo->pct);
    for (size_t i = 0; i < lo->decls.size(); ++i) {
        const std::string& prop = lo->decls[i].first;
        const std::string* hv = nullptr;
        for (size_t j = 0; j < hi->decls.size(); ++j) {
            if (hi->decls[j].first == prop) {
                hv = &hi->decls[j].second;
                break;
            }
        }
        if (hv) {
            std::string v;
            if (any_lerp(prop, lo->decls[i].second, *hv, t, v)) {
                style[prop] = v;
            } else {
                style[prop] = *hv; /* non-interpolable: take the later frame */
            }
        } else {
            style[prop] = lo->decls[i].second;
        }
    }
    for (size_t j = 0; j < hi->decls.size(); ++j) {
        const std::string& prop = hi->decls[j].first;
        if (style.find(prop) == style.end()) {
            style[prop] = hi->decls[j].second;
        }
    }
}

} // namespace

/* --- animation state --- */

struct whaleui_anim
{
    const whaleui_css_keyframes_t* kfs; /* borrowed */
    int active;
    int needs_layout; /* active animations touch a layout property */

    /* running keyframe animation: "name@el" */
    struct KfState
    {
        std::string name;
        uint64_t start;
        uint32_t dur;
        uint32_t delay;
        int iter;
        int dir;
        bool forwards;
        bool backwards;
        std::string timing;
        std::vector<std::string> props; /* animated properties */
    };
    std::map<std::string, KfState> kf;

    /* running transition: "prop@el" */
    struct Trans
    {
        std::string from, to;
        uint64_t start; /* absolute ms when interpolation begins (= now+delay) */
        uint32_t dur;
        uint32_t delay;
        std::string timing;
    };
    std::map<std::string, Trans> trans;

    /* last settled computed value per "prop@el" (transition baseline) */
    std::map<std::string, std::string> prev;

    /* current animated values per element (paint-only fast path) */
    std::map<lxb_dom_element*, std::map<std::string, std::string> > ov;

    /* base (pre-animation) values of the animated properties, snapped at
     * registration: a keyframes block that omits 0%/100% interpolates
     * toward/from these (spec: implied endpoints = the element's style) */
    std::map<lxb_dom_element*, WhaleUIComputedStyle> base;

    /* elements with running animations -> animated properties */
    std::map<lxb_dom_element*, std::set<std::string> > act;
};

extern "C" whaleui_anim_t* whaleui_anim_create(void)
{
    whaleui_anim_t* a = new whaleui_anim_t;
    a->kfs = nullptr;
    a->active = 0;
    a->needs_layout = 0;
    return a;
}

extern "C" void whaleui_anim_destroy(whaleui_anim_t* a)
{
    delete a;
}

extern "C" void whaleui_anim_set_keyframes(whaleui_anim_t* a,
                                           const whaleui_css_keyframes_t* keyframes)
{
    if (a) {
        a->kfs = keyframes;
    }
}

extern "C" void whaleui_anim_reset(whaleui_anim_t* a)
{
    if (a) {
        a->kf.clear();
        a->trans.clear();
        a->prev.clear();
        a->base.clear();
        a->active = 0;
    }
}

extern "C" uint64_t whaleui_anim_now(void)
{
    return SDL_GetTicks();
}

extern "C" int whaleui_transform_eval(const char* value, float self_w,
                                      float self_h, whaleui_transform_t* out)
{
    if (!out) {
        return -1;
    }
    out->tx = 0.0f;
    out->ty = 0.0f;
    out->sx = 1.0f;
    out->sy = 1.0f;
    if (!value || !*value || std::strcmp(value, "none") == 0) {
        return 0;
    }
    std::vector<TransformOp> ops;
    if (!transform_parse(value, ops)) {
        return -1;
    }
    for (size_t i = 0; i < ops.size(); ++i) {
        const TransformOp& op = ops[i];
        if (op.op == 0) {
            out->tx += op.au ? op.a * self_w / 100.0f : op.a;
            out->ty += op.bu ? op.b * self_h / 100.0f : op.b;
        } else if (op.op == 1) {
            out->sx *= op.a;
            out->sy *= op.b;
        } else if (op.op == 2) {
            out->sx *= op.a;
        } else {
            out->sy *= op.a;
        }
    }
    return 0;
}

namespace {

/* direction handling for keyframe progress. dir: 0=normal, 1=reverse,
 * 2=alternate, 3=alternate-reverse. cycle = zero-based iteration index. */
lxb_dom_element* el_from_key(const std::string& key); /* defined below */
float dir_progress(int dir, uint64_t cycle, float p)
{
    switch (dir) {
    case 1:
        return 1.0f - p;
    case 2:
        return (cycle & 1) ? 1.0f - p : p;
    case 3:
        return (cycle & 1) ? p : 1.0f - p;
    default:
        return p;
    }
}

/* first-frame progress shown during the delay (fill-mode backwards) */
float start_progress(int dir)
{
    return (dir == 1 || dir == 3) ? 1.0f : 0.0f;
}

/* last-frame progress held after the run (fill-mode forwards) */
float end_progress(int dir, int iter)
{
    uint64_t last = iter > 0 ? static_cast<uint64_t>(iter) - 1 : 0;
    switch (dir) {
    case 1:
        return 0.0f;
    case 2:
        return (last & 1) ? 0.0f : 1.0f;
    case 3:
        return (last & 1) ? 1.0f : 0.0f;
    default:
        return 1.0f;
    }
}

/* Play the `animation` shorthand (+ longhand properties): inject the current
 * keyframe values. */
void apply_keyframes(whaleui_anim_t* a, const std::string& elkey,
                     WhaleUIComputedStyle& style, uint64_t now)
{
    std::string av;
    WhaleUIComputedStyle::const_iterator ait = style.find("animation");
    if (ait != style.end()) {
        av = ait->second;
    }
    AnimSpec spec;
    if (av.empty() || av == "none") {
        /* longhand-only (e.g. animation-name + animation-delay rules) */
        spec = AnimSpec();
        spec.dur = 0;
        spec.iter = 1;
        merge_anim_longs(style, spec);
    } else {
        if (!parse_anim(av, spec)) {
            return;
        }
        merge_anim_longs(style, spec);
    }
    if (spec.name.empty() || spec.dur == 0) {
        return;
    }
    const whaleui_keyframes_t* kf = find_keyframes(a->kfs, spec.name.c_str());
    if (!kf) {
        return; /* unknown animation name: no effect */
    }
    std::string kkey = spec.name + "@" + elkey;
    std::map<std::string, whaleui_anim::KfState>::iterator it = a->kf.find(kkey);
    if (it == a->kf.end() || it->second.name != spec.name ||
        it->second.dur != spec.dur || it->second.iter != spec.iter ||
        it->second.dir != spec.dir) {
        whaleui_anim::KfState st;
        st.name = spec.name;
        st.start = now;
        st.dur = spec.dur;
        st.delay = spec.delay;
        st.iter = spec.iter;
        st.dir = spec.dir;
        st.forwards = spec.forwards;
        st.backwards = spec.backwards;
        st.timing = spec.timing;
        /* remember the animated properties (from the frames) so the tick
         * can decide paint-only vs layout */
        std::set<std::string> props;
        if (kf->frame_count) {
            for (size_t i = 0; i < kf->frame_count; ++i) {
                const char* enc = kf->frames[i];
                const char* eq = std::strchr(enc, '=');
                if (!eq) {
                    continue;
                }
                const char* p = eq + 1;
                while (*p) {
                    const char* s = p;
                    while (*p && *p != ';') {
                        ++p;
                    }
                    std::string decl(s, static_cast<size_t>(p - s));
                    if (*p == ';') {
                        ++p;
                    }
                    const char* dq = std::strchr(decl.c_str(), '=');
                    if (dq) {
                        props.insert(std::string(decl.c_str(),
                                                 static_cast<size_t>(dq - decl.c_str())));
                    }
                }
            }
        }
        st.props.assign(props.begin(), props.end());
        a->kf[kkey] = st;
        it = a->kf.find(kkey);
        /* snapshot the element's base values of the animated properties:
         * an implied 0%/100% frame (single-`from`/`to` blocks) lerps
         * toward/from them */
        {
            WhaleUIComputedStyle& b = a->base[el_from_key(kkey)];
            for (size_t pi = 0; pi < st.props.size(); ++pi) {
                WhaleUIComputedStyle::iterator si =
                    style.find(st.props[pi]);
                if (si != style.end()) {
                    b[st.props[pi]] = si->second;
                } else {
                    b.erase(st.props[pi]);
                }
            }
        }
    }
    /* restore the base values before interpolating: on later calls the
     * style holds the last animated value, but an implied endpoint must
     * lerp toward the PRE-animation value */
    {
        WhaleUIComputedStyle& b = a->base[el_from_key(kkey)];
        for (WhaleUIComputedStyle::iterator bi = b.begin();
             bi != b.end(); ++bi) {
            style[bi->first] = bi->second;
        }
    }
    uint64_t t = now - it->second.start;
    if (t < spec.delay) {
        /* delay period: fill-mode backwards/both shows the first frame
         * (kept active so the loop survives the delay and the run starts) */
        if (spec.backwards) {
            interp_frames(kf, start_progress(spec.dir), style);
        }
        a->active = 1;
        return;
    }
    t -= spec.delay;
    if (spec.iter < 0) { /* infinite */
        float p = dir_progress(spec.dir, t / spec.dur,
                               static_cast<float>(t % spec.dur) / static_cast<float>(spec.dur));
        interp_frames(kf, ease_value(spec.timing, p), style);
        a->active = 1;
    } else {
        uint64_t total = static_cast<uint64_t>(spec.dur) * static_cast<uint64_t>(spec.iter);
        if (t >= total) {
            if (spec.forwards) {
                interp_frames(kf, end_progress(spec.dir, spec.iter), style);
            }
            /* run finished; state kept so it doesn't restart */
        } else {
            float p = dir_progress(spec.dir, t / spec.dur,
                                   static_cast<float>(t % spec.dur) / static_cast<float>(spec.dur));
            interp_frames(kf, ease_value(spec.timing, p), style);
            a->active = 1;
        }
    }
}

/* Transition: interpolate changed properties toward their new values. */
static std::string prop_from_key(const std::string& key); /* defined below */
void apply_transition(whaleui_anim_t* a, const std::string& elkey,
                      WhaleUIComputedStyle& style, uint64_t now,
                      bool allow_retarget)
{
    TransSpec ts;
    build_trans_spec(style, ts);
    for (WhaleUIComputedStyle::iterator kv = style.begin(); kv != style.end(); ++kv) {
        if (kv->first == "transition" || kv->first == "transition-property" ||
            kv->first == "transition-duration" || kv->first == "transition-delay" ||
            kv->first == "transition-timing-function" || kv->first == "animation") {
            continue;
        }
        if (!trans_in_props(ts, kv->first)) {
            continue;
        }
        std::string key = kv->first + "@" + elkey;
        std::map<std::string, whaleui_anim::Trans>::iterator tr = a->trans.find(key);
        if (tr != a->trans.end()) {
            /* in flight: advance, retarget if the target changed mid-flight */
            float p = static_cast<float>(static_cast<int64_t>(now) -
                                         static_cast<int64_t>(tr->second.start)) /
                      static_cast<float>(tr->second.dur);
            if (p >= 1.0f) {
                kv->second = tr->second.to;
                a->prev[key] = tr->second.to;
                a->trans.erase(tr);
            } else {
                if (p < 0.0f) {
                    p = 0.0f; /* delay period: hold the old value */
                }
                /* retarget only when the style holds a FRESH CSS value
                 * (layout rebuild). On paint-only animation frames the
                 * style already holds the last interpolated value, which
                 * differs from `to` - retargeting on it re-anchors start
                 * every frame and the transition never finishes (the
                 * "hover color freezes mid-fade" report). */
                if (allow_retarget && kv->second != tr->second.to) {
                    std::string cur;
                    if (any_lerp(kv->first, tr->second.from, tr->second.to, p, cur)) {
                        tr->second.from = cur;
                    }
                    tr->second.to = kv->second;
                    tr->second.start = now; /* retarget skips the delay */
                    p = 0.0f;
                }
                any_lerp(kv->first, tr->second.from, tr->second.to,
                         ease_value(ts.timing, p), kv->second);
                a->active = 1;
            }
            continue;
        }
        std::map<std::string, std::string>::iterator pv = a->prev.find(key);
        if (pv == a->prev.end()) {
            /* the property appeared with a pseudo-class (the cascade is
             * static between relayouts, so a property that was never in
             * the style before can only have arrived via :hover/:active).
             * Its previous value is the property's neutral value: animate
             * from it so entering the state transitions instead of
             * snapping - without this a hover transform had no baseline
             * (prev was never recorded) and the lift/fade jumped
             * instantly. Only transform has a known neutral; everything
             * else falls back to recording the value (snap on change). */
            std::string tmp;
            if (ts.dur > 0 && kv->first == "transform" &&
                any_lerp("transform", "none", kv->second, 0.0f, tmp)) {
                whaleui_anim::Trans st;
                st.from = "none";
                st.to = kv->second;
                st.start = now + ts.delay;
                st.dur = ts.dur;
                st.delay = ts.delay;
                st.timing = ts.timing;
                a->trans[key] = st;
                kv->second = "none"; /* start from the neutral value */
                a->active = 1;
            } else {
                a->prev[key] = kv->second;
            }
            continue;
        }
        if (pv->second == kv->second) {
            continue;
        }
        std::string tmp;
        if (ts.dur > 0 && any_lerp(kv->first, pv->second, kv->second, 0.0f, tmp)) {
            whaleui_anim::Trans st;
            st.from = pv->second;
            st.to = kv->second;
            st.start = now + ts.delay;
            st.dur = ts.dur;
            st.delay = ts.delay;
            st.timing = ts.timing;
            a->trans[key] = st;
            kv->second = pv->second; /* start from the previous value */
            a->active = 1;
        } else {
            pv->second = kv->second; /* no transition / not interpolable: snap */
        }
    }
    /* a transitioned property that left the style (:hover/:active no
     * longer matches, so the cascade dropped it) must animate back to its
     * neutral value - the hover lift/color reverts smoothly instead of
     * jumping. Only transform has a known neutral ("none"). */
    for (std::map<std::string, std::string>::iterator it = a->prev.begin();
         it != a->prev.end();) {
        std::string prop = prop_from_key(it->first);
        std::string elk = it->first.substr(it->first.find('@') + 1);
        if (elk != elkey || !trans_in_props(ts, prop) ||
            style.find(prop) != style.end() || prop != "transform") {
            ++it;
            continue;
        }
        std::string tmp;
        if (ts.dur > 0 && any_lerp("transform", it->second, "none", 0.0f, tmp)) {
            whaleui_anim::Trans st;
            st.from = it->second;
            st.to = "none";
            st.start = now + ts.delay;
            st.dur = ts.dur;
            st.delay = ts.delay;
            st.timing = ts.timing;
            a->trans[prop + "@" + elkey] = st;
            a->active = 1;
        }
        it = a->prev.erase(it); /* the property is gone: drop the baseline */
    }
}

} // namespace

extern "C" int whaleui_anim_apply(whaleui_anim_t* a, struct lxb_dom_element* el,
                                  WhaleUIComputedStyle& style, uint64_t now)
{
    if (!a) {
        return 0;
    }
    a->active = 0;
    /* fast path: only elements that carry animation/transition styles can
     * animate - skip the key-string build + map lookups for the rest
     * (the bulk of nodes on rule-heavy pages) */
    if (style.find("animation") == style.end() &&
        style.find("animation-name") == style.end() &&
        style.find("transition") == style.end() &&
        style.find("transition-property") == style.end()) {
        return 0;
    }
    std::string elkey = std::to_string(reinterpret_cast<size_t>(el));
    /* keyframes first: while a keyframe animation runs, its values win and
     * transitions are skipped for the element */
    std::string av;
    WhaleUIComputedStyle::const_iterator ait = style.find("animation");
    if (ait != style.end()) {
        av = ait->second;
    }
    if (!av.empty() && av != "none") {
        apply_keyframes(a, elkey, style, now);
        if (a->active) {
            return 1;
        }
    }
    apply_transition(a, elkey, style, now, true);
    return a->active;
}

namespace {

/* properties whose animation requires a layout rebuild (box geometry) */
bool is_layout_prop(const std::string& p)
{
    static const char* kLayoutProps[] = {
        "width", "height", "min-width", "max-width", "min-height", "max-height",
        "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
        "border-width", "border-top-width", "border-right-width",
        "border-bottom-width", "border-left-width",
        "top", "right", "bottom", "left", "font-size", "line-height",
        "display", "position", "flex", "flex-grow", "flex-shrink", "flex-basis",
        "gap", "row-gap", "column-gap", "grid-template-columns",
        "grid-template-rows", "grid-column", "grid-row",
        "overflow", "overflow-x", "overflow-y", "z-index",
    };
    for (size_t i = 0; i < sizeof(kLayoutProps) / sizeof(kLayoutProps[0]); ++i) {
        if (p == kLayoutProps[i]) {
            return true;
        }
    }
    return false;
}

/* "name@12345" / "opacity@12345" key helpers */
lxb_dom_element* el_from_key(const std::string& key)
{
    size_t at = key.find_last_of('@');
    if (at == std::string::npos) {
        return nullptr;
    }
    return reinterpret_cast<lxb_dom_element*>(
        std::strtoull(key.c_str() + at + 1, nullptr, 10));
}

std::string prop_from_key(const std::string& key)
{
    size_t at = key.find_last_of('@');
    return at == std::string::npos ? std::string() : key.substr(0, at);
}

void act_prop(whaleui_anim_t* a, lxb_dom_element* el, const std::string& prop)
{
    a->act[el].insert(prop);
    if (!a->needs_layout && is_layout_prop(prop)) {
        a->needs_layout = 1;
    }
}

} // namespace

extern "C" int whaleui_anim_tick(whaleui_anim_t* a, uint64_t now)
{
    if (!a) {
        return 0;
    }
    a->active = 0;
    a->needs_layout = 0;
    a->act.clear();
    a->ov.clear();

    /* keyframe animations */
    for (std::map<std::string, whaleui_anim::KfState>::iterator it = a->kf.begin();
         it != a->kf.end(); ++it) {
        whaleui_anim::KfState& st = it->second;
        lxb_dom_element* el = el_from_key(it->first);
        const whaleui_keyframes_t* kf = find_keyframes(a->kfs, st.name.c_str());
        if (!kf) {
            continue;
        }
        uint64_t t = now - st.start;
        bool running = false;
        float p = -1.0f; /* -1: nothing to inject (finished) */
        if (t < st.delay) {
            if (st.backwards) {
                p = start_progress(st.dir);
            }
            running = true;
        } else {
            t -= st.delay;
            if (st.iter < 0) {
                p = dir_progress(st.dir, t / st.dur,
                                 static_cast<float>(t % st.dur) /
                                     static_cast<float>(st.dur));
                running = true;
            } else if (t >= static_cast<uint64_t>(st.dur) *
                                 static_cast<uint64_t>(st.iter)) {
                if (st.forwards) {
                    p = end_progress(st.dir, st.iter);
                }
            } else {
                p = dir_progress(st.dir, t / st.dur,
                                 static_cast<float>(t % st.dur) /
                                     static_cast<float>(st.dur));
                running = true;
            }
        }
        if (p >= 0.0f) {
            WhaleUIComputedStyle tmp;
            auto bi = a->base.find(el);
            if (bi != a->base.end()) {
                tmp = bi->second; /* implied 0%/100% frames need the base */
            }
            float ev = ease_value(st.timing, p);
            interp_frames(kf, ev, tmp);
            std::map<std::string, std::string>& target = a->ov[el];
            for (WhaleUIComputedStyle::iterator kv = tmp.begin();
                 kv != tmp.end(); ++kv) {
                target[kv->first] = kv->second;
                act_prop(a, el, kv->first);
            }
        }        if (running) {
            a->active = 1;
        }
    }

    /* transitions */
    for (std::map<std::string, whaleui_anim::Trans>::iterator it = a->trans.begin();
         it != a->trans.end();) {
        std::string prop = prop_from_key(it->first);
        lxb_dom_element* el = el_from_key(it->first);
        whaleui_anim::Trans& tr = it->second;
        float p = static_cast<float>(static_cast<int64_t>(now) -
                                     static_cast<int64_t>(tr.start)) /
                  static_cast<float>(tr.dur);
        if (p >= 1.0f) {
            if (el) {
                a->ov[el][prop] = tr.to;
                /* keep the element in the act table so the frame loop's
                 * apply_ov (gated on anim_has_el -> act) applies the
                 * completed value - otherwise the style stays at the last
                 * interpolated color ("hover freezes mid-fade"). */
                act_prop(a, el, prop);
            }
            a->prev[it->first] = tr.to;
            it = a->trans.erase(it);
            /* keep active for one more frame so the frame loop's
             * apply_ov applies the completed value (ov=to) to the style.
             * Without this, animating turns false the frame the
             * transition finishes and the style is left at its last
             * interpolated value - "hover color freezes mid-fade". */
            a->active = 1;
            continue;
        }
        if (p < 0.0f) {
            p = 0.0f; /* delay period */
        }
        if (el) {
            std::string v;
            any_lerp(prop, tr.from, tr.to, ease_value(tr.timing, p), v);
            a->ov[el][prop] = v;
            act_prop(a, el, prop);
        }
        a->active = 1;
        ++it;
    }
    return a->active;
}

extern "C" int whaleui_anim_active(const whaleui_anim_t* a)
{
    return a && a->active;
}

extern "C" int whaleui_anim_needs_layout(const whaleui_anim_t* a)
{
    return a && a->needs_layout;
}

extern "C" int whaleui_anim_has_el(const whaleui_anim_t* a,
                                   lxb_dom_element* el)
{
    return a && el && a->act.count(el) != 0;
}

extern "C" void whaleui_anim_apply_ov(const whaleui_anim_t* a,
                                      lxb_dom_element* el,
                                      WhaleUIComputedStyle& style)
{
    if (!a || !el) {
        return;
    }
    std::map<lxb_dom_element*, std::map<std::string, std::string> >::const_iterator
        it = a->ov.find(el);
    if (it == a->ov.end()) {
        return;
    }
    for (std::map<std::string, std::string>::const_iterator kv = it->second.begin();
         kv != it->second.end(); ++kv) {
        style[kv->first] = kv->second;
    }
}
