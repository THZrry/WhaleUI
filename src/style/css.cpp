/* CSS parser: public C API implementation.
 *
 * Self-contained lightweight parser (no third-party dep): strips comments,
 * groups @media/@keyframes/@font-face, splits comma selector lists, drops
 * !important markers (flagged on the rule), keeps custom properties (--*).
 * Selector matching and cascade live in style.cpp. */

#include "style/css.h"
#include "fs/fs.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace {

char* dup_range(const char* s, size_t n)
{
    char* d = static_cast<char*>(std::malloc(n + 1));
    if (d) {
        std::memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

char* trim(char* s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        ++s;
    }
    char* end = s + std::strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
        --end;
    }
    *end = '\0';
    return s;
}

/* strip a leading '@media ' / '@keyframes ' name from s, return the rest */
const char* at_name(const char* s, const char* kw, size_t kwlen)
{
    if (std::strncmp(s, kw, kwlen) == 0) {
        s += kwlen;
        while (*s == ' ' || *s == '\t') {
            ++s;
        }
        return s;
    }
    return nullptr;
}

void decl_append(whaleui_css_rule_t* rule, const char* name, const char* value,
                 bool important, bool* any_important)
{
    size_t n = std::strlen(name) + std::strlen(value) + 2;
    char* kv = static_cast<char*>(std::malloc(n));
    if (!kv) {
        return;
    }
    std::sprintf(kv, "%s=%s", name, value);
    char** nd = static_cast<char**>(std::realloc(rule->decls, (rule->decl_count + 1) * sizeof(char*)));
    if (!nd) {
        std::free(kv);
        return;
    }
    rule->decls = nd;
    rule->decls[rule->decl_count++] = kv;
    if (important && any_important) {
        *any_important = 1;
    }
}

/* parse one "prop: value[!important];" declaration into rule */
void parse_decl(whaleui_css_rule_t* rule, char* decl, bool* any_important)
{
    char* colon = std::strchr(decl, ':');
    if (!colon) {
        return;
    }
    *colon = '\0';
    char* name = trim(decl);
    char* value = trim(colon + 1);
    if (!*name || !*value) {
        return;
    }
    /* strip trailing ';' and !important */
    size_t vlen = std::strlen(value);
    if (vlen > 0 && value[vlen - 1] == ';') {
        value[vlen - 1] = '\0';
        value = trim(value);
    }
    bool important = false;
    char* bang = std::strstr(value, "!important");
    if (bang) {
        *bang = '\0';
        value = trim(value);
        important = true;
    }
    decl_append(rule, name, value, important, any_important);
}

void rule_destroy(whaleui_css_rule_t* rule)
{
    std::free(rule->selector);
    std::free(rule->media);
    for (size_t j = 0; j < rule->decl_count; ++j) {
        std::free(rule->decls[j]);
    }
    std::free(rule->decls);
    std::memset(rule, 0, sizeof(*rule));
}

/* append a copy of `rule` (deep) to arr */
int rules_append(whaleui_css_rule_t** arr, size_t* count, size_t* cap,
                 const whaleui_css_rule_t* rule)
{
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        whaleui_css_rule_t* na = static_cast<whaleui_css_rule_t*>(std::realloc(*arr, *cap * sizeof(*na)));
        if (!na) {
            return -1;
        }
        *arr = na;
    }
    whaleui_css_rule_t* dst = &(*arr)[(*count)++];
    std::memset(dst, 0, sizeof(*dst));
    dst->selector = dup_range(rule->selector, std::strlen(rule->selector));
    dst->media = rule->media ? dup_range(rule->media, std::strlen(rule->media)) : nullptr;
    dst->important = rule->important;
    for (size_t i = 0; i < rule->decl_count; ++i) {
        char* kv = dup_range(rule->decls[i], std::strlen(rule->decls[i]));
        if (!kv) {
            return -1;
        }
        char** nd = static_cast<char**>(std::realloc(dst->decls, (dst->decl_count + 1) * sizeof(char*)));
        if (!nd) {
            std::free(kv);
            return -1;
        }
        dst->decls = nd;
        dst->decls[dst->decl_count++] = kv;
    }
    return 0;
}

/* parse the decls of one rule block (already copied to `body`) */
void parse_body(whaleui_css_rule_t* rule, char* body)
{
    char* d = body;
    while (*d) {
        char* semicolon = std::strchr(d, ';');
        if (semicolon) {
            *semicolon = '\0';
        }
        bool any = false;
        parse_decl(rule, d, &any);
        if (any) {
            rule->important = 1;
        }
        if (!semicolon) {
            break;
        }
        d = semicolon + 1;
    }
}

/* strip /* ... *​/ comments in place */
void strip_comments(char* s)
{
    char* w = s;
    char* r = s;
    while (*r) {
        if (r[0] == '/' && r[1] == '*') {
            r += 2;
            while (*r && !(r[0] == '*' && r[1] == '/')) {
                ++r;
            }
            if (*r) {
                r += 2;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* full parse. Returns 0 on success (keyframes may be NULL). */
int parse_all(const char* css, size_t len,
              whaleui_css_rule_t** out_rules, size_t* out_count,
              whaleui_css_keyframes_t* out_kf)
{
    char* buf = dup_range(css, len);
    if (!buf) {
        return -2;
    }
    strip_comments(buf);

    whaleui_css_rule_t* arr = nullptr;
    size_t count = 0, cap = 0;
    whaleui_keyframes* kfs = nullptr;
    size_t kf_count = 0;

    char* p = buf;
    while (*p) {
        /* skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            ++p;
        }
        if (!*p) {
            break;
        }

        /* @-rules */
        if (*p == '@') {
            char* brace = std::strchr(p, '{');
            if (!brace) {
                break;
            }
            /* header: everything from p to brace */
            char* head = dup_range(p, static_cast<size_t>(brace - p));
            if (!head) {
                break;
            }
            char* h = trim(head);
            const char* cond = nullptr;
            if ((cond = at_name(h, "@media", 6)) != nullptr) {
                char* media = dup_range(cond, std::strlen(cond));
                std::free(head);
                /* find matching close brace (nested blocks possible) */
                int depth = 1;
                char* q = brace + 1;
                char* close = nullptr;
                while (*q && depth) {
                    if (*q == '{') {
                        ++depth;
                    } else if (*q == '}') {
                        --depth;
                        if (depth == 0) {
                            close = q;
                            break;
                        }
                    }
                    ++q;
                }
                if (!close) {
                    std::free(media);
                    break;
                }
                /* parse inner rules recursively with media context */
                char* inner = dup_range(brace + 1, static_cast<size_t>(close - brace - 1));
                if (inner) {
                    whaleui_css_rule_t* sub = nullptr;
                    size_t sub_count = 0;
                    whaleui_css_keyframes_t sub_kf = {nullptr, 0};
                    parse_all(inner, std::strlen(inner), &sub, &sub_count, &sub_kf);
                    for (size_t i = 0; i < sub_count; ++i) {
                        if (sub[i].media) {
                            std::free(sub[i].media);
                        }
                        sub[i].media = dup_range(media, std::strlen(media));
                        rules_append(&arr, &count, &cap, &sub[i]);
                        rule_destroy(&sub[i]);
                    }
                    std::free(sub);
                    std::free(inner);
                }
                std::free(media);
                p = close + 1;
                continue;
            }
            if ((cond = at_name(h, "@keyframes", 10)) != nullptr) {
                /* animation name: first token of cond */
                char* aname = dup_range(cond, std::strlen(cond));
                char* at = trim(aname);
                char* sp = std::strchr(at, ' ');
                if (sp) {
                    *sp = '\0';
                }
                std::free(head);
                int depth = 1;
                char* q = brace + 1;
                char* close = nullptr;
                while (*q && depth) {
                    if (*q == '{') {
                        ++depth;
                    } else if (*q == '}') {
                        --depth;
                        if (depth == 0) {
                            close = q;
                            break;
                        }
                    }
                    ++q;
                }
                if (!close) {
                    std::free(aname);
                    break;
                }
                /* collect frames: "key { decls }" blocks into one item */
                whaleui_keyframes kf;
                std::memset(&kf, 0, sizeof(kf));
                kf.name = dup_range(at, std::strlen(at));
                char* inner = dup_range(brace + 1, static_cast<size_t>(close - brace - 1));
                if (inner && out_kf) {
                    char* f = inner;
                    while (*f) {
                        char* fb = std::strchr(f, '{');
                        if (!fb) {
                            break;
                        }
                        char* fc = std::strchr(fb, '}');
                        if (!fc) {
                            break;
                        }
                        char* key = dup_range(f, static_cast<size_t>(fb - f));
                        char* kt = trim(key);
                        char* body = dup_range(fb + 1, static_cast<size_t>(fc - fb - 1));
                        if (kt && *kt && body) {
                            whaleui_css_rule_t tmp;
                            std::memset(&tmp, 0, sizeof(tmp));
                            parse_body(&tmp, body);
                            /* encode frame as "key=prop:value;prop:value" */
                            size_t need = std::strlen(kt) + 1;
                            for (size_t i = 0; i < tmp.decl_count; ++i) {
                                need += std::strlen(tmp.decls[i]) + 1;
                            }
                            char* enc = static_cast<char*>(std::malloc(need));
                            if (enc) {
                                char* w = enc;
                                w += std::sprintf(w, "%s=", kt);
                                for (size_t i = 0; i < tmp.decl_count; ++i) {
                                    w += std::sprintf(w, "%s;", tmp.decls[i]);
                                }
                                char** nf = static_cast<char**>(std::realloc(kf.frames, (kf.frame_count + 1) * sizeof(char*)));
                                if (nf) {
                                    kf.frames = nf;
                                    kf.frames[kf.frame_count++] = enc;
                                } else {
                                    std::free(enc);
                                }
                            }
                            rule_destroy(&tmp);
                        }
                        std::free(key);
                        std::free(body);
                        f = fc + 1;
                    }
                }
                std::free(inner);
                if (out_kf && kf.frame_count) {
                    whaleui_keyframes* nk = static_cast<whaleui_keyframes*>(
                        std::realloc(kfs, (kf_count + 1) * sizeof(*nk)));
                    if (nk) {
                        kfs = nk;
                        kfs[kf_count++] = kf;
                    } else {
                        std::free(kf.name);
                        for (size_t i = 0; i < kf.frame_count; ++i) {
                            std::free(kf.frames[i]);
                        }
                        std::free(kf.frames);
                    }
                } else {
                    std::free(kf.name);
                    std::free(kf.frames);
                }
                std::free(aname);
                p = close + 1;
                continue;
            }
            /* other at-rules (@font-face, @charset, ...): skip the block */
            int depth = 1;
            char* q = brace + 1;
            while (*q && depth) {
                if (*q == '{') {
                    ++depth;
                } else if (*q == '}') {
                    --depth;
                }
                ++q;
            }
            std::free(head);
            p = *q ? q + 1 : q;
            continue;
        }

        /* plain rule: selector { ... } */
        char* brace = std::strchr(p, '{');
        if (!brace) {
            break;
        }
        char* close = std::strchr(brace, '}');
        if (!close) {
            break;
        }
        char* sel = dup_range(p, static_cast<size_t>(brace - p));
        if (!sel) {
            break;
        }
        char* st = trim(sel);
        /* split comma-separated selectors into separate rules */
        char* body = dup_range(brace + 1, static_cast<size_t>(close - brace - 1));
        whaleui_css_rule_t tmp;
        std::memset(&tmp, 0, sizeof(tmp));
        if (body) {
            parse_body(&tmp, body);
            std::free(body);
        }
        char* s = st;
        while (*s) {
            char* comma = std::strchr(s, ',');
            if (comma) {
                *comma = '\0';
            }
            char* one = trim(s);
            if (*one) {
                tmp.selector = dup_range(one, std::strlen(one));
                rules_append(&arr, &count, &cap, &tmp);
                std::free(tmp.selector);
                tmp.selector = nullptr;
            }
            if (!comma) {
                break;
            }
            s = comma + 1;
        }
        rule_destroy(&tmp);
        std::free(sel);
        p = close + 1;
    }

    std::free(buf);
    if (out_rules) {
        *out_rules = arr;
    }
    if (out_count) {
        *out_count = count;
    }
    if (out_kf) {
        out_kf->items = kfs;
        out_kf->count = kf_count;
    }
    return 0;
}

} // namespace

/* internal: full parse including @keyframes (used by the style engine) */
extern "C" int whaleui_css_parse_full(const char* css, size_t len,
                                      whaleui_css_rule_t** rules, size_t* count,
                                      whaleui_css_keyframes_t* keyframes)
{
    if (!css || !rules || !count || !keyframes) {
        return -1;
    }
    *rules = nullptr;
    *count = 0;
    keyframes->items = nullptr;
    keyframes->count = 0;
    return parse_all(css, len, rules, count, keyframes);
}

extern "C" int whaleui_css_parse(whaleui_css_rule_t** rules, size_t* count,
                                 const char* css, size_t len)
{
    if (!rules || !count || !css) {
        return -1;
    }
    *rules = nullptr;
    *count = 0;
    whaleui_css_keyframes_t kf = {nullptr, 0};
    int rc = parse_all(css, len, rules, count, &kf);
    for (size_t i = 0; i < kf.count; ++i) {
        std::free(kf.items[i].name);
        for (size_t j = 0; j < kf.items[i].frame_count; ++j) {
            std::free(kf.items[i].frames[j]);
        }
        std::free(kf.items[i].frames);
    }
    std::free(kf.items);
    return rc;
}

extern "C" int whaleui_css_load(whaleui_css_rule_t** rules, size_t* count, const char* uri)
{
    if (!uri) {
        return -1;
    }
    char* css = nullptr;
    size_t len = 0;
    if (whaleui_fs_load(uri, &css, &len) != 0) {
        return -2;
    }
    int rc = whaleui_css_parse(rules, count, css, len);
    std::free(css);
    return rc;
}

extern "C" void whaleui_css_rules_destroy(whaleui_css_rule_t* rules, size_t count)
{
    if (!rules) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        rule_destroy(&rules[i]);
    }
    std::free(rules);
}

extern "C" void whaleui_css_keyframes_destroy(whaleui_css_keyframes_t* kf)
{
    if (!kf) {
        return;
    }
    for (size_t i = 0; i < kf->count; ++i) {
        std::free(kf->items[i].name);
        for (size_t j = 0; j < kf->items[i].frame_count; ++j) {
            std::free(kf->items[i].frames[j]);
        }
        std::free(kf->items[i].frames);
    }
    std::free(kf->items);
    kf->items = nullptr;
    kf->count = 0;
}

extern "C" const char* whaleui_css_selector(const whaleui_css_rule_t* rule)
{
    return rule ? rule->selector : nullptr;
}

namespace {
const char* decl_get(char* const* decls, size_t count, const char* name)
{
    size_t n = std::strlen(name);
    for (size_t i = 0; i < count; ++i) {
        if (std::strncmp(decls[i], name, n) == 0 && decls[i][n] == '=') {
            return decls[i] + n + 1;
        }
    }
    return nullptr;
}
} // namespace

extern "C" const char* whaleui_css_get_property(const whaleui_css_rule_t* rule, const char* name)
{
    if (!rule || !name) {
        return nullptr;
    }
    return decl_get(rule->decls, rule->decl_count, name);
}

extern "C" int whaleui_css_has_property(const whaleui_css_rule_t* rule, const char* name)
{
    return whaleui_css_get_property(rule, name) != nullptr;
}

/* css_apply is implemented by the style engine (style.cpp) */
