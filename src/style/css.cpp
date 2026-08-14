/* CSS parser: public C API implementation.
 *
 * Parsing is delegated to lexbor's CSS module (standard tokenizer: comments,
 * escapes, strings, @media/@keyframes handling). The result is converted
 * into the engine's flat rule table (selector + "prop=value" decls +
 * rule-level !important) that style.cpp's cascade consumes. Selector
 * matching and the cascade stay in style.cpp. */

#include "style/css.h"
#include "style/style.h"
#include "fs/fs.h"

#include <lexbor/css/parser.h>
#include <lexbor/css/stylesheet.h>
#include <lexbor/css/rule.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <functional>
#include <string>

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

/* trimmed slice of the source css */
std::string css_slice(const char* css, size_t begin, size_t end)
{
    while (begin < end && (css[begin] == ' ' || css[begin] == '\t' ||
                           css[begin] == '\n' || css[begin] == '\r')) {
        ++begin;
    }
    while (end > begin && (css[end - 1] == ' ' || css[end - 1] == '\t' ||
                           css[end - 1] == '\n' || css[end - 1] == '\r')) {
        --end;
    }
    return std::string(css + begin, end - begin);
}

/* copy the decl strings of src into dst */
void copy_decls(const whaleui_css_rule_t* src, whaleui_css_rule_t* dst)
{
    for (size_t i = 0; i < src->decl_count; ++i) {
        char* kv = dup_range(src->decls[i], std::strlen(src->decls[i]));
        if (!kv) {
            continue;
        }
        char** nd = static_cast<char**>(std::realloc(dst->decls, (dst->decl_count + 1) * sizeof(char*)));
        if (!nd) {
            std::free(kv);
            continue;
        }
        dst->decls = nd;
        dst->decls[dst->decl_count++] = kv;
    }
}

/* emit one rule per comma-separated selector from a style rule */
void emit_style_rule(const whaleui_css_rule_t* tmp, const std::string& sel,
                     const char* media,
                     whaleui_css_rule_t** arr, size_t* count, size_t* cap)
{
    size_t start = 0;
    for (size_t i = 0; i <= sel.size(); ++i) {
        if (i == sel.size() || sel[i] == ',') {
            std::string one = css_slice(sel.c_str(), start, i);
            start = i + 1;
            if (one.empty()) {
                continue;
            }
            whaleui_css_rule_t r;
            std::memset(&r, 0, sizeof(r));
            r.selector = dup_range(one.c_str(), one.size());
            if (!r.selector) {
                continue;
            }
            r.media = media ? dup_range(media, std::strlen(media)) : nullptr;
            r.important = tmp->important;
            copy_decls(tmp, &r);
            rules_append(arr, count, cap, &r);
            rule_destroy(&r);
        }
    }
}

/* collect the declarations of a style rule (name/value from the source
 * offsets so the original text is preserved) */
void collect_decls(lxb_css_rule_declaration_list_t* dl, const char* css,
                   whaleui_css_rule_t* out)
{
    if (!dl) {
        return;
    }
    for (lxb_css_rule_t* d = (lxb_css_rule_t*)dl->first; d; d = d->next) {
        if (d->type != LXB_CSS_RULE_DECLARATION) {
            continue;
        }
        lxb_css_rule_declaration_t* dec = (lxb_css_rule_declaration_t*)d;
        std::string nm = css_slice(css, dec->offset.name_begin,
                                   dec->offset.name_end);
        std::string vl = css_slice(css, dec->offset.value_begin,
                                   dec->offset.value_end);
        if (nm.empty() || vl.empty()) {
            continue;
        }
        bool any = false;
        decl_append(out, nm.c_str(), vl.c_str(), dec->important, &any);
        if (any) {
            out->important = 1;
        }
    }
}

/* collect a @keyframes block: frames "0%/from/to -> decls" encoded as
 * "key=prop=value;..." strings (matches the animate engine's format) */
void collect_keyframes(lxb_css_rule_t* rules, const char* css,
                       const std::string& name,
                       whaleui_keyframes** kfs, size_t* kf_count)
{
    whaleui_keyframes kf;
    std::memset(&kf, 0, sizeof(kf));
    kf.name = dup_range(name.c_str(), name.size());
    if (!kf.name) {
        return;
    }
    std::function<void(lxb_css_rule_t*)> walk = [&](lxb_css_rule_t* rs) {
        for (lxb_css_rule_t* r = rs; r; r = r->next) {
            if (r->type == LXB_CSS_RULE_LIST) {
                walk(((lxb_css_rule_list_t*)r)->first);
            } else if (r->type == LXB_CSS_RULE_STYLE ||
                       r->type == LXB_CSS_RULE_BAD_STYLE) {
                /* "0%" keyframe selectors parse as BAD_STYLE (not a valid
                 * selector); both carry prelude offsets + declarations */
                size_t pb = 0, pe = 0;
                lxb_css_rule_declaration_list_t* dl = nullptr;
                if (r->type == LXB_CSS_RULE_STYLE) {
                    lxb_css_rule_style_t* st = (lxb_css_rule_style_t*)r;
                    pb = st->prelude_begin;
                    pe = st->prelude_end;
                    dl = st->declarations;
                } else {
                    lxb_css_rule_bad_style_t* bd =
                        (lxb_css_rule_bad_style_t*)r;
                    pb = bd->prelude_begin;
                    pe = bd->prelude_end;
                    dl = bd->declarations;
                }
                std::string key = css_slice(css, pb, pe);
                if (key.empty()) {
                    continue;
                }
                std::string enc = key + "=";
                if (dl) {
                    for (lxb_css_rule_t* d = (lxb_css_rule_t*)dl->first; d;
                         d = d->next) {
                        if (d->type != LXB_CSS_RULE_DECLARATION) {
                            continue;
                        }
                        lxb_css_rule_declaration_t* dec =
                            (lxb_css_rule_declaration_t*)d;
                        std::string nm = css_slice(css, dec->offset.name_begin,
                                                   dec->offset.name_end);
                        std::string vl = css_slice(css, dec->offset.value_begin,
                                                   dec->offset.value_end);
                        if (!nm.empty() && !vl.empty()) {
                            enc += nm + "=" + vl + ";";
                        }
                    }
                }
                char* es = dup_range(enc.c_str(), enc.size());
                if (es) {
                    char** nf = static_cast<char**>(std::realloc(
                        kf.frames, (kf.frame_count + 1) * sizeof(char*)));
                    if (nf) {
                        kf.frames = nf;
                        kf.frames[kf.frame_count++] = es;
                    } else {
                        std::free(es);
                    }
                }
            }
        }
    };
    walk(rules);
    if (kf.frame_count) {
        whaleui_keyframes* nk = static_cast<whaleui_keyframes*>(
            std::realloc(*kfs, (*kf_count + 1) * sizeof(*nk)));
        if (nk) {
            *kfs = nk;
            (*kfs)[(*kf_count)++] = kf;
        } else {
            std::free(kf.name);
            std::free(kf.frames);
        }
    } else {
        std::free(kf.name);
        std::free(kf.frames);
    }
}

/* walk the lexbor rule tree into the flat rule table */
void collect_rules(lxb_css_rule_t* rules, const char* css,
                   whaleui_css_rule_t** arr, size_t* count, size_t* cap,
                   whaleui_keyframes** kfs, size_t* kf_count,
                   const char* media)
{
    for (lxb_css_rule_t* r = rules; r; r = r->next) {
        if (r->type == LXB_CSS_RULE_LIST) {
            collect_rules(((lxb_css_rule_list_t*)r)->first, css, arr, count,
                          cap, kfs, kf_count, media);
        } else if (r->type == LXB_CSS_RULE_STYLE ||
                   r->type == LXB_CSS_RULE_BAD_STYLE) {
            /* some valid-for-us rules parse as BAD_STYLE (e.g. ::after
             * pseudo-element selectors); both carry prelude offsets +
             * declarations */
            size_t pb = 0, pe = 0;
            lxb_css_rule_declaration_list_t* dl = nullptr;
            if (r->type == LXB_CSS_RULE_STYLE) {
                lxb_css_rule_style_t* st = (lxb_css_rule_style_t*)r;
                pb = st->prelude_begin;
                pe = st->prelude_end;
                dl = st->declarations;
            } else {
                lxb_css_rule_bad_style_t* bd =
                    (lxb_css_rule_bad_style_t*)r;
                pb = bd->prelude_begin;
                pe = bd->prelude_end;
                dl = bd->declarations;
            }
            whaleui_css_rule_t tmp;
            std::memset(&tmp, 0, sizeof(tmp));
            collect_decls(dl, css, &tmp);
            std::string sel = css_slice(css, pb, pe);
            emit_style_rule(&tmp, sel, media, arr, count, cap);
            rule_destroy(&tmp);
        } else if (r->type == LXB_CSS_RULE_AT_RULE) {
            lxb_css_rule_at_t* at = (lxb_css_rule_at_t*)r;
            if (at->type == LXB_CSS_AT_RULE_MEDIA && at->u.media) {
                std::string cond = css_slice(css, at->prelude_begin,
                                             at->prelude_end);
                collect_rules((lxb_css_rule_t*)at->u.media->block, css, arr,
                              count, cap, kfs, kf_count, cond.c_str());
            } else if (at->type == LXB_CSS_AT_RULE__CUSTOM && at->u.custom) {
                const lxb_char_t* nmd = at->u.custom->name.data;
                std::string nm(nmd ? (const char*)nmd : "",
                               at->u.custom->name.length);
                if (nm == "keyframes") {
                    std::string aname = css_slice(css, at->prelude_begin,
                                                  at->prelude_end);
                    if (!aname.empty()) {
                        collect_keyframes(
                            (lxb_css_rule_t*)at->u.custom->block, css,
                            aname, kfs, kf_count);
                    }
                }
            }
        }
    }
}

/* full parse. Returns 0 on success. */
int parse_all(const char* css, size_t len,
              whaleui_css_rule_t** out_rules, size_t* out_count,
              whaleui_css_keyframes_t* out_kf)
{
    lxb_css_parser_t* parser = lxb_css_parser_create();
    if (!parser) {
        return -2;
    }
    lxb_css_parser_init(parser, nullptr);
    lxb_css_stylesheet_t* sst = lxb_css_stylesheet_create(nullptr);
    if (!sst) {
        lxb_css_parser_destroy(parser, true);
        return -2;
    }
    lxb_status_t st = lxb_css_stylesheet_parse(sst, parser,
                                               (const lxb_char_t*)css, len);
    if (st != LXB_STATUS_OK) {
        lxb_css_stylesheet_destroy(sst, true);
        lxb_css_parser_destroy(parser, true);
        return -1;
    }
    whaleui_css_rule_t* arr = nullptr;
    size_t count = 0, cap = 0;
    whaleui_keyframes* kfs = nullptr;
    size_t kf_count = 0;
    collect_rules(sst->root, css, &arr, &count, &cap, &kfs, &kf_count,
                  nullptr);
    lxb_css_stylesheet_destroy(sst, true);
    lxb_css_parser_destroy(parser, true);
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

extern "C" const char* whaleui_css_get_property(const whaleui_css_rule_t* rule,
                                                const char* name)
{
    if (!rule || !name) {
        return nullptr;
    }
    return decl_get(rule->decls, rule->decl_count, name);
}

extern "C" int whaleui_css_has_property(const whaleui_css_rule_t* rule,
                                        const char* name)
{
    return whaleui_css_get_property(rule, name) != nullptr;
}

/* Drop rules whose @media condition does not match the current context
 * (theme, viewport width, reduced-motion). In-place compaction. */
extern "C" int whaleui_style_filter_media(whaleui_css_rule_t* rules,
                                          size_t* count, int theme,
                                          int viewport_w, int reduced_motion)
{
    if (!rules || !count) {
        return -1;
    }
    size_t w = 0;
    for (size_t i = 0; i < *count; ++i) {
        if (rules[i].media &&
            !whaleui_style_media_ok(rules[i].media, theme, viewport_w,
                                    reduced_motion)) {
            rule_destroy(&rules[i]);
            continue;
        }
        if (w != i) {
            rules[w] = rules[i];
        }
        ++w;
    }
    *count = w;
    return 0;
}
