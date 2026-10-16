/* DataTransfer + File: the drag-and-drop payload objects.
 *
 * A DataTransfer is a small typed-item bag ("text/plain", "text/uri-list",
 * any format string) plus a file list and the effectAllowed/dropEffect
 * negotiation pair - the W3C subset a page actually uses. Files carry a name
 * and a VFS uri; the bytes are read lazily (a dropped 2GB file costs nothing
 * until the page asks for it). */

#include "dom/data_transfer.h"
#include "fs/fs.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

/* basename of a path or uri ("C:/a/b.txt" or "file:///a/b.txt" -> "b.txt") */
std::string basename_of(const std::string& uri)
{
    size_t cut = uri.find_last_of("/\\");
    std::string base = cut == std::string::npos ? uri : uri.substr(cut + 1);
    /* strip a query/fragment if a loader handed us one */
    size_t q = base.find_first_of("?#");
    return q == std::string::npos ? base : base.substr(0, q);
}

} // namespace

extern "C" const char* whaleui_file_name(const whaleui_file_t* f)
{
    return f ? f->name.c_str() : "";
}

extern "C" const char* whaleui_file_uri(const whaleui_file_t* f)
{
    return f ? f->uri.c_str() : "";
}

extern "C" long long whaleui_file_size(const whaleui_file_t* f)
{
    if (!f) {
        return -1;
    }
    if (!f->measured) {
        /* measure through the VFS (the only loader that knows how to resolve
         * the uri); the length is cached so repeated size() is free */
        whaleui_file_t* m = const_cast<whaleui_file_t*>(f);
        char* data = nullptr;
        size_t len = 0;
        if (whaleui_fs_load(f->uri.c_str(), &data, &len) == 0) {
            m->size = static_cast<long long>(len);
            std::free(data);
        } else {
            m->size = -1;
        }
        m->measured = 1;
    }
    return f->size;
}

extern "C" int whaleui_file_read(const whaleui_file_t* f, char** out, size_t* outlen)
{
    if (!f || !out || !outlen) {
        return -1;
    }
    return whaleui_fs_load(f->uri.c_str(), out, outlen) == 0 ? 0 : -2;
}

extern "C" whaleui_data_transfer_t* whaleui_data_transfer_create(void)
{
    whaleui_data_transfer_t* dt = new whaleui_data_transfer_t;
    dt->effect_allowed = 0;
    dt->drop_effect = 0;
    return dt;
}

extern "C" void whaleui_data_transfer_destroy(whaleui_data_transfer_t* dt)
{
    if (!dt) {
        return;
    }
    for (size_t i = 0; i < dt->files.size(); ++i) {
        delete dt->files[i];
    }
    delete dt;
}

extern "C" int whaleui_data_transfer_set_data(whaleui_data_transfer_t* dt,
                                              const char* format,
                                              const char* data)
{
    if (!dt || !format || !*format || !data) {
        return -1;
    }
    for (size_t i = 0; i < dt->items.size(); ++i) {
        if (dt->items[i].first == format) {
            dt->items[i].second = data; /* replace in place (order kept) */
            return 0;
        }
    }
    dt->items.push_back(std::make_pair(std::string(format), std::string(data)));
    return 0;
}

extern "C" const char* whaleui_data_transfer_get_data(const whaleui_data_transfer_t* dt,
                                                      const char* format)
{
    if (!dt || !format) {
        return nullptr;
    }
    for (size_t i = 0; i < dt->items.size(); ++i) {
        if (dt->items[i].first == format) {
            return dt->items[i].second.c_str();
        }
    }
    return nullptr;
}

extern "C" int whaleui_data_transfer_has_format(const whaleui_data_transfer_t* dt,
                                                const char* format)
{
    return whaleui_data_transfer_get_data(dt, format) ? 1 : 0;
}

extern "C" int whaleui_data_transfer_clear_data(whaleui_data_transfer_t* dt)
{
    if (!dt) {
        return -1;
    }
    dt->items.clear();
    return 0;
}

extern "C" size_t whaleui_data_transfer_type_count(const whaleui_data_transfer_t* dt)
{
    return dt ? dt->items.size() : 0;
}

extern "C" const char* whaleui_data_transfer_type_item(const whaleui_data_transfer_t* dt,
                                                       size_t index)
{
    if (!dt || index >= dt->items.size()) {
        return nullptr;
    }
    return dt->items[index].first.c_str();
}

extern "C" int whaleui_data_transfer_set_text(whaleui_data_transfer_t* dt,
                                              const char* text)
{
    return whaleui_data_transfer_set_data(dt, "text/plain", text ? text : "");
}

extern "C" const char* whaleui_data_transfer_get_text(const whaleui_data_transfer_t* dt)
{
    return whaleui_data_transfer_get_data(dt, "text/plain");
}

extern "C" size_t whaleui_data_transfer_file_count(const whaleui_data_transfer_t* dt)
{
    return dt ? dt->files.size() : 0;
}

extern "C" const whaleui_file_t* whaleui_data_transfer_file_item(const whaleui_data_transfer_t* dt,
                                                                 size_t index)
{
    if (!dt || index >= dt->files.size()) {
        return nullptr;
    }
    return dt->files[index];
}

extern "C" int whaleui_data_transfer_add_file(whaleui_data_transfer_t* dt,
                                              const char* name,
                                              const char* uri)
{
    if (!dt || !uri) {
        return -1;
    }
    whaleui_file_t* f = new whaleui_file_t;
    f->uri = uri;
    f->name = (name && *name) ? name : basename_of(uri);
    f->size = -1;
    f->measured = 0;
    dt->files.push_back(f);
    return 0;
}

extern "C" int whaleui_data_transfer_effect_allowed(const whaleui_data_transfer_t* dt)
{
    return dt ? dt->effect_allowed : 0;
}

extern "C" void whaleui_data_transfer_set_effect_allowed(whaleui_data_transfer_t* dt,
                                                         int effect)
{
    if (dt) {
        dt->effect_allowed = effect;
    }
}

extern "C" int whaleui_data_transfer_drop_effect(const whaleui_data_transfer_t* dt)
{
    return dt ? dt->drop_effect : 0;
}

extern "C" void whaleui_data_transfer_set_drop_effect(whaleui_data_transfer_t* dt,
                                                      int effect)
{
    if (dt) {
        dt->drop_effect = effect;
    }
}
