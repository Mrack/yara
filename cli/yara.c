/*
Copyright (c) 2007-2013. The YARA Authors. All Rights Reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#if !defined(_WIN32) && !defined(__CYGWIN__)

// for getline(3)
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>

#else

#include <windows.h>

#define PRIx64 "I64x"
#define PRId64 "I64d"

#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <yara.h>

#include "args.h"
#include "common.h"
#include "threading.h"

#define ERROR_COULD_NOT_CREATE_THREAD 100

#ifndef MAX_PATH
#define MAX_PATH 256
#endif

#ifndef min
#define min(x, y) ((x < y) ? (x) : (y))
#endif

#ifdef _MSC_VER
#define snprintf _snprintf
#define strdup   _strdup
#endif

#define MAX_QUEUED_FILES 64

typedef struct _MODULE_DATA
{
  const char* module_name;
  YR_MAPPED_FILE mapped_file;
  struct _MODULE_DATA* next;

} MODULE_DATA;

typedef struct _CALLBACK_ARGS
{
  const char* file_path;
  int current_count;

} CALLBACK_ARGS;

typedef struct _THREAD_ARGS
{
  YR_SCANNER* scanner;
  CALLBACK_ARGS callback_args;
  time_t deadline;
  int current_count;

} THREAD_ARGS;

typedef struct _QUEUED_FILE
{
  char* path;

} QUEUED_FILE;

typedef struct COMPILER_RESULTS
{
  int errors;
  int warnings;

} COMPILER_RESULTS;

typedef struct SCAN_OPTIONS
{
  bool follow_symlinks;
  bool recursive_search;
  time_t deadline;

} SCAN_OPTIONS;

#define MAX_ARGS_TAG         32
#define MAX_ARGS_IDENTIFIER  32
#define MAX_ARGS_EXT_VAR     32
#define MAX_ARGS_MODULE_DATA 32

static char* atom_quality_table;
static char* tags[MAX_ARGS_TAG + 1];
static char* identifiers[MAX_ARGS_IDENTIFIER + 1];
static char* ext_vars[MAX_ARGS_EXT_VAR + 1];
static char* modules_data[MAX_ARGS_MODULE_DATA + 1];

static bool follow_symlinks = true;
static bool recursive_search = false;
static bool scan_list_search = false;
static bool show_module_data = false;
static bool show_tags = false;
static bool show_stats = false;
static bool show_rules = false;
static bool show_strings = false;
static bool show_string_length = false;
static bool show_meta = false;
static bool show_namespace = false;
static bool show_version = false;
static bool show_help = false;
static bool ignore_warnings = false;
static bool fast_scan = false;
static bool negate = false;
static bool print_count_only = false;
static bool fail_on_warnings = false;
static bool rules_are_compiled = false;
static int total_count = 0;
static int limit = 0;
static int timeout = 1000000;
static int stack_size = DEFAULT_STACK_SIZE;
static int threads = YR_MAX_THREADS;
static int max_strings_per_rule = DEFAULT_MAX_STRINGS_PER_RULE;

#define USAGE_STRING \
  "Usage: yara [OPTION]... [NAMESPACE:]RULES_FILE... FILE | DIR | PID"

args_option_t options[] = {
    OPT_STRING(
        0,
        "atom-quality-table",
        &atom_quality_table,
        "path to a file with the atom quality table",
        "FILE"),

    OPT_BOOLEAN(
        'C',
        "compiled-rules",
        &rules_are_compiled,
        "load compiled rules"),

    OPT_BOOLEAN(
        'c',
        "count",
        &print_count_only,
        "print only number of matches"),

    OPT_STRING_MULTI(
        'd',
        "define",
        &ext_vars,
        MAX_ARGS_EXT_VAR,
        "define external variable",
        "VAR=VALUE"),

    OPT_BOOLEAN(0, "fail-on-warnings", &fail_on_warnings, "fail on warnings"),

    OPT_BOOLEAN('f', "fast-scan", &fast_scan, "fast matching mode"),

    OPT_BOOLEAN('h', "help", &show_help, "show this help and exit"),

    OPT_STRING_MULTI(
        'i',
        "identifier",
        &identifiers,
        MAX_ARGS_IDENTIFIER,
        "print only rules named IDENTIFIER",
        "IDENTIFIER"),

    OPT_INTEGER(
        'l',
        "max-rules",
        &limit,
        "abort scanning after matching a NUMBER of rules",
        "NUMBER"),

    OPT_INTEGER(
        0,
        "max-strings-per-rule",
        &max_strings_per_rule,
        "set maximum number of strings per rule (default=10000)",
        "NUMBER"),

    OPT_STRING_MULTI(
        'x',
        "module-data",
        &modules_data,
        MAX_ARGS_MODULE_DATA,
        "pass FILE's content as extra data to MODULE",
        "MODULE=FILE"),

    OPT_BOOLEAN(
        'n',
        "negate",
        &negate,
        "print only not satisfied rules (negate)",
        NULL),

    OPT_BOOLEAN('w', "no-warnings", &ignore_warnings, "disable warnings"),

    OPT_BOOLEAN('m', "print-meta", &show_meta, "print metadata"),

    OPT_BOOLEAN(
        'D',
        "print-module-data",
        &show_module_data,
        "print module data"),

    OPT_BOOLEAN(
        'e',
        "print-namespace",
        &show_namespace,
        "print rules' namespace"),

    OPT_BOOLEAN('S', "print-stats", &show_stats, "print rules' statistics"),

    OPT_BOOLEAN('s', "print-strings", &show_strings, "print matching strings"),


    OPT_BOOLEAN('A', "print-rules", &show_rules, "print all rules strings"),

    OPT_BOOLEAN(
        'L',
        "print-string-length",
        &show_string_length,
        "print length of matched strings"),

    OPT_BOOLEAN('g', "print-tags", &show_tags, "print tags"),

    OPT_BOOLEAN(
        'r',
        "recursive",
        &recursive_search,
        "recursively search directories"),

    OPT_BOOLEAN(
        'N',
        "no-follow-symlinks",
        &follow_symlinks,
        "do not follow symlinks when scanning"),

    OPT_BOOLEAN(
        0,
        "scan-list",
        &scan_list_search,
        "scan files listed in FILE, one per line"),

    OPT_INTEGER(
        'k',
        "stack-size",
        &stack_size,
        "set maximum stack size (default=16384)",
        "SLOTS"),

    OPT_STRING_MULTI(
        't',
        "tag",
        &tags,
        MAX_ARGS_TAG,
        "print only rules tagged as TAG",
        "TAG"),

    OPT_INTEGER(
        'p',
        "threads",
        &threads,
        "use the specified NUMBER of threads to scan a directory",
        "NUMBER"),

    OPT_INTEGER(
        'a',
        "timeout",
        &timeout,
        "abort scanning after the given number of SECONDS",
        "SECONDS"),

    OPT_BOOLEAN('v', "version", &show_version, "show version information"),

    OPT_END()};

// file_queue is size-limited queue stored as a circular array, files are
// removed from queue_head position and new files are added at queue_tail
// position. The array has room for one extra element to avoid queue_head
// being equal to queue_tail in a full queue. The only situation where
// queue_head == queue_tail is when queue is empty.

QUEUED_FILE file_queue[MAX_QUEUED_FILES + 1];

int queue_head;
int queue_tail;

SEMAPHORE used_slots;
SEMAPHORE unused_slots;

MUTEX queue_mutex;
MUTEX output_mutex;

MODULE_DATA* modules_data_list = NULL;

static int file_queue_init()
{
  int result;

  queue_tail = 0;
  queue_head = 0;

  result = cli_mutex_init(&queue_mutex);

  if (result != 0)
    return result;

  result = cli_semaphore_init(&used_slots, 0);

  if (result != 0)
    return result;

  return cli_semaphore_init(&unused_slots, MAX_QUEUED_FILES);
}

static void file_queue_destroy()
{
  cli_mutex_destroy(&queue_mutex);
  cli_semaphore_destroy(&unused_slots);
  cli_semaphore_destroy(&used_slots);
}

static void file_queue_finish()
{
  for (int i = 0; i < YR_MAX_THREADS; i++) cli_semaphore_release(&used_slots);
}

static int file_queue_put(const char* file_path, time_t deadline)
{
  if (cli_semaphore_wait(&unused_slots, deadline) == ERROR_SCAN_TIMEOUT)
    return ERROR_SCAN_TIMEOUT;

  cli_mutex_lock(&queue_mutex);

  file_queue[queue_tail].path = strdup(file_path);
  queue_tail = (queue_tail + 1) % (MAX_QUEUED_FILES + 1);

  cli_mutex_unlock(&queue_mutex);
  cli_semaphore_release(&used_slots);

  return ERROR_SUCCESS;
}

static char* file_queue_get(time_t deadline)
{
  char* result;

  if (cli_semaphore_wait(&used_slots, deadline) == ERROR_SCAN_TIMEOUT)
    return NULL;

  cli_mutex_lock(&queue_mutex);

  if (queue_head == queue_tail)  // queue is empty
  {
    result = NULL;
  }
  else
  {
    result = file_queue[queue_head].path;
    queue_head = (queue_head + 1) % (MAX_QUEUED_FILES + 1);
  }

  cli_mutex_unlock(&queue_mutex);
  cli_semaphore_release(&unused_slots);

  return result;
}

#if defined(_WIN32) || defined(__CYGWIN__)

static bool is_directory(const char* path)
{
  DWORD attributes = GetFileAttributes(path);

  if (attributes != INVALID_FILE_ATTRIBUTES &&
      attributes & FILE_ATTRIBUTE_DIRECTORY)
    return true;
  else
    return false;
}

static int scan_dir(const char* dir, SCAN_OPTIONS* scan_opts)
{
  int result = ERROR_SUCCESS;
  static char path_and_mask[MAX_PATH];

  snprintf(path_and_mask, sizeof(path_and_mask), "%s\\*", dir);

  WIN32_FIND_DATA FindFileData;
  HANDLE hFind = FindFirstFile(path_and_mask, &FindFileData);

  if (hFind != INVALID_HANDLE_VALUE)
  {
    do
    {
      char full_path[MAX_PATH];

      snprintf(
          full_path, sizeof(full_path), "%s\\%s", dir, FindFileData.cFileName);

      if (!(FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      {
        result = file_queue_put(full_path, scan_opts->deadline);
      }
      else if (
          scan_opts->recursive_search &&
          strcmp(FindFileData.cFileName, ".") != 0 &&
          strcmp(FindFileData.cFileName, "..") != 0)
      {
        result = scan_dir(full_path, scan_opts);
      }

    } while (result != ERROR_SCAN_TIMEOUT &&
             FindNextFile(hFind, &FindFileData));

    FindClose(hFind);
  }

  return result;
}

static int populate_scan_list(const char* filename, SCAN_OPTIONS* scan_opts)
{
  DWORD nread;
  int result = ERROR_SUCCESS;

  HANDLE hFile = CreateFile(
      filename,
      GENERIC_READ,
      FILE_SHARE_READ,
      NULL,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      NULL);

  if (hFile == INVALID_HANDLE_VALUE)
  {
    fprintf(stderr, "error: could not open file \"%s\".\n", filename);
    return ERROR_COULD_NOT_OPEN_FILE;
  }

  DWORD fileSize = GetFileSize(hFile, NULL);

  if (fileSize == INVALID_FILE_SIZE)
  {
    fprintf(
        stderr, "error: could not determine size of file \"%s\".\n", filename);
    CloseHandle(hFile);
    return ERROR_COULD_NOT_READ_FILE;
  }

  // INVALID_FILE_SIZE is 0xFFFFFFFF, so (+1) will not overflow
  char* buf = (char*) VirtualAlloc(
      NULL, fileSize + 1, MEM_COMMIT, PAGE_READWRITE);

  if (buf == NULL)
  {
    fprintf(
        stderr,
        "error: could not allocate memory for file \"%s\".\n",
        filename);
    CloseHandle(hFile);
    return ERROR_INSUFFICIENT_MEMORY;
  }

  DWORD total = 0;

  while (total < fileSize)
  {
    if (!ReadFile(hFile, buf + total, fileSize - total, &nread, NULL))
    {
      fprintf(stderr, "error: could not read file \"%s\".\n", filename);
      CloseHandle(hFile);
      return ERROR_COULD_NOT_READ_FILE;
    }
    total += nread;
  }

  // Note: There's no need for reentrant strtok variants since this
  // function is run in single-threaded code.
  char* path = strtok(buf, "\n");

  while (result != ERROR_SCAN_TIMEOUT && path != NULL)
  {
    // Remove trailing carriage return, if present.
    if (*path != '\0')
    {
      char* final = path + strlen(path) - 1;
      if (*final == '\r')
        *final = '\0';
    }
    if (is_directory(path))
      result = scan_dir(path, scan_opts);
    else
      result = file_queue_put(path, scan_opts->deadline);

    path = strtok(NULL, "\n");
  }

  CloseHandle(hFile);

  return result;
}

#else

static bool is_directory(const char* path)
{
  struct stat st;

  if (stat(path, &st) == 0)
    return S_ISDIR(st.st_mode);

  return 0;
}

static int scan_dir(const char* dir, SCAN_OPTIONS* scan_opts)
{
  int result = ERROR_SUCCESS;
  DIR* dp = opendir(dir);

  if (dp)
  {
    struct dirent* de = readdir(dp);

    while (de && result != ERROR_SCAN_TIMEOUT)
    {
      char full_path[MAX_PATH];
      struct stat st;

      snprintf(full_path, sizeof(full_path), "%s/%s", dir, de->d_name);

      int err = lstat(full_path, &st);

      // If lstat returned error, or this directory entry is a symlink and the
      // user doesn't want to follow symlinks, skip the entry and continue with
      // the next one.
      if (err != 0 || (S_ISLNK(st.st_mode) && !scan_opts->follow_symlinks))
      {
        de = readdir(dp);
        continue;
      }
      // If the directory entry is a symlink, check if it points to . and
      // skip it in that case.
      else if (S_ISLNK(st.st_mode))
      {
        char buf[2];

        int len = readlink(full_path, buf, sizeof(buf));

        if (len == 1 && strcmp(buf, ".") == 0)
        {
          de = readdir(dp);
          continue;
        }
      }

      err = stat(full_path, &st);

      if (err == 0)
      {
        if (S_ISREG(st.st_mode))
        {
          result = file_queue_put(full_path, scan_opts->deadline);
        }
        else if (
            scan_opts->recursive_search && S_ISDIR(st.st_mode) &&
            strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
        {
          result = scan_dir(full_path, scan_opts);
        }
      }

      de = readdir(dp);
    }

    closedir(dp);
  }

  return result;
}

static int populate_scan_list(const char* filename, SCAN_OPTIONS* scan_opts)
{
  size_t nsize = 0;
  ssize_t nread;
  char* path = NULL;
  int result = ERROR_SUCCESS;

  FILE* fh_scan_list = fopen(filename, "r");

  if (fh_scan_list == NULL)
  {
    fprintf(stderr, "error: could not open file \"%s\".\n", filename);
    return ERROR_COULD_NOT_OPEN_FILE;
  }

  while (result != ERROR_SCAN_TIMEOUT &&
         (nread = getline(&path, &nsize, fh_scan_list)) != -1)
  {
    // remove trailing newline
    if (nread && path[nread - 1] == '\n')
    {
      path[nread - 1] = '\0';
      nread--;
    }

    if (is_directory(path))
      result = scan_dir(path, scan_opts);
    else
      result = file_queue_put(path, scan_opts->deadline);
  }

  free(path);
  fclose(fh_scan_list);

  return result;
}

#endif

static void print_string(const uint8_t* data, int length)
{
  for (int i = 0; i < length; i++)
  {
    if (data[i] >= 32 && data[i] <= 126)
      printf("%c", data[i]);
    else
      printf("\\x%02X", data[i]);
  }

  printf("\n");
}

static char cescapes[] = {
    0, 0, 0, 0, 0, 0, 0, 'a', 'b', 't', 'n', 'v', 'f', 'r', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,   0,   0,   0,   0,   0,   0,   0, 0,
};

static void print_escaped(const uint8_t* data, size_t length)
{
  for (size_t i = 0; i < length; i++)
  {
    switch (data[i])
    {
    case '\"':
    case '\'':
    case '\\':
      printf("\\%c", data[i]);
      break;

    default:
      if (data[i] >= 127)
        printf("\\%03o", data[i]);
      else if (data[i] >= 32)
        putchar(data[i]);
      else if (cescapes[data[i]] != 0)
        printf("\\%c", cescapes[data[i]]);
      else
        printf("\\%03o", data[i]);
    }
  }
}

static void print_hex_string(const uint8_t* data, int length)
{
  for (int i = 0; i < min(64, length); i++)
    printf("%s%02X", (i == 0 ? "" : " "), data[i]);

  puts(length > 64 ? " ..." : "");
}

static void print_error(int error)
{
  switch (error)
  {
  case ERROR_SUCCESS:
    break;
  case ERROR_COULD_NOT_ATTACH_TO_PROCESS:
    fprintf(stderr, "can not attach to process (try running as root)\n");
    break;
  case ERROR_INSUFFICIENT_MEMORY:
    fprintf(stderr, "not enough memory\n");
    break;
  case ERROR_SCAN_TIMEOUT:
    fprintf(stderr, "scanning timed out\n");
    break;
  case ERROR_COULD_NOT_OPEN_FILE:
    fprintf(stderr, "could not open file\n");
    break;
  case ERROR_UNSUPPORTED_FILE_VERSION:
    fprintf(stderr, "rules were compiled with a different version of YARA\n");
    break;
  case ERROR_INVALID_FILE:
    fprintf(stderr, "invalid compiled rules file.\n");
    break;
  case ERROR_CORRUPT_FILE:
    fprintf(stderr, "corrupt compiled rules file.\n");
    break;
  case ERROR_EXEC_STACK_OVERFLOW:
    fprintf(
        stderr,
        "stack overflow while evaluating condition "
        "(see --stack-size argument) \n");
    break;
  case ERROR_INVALID_EXTERNAL_VARIABLE_TYPE:
    fprintf(stderr, "invalid type for external variable\n");
    break;
  case ERROR_TOO_MANY_MATCHES:
    fprintf(stderr, "too many matches\n");
    break;
  default:
    fprintf(stderr, "error: %d\n", error);
    break;
  }
}

static void print_scanner_error(YR_SCANNER* scanner, int error)
{
  YR_RULE* rule = yr_scanner_last_error_rule(scanner);
  YR_STRING* string = yr_scanner_last_error_string(scanner);

  if (rule != NULL && string != NULL)
  {
    fprintf(
        stderr,
        "string \"%s\" in rule \"%s\" caused ",
        string->identifier,
        rule->identifier);
  }
  else if (rule != NULL)
  {
    fprintf(stderr, "rule \"%s\" caused ", rule->identifier);
  }

  print_error(error);
}

static void print_compiler_error(
    int error_level,
    const char* file_name,
    int line_number,
    const YR_RULE* rule,
    const char* message,
    void* user_data)
{
  char* msg_type;

  if (error_level == YARA_ERROR_LEVEL_ERROR)
  {
    msg_type = "error";
  }
  else if (!ignore_warnings)
  {
    COMPILER_RESULTS* compiler_results = (COMPILER_RESULTS*) user_data;
    compiler_results->warnings++;
    msg_type = "warning";
  }
  else
  {
    return;
  }

  if (rule != NULL)
  {
    fprintf(
        stderr,
        "%s: rule \"%s\" in %s(%d): %s\n",
        msg_type,
        rule->identifier,
        file_name,
        line_number,
        message);
  }
  else
  {
    fprintf(
        stderr, "%s(%d): %s: %s\n", file_name, line_number, msg_type, message);
  }
}

static void print_rules_stats(YR_RULES* rules)
{
  YR_RULES_STATS stats;

  int t = sizeof(stats.top_ac_match_list_lengths) /
          sizeof(stats.top_ac_match_list_lengths[0]);

  int result = yr_rules_get_stats(rules, &stats);

  if (result != ERROR_SUCCESS)
  {
    print_error(result);
    return;
  }

  printf("size of AC transition table        : %d\n", stats.ac_tables_size);

  printf(
      "average length of AC matches lists : %f\n",
      stats.ac_average_match_list_length);

  printf("number of rules                    : %d\n", stats.num_rules);

  printf("number of strings                  : %d\n", stats.num_strings);

  printf("number of AC matches               : %d\n", stats.ac_matches);

  printf(
      "number of AC matches in root node  : %d\n",
      stats.ac_root_match_list_length);

  printf("number of AC matches in top %d longest lists\n", t);

  for (int i = 0; i < t; i++)
    printf(" %3d: %d\n", i + 1, stats.top_ac_match_list_lengths[i]);

  printf("match list length percentiles\n");

  for (int i = 100; i >= 0; i--)
    printf(" %3d: %d\n", i, stats.ac_match_list_length_pctls[i]);
}

static int handle_message(
    YR_SCAN_CONTEXT* context,
    int message,
    YR_RULE* rule,
    void* data)
{
  const char* tag;
  bool show = true;

  if (tags[0] != NULL)
  {
    // The user specified one or more -t <tag> arguments, let's show this rule
    // only if it's tagged with some of the specified tags.

    show = false;

    for (int i = 0; !show && tags[i] != NULL; i++)
    {
      yr_rule_tags_foreach(rule, tag)
      {
        if (strcmp(tag, tags[i]) == 0)
        {
          show = true;
          break;
        }
      }
    }
  }

  if (identifiers[0] != NULL)
  {
    // The user specified one or more -i <identifier> arguments, let's show
    // this rule only if it's identifier is among of the provided ones.

    show = false;

    for (int i = 0; !show && identifiers[i] != NULL; i++)
    {
      if (strcmp(identifiers[i], rule->identifier) == 0)
      {
        show = true;
        break;
      }
    }
  }

  bool is_matching = (message == CALLBACK_MSG_RULE_MATCHING);

  show = show && ((!negate && is_matching) || (negate && !is_matching)) || show_rules;

  if (show && !print_count_only)
  {
    cli_mutex_lock(&output_mutex);

    if (!show_rules && show_namespace)
      printf("%s:", rule->ns->name);

    printf("%s \n{\n", rule->identifier);

    if (!show_rules && show_tags)
    {
      yr_rule_tags_foreach(rule, tag)
      {
        // print a comma except for the first tag
        if (tag != rule->tags)
          printf("\n");

        printf("%s", tag);
      }

      printf("\n");
    }

    // Show meta-data.

    if (show_rules || show_meta)
    {
      YR_META* meta;


      yr_rule_metas_foreach(rule, meta)
      {
        if (meta != rule->metas)
          printf("\n");

        if (meta->type == META_TYPE_INTEGER)
        {
          printf("%s=%" PRId64, meta->identifier, meta->integer);
        }
        else if (meta->type == META_TYPE_BOOLEAN)
        {
          printf("%s=%s", meta->identifier, meta->integer ? "true" : "false");
        }
        else
        {
          printf("%s=\"", meta->identifier);
          print_escaped((uint8_t*) (meta->string), strlen(meta->string));
        }
      }
    }

    if (show_rules || show_strings || show_string_length)
    {
      YR_STRING* string;
      printf("\nstrings:\n");
      yr_rule_strings_foreach(rule, string)
      {
        YR_MATCH* match;
        
        if (show_rules)
        {
          if (STRING_IS_HEX(string))
          {
            printf("	%s= {", string->identifier);
            for (int i = 0; i < string->length; i++)
              printf("%s%02X", (i == 0 ? "" : " "), string->string[i]);

            printf("}\n");
          }
          else
            printf("\t%s=\"%s\"\n", string->identifier, string->string, string->identifier);
        } else {
          yr_string_matches_foreach(context, string, match)
          {
            if (show_string_length)
              printf(
                  "0x%" PRIx64 ":%d:%s",
                  match->base + match->offset,
                  match->data_length,
                  string->identifier);
            else
              printf(
                  "0x%" PRIx64 ":%s",
                  match->base + match->offset,
                  string->identifier);

            if (show_strings)
            {
              printf(": ");

              if (STRING_IS_HEX(string))
                print_hex_string(match->data, match->data_length);
              else
                print_string(match->data, match->data_length);
            }
            else
            {
              printf("\n");
            }
          }
        }

      }
      printf("}\n");
    }

    cli_mutex_unlock(&output_mutex);
  }

  if (is_matching)
  {
    ((CALLBACK_ARGS*) data)->current_count++;
    total_count++;
  }

  if (limit != 0 && total_count >= limit)
    return CALLBACK_ABORT;

  return CALLBACK_CONTINUE;
}

static int callback(
    YR_SCAN_CONTEXT* context,
    int message,
    void* message_data,
    void* user_data)
{
  YR_MODULE_IMPORT* mi;
  YR_STRING* string;
  YR_RULE* rule;
  YR_OBJECT* object;
  MODULE_DATA* module_data;

  switch (message)
  {
  case CALLBACK_MSG_RULE_MATCHING:
  case CALLBACK_MSG_RULE_NOT_MATCHING:
    return handle_message(context, message, (YR_RULE*) message_data, user_data);

  case CALLBACK_MSG_IMPORT_MODULE:

    mi = (YR_MODULE_IMPORT*) message_data;
    module_data = modules_data_list;

    while (module_data != NULL)
    {
      if (strcmp(module_data->module_name, mi->module_name) == 0)
      {
        mi->module_data = (void*) module_data->mapped_file.data;
        mi->module_data_size = module_data->mapped_file.size;
        break;
      }

      module_data = module_data->next;
    }

    return CALLBACK_CONTINUE;

  case CALLBACK_MSG_MODULE_IMPORTED:

    if (show_module_data)
    {
      object = (YR_OBJECT*) message_data;

      cli_mutex_lock(&output_mutex);

      yr_object_print_data(object, 0, 1);
      printf("\n");

      cli_mutex_unlock(&output_mutex);
    }

    return CALLBACK_CONTINUE;

  case CALLBACK_MSG_TOO_MANY_MATCHES:

    if (ignore_warnings)
      return CALLBACK_CONTINUE;

    string = (YR_STRING*) message_data;
    rule = &context->rules->rules_table[string->rule_idx];

    fprintf(
        stderr,
        "warning: rule \"%s\": too many matches for %s, results for this rule "
        "may be incorrect\n",
        rule->identifier,
        string->identifier);

    if (fail_on_warnings)
      return CALLBACK_ERROR;

    return CALLBACK_CONTINUE;
  }

  return CALLBACK_ERROR;
}

#if defined(_WIN32) || defined(__CYGWIN__)
static DWORD WINAPI scanning_thread(LPVOID param)
#else
static void* scanning_thread(void* param)
#endif
{
  int result = ERROR_SUCCESS;
  THREAD_ARGS* args = (THREAD_ARGS*) param;

  char* file_path = file_queue_get(args->deadline);

  while (file_path != NULL)
  {
    args->callback_args.current_count = 0;
    args->callback_args.file_path = file_path;

    time_t current_time = time(NULL);

    if (current_time < args->deadline)
    {
      yr_scanner_set_timeout(args->scanner, args->deadline - current_time);

      result = yr_scanner_scan_file(args->scanner, file_path);

      if (print_count_only)
      {
        cli_mutex_lock(&output_mutex);
        printf("%s: %d\n", file_path, args->callback_args.current_count);
        cli_mutex_unlock(&output_mutex);
      }

      if (result != ERROR_SUCCESS)
      {
        cli_mutex_lock(&output_mutex);
        fprintf(stderr, "error scanning %s: ", file_path);
        print_scanner_error(args->scanner, result);
        cli_mutex_unlock(&output_mutex);
      }

      free(file_path);
      file_path = file_queue_get(args->deadline);
    }
    else
    {
      file_path = NULL;
    }
  }

  return 0;
}

static int define_external_variables(YR_RULES* rules, YR_COMPILER* compiler)
{
  int result = ERROR_SUCCESS;

  for (int i = 0; ext_vars[i] != NULL; i++)
  {
    char* equal_sign = strchr(ext_vars[i], '=');

    if (!equal_sign)
    {
      fprintf(stderr, "error: wrong syntax for `-d` option.\n");
      return ERROR_SUCCESS;
    }

    // Replace the equal sign with null character to split the external
    // variable definition (i.e: myvar=somevalue) in two strings: identifier
    // and value.

    *equal_sign = '\0';

    char* identifier = ext_vars[i];
    char* value = equal_sign + 1;

    if (is_float(value))
    {
      if (rules != NULL)
        result = yr_rules_define_float_variable(rules, identifier, atof(value));

      if (compiler != NULL)
        result = yr_compiler_define_float_variable(
            compiler, identifier, atof(value));
    }
    else if (is_integer(value))
    {
      if (rules != NULL)
        result = yr_rules_define_integer_variable(
            rules, identifier, atoi(value));

      if (compiler != NULL)
        result = yr_compiler_define_integer_variable(
            compiler, identifier, atoi(value));
    }
    else if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0)
    {
      if (rules != NULL)
        result = yr_rules_define_boolean_variable(
            rules, identifier, strcmp(value, "true") == 0);

      if (compiler != NULL)
        result = yr_compiler_define_boolean_variable(
            compiler, identifier, strcmp(value, "true") == 0);
    }
    else
    {
      if (rules != NULL)
        result = yr_rules_define_string_variable(rules, identifier, value);

      if (compiler != NULL)
        result = yr_compiler_define_string_variable(
            compiler, identifier, value);
    }
  }

  return result;
}

static int load_modules_data()
{
  for (int i = 0; modules_data[i] != NULL; i++)
  {
    char* equal_sign = strchr(modules_data[i], '=');

    if (!equal_sign)
    {
      fprintf(stderr, "error: wrong syntax for `-x` option.\n");
      return false;
    }

    *equal_sign = '\0';

    MODULE_DATA* module_data = (MODULE_DATA*) malloc(sizeof(MODULE_DATA));

    if (module_data != NULL)
    {
      module_data->module_name = modules_data[i];

      int result = yr_filemap_map(equal_sign + 1, &module_data->mapped_file);

      if (result != ERROR_SUCCESS)
      {
        free(module_data);
        fprintf(stderr, "error: could not open file \"%s\".\n", equal_sign + 1);
        return false;
      }

      module_data->next = modules_data_list;
      modules_data_list = module_data;
    }
  }

  return true;
}

static void unload_modules_data()
{
  MODULE_DATA* module_data = modules_data_list;

  while (module_data != NULL)
  {
    MODULE_DATA* next_module_data = module_data->next;

    yr_filemap_unmap(&module_data->mapped_file);
    free(module_data);

    module_data = next_module_data;
  }

  modules_data_list = NULL;
}

int main(int argc, const char** argv)
{
  COMPILER_RESULTS cr;

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;
  YR_SCANNER* scanner = NULL;
  SCAN_OPTIONS scan_opts;

  bool arg_is_dir = false;
  int flags = 0;
  int result;

  argc = args_parse(options, argc, argv);

  scan_opts.follow_symlinks = follow_symlinks;
  scan_opts.recursive_search = recursive_search;

  if (show_version)
  {
    printf("%s\n", YR_VERSION);
    return EXIT_SUCCESS;
  }

  if (show_help)
  {
    printf(
        "YARA %s, the pattern matching swiss army knife.\n"
        "%s\n\n"
        "Mandatory arguments to long options are mandatory for "
        "short options too.\n\n",
        YR_VERSION,
        USAGE_STRING);

    args_print_usage(options, 40);
    printf(
        "\nSend bug reports and suggestions to: vmalvarez@virustotal.com.\n");

    return EXIT_SUCCESS;
  }

  if (threads > YR_MAX_THREADS)
  {
    fprintf(stderr, "maximum number of threads is %d\n", YR_MAX_THREADS);
    return EXIT_FAILURE;
  }

  if (argc < 2)
  {
    // After parsing the command-line options we expect two additional
    // arguments, the rules file and the target file, directory or pid to
    // be scanned.

    fprintf(stderr, "yara: wrong number of arguments\n");
    fprintf(stderr, "%s\n\n", USAGE_STRING);
    fprintf(stderr, "Try `--help` for more options\n");

    return EXIT_FAILURE;
  }

  if (!load_modules_data())
    exit_with_code(EXIT_FAILURE);

  result = yr_initialize();

  if (result != ERROR_SUCCESS)
  {
    fprintf(stderr, "error: initialization error (%d)\n", result);
    exit_with_code(EXIT_FAILURE);
  }

  yr_set_configuration(YR_CONFIG_STACK_SIZE, &stack_size);
  yr_set_configuration(YR_CONFIG_MAX_STRINGS_PER_RULE, &max_strings_per_rule);

  // Try to load the rules file as a binary file containing
  // compiled rules first

  if (rules_are_compiled)
  {
    // When a binary file containing compiled rules is provided, yara accepts
    // only two arguments, the compiled rules file and the target to be scanned.

    if (argc != 2)
    {
      fprintf(
          stderr,
          "error: can't accept multiple rules files if one of them is in "
          "compiled form.\n");
      exit_with_code(EXIT_FAILURE);
    }

    result = yr_rules_load(argv[0], &rules);

    if (result == ERROR_SUCCESS)
      result = define_external_variables(rules, NULL);
  }
  else
  {
    // Rules file didn't contain compiled rules, let's handle it
    // as a text file containing rules in source form.

    if (yr_compiler_create(&compiler) != ERROR_SUCCESS)
      exit_with_code(EXIT_FAILURE);

    result = define_external_variables(NULL, compiler);

    if (result != ERROR_SUCCESS)
    {
      print_error(result);
      exit_with_code(EXIT_FAILURE);
    }

    if (atom_quality_table != NULL)
    {
      result = yr_compiler_load_atom_quality_table(
          compiler, atom_quality_table, 0);

      if (result != ERROR_SUCCESS)
      {
        fprintf(stderr, "error loading atom quality table: ");
        print_error(result);
        exit_with_code(EXIT_FAILURE);
      }
    }

    cr.errors = 0;
    cr.warnings = 0;

    yr_compiler_set_callback(compiler, print_compiler_error, &cr);

    if (!compile_files(compiler, argc, argv))
      exit_with_code(EXIT_FAILURE);

    if (cr.errors > 0)
      exit_with_code(EXIT_FAILURE);

    if (fail_on_warnings && cr.warnings > 0)
      exit_with_code(EXIT_FAILURE);

    result = yr_compiler_get_rules(compiler, &rules);

    yr_compiler_destroy(compiler);

    compiler = NULL;
  }

  if (result != ERROR_SUCCESS)
  {
    print_error(result);
    exit_with_code(EXIT_FAILURE);
  }

  if (show_stats)
    print_rules_stats(rules);

  cli_mutex_init(&output_mutex);

  if (fast_scan)
    flags |= SCAN_FLAGS_FAST_MODE;

  scan_opts.deadline = time(NULL) + timeout;

  arg_is_dir = is_directory(argv[argc - 1]);

  if (scan_list_search && arg_is_dir)
  {
    fprintf(stderr, "error: cannot use a directory as scan list.\n");
    exit_with_code(EXIT_FAILURE);
  }
  else if (scan_list_search || arg_is_dir)
  {
    if (file_queue_init() != 0)
    {
      print_error(ERROR_INTERNAL_FATAL_ERROR);
      exit_with_code(EXIT_FAILURE);
    }

    THREAD thread[YR_MAX_THREADS];
    THREAD_ARGS thread_args[YR_MAX_THREADS];

    for (int i = 0; i < threads; i++)
    {
      thread_args[i].deadline = scan_opts.deadline;
      thread_args[i].current_count = 0;

      result = yr_scanner_create(rules, &thread_args[i].scanner);

      if (result != ERROR_SUCCESS)
      {
        print_error(result);
        exit_with_code(EXIT_FAILURE);
      }

      yr_scanner_set_callback(
          thread_args[i].scanner, callback, &thread_args[i].callback_args);

      yr_scanner_set_flags(thread_args[i].scanner, flags);

      if (cli_create_thread(
              &thread[i], scanning_thread, (void*) &thread_args[i]))
      {
        print_error(ERROR_COULD_NOT_CREATE_THREAD);
        exit_with_code(EXIT_FAILURE);
      }
    }

    if (arg_is_dir)
    {
      scan_dir(argv[argc - 1], &scan_opts);
    }
    else
    {
      result = populate_scan_list(argv[argc - 1], &scan_opts);

      if (result != ERROR_SUCCESS)
        exit_with_code(EXIT_FAILURE);
    }

    file_queue_finish();

    // Wait for scan threads to finish
    for (int i = 0; i < threads; i++) cli_thread_join(&thread[i]);

    for (int i = 0; i < threads; i++)
      yr_scanner_destroy(thread_args[i].scanner);

    file_queue_destroy();
  }
  else
  {
    CALLBACK_ARGS user_data = {argv[argc - 1], 0};

    result = yr_scanner_create(rules, &scanner);

    if (result != ERROR_SUCCESS)
    {
      fprintf(stderr, "error: %d\n", result);
      exit_with_code(EXIT_FAILURE);
    }

    yr_scanner_set_callback(scanner, callback, &user_data);
    yr_scanner_set_flags(scanner, flags);
    yr_scanner_set_timeout(scanner, timeout);

    if (is_integer(argv[argc - 1]))
      result = yr_scanner_scan_proc(scanner, atoi(argv[argc - 1]));
    else
      result = yr_scanner_scan_file(scanner, argv[argc - 1]);

    if (result != ERROR_SUCCESS)
    {
      fprintf(stderr, "error scanning %s: ", argv[argc - 1]);
      print_scanner_error(scanner, result);
      exit_with_code(EXIT_FAILURE);
    }

    if (print_count_only)
      printf("%d\n", user_data.current_count);

#ifdef YR_PROFILING_ENABLED
    yr_scanner_print_profiling_info(scanner);
#endif
  }

  result = EXIT_SUCCESS;

_exit:

  unload_modules_data();

  if (scanner != NULL)
    yr_scanner_destroy(scanner);

  if (compiler != NULL)
    yr_compiler_destroy(compiler);

  if (rules != NULL)
    yr_rules_destroy(rules);

  yr_finalize();

  return result;
}
