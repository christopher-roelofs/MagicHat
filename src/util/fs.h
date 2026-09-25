/*
 * fs.h — the few filesystem calls POSIX and Windows spell differently.
 *
 * Paths stay forward-slashed everywhere. Windows accepts '/' as well as '\',
 * and the rest of the emulator splits paths on '/', so the Windows side of
 * mrc_realpath converts on the way out rather than every caller learning both.
 */
#ifndef MRC_UTIL_FS_H
#define MRC_UTIL_FS_H

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif

/* Windows also takes '\', which is what Explorer and cmd hand a program. */
static inline bool mrc_is_path_separator(char c)
{
#ifdef _WIN32
    if (c == '\\') return true;
#endif
    return c == '/';
}

/* The last separator in a path, or NULL when there is none. */
static inline const char *mrc_path_last_separator(const char *path)
{
    const char *last = NULL;
    for (const char *p = path; *p; p++)
        if (mrc_is_path_separator(*p)) last = p;
    return last;
}

/* The final component: the filename without its directories. */
static inline const char *mrc_path_base(const char *path)
{
    const char *slash = mrc_path_last_separator(path);
    return slash ? slash + 1 : path;
}

/* Absolute form of an existing path. False when it does not exist, as with
 * realpath, or when the result would not fit. */
static inline bool mrc_realpath(const char *path, char *out, size_t size)
{
#ifdef _WIN32
    if (!_fullpath(out, path, size)) return false;
    struct stat st;
    if (stat(out, &st) != 0) return false;
    for (char *p = out; *p; p++)
        if (*p == '\\') *p = '/';
    return true;
#else
    char resolved[PATH_MAX];
    if (!realpath(path, resolved) || strlen(resolved) >= size) return false;
    memcpy(out, resolved, strlen(resolved) + 1);
    return true;
#endif
}

/* A directory only this user can read, where permissions mean anything. */
static inline int mrc_mkdir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

/* True for the top of a filesystem: "/" or, on Windows, a drive such as
 * "C:/". There is nothing above it to go up to. */
static inline bool mrc_path_is_root(const char *path)
{
#ifdef _WIN32
    if (path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\') && !path[3])
        return true;
#endif
    return !strcmp(path, "/");
}

/* Where scratch files go. */
static inline const char *mrc_temp_dir(void)
{
#ifdef _WIN32
    static char dir[1024];
    const char *env = getenv("TEMP");
    if (!env || !*env) env = getenv("TMP");
    snprintf(dir, sizeof(dir), "%s", env && *env ? env : ".");
    for (char *p = dir; *p; p++)
        if (*p == '\\') *p = '/';
    return dir;
#else
    return "/tmp";
#endif
}

/* mkdtemp, which Windows lacks: the pattern ends in XXXXXX. */
static inline char *mrc_mkdtemp(char *pattern)
{
#ifdef _WIN32
    if (_mktemp_s(pattern, strlen(pattern) + 1) || _mkdir(pattern)) return NULL;
    return pattern;
#else
    return mkdtemp(pattern);
#endif
}

static inline int mrc_setenv(const char *name, const char *value)
{
#ifdef _WIN32
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}

#endif /* MRC_UTIL_FS_H */
