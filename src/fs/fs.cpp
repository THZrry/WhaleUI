/* Virtual file system: default disk loader + public API.
 * Step 2: contract implementation (disk loader real, protocol dispatch stub). */

#include "fs/fs.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct WhaleUIFSLoader
{
    whaleui_fs_open_fn open = nullptr;
    whaleui_fs_read_fn read = nullptr;
    whaleui_fs_close_fn close = nullptr;
    void* userdata = nullptr;
};

WhaleUIFSLoader& loader()
{
    static WhaleUIFSLoader l;
    return l;
}

/* strip "file://" prefix -> bare path */
std::string path_from_uri(const char* uri)
{
    const char* p = uri;
    if (std::strncmp(p, "file://", 7) == 0) {
        p += 7;
    }
    return p;
}

/* default disk loader callbacks */
void* disk_open(const char* uri, void*)
{
    std::string path = path_from_uri(uri);
    return std::fopen(path.c_str(), "rb");
}

size_t disk_read(void* handle, char* buf, size_t size, void*)
{
    return std::fread(buf, 1, size, static_cast<FILE*>(handle));
}

void disk_close(void* handle, void*)
{
    if (handle) {
        std::fclose(static_cast<FILE*>(handle));
    }
}

} // namespace

extern "C" void whaleui_fs_set_loader_impl(whaleui_fs_open_fn open, whaleui_fs_read_fn read,
                                           whaleui_fs_close_fn close, void* userdata)
{
    loader().open = open;
    loader().read = read;
    loader().close = close;
    loader().userdata = userdata;
}

extern "C" int whaleui_fs_set_loader(whaleui_fs_open_fn open, whaleui_fs_read_fn read,
                                     whaleui_fs_close_fn close, void* userdata)
{
    if (open || read || close) {
        if (!open || !read || !close) {
            return -1; /* partial loader rejected */
        }
        whaleui_fs_set_loader_impl(open, read, close, userdata);
    } else {
        whaleui_fs_set_loader_impl(nullptr, nullptr, nullptr, nullptr);
    }
    return 0;
}

extern "C" int whaleui_fs_load(const char* uri, char** out, size_t* outlen)
{
    if (!uri || !out || !outlen) {
        return -1;
    }
    *out = nullptr;
    *outlen = 0;

    whaleui_fs_open_fn open = loader().open ? loader().open : disk_open;
    whaleui_fs_read_fn read = loader().read ? loader().read : disk_read;
    whaleui_fs_close_fn close = loader().close ? loader().close : disk_close;
    void* ud = loader().userdata;

    void* h = open(uri, ud);
    if (!h) {
        return -2;
    }

    /* grow as needed; start 4k */
    size_t cap = 4096, len = 0;
    char* buf = static_cast<char*>(std::malloc(cap));
    if (!buf) {
        close(h, ud);
        return -3;
    }
    for (;;) {
        if (len == cap) {
            cap *= 2;
            char* nb = static_cast<char*>(std::realloc(buf, cap));
            if (!nb) {
                std::free(buf);
                close(h, ud);
                return -3;
            }
            buf = nb;
        }
        size_t n = read(h, buf + len, cap - len, ud);
        if (n == 0) {
            break;
        }
        len += n;
    }
    close(h, ud);

    buf = static_cast<char*>(std::realloc(buf, len + 1));
    buf[len] = '\0';
    *out = buf;
    *outlen = len;
    return 0;
}
