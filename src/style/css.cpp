/* CSS: public C API implementation.
 * Step 2: contract implementation - minimal CSS parsing (selectors + one
 * block each, "prop: value;" declarations). Step 3 adds cascade, media
 * queries, keyframes, var() and theme mapping. */

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

/* parse one "prop: value;" declaration, append to rule */
void parse_decl(whaleui_css_rule_t* rule, char* decl)
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
    /* strip trailing ';' */
    size_t vlen = std::strlen(value);
    if (vlen > 0 && value[vlen - 1] == ';') {
        value[vlen - 1] = '\0';
        value = trim(value);
    }
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
}

} // namespace

extern "C" int whaleui_css_parse(whaleui_css_rule_t** rules, size_t* count,
                                 const char* css, size_t len)
{
    if (!rules || !count || !css) {
        return -1;
    }
    *rules = nullptr;
    *count = 0;

    char* buf = dup_range(css, len);
    if (!buf) {
        return -2;
    }

    size_t cap = 0;
    whaleui_css_rule_t* arr = nullptr;

    /* scan for "selector { ... }" blocks */
    char* p = buf;
    while (*p) {
        char* brace = std::strchr(p, '{');
        if (!brace) {
            break;
        }
        char* close = std::strchr(brace, '}');
        if (!close) {
            break;
        }
        /* selector */
        char* sel = dup_range(p, static_cast<size_t>(brace - p));
        if (!sel) {
            break;
        }
        /* trim in place, keeping the malloc'd base pointer for free() */
        {
            char* s = sel;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
                ++s;
            }
            char* e = s + std::strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) {
                --e;
            }
            *e = '\0';
            if (s != sel) {
                std::memmove(sel, s, static_cast<size_t>(e - s + 1));
            }
        }
        whaleui_css_rule_t rule;
        std::memset(&rule, 0, sizeof(rule));
        rule.selector = sel;

        /* declarations */
        char* body = dup_range(brace + 1, static_cast<size_t>(close - brace - 1));
        if (body) {
            /* split on ';', then parse each decl */
            char* d = body;
            while (*d) {
                char* semicolon = std::strchr(d, ';');
                if (semicolon) {
                    *semicolon = '\0';
                }
                parse_decl(&rule, d);
                if (!semicolon) {
                    break;
                }
                d = semicolon + 1;
            }
            std::free(body);
        }

        if (*count == cap) {
            cap = cap ? cap * 2 : 4;
            whaleui_css_rule_t* na = static_cast<whaleui_css_rule_t*>(std::realloc(arr, cap * sizeof(*arr)));
            if (!na) {
                std::free(sel);
                break;
            }
            arr = na;
        }
        arr[(*count)++] = rule;
        p = close + 1;
    }

    std::free(buf);
    *rules = arr;
    return 0;
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
        std::free(rules[i].selector);
        for (size_t j = 0; j < rules[i].decl_count; ++j) {
            std::free(rules[i].decls[j]);
        }
        std::free(rules[i].decls);
    }
    std::free(rules);
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

extern "C" int whaleui_css_apply(whaleui_dom_document_t* doc,
                                 const whaleui_css_rule_t* rules, size_t count)
{
    (void)doc;
    (void)rules;
    (void)count;
    /* Step 2: selector matching lands with real DOM in step 3. */
    return 0;
}
