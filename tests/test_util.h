#ifndef WHALEUI_TEST_UTIL_H
#define WHALEUI_TEST_UTIL_H

/* WHALEUI_TEST_ROOT is injected by xmake: absolute repo root (forward slashes).
 * Build a file:// URI relative to it. */
#ifdef WHALEUI_TEST_ROOT
#define TEST_URI_RAW(rel) "file://" WHALEUI_TEST_ROOT "/" rel
/* reference pages moved out of the repo root into temp/ */
#define TEST_URI_TEMP(rel) "file://" WHALEUI_TEST_ROOT "/temp/" rel
#else
#define TEST_URI_RAW(rel) "file://" rel
#define TEST_URI_TEMP(rel) "file://temp/" rel
#endif

#endif /* WHALEUI_TEST_UTIL_H */
