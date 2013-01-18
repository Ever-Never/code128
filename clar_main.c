#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

/* required for sandboxing */
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#	include <windows.h>
#	include <io.h>
#	include <shellapi.h>
#	include <direct.h>

#	define _MAIN_CC __cdecl

#	define stat(path, st) _stat(path, st)
#	define mkdir(path, mode) _mkdir(path)
#	define chdir(path) _chdir(path)
#	define access(path, mode) _access(path, mode)
#	define strdup(str) _strdup(str)
#	define strcasecmp(a,b) _stricmp(a,b)

#	ifndef __MINGW32__
#		pragma comment(lib, "shell32")
#		define strncpy(to, from, to_size) strncpy_s(to, to_size, from, _TRUNCATE)
#		define W_OK 02
#		define S_ISDIR(x) ((x & _S_IFDIR) != 0)
#		define snprint_eq(buf,sz,fmt,a,b) _snprintf_s(buf,sz,_TRUNCATE,fmt,a,b)
#	else
#		define snprint_eq snprintf
#	endif
	typedef struct _stat STAT_T;
#else
#	include <sys/wait.h> /* waitpid(2) */
#	include <unistd.h>
#	define _MAIN_CC
#	define snprint_eq snprintf
	typedef struct stat STAT_T;
#endif

#include "clar.h"

static void fs_rm(const char *_source);
static void fs_copy(const char *_source, const char *dest);

static const char *
fixture_path(const char *base, const char *fixture_name);

struct clar_error {
	const char *test;
	int test_number;
	const char *suite;
	const char *file;
	int line_number;
	const char *error_msg;
	char *description;

	struct clar_error *next;
};

static struct {
	const char *active_test;
	const char *active_suite;

	int suite_errors;
	int total_errors;

	int test_count;

	int report_errors_only;
	int exit_on_error;

	struct clar_error *errors;
	struct clar_error *last_error;

	void (*local_cleanup)(void *);
	void *local_cleanup_payload;

	jmp_buf trampoline;
	int trampoline_enabled;
} _clar;

struct clar_func {
	const char *name;
	void (*ptr)(void);
};

struct clar_suite {
	int index;
	const char *name;
	struct clar_func initialize;
	struct clar_func cleanup;
	const char **categories;
	const struct clar_func *tests;
	size_t test_count;
};

/* From clar_print_*.c */
static void clar_print_init(int test_count, int suite_count, const char *suite_names);
static void clar_print_shutdown(int test_count, int suite_count, int error_count);
static void clar_print_error(int num, const struct clar_error *error);
static void clar_print_ontest(const char *test_name, int test_number, int failed);
static void clar_print_onsuite(const char *suite_name, int suite_index);
static void clar_print_onabort(const char *msg, ...);

/* From clar_sandbox.c */
static void clar_unsandbox(void);
static int clar_sandbox(void);

/* From clar_categorize.c */
static int clar_category_is_suite_enabled(const struct clar_suite *);
static void clar_category_enable(const char *category);
static void clar_category_enable_all(size_t, const struct clar_suite *);
static void clar_category_print_enabled(const char *prefix);

/* Event callback overrides */
#define clar_on_init() /* nop */
#define clar_on_shutdown() /* nop */
#define clar_on_test() /* nop */
#define clar_on_suite() /* nop */

/* Autogenerated test data by clar */
static const struct clar_func _clar_cb_encode[] = {
    {"all_code128a_points", &test_encode__all_code128a_points}
};



static const struct clar_suite _clar_suites[] = {
    {
        0,
        "encode",
        {NULL, NULL},
        {NULL, NULL},
        NULL,
        _clar_cb_encode, 1
    }
};

static size_t _clar_suite_count = 1;
static size_t _clar_callback_count = 1;

/* Core test functions */
static void
clar_report_errors(void)
{
	int i = 1;
	struct clar_error *error, *next;

	error = _clar.errors;
	while (error != NULL) {
		next = error->next;
		clar_print_error(i++, error);
		free(error->description);
		free(error);
		error = next;
	}

	_clar.errors = _clar.last_error = NULL;
}

static void
clar_run_test(
	const struct clar_func *test,
	const struct clar_func *initialize,
	const struct clar_func *cleanup)
{
	int error_st = _clar.suite_errors;

	clar_on_test();
	_clar.trampoline_enabled = 1;

	if (setjmp(_clar.trampoline) == 0) {
		if (initialize->ptr != NULL)
			initialize->ptr();

		test->ptr();
	}

	_clar.trampoline_enabled = 0;

	if (_clar.local_cleanup != NULL)
		_clar.local_cleanup(_clar.local_cleanup_payload);

	if (cleanup->ptr != NULL)
		cleanup->ptr();

	_clar.test_count++;

	/* remove any local-set cleanup methods */
	_clar.local_cleanup = NULL;
	_clar.local_cleanup_payload = NULL;

	if (_clar.report_errors_only)
		clar_report_errors();
	else
		clar_print_ontest(
			test->name,
			_clar.test_count,
			(_clar.suite_errors > error_st)
			);
}

static void
clar_run_suite(const struct clar_suite *suite)
{
	const struct clar_func *test = suite->tests;
	size_t i;

	if (!clar_category_is_suite_enabled(suite))
		return;

	if (_clar.exit_on_error && _clar.total_errors)
		return;

	if (!_clar.report_errors_only)
		clar_print_onsuite(suite->name, suite->index);
	clar_on_suite();

	_clar.active_suite = suite->name;
	_clar.suite_errors = 0;

	for (i = 0; i < suite->test_count; ++i) {
		_clar.active_test = test[i].name;
		clar_run_test(&test[i], &suite->initialize, &suite->cleanup);

		if (_clar.exit_on_error && _clar.total_errors)
			return;
	}
}

#if 0 /* temporarily disabled */
static void
clar_run_single(const struct clar_func *test,
	const struct clar_suite *suite)
{
	_clar.suite_errors = 0;
	_clar.active_suite = suite->name;
	_clar.active_test = test->name;

	clar_run_test(test, &suite->initialize, &suite->cleanup);
}
#endif

static void
clar_usage(const char *arg)
{
	printf("Usage: %s [options]\n\n", arg);
	printf("Options:\n");
	printf("  -sXX\t\tRun only the suite number or name XX\n");
	printf("  -i<name>\tInclude category <name> tests\n");
	printf("  -q  \t\tOnly report tests that had an error\n");
	printf("  -Q  \t\tQuit as soon as a test fails\n");
	printf("  -l  \t\tPrint suite names and category names\n");
	exit(-1);
}

static void
clar_parse_args(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; ++i) {
		char *argument = argv[i];

		if (argument[0] != '-')
			clar_usage(argv[0]);

		switch (argument[1]) {
		case 's': { /* given suite number, name, or prefix */
			int num = 0, offset = (argument[2] == '=') ? 3 : 2;
			int len = 0, is_num = 1, has_colon = 0, j;

			for (argument += offset; *argument; ++argument) {
				len++;
				if (*argument >= '0' && *argument <= '9')
					num = (num * 10) + (*argument - '0');
				else {
					is_num = 0;
					if (*argument == ':')
						has_colon = 1;
				}
			}

			argument = argv[i] + offset;

			if (!len)
				clar_usage(argv[0]);
			else if (is_num) {
				if ((size_t)num >= _clar_suite_count) {
					clar_print_onabort("Suite number %d does not exist.\n", num);
					exit(-1);
				}
				clar_run_suite(&_clar_suites[num]);
			}
			else if (!has_colon || argument[-1] == ':') {
				for (j = 0; j < (int)_clar_suite_count; ++j)
					if (strncmp(argument, _clar_suites[j].name, len) == 0)
						clar_run_suite(&_clar_suites[j]);
			}
			else {
				for (j = 0; j < (int)_clar_suite_count; ++j)
					if (strcmp(argument, _clar_suites[j].name) == 0) {
						clar_run_suite(&_clar_suites[j]);
						break;
					}
			}
			if (_clar.active_suite == NULL) {
				clar_print_onabort("No suite matching '%s' found.\n", argument);
				exit(-1);
			}
			break;
		}

		case 'q':
			_clar.report_errors_only = 1;
			break;

		case 'Q':
			_clar.exit_on_error = 1;
			break;

		case 'i': {
			int offset = (argument[2] == '=') ? 3 : 2;
			if (strcasecmp("all", argument + offset) == 0)
				clar_category_enable_all(_clar_suite_count, _clar_suites);
			else
				clar_category_enable(argument + offset);
			break;
		}

		case 'l': {
			size_t j;
			printf("Test suites (use -s<name> to run just one):\n");
			for (j = 0; j < _clar_suite_count; ++j)
				printf(" %3d: %s\n", (int)j, _clar_suites[j].name);

			printf("\nCategories (use -i<category> to include):\n");
			clar_category_enable_all(_clar_suite_count, _clar_suites);
			clar_category_print_enabled(" - ");

			exit(0);
		}

		default:
			clar_usage(argv[0]);
		}
	}
}

static int
clar_test(int argc, char **argv)
{
	clar_print_init(
		(int)_clar_callback_count,
		(int)_clar_suite_count,
		""
	);

	if (clar_sandbox() < 0) {
		clar_print_onabort("Failed to sandbox the test runner.\n");
		exit(-1);
	}

	clar_on_init();

	if (argc > 1)
		clar_parse_args(argc, argv);

	if (_clar.active_suite == NULL) {
		size_t i;
		for (i = 0; i < _clar_suite_count; ++i)
			clar_run_suite(&_clar_suites[i]);
	}

	clar_print_shutdown(
		_clar.test_count,
		(int)_clar_suite_count,
		_clar.total_errors
	);

	clar_on_shutdown();

	clar_unsandbox();
	return _clar.total_errors;
}

void
clar__assert(
	int condition,
	const char *file,
	int line,
	const char *error_msg,
	const char *description,
	int should_abort)
{
	struct clar_error *error;

	if (condition)
		return;

	error = calloc(1, sizeof(struct clar_error));

	if (_clar.errors == NULL)
		_clar.errors = error;

	if (_clar.last_error != NULL)
		_clar.last_error->next = error;

	_clar.last_error = error;

	error->test = _clar.active_test;
	error->test_number = _clar.test_count;
	error->suite = _clar.active_suite;
	error->file = file;
	error->line_number = line;
	error->error_msg = error_msg;

	if (description != NULL)
		error->description = strdup(description);

	_clar.suite_errors++;
	_clar.total_errors++;

	if (should_abort) {
		if (!_clar.trampoline_enabled) {
			clar_print_onabort(
				"Fatal error: a cleanup method raised an exception.");
			clar_report_errors();
			exit(-1);
		}

		longjmp(_clar.trampoline, -1);
	}
}

void clar__assert_equal_s(
	const char *s1,
	const char *s2,
	const char *file,
	int line,
	const char *err,
	int should_abort)
{
	int match = (s1 == NULL || s2 == NULL) ? (s1 == s2) : (strcmp(s1, s2) == 0);

	if (!match) {
		char buf[4096];
		snprint_eq(buf, 4096, "'%s' != '%s'", s1, s2);
		clar__assert(0, file, line, err, buf, should_abort);
	}
}

void clar__assert_equal_i(
	int i1,
	int i2,
	const char *file,
	int line,
	const char *err,
	int should_abort)
{
	if (i1 != i2) {
		char buf[128];
		snprint_eq(buf, 128, "%d != %d", i1, i2);
		clar__assert(0, file, line, err, buf, should_abort);
	}
}

void cl_set_cleanup(void (*cleanup)(void *), void *opaque)
{
	_clar.local_cleanup = cleanup;
	_clar.local_cleanup_payload = opaque;
}

static char _clar_path[4096];

static int
is_valid_tmp_path(const char *path)
{
	STAT_T st;

	if (stat(path, &st) != 0)
		return 0;

	if (!S_ISDIR(st.st_mode))
		return 0;

	return (access(path, W_OK) == 0);
}

static int
find_tmp_path(char *buffer, size_t length)
{
#ifndef _WIN32
	static const size_t var_count = 4;
	static const char *env_vars[] = {
		"TMPDIR", "TMP", "TEMP", "USERPROFILE"
 	};

 	size_t i;

	for (i = 0; i < var_count; ++i) {
		const char *env = getenv(env_vars[i]);
		if (!env)
			continue;

		if (is_valid_tmp_path(env)) {
			strncpy(buffer, env, length);
			return 0;
		}
	}

	/* If the environment doesn't say anything, try to use /tmp */
	if (is_valid_tmp_path("/tmp")) {
		strncpy(buffer, "/tmp", length);
		return 0;
	}

#else
	if (GetTempPath((DWORD)length, buffer))
		return 0;
#endif

	/* This system doesn't like us, try to use the current directory */
	if (is_valid_tmp_path(".")) {
		strncpy(buffer, ".", length);
		return 0;
	}

	return -1;
}

static void clar_unsandbox(void)
{
	if (_clar_path[0] == '\0')
		return;

#ifdef _WIN32
	chdir("..");
#endif

	fs_rm(_clar_path);
}

static int build_sandbox_path(void)
{
	const char path_tail[] = "clar_tmp_XXXXXX";
	size_t len;

	if (find_tmp_path(_clar_path, sizeof(_clar_path)) < 0)
		return -1;

	len = strlen(_clar_path);

#ifdef _WIN32
	{ /* normalize path to POSIX forward slashes */
		size_t i;
		for (i = 0; i < len; ++i) {
			if (_clar_path[i] == '\\')
				_clar_path[i] = '/';
		}
	}
#endif

	if (_clar_path[len - 1] != '/') {
		_clar_path[len++] = '/';
	}

	strncpy(_clar_path + len, path_tail, sizeof(_clar_path) - len);

#if defined(__MINGW32__)
	if (_mktemp(_clar_path) == NULL)
		return -1;

	if (mkdir(_clar_path, 0700) != 0)
		return -1;
#elif defined(_WIN32)
	if (_mktemp_s(_clar_path, sizeof(_clar_path)) != 0)
		return -1;

	if (mkdir(_clar_path, 0700) != 0)
		return -1;
#else
	if (mkdtemp(_clar_path) == NULL)
		return -1;
#endif

	return 0;
}

static int clar_sandbox(void)
{
	if (_clar_path[0] == '\0' && build_sandbox_path() < 0)
		return -1;

	if (chdir(_clar_path) != 0)
		return -1;

	return 0;
}


static const char *
fixture_path(const char *base, const char *fixture_name)
{
	static char _path[4096];
	size_t root_len;

	root_len = strlen(base);
	strncpy(_path, base, sizeof(_path));

	if (_path[root_len - 1] != '/')
		_path[root_len++] = '/';

	if (fixture_name[0] == '/')
		fixture_name++;

	strncpy(_path + root_len,
		fixture_name,
		sizeof(_path) - root_len);

	return _path;
}

#ifdef CLAR_FIXTURE_PATH
const char *cl_fixture(const char *fixture_name)
{
	return fixture_path(CLAR_FIXTURE_PATH, fixture_name);
}

void cl_fixture_sandbox(const char *fixture_name)
{
	fs_copy(cl_fixture(fixture_name), _clar_path);
}

void cl_fixture_cleanup(const char *fixture_name)
{
	fs_rm(fixture_path(_clar_path, fixture_name));
}
#endif

#ifdef _WIN32

#define RM_RETRY_COUNT	5
#define RM_RETRY_DELAY	10

#ifdef __MINGW32__

/* These security-enhanced functions are not available
 * in MinGW, so just use the vanilla ones */
#define wcscpy_s(a, b, c) wcscpy((a), (c))
#define wcscat_s(a, b, c) wcscat((a), (c))

#endif /* __MINGW32__ */

static int 
fs__dotordotdot(WCHAR *_tocheck)
{
	return _tocheck[0] == '.' &&
		(_tocheck[1] == '\0' ||
		 (_tocheck[1] == '.' && _tocheck[2] == '\0'));
}

static int
fs_rmdir_rmdir(WCHAR *_wpath)
{
	unsigned retries = 1;

	while (!RemoveDirectoryW(_wpath)) {
		/* Only retry when we have retries remaining, and the
		 * error was ERROR_DIR_NOT_EMPTY. */
		if (retries++ > RM_RETRY_COUNT ||
			ERROR_DIR_NOT_EMPTY != GetLastError())
			return -1;

		/* Give whatever has a handle to a child item some time
		 * to release it before trying again */
		Sleep(RM_RETRY_DELAY * retries * retries);
	}

	return 0;
}

static void
fs_rmdir_helper(WCHAR *_wsource)
{
	WCHAR buffer[MAX_PATH];
	HANDLE find_handle;
	WIN32_FIND_DATAW find_data;
	int buffer_prefix_len;

	/* Set up the buffer and capture the length */
	wcscpy_s(buffer, MAX_PATH, _wsource);
	wcscat_s(buffer, MAX_PATH, L"\\");
	buffer_prefix_len = wcslen(buffer);

	/* FindFirstFile needs a wildcard to match multiple items */
	wcscat_s(buffer, MAX_PATH, L"*");
	find_handle = FindFirstFileW(buffer, &find_data);
	cl_assert(INVALID_HANDLE_VALUE != find_handle);

	do {
		/* FindFirstFile/FindNextFile gives back . and ..
		 * entries at the beginning */
		if (fs__dotordotdot(find_data.cFileName))
			continue;

		wcscpy_s(buffer + buffer_prefix_len, MAX_PATH - buffer_prefix_len, find_data.cFileName);

		if (FILE_ATTRIBUTE_DIRECTORY & find_data.dwFileAttributes)
			fs_rmdir_helper(buffer);
		else {
			/* If set, the +R bit must be cleared before deleting */
			if (FILE_ATTRIBUTE_READONLY & find_data.dwFileAttributes)
				cl_assert(SetFileAttributesW(buffer, find_data.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY));

			cl_assert(DeleteFileW(buffer));
		}
	}
	while (FindNextFileW(find_handle, &find_data));

	/* Ensure that we successfully completed the enumeration */
	cl_assert(ERROR_NO_MORE_FILES == GetLastError());

	/* Close the find handle */
	FindClose(find_handle);

	/* Now that the directory is empty, remove it */
	cl_assert(0 == fs_rmdir_rmdir(_wsource));
}

static int
fs_rm_wait(WCHAR *_wpath)
{
	unsigned retries = 1;
	DWORD last_error;

	do {
		if (INVALID_FILE_ATTRIBUTES == GetFileAttributesW(_wpath))
			last_error = GetLastError();
		else
			last_error = ERROR_SUCCESS;

		/* Is the item gone? */
		if (ERROR_FILE_NOT_FOUND == last_error ||
			ERROR_PATH_NOT_FOUND == last_error)
			return 0;

		Sleep(RM_RETRY_DELAY * retries * retries);	
	}
	while (retries++ <= RM_RETRY_COUNT);

	return -1;
}

static void
fs_rm(const char *_source)
{
	WCHAR wsource[MAX_PATH];
	DWORD attrs;

	/* The input path is UTF-8. Convert it to wide characters
	 * for use with the Windows API */
	cl_assert(MultiByteToWideChar(CP_UTF8,
				MB_ERR_INVALID_CHARS,
				_source,
				-1, /* Indicates NULL termination */
				wsource,
				MAX_PATH));

	/* Does the item exist? If not, we have no work to do */
	attrs = GetFileAttributesW(wsource);

	if (INVALID_FILE_ATTRIBUTES == attrs)
		return;

	if (FILE_ATTRIBUTE_DIRECTORY & attrs)
		fs_rmdir_helper(wsource);
	else {
		/* The item is a file. Strip the +R bit */
		if (FILE_ATTRIBUTE_READONLY & attrs)
			cl_assert(SetFileAttributesW(wsource, attrs & ~FILE_ATTRIBUTE_READONLY));

		cl_assert(DeleteFileW(wsource));
	}

	/* Wait for the DeleteFile or RemoveDirectory call to complete */
	cl_assert(0 == fs_rm_wait(wsource));
}

static void
fs_copydir_helper(WCHAR *_wsource, WCHAR *_wdest)
{
	WCHAR buf_source[MAX_PATH], buf_dest[MAX_PATH];
	HANDLE find_handle;
	WIN32_FIND_DATAW find_data;
	int buf_source_prefix_len, buf_dest_prefix_len;

	wcscpy_s(buf_source, MAX_PATH, _wsource);
	wcscat_s(buf_source, MAX_PATH, L"\\");
	buf_source_prefix_len = wcslen(buf_source);

	wcscpy_s(buf_dest, MAX_PATH, _wdest);
	wcscat_s(buf_dest, MAX_PATH, L"\\");
	buf_dest_prefix_len = wcslen(buf_dest);

	/* Get an enumerator for the items in the source. */
	wcscat_s(buf_source, MAX_PATH, L"*");
	find_handle = FindFirstFileW(buf_source, &find_data);
	cl_assert(INVALID_HANDLE_VALUE != find_handle);

	/* Create the target directory. */
	cl_assert(CreateDirectoryW(_wdest, NULL));

	do {
		/* FindFirstFile/FindNextFile gives back . and ..
		 * entries at the beginning */
		if (fs__dotordotdot(find_data.cFileName))
			continue;

		wcscpy_s(buf_source + buf_source_prefix_len, MAX_PATH - buf_source_prefix_len, find_data.cFileName);
		wcscpy_s(buf_dest + buf_dest_prefix_len, MAX_PATH - buf_dest_prefix_len, find_data.cFileName);

		if (FILE_ATTRIBUTE_DIRECTORY & find_data.dwFileAttributes)
			fs_copydir_helper(buf_source, buf_dest);
		else
			cl_assert(CopyFileW(buf_source, buf_dest, TRUE));
	}
	while (FindNextFileW(find_handle, &find_data));

	/* Ensure that we successfully completed the enumeration */
	cl_assert(ERROR_NO_MORE_FILES == GetLastError());

	/* Close the find handle */
	FindClose(find_handle);
}

static void
fs_copy(const char *_source, const char *_dest)
{
	WCHAR wsource[MAX_PATH], wdest[MAX_PATH];
	DWORD source_attrs, dest_attrs;
	HANDLE find_handle;
	WIN32_FIND_DATAW find_data;
	
	/* The input paths are UTF-8. Convert them to wide characters
	 * for use with the Windows API. */
	cl_assert(MultiByteToWideChar(CP_UTF8,
				MB_ERR_INVALID_CHARS,
				_source,
				-1,
				wsource,
				MAX_PATH));

	cl_assert(MultiByteToWideChar(CP_UTF8,
				MB_ERR_INVALID_CHARS,
				_dest,
				-1,
				wdest,
				MAX_PATH));

	/* Check the source for existence */
	source_attrs = GetFileAttributesW(wsource);
	cl_assert(INVALID_FILE_ATTRIBUTES != source_attrs);

	/* Check the target for existence */
	dest_attrs = GetFileAttributesW(wdest);

	if (INVALID_FILE_ATTRIBUTES != dest_attrs) {
		/* Target exists; append last path part of source to target.
		 * Use FindFirstFile to parse the path */
		find_handle = FindFirstFileW(wsource, &find_data);
		cl_assert(INVALID_HANDLE_VALUE != find_handle);
		wcscat_s(wdest, MAX_PATH, L"\\");
		wcscat_s(wdest, MAX_PATH, find_data.cFileName);
		FindClose(find_handle);

		/* Check the new target for existence */
		cl_assert(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(wdest));
	}

	if (FILE_ATTRIBUTE_DIRECTORY & source_attrs)
		fs_copydir_helper(wsource, wdest);
	else
		cl_assert(CopyFileW(wsource, wdest, TRUE));
}

void
cl_fs_cleanup(void)
{
	fs_rm(fixture_path(_clar_path, "*"));
}

#else
static int
shell_out(char * const argv[])
{
	int status;
	pid_t pid;

	pid = fork();

	if (pid < 0) {
		fprintf(stderr,
			"System error: `fork()` call failed.\n");
		exit(-1);
	}

	if (pid == 0) {
		execv(argv[0], argv);
	}

	waitpid(pid, &status, 0);
	return WEXITSTATUS(status);
}

static void
fs_copy(const char *_source, const char *dest)
{
	char *argv[5];
	char *source;
	size_t source_len;

	source = strdup(_source);
	source_len = strlen(source);

	if (source[source_len - 1] == '/')
		source[source_len - 1] = 0;

	argv[0] = "/bin/cp";
	argv[1] = "-R";
	argv[2] = source;
	argv[3] = (char *)dest;
	argv[4] = NULL;

	cl_must_pass_(
		shell_out(argv),
		"Failed to copy test fixtures to sandbox"
	);

	free(source);
}

static void
fs_rm(const char *source)
{
	char *argv[4];

	argv[0] = "/bin/rm";
	argv[1] = "-Rf";
	argv[2] = (char *)source;
	argv[3] = NULL;

	cl_must_pass_(
		shell_out(argv),
		"Failed to cleanup the sandbox"
	);
}

void
cl_fs_cleanup(void)
{
	clar_unsandbox();
	clar_sandbox();
}
#endif

#define CLAR_CATEGORY_DEFAULT "default"

typedef struct {
	const char **names;
	int count;
	int alloc;
} clar_category_list;

static clar_category_list _clar_categorize_enabled;

static int clar_category_in_list(clar_category_list *list, const char *cat)
{
	int i;
	for (i = 0; i < list->count; ++i)
		if (strcasecmp(cat, list->names[i]) == 0)
			return 1;
	return 0;
}

static void clar_category_add_to_list(clar_category_list *list, const char *cat)
{
	if (clar_category_in_list(list, cat))
		return;

	if (list->count >= list->alloc) {
		list->alloc += 10;
		list->names = (const char **)realloc(
			(void *)list->names, list->alloc * sizeof(const char *));
	}

	list->names[list->count++] = cat;
}

static void clar_category_enable(const char *category)
{
	clar_category_add_to_list(&_clar_categorize_enabled, category);
}

static void clar_category_enable_all(size_t suite_count, const struct clar_suite *suites)
{
	size_t i;
	const char **cat;

	clar_category_enable(CLAR_CATEGORY_DEFAULT);

	for (i = 0; i < suite_count; i++)
		for (cat = suites[i].categories; cat && *cat; cat++)
			clar_category_enable(*cat);
}

static int _MAIN_CC clar_category_cmp(const void *a, const void *b)
{
	return - strcasecmp(a,b);
}

static void clar_category_print_enabled(const char *prefix)
{
	int i;

	qsort((void *)_clar_categorize_enabled.names,
		_clar_categorize_enabled.count,
		sizeof(const char *), clar_category_cmp);

	for (i = 0; i < _clar_categorize_enabled.count; ++i)
		printf("%s%s\n", prefix, _clar_categorize_enabled.names[i]);
}

static int clar_category_is_suite_enabled(const struct clar_suite *suite)
{
	const char **scan;

	if (!_clar_categorize_enabled.count)
		clar_category_enable(CLAR_CATEGORY_DEFAULT);

	if (!suite->categories)
		return clar_category_in_list(
			&_clar_categorize_enabled, CLAR_CATEGORY_DEFAULT);

	for (scan = suite->categories; *scan != NULL; scan++)
		if (clar_category_in_list(&_clar_categorize_enabled, *scan))
			return 1;

	return 0;
}


static void clar_print_init(int test_count, int suite_count, const char *suite_names)
{
	(void)test_count;
	printf("Loaded %d suites: %s\n", (int)suite_count, suite_names);
	printf("Started\n");
}

static void clar_print_shutdown(int test_count, int suite_count, int error_count)
{
	(void)test_count;
	(void)suite_count;
	(void)error_count;

	printf("\n\n");
	clar_report_errors();
}

static void clar_print_error(int num, const struct clar_error *error)
{
	printf("  %d) Failure:\n", num);

	printf("%s::%s (%s) [%s:%d] [-t%d]\n",
		error->suite,
		error->test,
		"no description",
		error->file,
		error->line_number,
		error->test_number);

	printf("  %s\n", error->error_msg);

	if (error->description != NULL)
		printf("  %s\n", error->description);

	printf("\n");
	fflush(stdout);
}

static void clar_print_ontest(const char *test_name, int test_number, int failed)
{
	(void)test_name;
	(void)test_number;
	printf("%c", failed ? 'F' : '.');
}

static void clar_print_onsuite(const char *suite_name, int suite_index)
{
	/* noop */
	(void)suite_index;
	(void)suite_name;
}

static void clar_print_onabort(const char *msg, ...)
{
	va_list argp;
	va_start(argp, msg);
	vfprintf(stderr, msg, argp);
	va_end(argp);
}


int _MAIN_CC main(int argc, char *argv[])
{
    return clar_test(argc, argv);
}