/* Animation engine: CSS `transition` + `@keyframes`/`animation`.
 *
 * See animate.h for the design. Pure logic (no GPU/window), the clock is
 * caller-supplied so tests drive time deterministically. */

#include "animate/animate.h"

#include "render/render.h" /* whaleui_render_parse_color */

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
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

/* timing functions -> progress in [0,1] (ease falls back to ease-in-out) */
float ease_value(const std::string& name, float t)
{
    if (name == "linear") {
        return t;
    }
    if (name == "ease-in") {
        return t * t;
    }
    if (name == "ease-out") {
        float u = 1.0f - t;
        return 1.0f - u * u;
    }
    /* ease / ease-in-out / default */
    if (t < 0.5f) {
        return 2.0f * t * t;
    }
    float u = -2.0f * t + 2.0f;
    return 1.0f - u * u / 2.0f;
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

/* Parsed `animation` shorthand. iter: -1 = infinite. */
struct AnimSpec
{
    std::string name;
    uint32_t dur;
    uint32_t delay;
    int iter;
    bool forwards;
    std::string timing;
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
            continue; /* treated as fill-mode none */
        }
        if (tok == "linear" || tok == "ease" || tok == "ease-in" ||
            tok == "ease-out" || tok == "ease-in-out") {
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

/* First duration in a `transition` value ("background-color 0.3s ease",
 * "all 0.1s"); 0 when none. One duration applies to every interpolable
 * property (approximates `transition: all`). */
uint32_t transition_dur(const std::string& v)
{
    if (v.empty() || v == "none") {
        return 0;
    }
    const char* p = v.c_str();
    while (*p) {
        if ((*p >= '0' && *p <= '9') || *p == '.') {
            float num = static_cast<float>(std::atof(p));
            const char* q = p;
            while ((*q >= '0' && *q <= '9') || *q == '.') {
                ++q;
            }
            while (*q == ' ' || *q == '\t') {
                ++q;
            }
            if (*q == 'm' && q[1] == 's') {
                return static_cast<uint32_t>(num);
            }
            if (*q == 's') {
                return static_cast<uint32_t>(num * 1000.0f);
            }
            p = q;
        } else {
            ++p;
        }
    }
    return 0;
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
    if (!lo) {
        lo = hi;
    }
    if (!hi) {
        hi = lo;
    }
    if (!lo || !hi) {
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
            if (value_lerp(lo->decls[i].second, *hv, t, v)) {
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

    /* running keyframe animation: "name@el" */
    struct KfState
    {
        std::string name;
        uint64_t start;
        uint32_t dur;
        int iter;
    };
    std::map<std::string, KfState> kf;

    /* running transition: "prop@el" */
    struct Trans
    {
        std::string from, to;
        uint64_t start;
        uint32_t dur;
    };
    std::map<std::string, Trans> trans;

    /* last settled computed value per "prop@el" (transition baseline) */
    std::map<std::string, std::string> prev;
};

extern "C" whaleui_anim_t* whaleui_anim_create(void)
{
    whaleui_anim_t* a = new whaleui_anim_t;
    a->kfs = nullptr;
    a->active = 0;
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
        a->active = 0;
    }
}

extern "C" int whaleui_anim_active(const whaleui_anim_t* a)
{
    return a && a->active;
}

extern "C" uint64_t whaleui_anim_now(void)
{
    return SDL_GetTicks();
}

namespace {

/* Play the `animation` shorthand: inject the current keyframe values. */
void apply_keyframes(whaleui_anim_t* a, const std::string& elkey,
                     WhaleUIComputedStyle& style, uint64_t now)
{
    std::string av;
    WhaleUIComputedStyle::const_iterator ait = style.find("animation");
    if (ait != style.end()) {
        av = ait->second;
    }
    if (av.empty() || av == "none") {
        return;
    }
    AnimSpec spec;
    if (!parse_anim(av, spec)) {
        return;
    }
    const whaleui_keyframes_t* kf = find_keyframes(a->kfs, spec.name.c_str());
    if (!kf) {
        return; /* unknown animation name: no effect */
    }
    std::string kkey = spec.name + "@" + elkey;
    std::map<std::string, whaleui_anim::KfState>::iterator it = a->kf.find(kkey);
    if (it == a->kf.end() || it->second.name != spec.name ||
        it->second.dur != spec.dur || it->second.iter != spec.iter) {
        whaleui_anim::KfState st;
        st.name = spec.name;
        st.start = now;
        st.dur = spec.dur;
        st.iter = spec.iter;
        a->kf[kkey] = st;
        it = a->kf.find(kkey);
    }
    uint64_t t = now - it->second.start;
    if (t < spec.delay) {
        return; /* delay period (fill-mode backwards/both not supported) */
    }
    t -= spec.delay;
    if (spec.iter < 0) { /* infinite */
        float p = static_cast<float>(t % spec.dur) / static_cast<float>(spec.dur);
        interp_frames(kf, ease_value(spec.timing, p), style);
        a->active = 1;
    } else {
        uint64_t total = static_cast<uint64_t>(spec.dur) * static_cast<uint64_t>(spec.iter);
        if (t >= total) {
            if (spec.forwards) {
                interp_frames(kf, 1.0f, style); /* hold the last frame */
            }
            /* run finished; state kept so it doesn't restart */
        } else {
            float p = static_cast<float>(t % spec.dur) / static_cast<float>(spec.dur);
            interp_frames(kf, ease_value(spec.timing, p), style);
            a->active = 1;
        }
    }
}

/* Transition: interpolate changed properties toward their new values. */
void apply_transition(whaleui_anim_t* a, const std::string& elkey,
                      WhaleUIComputedStyle& style, uint64_t now)
{
    std::string tv;
    WhaleUIComputedStyle::const_iterator tit = style.find("transition");
    if (tit != style.end()) {
        tv = tit->second;
    }
    const uint32_t dur = transition_dur(tv);
    for (WhaleUIComputedStyle::iterator kv = style.begin(); kv != style.end(); ++kv) {
        if (kv->first == "transition" || kv->first == "animation") {
            continue;
        }
        std::string key = kv->first + "@" + elkey;
        std::map<std::string, whaleui_anim::Trans>::iterator tr = a->trans.find(key);
        if (tr != a->trans.end()) {
            /* in flight: advance, retarget if the target changed mid-flight */
            float p = static_cast<float>(now - tr->second.start) /
                      static_cast<float>(tr->second.dur);
            if (p >= 1.0f) {
                kv->second = tr->second.to;
                a->prev[key] = tr->second.to;
                a->trans.erase(tr);
            } else {
                if (kv->second != tr->second.to) {
                    std::string cur;
                    if (value_lerp(tr->second.from, tr->second.to, p, cur)) {
                        tr->second.from = cur;
                    }
                    tr->second.to = kv->second;
                    tr->second.start = now;
                    p = 0.0f;
                }
                value_lerp(tr->second.from, tr->second.to, p, kv->second);
                a->active = 1;
            }
            continue;
        }
        std::map<std::string, std::string>::iterator pv = a->prev.find(key);
        if (pv == a->prev.end()) {
            a->prev[key] = kv->second;
            continue;
        }
        if (pv->second == kv->second) {
            continue;
        }
        std::string tmp;
        if (dur > 0 && value_lerp(pv->second, kv->second, 0.0f, tmp)) {
            whaleui_anim::Trans st;
            st.from = pv->second;
            st.to = kv->second;
            st.start = now;
            st.dur = dur;
            a->trans[key] = st;
            kv->second = pv->second; /* start from the previous value */
            a->active = 1;
        } else {
            pv->second = kv->second; /* no transition / not interpolable: snap */
        }
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
    apply_transition(a, elkey, style, now);
    return a->active;
}
