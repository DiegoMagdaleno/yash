/* Yash: yet another shell */
/* path.c: filename-related utilities */
/* (C) 2007-2010 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "common.h"
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#if HAVE_GETTEXT
# include <libintl.h>
#endif
#if HAVE_PATHS_H
# include <paths.h>
#endif
#include "builtin.h"
#include "exec.h"
#include "expand.h"
#include "hashtable.h"
#include "option.h"
#include "path.h"
#include "plist.h"
#include "redir.h"
#include "sig.h"
#include "strbuf.h"
#include "util.h"
#include "variable.h"
#include "xfnmatch.h"
#include "yash.h"


#if HAVE_FACCESSAT
# ifndef faccessat
extern int faccessat(int fd, const char *path, int amode, int flags)
    __attribute__((nonnull));
# endif
#elif HAVE_EACCESS
# ifndef eaccess
extern int eaccess(const char *path, int amode)
    __attribute__((nonnull));
# endif
#endif
static bool check_access(const char *path, mode_t mode, int amode)
    __attribute__((nonnull));
static inline bool not_dotdot(const wchar_t *p)
    __attribute__((nonnull,pure));

/* Checks if `path' is an existing file. */
bool is_file(const char *path)
{
    return access(path, F_OK) == 0;
    // struct stat st;
    // return (stat(path, &st) == 0);
}

/* Checks if `path' is a regular file. */
bool is_regular_file(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
}

/* Checks if `path' is a non-regular file. */
bool is_irregular_file(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) && !S_ISREG(st.st_mode);
}

/* Checks if `path' is a readable file. */
bool is_readable(const char *path)
{
    return check_access(path, S_IRUSR | S_IRGRP | S_IROTH, R_OK);
}

/* Checks if `path' is a writable file. */
bool is_writable(const char *path)
{
    return check_access(path, S_IWUSR | S_IWGRP | S_IWOTH, W_OK);
}

/* Checks if `path' is an executable file (or a searchable directory). */
bool is_executable(const char *path)
{
    return check_access(path, S_IXUSR | S_IXGRP | S_IXOTH, X_OK);
}

/* Checks if this process has a proper permission to access the specified file.
 * Returns false if the file does not exist. */
bool check_access(const char *path, mode_t mode, int amode)
{
    /* Even if the faccessat/eaccess function was considered available by
     * `configure', the OS kernel may not support it. We fall back on our own
     * checking function if faccessat/eaccess was rejected. */
#if HAVE_FACCESSAT
    int result = faccessat(AT_FDCWD, path, amode, AT_EACCESS);
    if (result == 0 || (errno != ENOSYS && errno != EINVAL))
	return result == 0;
#elif HAVE_EACCESS
    int result = eaccess(path, amode);
    if (result == 0 || (errno != ENOSYS && errno != EINVAL))
	return result == 0;
#else
    (void) amode;
#endif

    /* The algorithm below is not 100% valid for all POSIX systems. */
    struct stat st;
    uid_t uid;
    gid_t gid;

    if (stat(path, &st) < 0)
	return false;

    uid = geteuid();
#if !YASH_DISABLE_SUPERUSER
    if (uid == 0) {
	/* the "root" user has special permissions */
	return (mode & (S_IRUSR | S_IRGRP | S_IROTH
	              | S_IWUSR | S_IWGRP | S_IWOTH))
	    || S_ISDIR(st.st_mode)
	    || (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH));
    }
#endif

    st.st_mode &= mode;
    if (uid == st.st_uid)
	return st.st_mode & S_IRWXU;
    gid = getegid();
    if (gid == st.st_gid)
	return st.st_mode & S_IRWXG;

    int gcount = getgroups(0, &gid);  /* the second argument is a dummy */
    if (gcount > 0) {
	gid_t groups[gcount];
	gcount = getgroups(gcount, groups);
	if (gcount > 0) {
	    for (int i = 0; i < gcount; i++)
		if (gid == groups[i])
		    return st.st_mode & S_IRWXG;
	}
    }

    return st.st_mode & S_IRWXO;
}

/* Checks if `path' is a readable regular file. */
bool is_readable_regular(const char *path)
{
    return is_regular_file(path) && is_readable(path);
}

/* Checks if `path' is an executable regular file. */
bool is_executable_regular(const char *path)
{
    return is_regular_file(path) && is_executable(path);
}

/* Checks if `path' is a directory. */
bool is_directory(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}

/* Checks if two files are the same file. */
bool is_same_file(const char *path1, const char *path2)
{
    struct stat stat1, stat2;
    return stat(path1, &stat1) == 0 && stat(path2, &stat2) == 0
	&& stat1.st_dev == stat2.st_dev && stat1.st_ino == stat2.st_ino;
}

/* Canonicalizes a pathname.
 *  * Dot components are removed.
 *  * Dot-dot components are removed together with the preceding components. If
 *    any of the preceding components is not a directory, it is an error.
 *  * Redundant slashes are removed.
 * `path' must not be NULL.
 * The result is a newly malloced string if successful. NULL is returned on
 * error. */
wchar_t *canonicalize_path(const wchar_t *path)
{
    wchar_t *const canon = xmalloc((wcslen(path) + 1) * sizeof *canon);
    size_t len = 0;
    plist_T clist;

    pl_init(&clist);

    if (*path == L'/') {  /* first slash */
	path++;
	canon[len++] = '/';
	if (*path == L'/') {  /* second slash */
	    path++;
	    if (*path != L'/')  /* third slash */
		canon[len++] = '/';
	}
    }

    for (;;) {
	canon[len] = L'\0';
	while (*path == L'/')
	    path++;
	if (path[0] == L'\0')
	    break;
	if (path[0] == L'.') {
	    if (path[1] == L'\0') {
		/* ignore trailing dot component */
		break;
	    } else if (path[1] == L'/') {
		/* skip dot component */
		path += 2;
		continue;
	    } else if (path[1] == L'.' && (path[2] == L'\0' || path[2] == L'/')
		    && clist.length > 0) {
		/* dot-dot component */
		wchar_t *prec = clist.contents[clist.length - 1];
		if (not_dotdot(prec)) {
		    char *mbsprec = malloc_wcstombs(canon);
		    bool isdir = mbsprec != NULL && is_directory(mbsprec);
		    free(mbsprec);
		    if (isdir) {
			len = prec - canon;
			/* canon[len] = L'\0'; */
			pl_remove(&clist, clist.length - 1, 1);
			path += 2;
			continue;
		    } else {
			/* error */
			pl_destroy(&clist);
			free(canon);
			return NULL;
		    }
		}
	    }
	}

	/* others */
	pl_add(&clist, canon + len);
	if (clist.length > 1)
	    canon[len++] = L'/';
	while (*path != L'\0' && *path != L'/')  /* copy next component */
	    canon[len++] = *path++;
    }
    pl_destroy(&clist);
    assert(canon[len] == L'\0');
    return canon;
}

bool not_dotdot(const wchar_t *p)
{
    if (*p == L'/')
	p++;
    return wcscmp(p, L"..") != 0;
}

/* Checks if the specified `path' is normalized, that is, containing no "."
 * or ".." in it. */
/* Note that a normalized path may contain redundant slashes. */
bool is_normalized_path(const wchar_t *path)
{
    while (path[0] != L'\0') {
	if (path[0] == L'.' &&
		(path[1] == L'\0' || path[1] == L'/' ||
		 (path[1] == L'.' && (path[2] == L'\0' || path[2] == L'/'))))
	    return false;
	path = wcschr(path, L'/');
	if (!path)
	    break;
	path++;
    }
    return true;
}

/* Returns the result of `getcwd' as a newly malloced string.
 * On error, `errno' is set and NULL is returned. */
char *xgetcwd(void)
{
#if GETCWD_AUTO_MALLOC
    char *pwd = getcwd(NULL, 0);
    return pwd ? xrealloc(pwd, strlen(pwd) + 1) : NULL;
#else
    size_t pwdlen = 40;
    char *pwd = xmalloc(pwdlen);
    while (getcwd(pwd, pwdlen) == NULL) {
	if (errno == ERANGE) {
	    pwdlen *= 2;
	    pwd = xrealloc(pwd, pwdlen);
	} else {
	    int saveerrno = errno;
	    free(pwd);
	    pwd = NULL;
	    errno = saveerrno;
	    break;
	}
    }
    return pwd;
#endif
}


/* Searches the specified directories `dirs' for a file named `name' that
 * satisfies the specified condition.
 * name:   the pathname of the file to be searched for.
 *         If `name' is an absolute path, a copy of it is simply returned.
 * dirs:   a NULL-terminated array of strings that are the names of
 *         directories to search. An empty string is treated as the current
 *         directory. If `dirs' is NULL, no directory is searched.
 * cond:   the function that determines the specified pathname satisfies a
 *         certain condition.
 * For each directory in `dirs', in order, the directory name and "/" and
 * `name' are concatenated to produce a pathname and `cond' is called with
 * the pathname. If `cond' returns true, search is complete and the pathname
 * is returned as a newly malloced string. If `cond' returns false to all the
 * produced pathnames, NULL is returned.
 * `name' and all the directory names in `dirs' must start and end in a initial
 * shift state. */
char *which(
	const char *restrict name,
	char *const *restrict dirs,
	bool cond(const char *path))
{
    if (!name[0])
	return NULL;
    if (name[0] == '/')
	return xstrdup(name);
    if (!dirs)
	return NULL;

    size_t namelen = strlen(name);
    const char *dir;
    while ((dir = *dirs++)) {
	size_t dirlen = strlen(dir);
	char path[dirlen + namelen + 3];
	if (dirlen > 0) {
	    /* concatenate `dir' and `name' to produce a pathname `path' */
	    strcpy(path, dir);
	    if (path[dirlen - 1] != '/')
		path[dirlen++] = '/';
	    strcpy(path + dirlen, name);
	} else {
	    /* if `dir' is empty, it's considered to be the current directory */
	    strcpy(path, name);
	}
	if (cond(path))
	    return xstrdup(path);
    }
    return NULL;
}

/* Creates a new temporary file under "/tmp".
 * `mode' is the access permission bits of the file.
 * On successful completion, a file descriptor is returned, which is both
 * readable and writeable regardless of `mode', and a pointer to a string
 * containing the filename is assigned to `*filename', which should be freed by
 * the caller. The filename consists only of portable filename characters.
 * On failure, -1 is returned with `**filename' left unchanged and errno is set
 * to the error value. */
int create_temporary_file(char **filename, mode_t mode)
{
    static uintmax_t num;
    uintmax_t n;
    int fd;
    xstrbuf_T buf;

    n = (uintmax_t) shell_pid * 272229637312669;
    if (!num)
	num = (uintmax_t) time(NULL) * 5131212142718371 << 1 | 1;
    sb_init(&buf);
    for (int i = 0; i < 100; i++) {
	num = (num ^ n) * 16777619;
	sb_printf(&buf, "/tmp/yash-%X", (unsigned) (num >> 30));
	/* The filename must be 14 bytes long at most. */
	fd = open(buf.contents, O_RDWR | O_CREAT | O_EXCL, mode);
	if (fd >= 0) {
	    *filename = sb_tostr(&buf);
	    return fd;
	} else if (errno != EEXIST && errno != EINTR) {
	    int saveerrno = errno;
	    sb_destroy(&buf);
	    errno = saveerrno;
	    return -1;
	}
	sb_clear(&buf);
    }
    sb_destroy(&buf);
    errno = EAGAIN;
    return -1;
}


/********** Command Hashtable **********/

static inline void forget_command_path(const char *command)
    __attribute__((nonnull));
static wchar_t *get_default_path(void)
    __attribute__((malloc,warn_unused_result));

/* A hashtable from command names to their full path.
 * Keys are pointers to a multibyte string containing a command name and
 * values are pointers to a multibyte string containing the commands' full path.
 * For each entry, the key string is part of the value, that is, the last
 * pathname component of it. */
static hashtable_T cmdhash;

/* Initializes the command hashtable. */
void init_cmdhash(void)
{
    assert(cmdhash.capacity == 0);
    ht_init(&cmdhash, hashstr, htstrcmp);
}

/* Empties the command hashtable. */
void clear_cmdhash(void)
{
    ht_clear(&cmdhash, vfree);
}

/* Searches PATH for the specified command and returns its full pathname.
 * If `forcelookup' is false and the command is already entered in the command
 * hashtable, the value in the hashtable is returned. Otherwise, `which' is
 * called to search for the command, the result is entered into the hashtable
 * and then it is returned. If no command is found, NULL is returned. */
const char *get_command_path(const char *name, bool forcelookup)
{
    const char *path;

    if (!forcelookup) {
	path = ht_get(&cmdhash, name).value;
	if (path && is_executable_regular(path))
	    return path;
    }

    path = which(name, get_path_array(PA_PATH), is_executable_regular);
    if (path && path[0] == '/') {
	size_t namelen = strlen(name), pathlen = strlen(path);
	const char *pathname = path + pathlen - namelen;
	assert(strcmp(name, pathname) == 0);
	vfree(ht_set(&cmdhash, pathname, path));
    } else {
	forget_command_path(name);
    }
    return path;
}

/* Removes the specified command from the command hashtable. */
void forget_command_path(const char *command)
{
    vfree(ht_remove(&cmdhash, command));
}

/* Last result of `get_command_path_default'. */
static char *gcpd_value;
/* Paths for `get_command_path_default'. */
static char **default_path;

/* Searches for the specified command using the system's default PATH.
 * The full path of the command is returned if found, NULL otherwise.
 * The return value is valid until the next call to this function. */
const char *get_command_path_default(const char *name)
{
    assert(name != gcpd_value);
    free(gcpd_value);

    if (!default_path) {
	wchar_t *defpath = get_default_path();
	if (!defpath)
	    return gcpd_value = NULL;
	default_path = decompose_paths(defpath);
	free(defpath);
    }
    return gcpd_value = which(name, default_path, is_executable_regular);
}

/* Returns the system's default PATH as a newly malloced string.
 * The default PATH is (assumed to be) guaranteed to contain all the standard
 * utilities. */
wchar_t *get_default_path(void)
{
#if HAVE_PATHS_H && defined _PATH_STDPATH
    return malloc_mbstowcs(_PATH_STDPATH);
#else
    size_t size = 100;
    char *buf = xmalloc(size);
    size_t s = confstr(_CS_PATH, buf, size);

    if (s > size) {
	size = s;
	buf = xrealloc(buf, size);
	s = confstr(_CS_PATH, buf, size);
    }
    if (s == 0 || s > size) {
	free(buf);
	return NULL;
    }
    return realloc_mbstowcs(buf);
#endif
}


/********** Home Directory Cache **********/

static struct passwd *xgetpwnam(const char *name)
    __attribute__((nonnull));
static void clear_homedirhash(void);
static inline void forget_home_directory(const wchar_t *username);

/* Calls `getpwnam' until it doesn't return EINTR. */
struct passwd *xgetpwnam(const char *name)
{
    struct passwd *pw;
    do {
	errno = 0;
	pw = getpwnam(name);
    } while (!pw && errno == EINTR);
    return pw;
}

/* A hashtable from users' names to their home directory paths.
 * Keys are pointers to a wide string containing a user's login name and
 * values are pointers to a wide string containing their home directory name.
 * A memory block for the key/value string must be allocated at once;
 * When the value is `free'd, the key is `free'd as well. */
static hashtable_T homedirhash;

/* Initializes the home directory hashtable. */
void init_homedirhash(void)
{
    assert(homedirhash.capacity == 0);
    ht_init(&homedirhash, hashwcs, htwcscmp);
}

/* Empties the home directory hashtable. */
void clear_homedirhash(void)
{
    ht_clear(&homedirhash, vfree);
}

/* Returns the full pathname of the specified user's home directory.
 * If `forcelookup' is false and the path is already entered in the home
 * directory hashtable, the value in the hashtable is returned. Otherwise,
 * `getpwnam' is called, the result is entered into the hashtable and then
 * it is returned. If no entry is returned by `getpwnam', NULL is returned.  */
const wchar_t *get_home_directory(const wchar_t *username, bool forcelookup)
{
    const wchar_t *path;

    if (!forcelookup) {
	path = ht_get(&homedirhash, username).value;
	if (path)
	    return path;
    }

    char *mbsusername = malloc_wcstombs(username);
    if (!mbsusername)
	return NULL;

    struct passwd *pw = xgetpwnam(mbsusername);
    free(mbsusername);
    if (!pw)
	return NULL;

    xwcsbuf_T dir;
    wb_init(&dir);
    if (wb_mbscat(&dir, pw->pw_dir) != NULL) {
	wb_destroy(&dir);
	return NULL;
    }
    wb_wccat(&dir, L'\0');
    size_t usernameindex = dir.length;
    wb_cat(&dir, username);
    wchar_t *dirname = wb_towcs(&dir);
    vfree(ht_set(&homedirhash, dirname + usernameindex, dirname));
    return dirname;
}

/* Forget the specified user's home directory. */
void forget_home_directory(const wchar_t *username)
{
    vfree(ht_remove(&homedirhash, username));
}


/********** wglob **********/

/* The type of compiled glob patterns */
struct wglob_pattern {
    struct wglob_pattern *next;
    enum {
	WGLOB_LITERAL, WGLOB_MATCH, WGLOB_RECSEARCH,
    } type;
    union {
	struct {
	    char *name;
	    const wchar_t *wname;
	} literal;
	struct {
	    xfnmatch_T *pattern;
	} match;
	struct {
	    bool followlink, allowperiod;
	} recsearch;
    } value;
};

struct wglob_dirstack {
    struct wglob_dirstack *prev;
    struct stat st;
};

static struct wglob_pattern *wglob_parse_pattern(
	wchar_t *pat, enum wglbflags flags)
    __attribute__((malloc,warn_unused_result,nonnull));
static struct wglob_pattern *wglob_create_recsearch_pattern(
	bool followlink, bool allowperiod)
    __attribute__((malloc,warn_unused_result));
static struct wglob_pattern *wglob_parse_pattern_part(
	wchar_t *pat, enum wglbflags flags)
    __attribute__((malloc,warn_unused_result,nonnull));
static void wglob_free_pattern(struct wglob_pattern *p);
static void wglob_search(
	const struct wglob_pattern *restrict pattern,
	enum wglbflags flags,
	xstrbuf_T *restrict path,
	xwcsbuf_T *restrict wpath,
	plist_T *restrict list)
    __attribute__((nonnull));
static void wglob_search_literal(
	const struct wglob_pattern *restrict pattern,
	enum wglbflags flags,
	xstrbuf_T *restrict path,
	xwcsbuf_T *restrict wpath,
	plist_T *restrict list)
    __attribute__((nonnull));
static void wglob_search_match(
	const struct wglob_pattern *restrict pattern,
	enum wglbflags flags,
	xstrbuf_T *restrict path,
	xwcsbuf_T *restrict wpath,
	plist_T *restrict list)
    __attribute__((nonnull));
static void wglob_search_recsearch(
	const struct wglob_pattern *restrict pattern,
	enum wglbflags flags,
	xstrbuf_T *restrict path,
	xwcsbuf_T *restrict wpath,
	plist_T *restrict list,
	struct wglob_dirstack *dirstack)
    __attribute__((nonnull(1,3,4,5)));
static bool is_reentry(
	const struct stat *st, const struct wglob_dirstack *dirstack)
    __attribute__((nonnull(1)));
static int wglob_sortcmp(const void *v1, const void *v2)
    __attribute__((pure,nonnull));

/* A wide string version of `glob'.
 * Adds all pathnames that matches the specified pattern to the specified list.
 * pattern: the pattern to match
 * flags:   a bitwise OR of the following flags:
 *          WGLB_MARK:     directory items have '/' appended to their name
 *          WGLB_CASEFOLD: do matching case-insensitively
 *          WGLB_PERIOD:   L'*' and L'?' match L'.' at the head
 *          WGLB_NOSORT:   don't sort resulting items
 *          WGLB_RECDIR:   allow recursive search with L"**"
 * list:    a list of pointers to wide strings to which resulting items are
 *          added.
 * Returns true iff successful. However, some result items may be added to the
 * list even if unsuccessful.
 * If the pattern is invalid, immediately returns false.
 * If the shell is interactive and SIGINT is not blocked, this function can be
 * interrupted, in which case false is returned.
 * Minor errors such as permission errors are ignored. */
bool wglob(const wchar_t *restrict pattern, enum wglbflags flags,
	plist_T *restrict list)
{
    size_t listbase = list->length;
    xstrbuf_T path;
    xwcsbuf_T wpath;
    struct wglob_pattern *p;
    wchar_t savepattern[wcslen(pattern) + 1];

    p = wglob_parse_pattern(wcscpy(savepattern, pattern), flags);
    if (p == NULL)
	return false;

    sb_init(&path);
    wb_init(&wpath);
    wglob_search(p, flags, &path, &wpath, list);
    sb_destroy(&path);
    wb_destroy(&wpath);
    wglob_free_pattern(p);

    if (!(flags & WGLB_NOSORT)) {
	size_t count = list->length - listbase;  /* # of resulting items */
	if (count > 0) {
	    /* sort the items */
	    qsort(list->contents + listbase, count, sizeof (void *),
		    wglob_sortcmp);

	    /* remove duplicates */
	    for (size_t i = list->length; --i > listbase; ) {
		if (wcscmp(list->contents[i], list->contents[i-1]) == 0) {
		    free(list->contents[i]);
		    pl_remove(list, i, 1);
		}
	    }
	}
    }
    return !is_interrupted();
}

/* Parses the specified pattern.
 * Pattern `pat' may be modified in this function and must be unchanged until
 * the return value is freed by `wglob_free_pattern'.
 * WGLB_CASEFOLD, WGLB_PERIOD and WGLB_RECDIR in `flags' affect the results. */
struct wglob_pattern *wglob_parse_pattern(wchar_t *pat, enum wglbflags flags)
{
    struct wglob_pattern *result = NULL, **lastp = &result;
    struct wglob_pattern *p;

    for (;;) {
	wchar_t *slash = wcschr(pat, L'/');
	if (slash != NULL) {
	    *slash = L'\0';
	    if (!(flags & WGLB_RECDIR))
		goto normal;
	    if (wcscmp(pat, L"**") == 0)
		p = wglob_create_recsearch_pattern(false, false);
	    else if (wcscmp(pat, L"***") == 0)
		p = wglob_create_recsearch_pattern(true, false);
	    else if (wcscmp(pat, L".**") == 0)
		p = wglob_create_recsearch_pattern(false, true);
	    else if (wcscmp(pat, L".***") == 0)
		p = wglob_create_recsearch_pattern(true, true);
	    else
		goto normal;
	} else {
normal:
	    p = wglob_parse_pattern_part(pat, flags);
	}
	if (p == NULL)
	    goto fail;
	*lastp = p;
	lastp = &p->next;
	if (slash == NULL)
	    return result;
	pat = slash + 1;
    }

fail:
    wglob_free_pattern(result);
    return NULL;
}

struct wglob_pattern *wglob_create_recsearch_pattern(
	bool followlink, bool allowperiod)
{
    struct wglob_pattern *result = xmalloc(sizeof *result);
    result->next = NULL;
    result->type = WGLOB_RECSEARCH;
    result->value.recsearch.followlink = followlink;
    result->value.recsearch.allowperiod = allowperiod;
    return result;
}

/* Parses the specified pattern that contains one pathname component.
 * `pat' must not contain a slash. */
struct wglob_pattern *wglob_parse_pattern_part(
	wchar_t *pat, enum wglbflags flags)
{
    struct wglob_pattern *result = xmalloc(sizeof *result);
    result->next = NULL;

    assert(!wcschr(pat, L'/'));
    if (is_matching_pattern(pat)) {
	xfnmflags_T xflags = XFNM_HEADONLY | XFNM_TAILONLY;
	if (flags & WGLB_CASEFOLD)
	    xflags |= XFNM_CASEFOLD;
	if (!(flags & WGLB_PERIOD))
	    xflags |= XFNM_PERIOD;
	result->type = WGLOB_MATCH;
	result->value.match.pattern = xfnm_compile(pat, xflags);
	if (!result->value.match.pattern)
	    goto fail;
    } else {
	wchar_t *value = unescape(pat);
	assert(wcslen(value) <= wcslen(pat));
	result->type = WGLOB_LITERAL;
	result->value.literal.wname = wcscpy(pat, value);
	result->value.literal.name = realloc_wcstombs(value);
	if (!result->value.literal.name)
	    goto fail;
    }
    return result;

fail:
    free(result);
    return NULL;
}

void wglob_free_pattern(struct wglob_pattern *p)
{
    while (p) {
	struct wglob_pattern *next = p->next;
	switch (p->type) {
	    case WGLOB_LITERAL:
		free(p->value.literal.name);
		break;
	    case WGLOB_MATCH:
		xfnm_free(p->value.match.pattern);
		break;
	    case WGLOB_RECSEARCH:
		break;
	}
	free(p);
	p = next;
    }
}

/* Searches the directory designated in `path' and add matching pathnames to
 * `list'.
 * `path' and `wpath' must contain the same pathname, which must be empty or
 * end with a slash. The contents of `path' and `wpath' may be changed during
 * the search, but when the function returns the contents are restored to the
 * original value.
 * Only the WGLB_MARK flag in `flags' affects the results. */
void wglob_search(
	const struct wglob_pattern *restrict pattern,
	enum wglbflags flags,
	xstrbuf_T *restrict path,
	xwcsbuf_T *restrict wpath,
	plist_T *restrict list)
{
    assert(path->length == 0 || path->contents[path->length - 1] == '/');
    assert(wpath->length == 0 || wpath->contents[wpath->length - 1] == L'/');
    switch (pattern->type) {
	case WGLOB_LITERAL:
	    wglob_search_literal(pattern, flags, path, wpath, list);
	    break;
	case WGLOB_MATCH:
	    wglob_search_match(pattern, flags, path, wpath, list);
	    break;
	case WGLOB_RECSEARCH:
	    wglob_search_recsearch(pattern, flags, path, wpath, list, NULL);
	    break;
    }
}

/* Searches for the pathname component that does not require pattern matching.*/
void wglob_search_literal(
	const struct wglob_pattern *restrict pattern,
	enum wglbflags flags,
	xstrbuf_T *restrict path,
	xwcsbuf_T *restrict wpath,
	plist_T *restrict list)
{
    const size_t savepathlen = path->length;
    const size_t savewpathlen = wpath->length;

    assert(pattern->type == WGLOB_LITERAL);
    if (pattern->next == NULL) {
	struct stat st;
	sb_cat(path, pattern->value.literal.name);
	if (stat(path->contents, &st) >= 0) {
	    if (pattern->value.literal.wname[0] != L'\0') {
		wb_cat(wpath, pattern->value.literal.wname);
		if ((flags & WGLB_MARK) && S_ISDIR(st.st_mode))
		    wb_wccat(wpath, L'/');
	    }
	    pl_add(list, xwcsdup(wpath->contents));
	}
    } else {
	sb_ccat(sb_cat(path, pattern->value.literal.name), '/');
	wb_wccat(wb_cat(wpath, pattern->value.literal.wname), L'/');
	wglob_search(pattern->next, flags, path, wpath, list);
    }

    sb_truncate(path, savepathlen);
    wb_truncate(wpath, savewpathlen);
}

/* Searches the directory for files that match with the specified pattern. */
void wglob_search_match(
	const struct wglob_pattern *restrict pattern,
	enum wglbflags flags,
	xstrbuf_T *restrict path,
	xwcsbuf_T *restrict wpath,
	plist_T *restrict list)
{
    assert(pattern->type == WGLOB_MATCH);

    if (is_interrupted())
	return;

    DIR *dir = opendir((path->length == 0) ? "." : path->contents);
    if (!dir)
	return;

    const size_t savepathlen = path->length;
    const size_t savewpathlen = wpath->length;
    struct dirent *de;
    while ((de = readdir(dir))) {
	if (xfnm_match(pattern->value.match.pattern, de->d_name) == 0) {
	    if (wb_mbscat(wpath, de->d_name) != NULL)
		goto next;
	    sb_cat(path, de->d_name);
	    if (pattern->next == NULL) {
		if (flags & WGLB_MARK) {
		    if (is_directory(path->contents))
			wb_wccat(wpath, L'/');
		}
		pl_add(list, xwcsdup(wpath->contents));
	    } else {
		sb_ccat(path, '/');
		wb_wccat(wpath, L'/');
		wglob_search(pattern->next, flags, path, wpath, list);
	    }
next:
	    sb_truncate(path, savepathlen);
	    wb_truncate(wpath, savewpathlen);
	}
    }
    closedir(dir);
}

/* Searches the directory recursively for files that match with the specified
 * pattern. */
void wglob_search_recsearch(
	const struct wglob_pattern *restrict pattern,
	enum wglbflags flags,
	xstrbuf_T *restrict path,
	xwcsbuf_T *restrict wpath,
	plist_T *restrict list,
	struct wglob_dirstack *dirstack)
{
    assert(pattern->type == WGLOB_RECSEARCH);
    assert(pattern->next != NULL);

    if (is_interrupted())
	return;

    /* Step 1: search `path' itself */
    wglob_search(pattern->next, flags, path, wpath, list);

    /* Step 2: search the subdirectories of `path' recursively */
    DIR *dir = opendir((path->length == 0) ? "." : path->contents);
    if (!dir)
	return;

    const size_t savepathlen = path->length;
    const size_t savewpathlen = wpath->length;
    struct dirent *de;
    int (*statfunc)(const char *path, struct stat *st) =
	pattern->value.recsearch.followlink ? stat : lstat;
    while ((de = readdir(dir))) {
	if (pattern->value.recsearch.allowperiod
		? strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0
		: de->d_name[0] == '.')
	    continue;

	struct wglob_dirstack newstack;
	sb_cat(path, de->d_name);
	if (statfunc(path->contents, &newstack.st) >= 0
		&& S_ISDIR(newstack.st.st_mode)
		&& !is_reentry(&newstack.st, dirstack)) {

	    if (wb_mbscat(wpath, de->d_name) != NULL)
		goto next;
	    wb_wccat(wpath, L'/');
	    sb_ccat(path, '/');
	    newstack.prev = dirstack;
	    wglob_search_recsearch(
		    pattern, flags, path, wpath, list, &newstack);
	    wb_truncate(wpath, savewpathlen);
	}
next:
	sb_truncate(path, savepathlen);
    }
    closedir(dir);
}

/* Returns true iff the file designated by `st' is contained in `dirstack'. */
bool is_reentry(const struct stat *st, const struct wglob_dirstack *dirstack)
{
    while (dirstack) {
	if (st->st_dev == dirstack->st.st_dev
		&& st->st_ino == dirstack->st.st_ino)
	    return true;
	dirstack = dirstack->prev;
    }
    return false;
}

int wglob_sortcmp(const void *v1, const void *v2)
{
    return wcscoll(*(const wchar_t *const *) v1, *(const wchar_t *const *) v2);
}


/********** Builtins **********/

static void canonicalize_path_ex(xwcsbuf_T *buf)
    __attribute__((nonnull));
static bool starts_with_root_parent(const wchar_t *path)
    __attribute__((nonnull,pure));
static void print_command_paths(bool all);
static void print_home_directories(void);
static bool print_umask_octal(mode_t mode);
static bool print_umask_symbolic(mode_t mode);
static int set_umask(const wchar_t *newmask)
    __attribute__((nonnull));
static inline mode_t copy_user_mask(mode_t mode)
    __attribute__((const));
static inline mode_t copy_group_mask(mode_t mode)
    __attribute__((const));
static inline mode_t copy_other_mask(mode_t mode)
    __attribute__((const));

/* The "cd" builtin, which accepts the following options:
 *  -L: don't resolve symlinks (default)
 *  -P: resolve symlinks
 *  --default-directory=<dir>: go to <dir> when the operand is missing
 * -L and -P are mutually exclusive: the one specified last is used. */
int cd_builtin(int argc, void **argv)
{
    bool logical = true;
    const wchar_t *newpwd = NULL;

    wchar_t opt;
    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"-LP", cd_options, NULL))) {
	switch (opt) {
	    case L'L':  logical = true;    break;
	    case L'P':  logical = false;   break;
	    case L'd':  newpwd = xoptarg;  break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:  print_usage:
		fprintf(stderr, gt("Usage:  %ls [-L|-P] [dir]\n"), L"cd");
		return Exit_ERROR;
	}
    }

    bool printnewdir = false;
    switch (argc - xoptind) {
	case 0:
	    if (newpwd == NULL) {
		newpwd = getvar(L VAR_HOME);
		if (newpwd == NULL || newpwd[0] == L'\0') {
		    xerror(0, Ngt("$HOME is not set"));
		    return Exit_FAILURE;
		}
	    }
	    break;
	case 1:
	    if (wcscmp(ARGV(xoptind), L"-") == 0) {
		newpwd = getvar(L VAR_OLDPWD);
		if (newpwd == NULL || newpwd[0] == L'\0') {
		    xerror(0, Ngt("$OLDPWD is not set"));
		    return Exit_FAILURE;
		}
		printnewdir = true;
	    } else {
		newpwd = ARGV(xoptind);
	    }
	    break;
	default:
	    goto print_usage;
    }
    return change_directory(newpwd, printnewdir, logical);
}

/* Changes the working directory to `newpwd'.
 * This function implements the almost whole part of the "cd" builtin.
 * $PWD and $OLDPWD are set in this function.
 * If `printnewdir' is true or the new directory is found from $CDPATH, the new
 * directory is printed to stdout.
 * Returns Exit_SUCCESS, Exit_FAILURE or Exit_ERROR. */
int change_directory(const wchar_t *newpwd, bool printnewdir, bool logical)
{
    const wchar_t *oldpwd;
    xwcsbuf_T curpath;
    size_t curpathoffset = 0;

    /* get the current value of $PWD as `oldpwd' */
    oldpwd = getvar(L VAR_PWD);
    if (oldpwd == NULL || oldpwd[0] != L'/') {
	if (oldpwd == newpwd) {
	    xerror(0, Ngt("$PWD has an invalid value"));
	    return Exit_FAILURE;
	}
	/* we have to assure `oldpwd != newpwd' because we're going to
	 * re-assign $PWD */

	char *pwd = xgetcwd();
	if (pwd == NULL) {
	    if (logical) {
		xerror(errno, Ngt("cannot determine the current directory"));
		return Exit_FAILURE;
	    }
	} else {
	    wchar_t *wpwd = realloc_mbstowcs(pwd);
	    if (wpwd != NULL) {
		if (set_variable(L VAR_PWD, wpwd, SCOPE_GLOBAL, false))
		    oldpwd = getvar(L VAR_PWD);
		else
		    logical = false, oldpwd = NULL;
	    } else {
		xerror(EILSEQ, Ngt("unexpected error"));
		return Exit_ERROR;
	    }
	}
    }
    assert(!logical || oldpwd != NULL);
    assert(oldpwd == NULL || oldpwd[0] == L'/');

    wb_init(&curpath);
    
    /* step 3 */
    if (newpwd[0] == L'/') {
	wb_cat(&curpath, newpwd);
	goto step7;
    }

    /* step 4: check if `newpwd' starts with "." or ".." */
    if (newpwd[0] == L'.' && (newpwd[1] == L'\0' || newpwd[1] == L'/' ||
	    (newpwd[1] == L'.' && (newpwd[2] == L'\0' || newpwd[2] == L'/'))))
	goto step6;

    /* step 5: search $CDPATH */
    {
	char *mbsnewpwd = malloc_wcstombs(newpwd);
	if (mbsnewpwd == NULL) {
	    wb_destroy(&curpath);
	    xerror(EILSEQ, Ngt("unexpected error"));
	    return Exit_ERROR;
	}
	char *const *cdpath = get_path_array(PA_CDPATH);
	char *path = which(mbsnewpwd,
		cdpath != NULL ? cdpath : (char *[]) { "", NULL },
		is_directory);
	if (path != NULL) {
	    if (strcmp(mbsnewpwd, path) != 0)
		printnewdir = true;
	    wb_mbscat(&curpath, path);
	    free(mbsnewpwd);
	    free(path);
	    goto step7;
	}
	free(mbsnewpwd);
    }

step6:  /* set the value of `curpath' */
    assert(newpwd[0] != L'/');
    assert(curpath.length == 0);
    if (logical) {
	wb_cat(&curpath, oldpwd);
	if (curpath.length == 0 || curpath.contents[curpath.length - 1] != L'/')
	    wb_wccat(&curpath, L'/');
    }
    wb_cat(&curpath, newpwd);

step7:  /* ensure the value of `curpath' is an absolute path */
    if (!logical)
	goto step10;
    if (curpath.contents[0] != L'/') {
	wchar_t *oldcurpath = wb_towcs(&curpath);
	wb_init(&curpath);
	wb_cat(&curpath, oldpwd);
	if (curpath.length == 0 || curpath.contents[curpath.length - 1] != L'/')
	    wb_wccat(&curpath, L'/');
	wb_catfree(&curpath, oldcurpath);
    }

    /* step 8: canonicalization */
    assert(logical);
    {
	wchar_t *oldcurpath = wb_towcs(&curpath);
	wchar_t *canon = canonicalize_path(oldcurpath);
	free(oldcurpath);
	if (canon == NULL) {
	    xerror(ENOTDIR, "%ls", newpwd);
	    return Exit_FAILURE;
	}
	wb_initwith(&curpath, canon);
    }

    /* step 9: determine `curpathoffset' */
    assert(logical);
    if (oldpwd[wcsspn(oldpwd, L"/")] != L'\0') {
	/* If `oldpwd' contains a character other than '/' and if the head of
	 * `curpath' is equal to `oldpwd', then a relative path to the new
	 * working directory can be obtained by removing the matching head of
	 * `curpath'. */
	wchar_t *s = matchwcsprefix(curpath.contents, oldpwd);
	if (s != NULL && (s[-1] == L'/' || s[0] == L'/')) {
	    if (*s == L'/')
		s++;
	    assert(*s != L'/');
	    curpathoffset = s - curpath.contents;
	}
    }

step10:  /* do chdir */
    assert(curpathoffset <= curpath.length);
    {
	char *mbscurpath = malloc_wcstombs(curpath.contents + curpathoffset);
	if (mbscurpath == NULL) {
	    xerror(EILSEQ, Ngt("unexpected error"));
	    wb_destroy(&curpath);
	    return Exit_ERROR;
	}
	if (chdir(mbscurpath) < 0) {
	    xerror(errno, Ngt("`%s'"), mbscurpath);
	    free(mbscurpath);
	    wb_destroy(&curpath);
	    return Exit_FAILURE;
	}
	free(mbscurpath);
    }

#ifndef NDEBUG
    newpwd = NULL;
    /* `newpwd' must not be used any more because it may be pointing to the
     * current value of $OLDPWD, which is going to be re-assigned to. */
#endif

    /* set $OLDPWD and $PWD */
    if (oldpwd != NULL)
	set_variable(L VAR_OLDPWD, xwcsdup(oldpwd), SCOPE_GLOBAL, false);
    if (logical) {
	if (!posixly_correct)
	    canonicalize_path_ex(&curpath);
	if (printnewdir)
	    printf("%ls\n", curpath.contents);
	set_variable(L VAR_PWD, wb_towcs(&curpath), SCOPE_GLOBAL, false);
    } else {
	wb_destroy(&curpath);

	char *mbsnewpwd = xgetcwd();
	if (mbsnewpwd != NULL) {
	    if (printnewdir)
		printf("%s\n", mbsnewpwd);

	    wchar_t *wnewpwd = realloc_mbstowcs(mbsnewpwd);
	    if (wnewpwd != NULL)
		set_variable(L VAR_PWD, wnewpwd, SCOPE_GLOBAL, false);
	}
    }
    if (!posixly_correct)
	exec_variable_as_commands(L VAR_YASH_AFTER_CD, VAR_YASH_AFTER_CD);

    return Exit_SUCCESS;
}

/* Removes "/.." components at the beginning of the string in the buffer
 * if the root directory and its parent are the same directory. */
void canonicalize_path_ex(xwcsbuf_T *buf)
{
    if (starts_with_root_parent(buf->contents) && is_same_file("/", "/..")) {
	do {
	    wb_remove(buf, 0, 3);
	} while (starts_with_root_parent(buf->contents));
	if (buf->length == 0)
	    wb_wccat(buf, L'/');
    }
}

/* Returns true iff the given path starts with "/..". */
bool starts_with_root_parent(const wchar_t *path)
{
    return path[0] == L'/' && path[1] == L'.' && path[2] == L'.' &&
	(path[3] == L'\0' || path[3] == L'/');
}

#if YASH_ENABLE_HELP
const char *cd_help[] = { Ngt(
"cd - change the working directory\n"
), Ngt(
"\tcd [-L|-P] [dir]\n"
), Ngt(
"The cd built-in changes the working directory to <dir>.\n"
"If <dir> is `-', the working directory is changed to $OLDPWD.\n"
"If <dir> is a relative path that does not start with `.' or `..',\n"
"paths in $CDPATH are searched to find a new directory.\n"
"If the working directory is successfully changed, $PWD is set to the new\n"
"directory and $OLDPWD is set to the previous $PWD.\n"
), (
"\n"
), Ngt(
"When the -L (--logical) option is specified, symbolic links in $PWD are left\n"
"unchanged. When the -P (--physical) option is specified, symbolic links are\n"
"resolved so that $PWD does not contain any symbolic links.\n"
), Ngt(
"-L and -P are mutually exclusive: only the last specified one is effective.\n"
"If neither is specified, -L is the default.\n"
), (
"\n"
), Ngt(
"If <dir> is not specified, the working directory is changed to the directory\n"
"specified by the --default-directory=... option. If this option is not\n"
"specified either, the default is $HOME.\n"
), NULL };
#endif

/* The "pwd" builtin, which accepts the following options:
 *  -L: don't resolve symlinks (default)
 *  -P: resolve symlinks
 * -L and -P are mutually exclusive: the one specified last is used. */
int pwd_builtin(int argc __attribute__((unused)), void **argv)
{
    bool logical = true;
    char *mbspwd;
    wchar_t opt;

    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"-LP", pwd_options, NULL))) {
	switch (opt) {
	    case L'L':  logical = true;   break;
	    case L'P':  logical = false;  break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:  print_usage:
		fprintf(stderr, gt("Usage:  pwd [-L|-P]\n"));
		return Exit_ERROR;
	}
    }

    if (xoptind != argc)
	goto print_usage;

    if (logical) {
	const wchar_t *pwd = getvar(L VAR_PWD);
	if (pwd != NULL && pwd[0] == L'/' && is_normalized_path(pwd)) {
	    mbspwd = malloc_wcstombs(pwd);
	    if (mbspwd != NULL) {
		if (is_same_file(mbspwd, "."))
		    goto print;
		free(mbspwd);
	    }
	}
    }

    mbspwd = xgetcwd();
    if (mbspwd == NULL) {
	xerror(errno, Ngt("cannot determine the current directory"));
	return Exit_FAILURE;
    }
print:
    if (printf("%s\n", mbspwd) < 0)
	xerror(errno, Ngt("cannot print to the standard output"));
    free(mbspwd);
    return (yash_error_message_count == 0) ? Exit_SUCCESS : Exit_FAILURE;
}

#if YASH_ENABLE_HELP
const char *pwd_help[] = { Ngt(
"pwd - print the working directory\n"
), Ngt(
"\tpwd [-L|-P]\n"
), Ngt(
"The pwd built-in prints the absolute pathname of the current working\n"
"directory.\n"
), (
"\n"
), Ngt(
"When the -L (--logical) option is specified, $PWD is printed if it is\n"
"correct. It may contain symbolic links.\n"
"When the -P (--physical) option is specified, the printed pathname does not\n"
"contain any symbolic links.\n"
), Ngt(
"-L and -P are mutually exclusive: only the last specified one is effective.\n"
"If neither is specified, -L is the default.\n"
), NULL };
#endif

/* The "hash" builtin, which accepts the following options:
 *  -a: print all entries
 *  -d: use the directory cache
 *  -r: remove cache entries */
int hash_builtin(int argc, void **argv)
{
    static const struct xoption long_options[] = {
	{ L"all",       OPTARG_NONE, L'a', },
	{ L"directory", OPTARG_NONE, L'd', },
	{ L"remove",    OPTARG_NONE, L'r', },
#if YASH_ENABLE_HELP
	{ L"help",      OPTARG_NONE, L'-', },
#endif
	{ NULL, 0, 0, },
    };

    bool remove = false, all = false, dir = false;

    wchar_t opt;
    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, posixly_correct ? L"r" : L"adr",
		    long_options, NULL))) {
	switch (opt) {
	    case L'a':  all    = true;  break;
	    case L'd':  dir    = true;  break;
	    case L'r':  remove = true;  break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:
		goto print_usage;
	}
    }
    if (all && xoptind != argc)
	goto print_usage;

    if (dir) {
	if (remove) {
	    if (xoptind == argc) {  // forget all
		clear_homedirhash();
	    } else {                // forget the specified
		for (int i = xoptind; i < argc; i++)
		    forget_home_directory(ARGV(i));
	    }
	} else {
	    if (xoptind == argc) {  // print all
		print_home_directories();
	    } else {                // remember the specified
		for (int i = xoptind; i < argc; i++)
		    if (!get_home_directory(ARGV(i), true))
			xerror(0, Ngt("no such user `%ls'"), ARGV(i));
	    }
	}
    } else {
	if (remove) {
	    if (xoptind == argc) {  // forget all
		clear_cmdhash();
	    } else {                // forget the specified
		for (int i = xoptind; i < argc; i++) {
		    char *cmd = malloc_wcstombs(ARGV(i));
		    if (cmd) {
			forget_command_path(cmd);
			free(cmd);
		    }
		}
	    }
	} else {
	    if (xoptind == argc) {  // print all
		print_command_paths(all);
	    } else {                // remember the specified
		for (int i = xoptind; i < argc; i++) {
		    if (wcschr(ARGV(i), L'/')) {
			xerror(0, Ngt("`%ls': a command name must not contain "
				    "`/'"), ARGV(i));
			continue;
		    }

		    char *cmd = malloc_wcstombs(ARGV(i));
		    if (cmd) {
			if (!get_command_path(cmd, true))
			    xerror(0, Ngt("command `%s' was not found "
					"in $PATH"), cmd);
			free(cmd);
		    }
		}
	    }
	}
    }
    return (yash_error_message_count == 0) ? Exit_SUCCESS : Exit_FAILURE;

print_usage:
    fprintf(stderr, gt(posixly_correct
		? Ngt("Usage:  hash [-r] [command...]\n")
		: Ngt("Usage:  hash [-dr] [command/username...]\n"
		      "        hash [-adr]\n")));
    return Exit_ERROR;
}

/* Prints the entries of the command hashtable.
 * Prints an error message to the standard error if failed to print to the
 * standard output. */
void print_command_paths(bool all)
{
    kvpair_T kv;
    size_t index = 0;

    while ((kv = ht_next(&cmdhash, &index)).key) {
	if (all || !get_builtin(kv.key)) {
	    if (printf("%s\n", (char *) kv.value) < 0) {
		xerror(errno, Ngt("cannot print to the standard output"));
		break;
	    }
	}
    }
}

/* Prints the entries of the home directory hashtable.
 * Prints an error message to the standard error if failed to print to the
 * standard output. */
void print_home_directories(void)
{
    kvpair_T kv;
    size_t index = 0;

    while ((kv = ht_next(&homedirhash, &index)).key) {
	const wchar_t *key = kv.key, *value = kv.value;
	if (printf("~%ls=%ls\n", key, value) < 0) {
	    xerror(errno, Ngt("cannot print to the standard output"));
	    break;
	}
    }
}

#if YASH_ENABLE_HELP
const char *hash_help[] = { Ngt(
"hash - remember, forget, or report command locations\n"
), Ngt(
"\thash command...\n"
"\thash -r [command...]\n"
"\thash [-a]\n"
"\thash -d user...\n"
"\thash -d -r [user...]\n"
"\thash -d\n"
), Ngt(
"The first form immediately performs command path search and caches\n"
"<command>s' full paths.\n"
), Ngt(
"The second form, using the -r (--remove) option, removes the paths of\n"
"<command>s (or all the paths if none specified) from the cache. Note that\n"
"assignment to $PATH also removes all paths from the cache.\n"
), Ngt(
"The third form prints the currently cached paths. Without the -a (--all)\n"
"option, paths for built-in commands are not printed.\n"
), Ngt(
"With the -d (--directory) option, this command does the same things to the\n"
"home directory cache, rather than the command path cache.\n"
), Ngt(
"In the POSIXly correct mode, the -r option is the only available option.\n"
), NULL };
#endif

/* The "umask" builtin, which accepts the following option:
 *  -S: symbolic output */
int umask_builtin(int argc, void **argv)
{
    static const struct xoption long_options[] = {
	{ L"symbolic", OPTARG_NONE, L'S', },
#if YASH_ENABLE_HELP
	{ L"help",     OPTARG_NONE, L'-', },
#endif
	{ NULL, 0, 0, },
    };

    wchar_t opt;
    bool symbolic = false;

    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"S", long_options, NULL))) {
	switch (opt) {
	    case L'S':
		symbolic = true;
		break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:
		goto print_usage;
	}
    }

    if (xoptind == argc) {
	mode_t mode = umask(0);
	umask(mode);
	if (symbolic)
	    return print_umask_symbolic(mode);
	else
	    return print_umask_octal(mode);
    } else if (xoptind + 1 == argc) {
	return set_umask(ARGV(xoptind));
    } else {
	goto print_usage;
    }

print_usage:
    fprintf(stderr, gt("Usage:  umask [-S] [mask]\n"));
    return Exit_ERROR;
}

bool print_umask_octal(mode_t mode)
{
    if (printf("0%.3jo\n", (uintmax_t) mode) >= 0) {
	return Exit_SUCCESS;
    } else {
	xerror(errno, Ngt("cannot print to the standard output"));
	return Exit_FAILURE;
    }
}

bool print_umask_symbolic(mode_t mode)
{
    clearerr(stdout);

    putchar('u');
    putchar('=');
    if (!(mode & S_IRUSR)) putchar('r');
    if (!(mode & S_IWUSR)) putchar('w');
    if (!(mode & S_IXUSR)) putchar('x');
    putchar(',');
    putchar('g');
    putchar('=');
    if (!(mode & S_IRGRP)) putchar('r');
    if (!(mode & S_IWGRP)) putchar('w');
    if (!(mode & S_IXGRP)) putchar('x');
    putchar(',');
    putchar('o');
    putchar('=');
    if (!(mode & S_IROTH)) putchar('r');
    if (!(mode & S_IWOTH)) putchar('w');
    if (!(mode & S_IXOTH)) putchar('x');
    putchar('\n');

    if (!ferror(stdout)) {
	return Exit_SUCCESS;
    } else {
	xerror(errno, Ngt("cannot print to the standard output"));
	return Exit_FAILURE;
    }
}

int set_umask(const wchar_t *maskstr)
{
    if (iswdigit(maskstr[0])) {
	/* parse as an octal number */
	uintmax_t mask;
	wchar_t *end;

	errno = 0;
	mask = wcstoumax(maskstr, &end, 8);
	if (errno || *end) {
	    xerror(0, Ngt("`%ls' is not a valid mask specification"), maskstr);
	    return Exit_ERROR;
	}
	umask((mode_t) (mask & (S_IRWXU | S_IRWXG | S_IRWXO)));
	return Exit_SUCCESS;
    }

    /* otherwise parse as a symbolic mode specification */
    mode_t origmask = ~umask(0);
    mode_t newmask = origmask;
    const wchar_t *savemaskstr = maskstr;

    do {
	mode_t who, perm;
	char op;  /* '+', '-' or '=' */

	/* parse 'who' */
	who = 0;
	for (;; maskstr++) switch (*maskstr) {
	    case L'u':  who |= S_IRWXU;  break;
	    case L'g':  who |= S_IRWXG;  break;
	    case L'o':  who |= S_IRWXO;  break;
	    case L'a':  who = S_IRWXU | S_IRWXG | S_IRWXO;  break;
	    default:    goto who_end;
	}
who_end:
	if (who == 0)
	    who = S_IRWXU | S_IRWXG | S_IRWXO;

	/* parse 'op' */
op_start:
	op = *maskstr++;
	switch (op) {
	    case L'+':  case L'-':  case L'=':
		break;
	    default:
		goto err;
	}

	/* parse 'perm' */
	switch (*maskstr) {
	    case L'u':  perm = copy_user_mask(origmask);  maskstr++;  break;
	    case L'g':  perm = copy_group_mask(origmask); maskstr++;  break;
	    case L'o':  perm = copy_other_mask(origmask); maskstr++;  break;
	    default:
		perm = 0;
		for (;; maskstr++) switch (*maskstr) {
		    case L'r':  perm |= S_IRUSR | S_IRGRP | S_IROTH;  break;
		    case L'w':  perm |= S_IWUSR | S_IWGRP | S_IWOTH;  break;
		    case L'X':
			if (!(origmask & (S_IXUSR | S_IXGRP | S_IXOTH)))
			    break;
			/* falls thru! */
		    case L'x':  perm |= S_IXUSR | S_IXGRP | S_IXOTH;  break;
		    case L's':  perm |= S_ISUID | S_ISGID;            break;
		    default:    goto perm_end;
		}
perm_end:
		break;
	}

	/* set newmask */
	switch (op) {
	    case L'+':  newmask |= who & perm;                      break;
	    case L'-':  newmask &= ~(who & perm);                   break;
	    case L'=':  newmask = (~who & newmask) | (who & perm);  break;
	    default:    assert(false);
	}

	switch (*maskstr) {
	    case L'\0':  goto parse_end;
	    case L',':   break;
	    default:     goto op_start;
	}
	maskstr++;
    } while (1);
parse_end:
    umask(~newmask);
    return Exit_SUCCESS;

err:
    umask(~origmask);
    xerror(0, Ngt("`%ls' is not a valid mask specification"), savemaskstr);
    return Exit_ERROR;
}

mode_t copy_user_mask(mode_t mode)
{
    return ((mode & S_IRUSR) ? (S_IRUSR | S_IRGRP | S_IROTH) : 0)
         | ((mode & S_IWUSR) ? (S_IWUSR | S_IWGRP | S_IWOTH) : 0)
	 | ((mode & S_IXUSR) ? (S_IXUSR | S_IXGRP | S_IXOTH) : 0);
}

mode_t copy_group_mask(mode_t mode)
{
    return ((mode & S_IRGRP) ? (S_IRUSR | S_IRGRP | S_IROTH) : 0)
         | ((mode & S_IWGRP) ? (S_IWUSR | S_IWGRP | S_IWOTH) : 0)
	 | ((mode & S_IXGRP) ? (S_IXUSR | S_IXGRP | S_IXOTH) : 0);
}

mode_t copy_other_mask(mode_t mode)
{
    return ((mode & S_IROTH) ? (S_IRUSR | S_IRGRP | S_IROTH) : 0)
         | ((mode & S_IWOTH) ? (S_IWUSR | S_IWGRP | S_IWOTH) : 0)
	 | ((mode & S_IXOTH) ? (S_IXUSR | S_IXGRP | S_IXOTH) : 0);
}

#if YASH_ENABLE_HELP
const char *umask_help[] = { Ngt(
"umask - print or set the file creation mask\n"
), Ngt(
"\tumask mode\n"
"\tumask [-S]\n"
), Ngt(
"The umask built-in sets the file mode creation mask of the shell to <mode>.\n"
"The mask will be inherited by commands invoked by the shell.\n"
"If <mode> is not specified, the current setting of the mask is printed.\n"
"The -S (--symbolic) option makes a symbolic output.\n"
), NULL };
#endif


/* vim: set ts=8 sts=4 sw=4 noet tw=80: */
