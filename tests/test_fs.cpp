// test_fs: virtual file system contract tests.
#include "whaleui.h"
#include "test_util.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* TEST_URI = TEST_URI_RAW("tests/data/hello.txt");

namespace {

int g_opened = 0;
int g_eof = 0;
const char FAKE[] = "via-loader";

void* my_open(const char*, void*)
{
    g_opened = 1;
    g_eof = 0;
    return reinterpret_cast<void*>(0x1);
}

size_t my_read(void*, char* buf, size_t size, void*)
{
    if (g_eof) {
        return 0;
    }
    size_t n = sizeof(FAKE) - 1;
    if (size > n) {
        size = n;
    }
    std::memcpy(buf, FAKE, size);
    g_eof = 1;
    return size;
}

void my_close(void*, void*) {}

} // namespace

int main(void)
{
    /* default disk loader: read a file that exists */
    {
        char* out = nullptr;
        size_t n = 0;
        int rc = whaleui_fs_load(TEST_URI, &out, &n);
        assert(rc == 0);
        assert(out != nullptr);
        assert(std::strcmp(out, "hello, whaleui\r\n") == 0 ||
               std::strcmp(out, "hello, whaleui\n") == 0);
        assert(n == std::strlen(out));
        std::free(out);
    }

    /* missing file -> failure, no output */
    {
        char* out = reinterpret_cast<char*>(0x1);
        size_t n = 1;
        int rc = whaleui_fs_load(TEST_URI_RAW("tests/data/nope.txt"), &out, &n);
        assert(rc != 0);
        assert(out == nullptr);
        assert(n == 0);
    }

    /* null args rejected */
    {
        assert(whaleui_fs_load(nullptr, nullptr, nullptr) != 0);
    }

    /* custom loader: replace, verify it is used, restore */
    {
        int rc = whaleui_fs_set_loader(my_open, my_read, my_close, nullptr);
        assert(rc == 0);

        char* out = nullptr;
        size_t n = 0;
        rc = whaleui_fs_load("http://cdn.example.com/app.css", &out, &n);
        assert(rc == 0);
        assert(g_opened == 1);
        assert(n == sizeof(FAKE) - 1);
        assert(std::strcmp(out, FAKE) == 0);
        std::free(out);

        /* partial loader rejected */
        rc = whaleui_fs_set_loader(my_open, nullptr, nullptr, nullptr);
        assert(rc != 0);

        /* restore default */
        rc = whaleui_fs_set_loader(nullptr, nullptr, nullptr, nullptr);
        assert(rc == 0);
    }

    /* disk loader works again after restore */
    {
        char* out = nullptr;
        size_t n = 0;
        assert(whaleui_fs_load(TEST_URI, &out, &n) == 0);
        assert(std::strcmp(out, "hello, whaleui\r\n") == 0 || std::strcmp(out, "hello, whaleui\n") == 0);
        std::free(out);
    }

    return 0;
}


