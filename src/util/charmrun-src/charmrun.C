#include "converse.h"

#include "sockRoutines.h"
#include "sockRoutines.C"
#include "ccs-auth.h"
#include "ccs-auth.C"
#include "ccs-server.h"
#include "ccs-server.C"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include <limits.h>
#if CMK_USE_POLL
#include <poll.h>
#endif
#include <sys/stat.h>

#include <unordered_map>
#include <map>
#include <string>
#include <vector>
#include <queue>
#include <utility>
#include <algorithm>

#if defined(_WIN32)
/*Win32 has screwy names for the standard UNIX calls:*/
#define getcwd _getcwd
#define strdup _strdup
#define unlink _unlink
#define open _open
#define fdopen _fdopen
#define ftruncate _chsize
#include <winbase.h>
#include <direct.h>
#include <io.h>
#include <sys/timeb.h>
#include <process.h>
#define DIRSEP "\\"
#define SIGBUS -1 /*These signals don't exist in Win32*/
#define SIGKILL -1
#define SIGQUIT -1

#else            /*UNIX*/
#include <pwd.h> /*getcwd*/
#include <unistd.h>
#define DIRSEP "/"
#endif

#if CMK_HAS_ADDR_NO_RANDOMIZE
#include <sys/personality.h>
#endif

#if CMK_HAS_POSIX_SPAWN
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <spawn.h>
#ifdef __APPLE__
#ifndef _POSIX_SPAWN_DISABLE_ASLR
#define _POSIX_SPAWN_DISABLE_ASLR 0x100
#endif
#endif
#endif

#define PRINT(a) (arg_quiet ? 1 : printf a)

#if CMK_SSH_NOT_NEEDED /*No SSH-- use daemon to start node-programs*/
#define CMK_USE_SSH 0

#else /*Use SSH to start node-programs*/
#define CMK_USE_SSH 1
#if CMK_SSH_IS_A_COMMAND
#define SSH_CMD "ssh"
#endif

#endif

#include "daemon.h"

/*#define DEBUGF(x) printf x*/
#define DEBUGF(x)

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

static const int MAX_NUM_RETRIES = 3;

//#define HSTART
#ifdef HSTART
/*Hierarchical-start routines*/
static int mynodes_start; /* To keep a global node numbering */

#endif

static double ftTimer;

static double start_timer;

static double GetClock(void)
{
#if defined(_WIN32)
  struct _timeb tv;
  _ftime(&tv);
  return (tv.time * 1.0 + tv.millitm * 1.0E-3);
#else
  struct timeval tv;
  int ok = gettimeofday(&tv, NULL);
  if (ok < 0) {
    perror("gettimeofday");
    exit(1);
  }
  return (tv.tv_sec * 1.0 + tv.tv_usec * 1.0E-6);
#endif
}

static int probefile(const char *path)
{
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return 0;
  fclose(f);
  return 1;
}

static const char *mylogin(void)
{
#if defined(_WIN32)
  static char name[100] = {'d', 'u', 'n', 'n', 'o', 0};
  unsigned int len = 100;
  GetUserName(name, (LPDWORD) &len);
  return name;
#else /*UNIX*/
  struct passwd *self = getpwuid(getuid());
  if (self == 0) {
#if CMK_HAS_POPEN
    char cmd[16];
    char uname[64];
    FILE *p;
    sprintf(cmd, "id -u -n");
    p = popen(cmd, "r");
    if (p) {
      if (fscanf(p, "%63s", uname) != 1) {
        fprintf(stderr, "charmrun> fscanf() failed!\n");
        exit(1);
      }
      pclose(p);
      return strdup(uname);
    } else
      return "unknown";
#else
    return "unknown";
#endif
  }
  return self->pw_name;
#endif
}

/**************************************************************************
 *
 * Pathfix : alters a path according to a set of rewrite rules
 *
 *************************************************************************/

typedef struct s_pathfixlist {
  char *s1;
  char *s2;
  struct s_pathfixlist *next;
} * pathfixlist;

static pathfixlist pathfix_append(char *s1, char *s2, pathfixlist l)
{
  pathfixlist pf = (pathfixlist) malloc(sizeof(s_pathfixlist));
  pf->s1 = s1;
  pf->s2 = s2;
  pf->next = l;
  return pf;
}

static char *pathfix(const char *path, pathfixlist fixes)
{
  char buffer[MAXPATHLEN];
  char buf2[MAXPATHLEN];
  strcpy(buffer, path);
  int mod = 1;
  while (mod) {
    mod = 0;
    for (pathfixlist l = fixes; l; l = l->next) {
      int len = strlen(l->s1);
      char *offs = strstr(buffer, l->s1);
      if (offs) {
        offs[0] = 0;
        sprintf(buf2, "%s%s%s", buffer, l->s2, offs + len);
        strcpy(buffer, buf2);
        mod = 1;
      }
    }
  }
  return strdup(buffer);
}

static char *pathextfix(const char *path, pathfixlist fixes, char *ext)
{
  char *newpath = pathfix(path, fixes);
  if (ext == NULL)
    return newpath;
  char *ret = (char *) malloc(strlen(newpath) + strlen(ext) + 2);
  strcpy(ret, newpath);
  strcat(ret, ext);
  free(newpath);
  return ret;
}

/****************************************************************************
 *
 * Miscellaneous minor routines.
 *
 ****************************************************************************/

static int is_quote(char c) { return (c == '\'' || c == '"'); }

static void zap_newline(char *s)
{
  char *p = s + strlen(s) - 1;
  if (*p == '\n')
    *p = '\0';
  /* in case of DOS ^m */
  p--;
  if (*p == '\15')
    *p = '\0';
}

/* get substring from lo to hi, remove quote chars */
static char *substr(const char *lo, const char *hi)
{
  if (is_quote(*lo))
    lo++;
  if (is_quote(*(hi - 1)))
    hi--;
  int len = hi - lo;
  char *res = (char *) malloc(1 + len);
  memcpy(res, lo, len);
  res[len] = 0;
  return res;
}

static int subeqs(const char *lo, const char *hi, const char *str)
{
  int len = strlen(str);
  if (hi - lo != len)
    return 0;
  if (memcmp(lo, str, len))
    return 0;
  return 1;
}

/* advance pointer over blank characters */
static const char *skipblanks(const char *p)
{
  while ((*p == ' ') || (*p == '\t'))
    p++;
  return p;
}

/* advance pointer over nonblank characters and a quoted string */
static const char *skipstuff(const char *p)
{
  char quote = 0;
  if (*p && (*p == '\'' || *p == '"')) {
    quote = *p;
    p++;
  }
  if (quote != 0) {
    while (*p && *p != quote)
      p++;
    if (*p != quote) {
      fprintf(stderr, "ERROR> Unmatched quote in nodelist file.\n");
      exit(1);
    }
    p++;
  } else
    while ((*p) && (*p != ' ') && (*p != '\t'))
      p++;
  return p;
}

static char *cstring_join(const std::vector<const char *> & vec, const char *separator)
{
  const size_t separator_length = strlen(separator);
  size_t length = 0;
  for (const char *p : vec)
    length += strlen(p) + separator_length;

  char * const str = (char *)malloc(length + 1);

  if (0 < vec.size())
    strcpy(str, vec[0]);
  for (int i = 1; i < vec.size(); ++i)
  {
    strcat(str, separator);
    strcat(str, vec[i]);
  }

  return str;
}

#if CMK_USE_SSH
static const char *getenv_ssh()
{
  char *e = getenv("CONV_RSH");
  return e ? e : SSH_CMD;
}
#endif

#if !defined(_WIN32)
static char *getenv_display()
{
  static char result[100], ipBuf[200];

  char *e = getenv("DISPLAY");
  if (e == 0)
    return NULL;
  char *p = strrchr(e, ':');
  if (p == 0)
    return NULL;
  if ((e[0] == ':') || (strncmp(e, "unix:", 5) == 0)) {
    sprintf(result, "%s:%s", skt_print_ip(ipBuf, skt_my_ip()), p + 1);
  } else
    strcpy(result, e);
  return result;
}
static char *getenv_display_no_tamper()
{
  static char result[100], ipBuf[200];

  char *e = getenv("DISPLAY");
  if (e == 0)
    return NULL;
  char *p = strrchr(e, ':');
  if (p == 0)
    return NULL;
  strcpy(result, e);
  return result;
}

#endif

static unsigned int server_port;
static char server_addr[1024]; /* IP address or hostname of charmrun*/
static SOCKET server_fd;
/*****************************************************************************
 *                                                                           *
 * PPARAM - obtaining "program parameters" from the user.                    *
 *                                                                           *
 *****************************************************************************/

typedef struct s_ppdef {
  union {
    int *i;
    double *r;
    const char **s;
    int *f;
  } where;           /*Where to store result*/
  const char *lname; /*Argument name on command line*/
  const char *doc;
  char type; /*One of i, r, s, f.*/
  bool initFlag; // if 0 means, user input paramater is inserted. 1 means, it holds a default value
  struct s_ppdef *next;
} * ppdef;

static ppdef ppdefs;

static int pparam_pos;
static const char **pparam_argv;
static char pparam_optc = '-';
static char pparam_error[100];

struct ppdeffind
{
  ppdef def;
  int enable;
};

static ppdeffind pparam_find(const char *lname)
{
  ppdef def;
  for (def = ppdefs; def; def = def->next)
  {
    if (strcmp(def->lname, lname) == 0)
      return {def, 1};

    static const char no_prefix[] = "no-";
    if (strncmp(no_prefix, lname, sizeof(no_prefix)-1) == 0)
    {
      if (strcmp(def->lname, lname + (sizeof(no_prefix)-1)) == 0)
        return {def, 0};
    }
  }
  return {nullptr, 1};
}

static ppdef pparam_cell(const char *lname)
{
  ppdeffind deffind = pparam_find(lname);
  if (deffind.def)
    return deffind.def;

  auto def = (ppdef)malloc(sizeof(s_ppdef));
  def->lname = lname;
  def->type = 's';
  def->doc = "(undocumented)";
  def->next = ppdefs;
  def->initFlag = true;
  ppdefs = def;
  return def;
}

static void pparam_int(int *where, int defValue, const char *arg, const char *doc)
{
  ppdef def = pparam_cell(arg);
  def->type = 'i';
  def->where.i = where;
  *where = defValue;
  def->lname = arg;
  def->doc = doc;
}

static void pparam_flag(int *where, int defValue, const char *arg, const char *doc)
{
  ppdef def = pparam_cell(arg);
  def->type = 'f';
  def->where.f = where;
  *where = defValue;
  def->lname = arg;
  def->doc = doc;
}

static void pparam_real(double *where, double defValue, const char *arg,
                 const char *doc)
{
  ppdef def = pparam_cell(arg);
  def->type = 'r';
  def->where.r = where;
  *where = defValue;
  def->lname = arg;
  def->doc = doc;
}

static void pparam_str(const char **where, const char *defValue, const char *arg,
                const char *doc)
{
  ppdef def = pparam_cell(arg);
  def->type = 's';
  def->where.s = where;
  *where = defValue;
  def->lname = arg;
  def->doc = doc;
}

static int pparam_setdef(ppdef def, const char *value)
{
  char *p;
  if (def->initFlag)
    def->initFlag = false;
  else {
    fprintf(stderr, "Option \'%s\' is used more than once. Please remove duplicate arguments for this option\n", def->lname);
    exit(1);
  }

  switch (def->type) {
  case 'i':
    *def->where.i = strtol(value, &p, 10);
    if (*p)
      return -1;
    return 0;
  case 'r':
    *def->where.r = strtod(value, &p);
    if (*p)
      return -1;
    return 0;
  case 's': {
    /* Parse input string and convert a literal "\n" into '\n'. */
    *def->where.s = (char *) calloc(strlen(value) + 1, sizeof(char));
    char *parsed_value = (char *) *def->where.s;
    for (int i = 0, j = 0; i < strlen(value); i++) {
      if (i + 1 < strlen(value)) {
        if (value[i] == '\\' && value[i + 1] == 'n') {
          parsed_value[j++] = '\n';
          i++;
          continue;
        }
      }
      parsed_value[j++] = value[i];
    }
    return 0;
  }
  case 'f':
    *def->where.f = strtol(value, &p, 10);
    if (*p)
      return -1;
    return 0;
  }
  return -1;
}

static int pparam_set(char *lname, char *value)
{
  ppdef def = pparam_cell(lname);
  return pparam_setdef(def, value);
}

static const char *pparam_getdef(ppdef def)
{
  static char result[100];
  switch (def->type) {
  case 'i':
    sprintf(result, "%d", *def->where.i);
    return result;
  case 'r':
    sprintf(result, "%f", *def->where.r);
    return result;
  case 's':
    return *def->where.s ? *def->where.s : "";
  case 'f':
    sprintf(result, *def->where.f ? "true" : "false");
    return result;
  }
  return NULL;
}

static void pparam_printdocs()
{
  int maxname = 0, maxdoc = 0;
  for (ppdef def = ppdefs; def; def = def->next) {
    int len;
    len = strlen(def->lname);
    if (len > maxname)
      maxname = len;
    len = strlen(def->doc);
    if (len > maxdoc)
      maxdoc = len;
  }
  fprintf(stderr, "\n");
  fprintf(stderr, "Charmrun Command-line Parameters:\n");
  fprintf(stderr, "  (Boolean parameters may be prefixed with \"no-\" to negate their effect, for example \"++no-scalable-start\".)\n");
  for (ppdef def = ppdefs; def; def = def->next) {
    fprintf(stderr, "  %c%c%-*s ", pparam_optc, pparam_optc, maxname,
            def->lname);
    fprintf(stderr, "  %-*s [%s]\n", maxdoc, def->doc, pparam_getdef(def));
  }
  fprintf(stderr, "\n");
}

static void pparam_delarg(int i)
{
  for (int j = i; pparam_argv[j]; j++)
    pparam_argv[j] = pparam_argv[j + 1];
}

static int pparam_countargs(const char **argv)
{
  int argc;
  for (argc = 0; argv[argc]; argc++)
    ;
  return argc;
}

static int pparam_parseopt()
{
  const char *opt = pparam_argv[pparam_pos];
  /* handle ++ by skipping to end */
  if ((opt[1] == '+') && (opt[2] == 0)) {
    pparam_delarg(pparam_pos);
    while (pparam_argv[pparam_pos])
      pparam_pos++;
    return 0;
  }
  /* handle + by itself - an error */
  if (opt[1] == 0) {
    sprintf(pparam_error, "Illegal option +\n");
    return -1;
  }
  /* look up option definition */
  ppdeffind deffind{};
  if (opt[1] == '+')
    deffind = pparam_find(opt + 2);
  else {
    char name[2];
    name[0] = opt[1];
    if (strlen(opt) <= 2 || !isalpha(opt[2])) {
      name[1] = 0;
      deffind = pparam_find(name);
    }
  }
  if (deffind.def == nullptr) {
    if (opt[1] == '+') {
      sprintf(pparam_error, "Option %s not recognized.", opt);
      return -1;
    } else {
      /*Unrecognized single '+' option-- skip it.*/
      pparam_pos++;
      return 0;
    }
  }
  auto def = deffind.def;
  /* handle flag-options */
  if ((def->type == 'f') && (opt[1] != '+') && (opt[2] != '\0')) {
    sprintf(pparam_error, "Option %s should not include a value", opt);
    return -1;
  }
  if (def->type == 'f') {
    *def->where.f = deffind.enable;
    pparam_delarg(pparam_pos);
    return 0;
  }
  /* handle non-flag options */
  const char * optname = opt;
  if ((opt[1] == '+') || (opt[2] == '\0')) { // special single '+' handling
    pparam_delarg(pparam_pos);
    opt = pparam_argv[pparam_pos];
  } else
    opt += 2;
  if ((opt == nullptr) || (opt[0] == '\0')) {
    sprintf(pparam_error, "%s must be followed by a value.", optname);
    return -1;
  }
  int ok = pparam_setdef(def, opt);
  pparam_delarg(pparam_pos);
  if (ok < 0) {
    sprintf(pparam_error, "Illegal value for %s", optname);
    return -1;
  }
  return 0;
}

static int pparam_parsecmd(char optchr, const char **argv)
{
  pparam_error[0] = 0;
  pparam_argv = argv;
  pparam_optc = optchr;
  pparam_pos = 0;
  while (1) {
    const char *opt = pparam_argv[pparam_pos];
    if (opt == 0)
      break;
    if (opt[0] != optchr)
      pparam_pos++;
    else if (pparam_parseopt() < 0)
      return -1;
  }
  return 0;
}

#ifdef HSTART
static char **dupargv(const char **argv)
{
  if (argv == NULL)
    return NULL;

  int argc;

  /* the vector */
  for (argc = 0; argv[argc] != NULL; argc++)
    ;
  char **copy = (char **) malloc((argc + 2) * sizeof(char *));
  if (copy == NULL)
    return NULL;

  /* the strings */
  for (argc = 0; argv[argc] != NULL; argc++) {
    int len = strlen(argv[argc]);
    copy[argc] = (char *)malloc(sizeof(char) * (len + 1));
    strcpy(copy[argc], argv[argc]);
  }
  copy[argc] = NULL;
  return copy;
}

#endif

/****************************************************************************
 *
 * ARG
 *
 * The following module computes a whole bunch of miscellaneous values, which
 * are all constant throughout the program.  Naturally, this includes the
 * value of the command-line arguments.
 *
 *****************************************************************************/

#define MAX_LINE_LENGTH 1000

static const char **arg_argv;
static int arg_argc;

static int arg_requested_pes;
static int arg_requested_nodes;
static int arg_requested_numhosts;

static int arg_timeout;
static int arg_timelimit;
static int arg_verbose;
static const char *arg_nodelist;
static const char *arg_nodegroup;
static const char *arg_runscript; /* script to run the node-program with */
static const char *arg_charmrunip;

static int arg_debug;
static int arg_debug_no_pause;
static int arg_debug_no_xrdb;
static int arg_charmdebug;
static const char *
    arg_debug_commands; /* commands that are provided by a ++debug-commands
                           flag. These are passed into gdb. */

static int arg_quiet;       /* omit charmrun standard output */
static int arg_local;       /* start node programs directly by exec on localhost */
static int arg_batch_spawn; /* control starting node programs, several at a time */
static int arg_scalable_start;

#ifdef HSTART
static int arg_hierarchical_start;
static int arg_child_charmrun;
#endif
static int arg_help; /* print help message */
static int arg_ppn;  /* pes per node */
static int arg_usehostname;
static int arg_interactive; /* for charm4py interactive sessions when using ssh */

#if CMK_SHRINK_EXPAND
static char **saved_argv;
static int saved_argc;
static int arg_realloc_pes;
static int arg_old_pes;
static int arg_shrinkexpand;
static int arg_charmrun_port;
static const char *arg_shrinkexpand_basedir;
#endif

#if CMK_USE_SSH
static int arg_maxssh;
static const char *arg_shell;
static int arg_in_xterm;
static const char *arg_debugger;
static const char *arg_xterm;
static const char *arg_display;
static int arg_ssh_display;
static const char *arg_mylogin;
#endif
static int arg_mpiexec;
static int arg_mpiexec_no_n;
static int arg_va_rand;

static const char *arg_nodeprog_a;
static const char *arg_nodeprog_r;
static char *arg_currdir_a;
static char *arg_currdir_r;

static int arg_server;
static int arg_server_port = 0;
static const char *arg_server_auth = NULL;
static int replay_single = 0;


struct TopologyRequest
{
  int host, socket, core, pu;

  enum class Unit
  {
    Host,
    Socket,
    Core,
    PU,
    None,
  };

  int active() const
  {
    return (host > 0) + (socket > 0) + (core > 0) + (pu > 0);
  }

  Unit unit() const
  {
    if (host > 0)
      return Unit::Host;
    else if (socket > 0)
      return Unit::Socket;
    else if (core > 0)
      return Unit::Core;
    else if (pu > 0)
      return Unit::PU;
    else
      return Unit::None;
  }
};

TopologyRequest proc_per;
TopologyRequest onewth_per;
int auto_provision;

static void arg_init(int argc, const char **argv)
{
  static char buf[1024];

  int local_def = 0;
#if CMK_CHARMRUN_LOCAL
  local_def = 1; /*++local is the default*/
#endif

  pparam_int(&arg_requested_pes, 0, "p", "Number of PEs to create");
  pparam_int(&arg_requested_numhosts, 0, "numHosts", "Number of hosts to use from nodelist file");

  pparam_int(&arg_requested_nodes, 0, "n", "Number of processes to create");
  pparam_int(&arg_requested_nodes, 0, "np", "Number of processes to create");

  pparam_int(&arg_timeout, 60, "timeout",
             "Seconds to wait per host connection");
  pparam_int(&arg_timelimit, -1, "timelimit",
             "Seconds to wait for program to complete");
  pparam_flag(&arg_verbose, 0, "verbose", "Print diagnostic messages");
  pparam_flag(&arg_quiet, 0, "quiet", "Omit non-error runtime messages");
  pparam_str(&arg_nodelist, 0, "nodelist", "File containing list of physical nodes");
  pparam_str(&arg_nodegroup, "main", "nodegroup",
             "Which group of physical nodes to use");

#if CMK_CCS_AVAILABLE
  pparam_flag(&arg_server, 0, "server", "Enable client-server (CCS) mode");
  pparam_int(&arg_server_port, 0, "server-port",
             "Port to listen for CCS requests");
  pparam_str(&arg_server_auth, 0, "server-auth", "CCS Authentication file");
#endif
  pparam_flag(&arg_local, local_def, "local",
              "Start node programs locally without daemon");
  pparam_int(&arg_batch_spawn, 0, "batch", "Launch connections to this many "
                                           "node programs at a time, avoiding "
                                           "overloading charmrun PE");
#ifndef _WIN32
  pparam_flag(&arg_scalable_start, 1, "scalable-start", "Enable scalable start");
#endif
#ifdef HSTART
  pparam_flag(&arg_hierarchical_start, 0, "hierarchical-start",
              "hierarchical start");
  pparam_flag(&arg_child_charmrun, 0, "child-charmrun", "child charmrun");
#endif
#if CMK_SHRINK_EXPAND
  pparam_int(&arg_realloc_pes, 1, "newp", "New number of processes to create");
  pparam_int(&arg_old_pes, 1, "oldp", "Old number of processes to create");
  pparam_flag(&arg_shrinkexpand, 0, "shrinkexpand", "Enable shrink/expand support");
  pparam_int(&arg_charmrun_port, 0, "charmrun_port", "Make charmrun listen on this port");
#endif
  pparam_flag(&arg_interactive, 0, "interactive", "Force tty allocation for process 0 when using ssh");
  pparam_flag(&arg_usehostname, 0, "usehostname",
              "Send nodes our symbolic hostname instead of IP address");
  pparam_str(&arg_charmrunip, 0, "useip",
             "Use IP address provided for charmrun IP");
  pparam_flag(&arg_mpiexec, 0, "mpiexec", "Use mpiexec to start jobs");
  pparam_flag(&arg_mpiexec_no_n, 0, "mpiexec-no-n", "Use mpiexec to start jobs without -n procs");
#if CMK_USE_SSH
  pparam_flag(&arg_debug, 0, "debug",
              "Run each node under gdb in an xterm window");
  pparam_flag(&arg_debug_no_pause, 0, "debug-no-pause",
              "Like debug, except doesn't pause at beginning");
  pparam_str(&arg_debug_commands, 0, "debug-commands",
             "Commands to be run inside gdb at startup");
  pparam_flag(&arg_debug_no_xrdb, 0, "no-xrdb", "Don't check xrdb");

/* When the ++charmdebug flag is used, charmrun listens from its stdin for
   commands, and forwards them to the gdb info program (a child), or to the
   processor gdbs. The stderr is redirected to the stdout, so the two streams
   are mixed together. The channel for stderr is reused to forward the replies
   of gdb back to the java debugger. */
#if !defined(_WIN32)
  pparam_flag(&arg_charmdebug, 0, "charmdebug",
              "Used only when charmrun is started by charmdebug");
#endif

  pparam_int(&arg_maxssh, 16, "maxssh",
             "Maximum number of ssh's to run at a time");
  pparam_str(&arg_shell, 0, "remote-shell",
             "Which remote shell to use (default $CONV_RSH or " SSH_CMD ")");
  pparam_str(&arg_debugger, 0, "debugger", "Which debugger to use");
  pparam_str(&arg_display, 0, "display", "X Display for xterm");
  pparam_flag(&arg_ssh_display, 0, "ssh-display",
              "Use own X Display for each ssh session");
  pparam_flag(&arg_in_xterm, 0, "in-xterm", "Run each node in an xterm window");
  pparam_str(&arg_xterm, 0, "xterm", "Which xterm to use");
#endif
  pparam_str(&arg_runscript, 0, "runscript", "Script to run node-program with");
  pparam_flag(&arg_help, 0, "help", "Print help messages");
  pparam_int(&arg_ppn, 0, "ppn", "Number of PEs per Charm++ node (=OS process)");
  pparam_flag(&arg_va_rand, 0, "va-randomization",
              "Allows randomization of the virtual address space (ASLR)");

  // Process Binding Parameters
  pparam_int(&proc_per.host, 0,
             "processPerHost", "assign N processes per host");
  pparam_int(&proc_per.socket, 0,
             "processPerSocket", "assign N processes per socket");
  pparam_int(&proc_per.core, 0,
             "processPerCore", "assign N processes per core");
  pparam_int(&proc_per.pu, 0,
             "processPerPU", "assign N processes per PU");

  // Worker Thread Binding Parameters
  pparam_flag(&onewth_per.host, 0,
             "oneWthPerHost", "assign one worker thread per host");
  pparam_flag(&onewth_per.socket, 0,
             "oneWthPerSocket", "assign one worker thread per socket");
  pparam_flag(&onewth_per.core, 0,
             "oneWthPerCore", "assign one worker thread per core");
  pparam_flag(&onewth_per.pu, 0,
             "oneWthPerPU", "assign one worker thread per PU");

  pparam_flag(&auto_provision, 0, "auto-provision", "fully utilize available resources");
  pparam_flag(&auto_provision, 0, "autoProvision", "fully utilize available resources");

#if CMK_SHRINK_EXPAND
  /* move it to a function */
  saved_argc = argc;
  saved_argv = (char **) malloc(sizeof(char *) * (saved_argc));
  for (int i = 0; i < saved_argc; i++) {
    //  MACHSTATE1(2,"Parameters %s",Cmi_argvcopy[i]);
    saved_argv[i] = (char *) argv[i];
  }
#endif

  if (pparam_parsecmd('+', argv) < 0) {
    fprintf(stderr, "ERROR> syntax: %s\n", pparam_error);
    pparam_printdocs();
    exit(1);
  }

  if (arg_help) {
    pparam_printdocs();
    /*exit(0);*/
  }

  if ( arg_mpiexec_no_n ) arg_mpiexec = arg_mpiexec_no_n;

#if CMK_SHRINK_EXPAND
  if (arg_shrinkexpand) {
    arg_requested_pes = arg_realloc_pes;
    printf("\n \nCharmrun> %d Reallocated pes\n \n", arg_requested_pes);
  }
#endif

#ifdef HSTART
  if (arg_hierarchical_start && !arg_child_charmrun)
    arg_argv = (const char **)dupargv(argv);
  else
#endif
    arg_argv = argv + 1; /*Skip over charmrun (0) here and program name (1) later*/
  arg_argc = pparam_countargs(arg_argv);
  if (arg_argc < 1) {
    if (!arg_help)
    {
      fprintf(stderr, "ERROR> You must specify a node-program.\n");
      pparam_printdocs();
    }
    exit(1);
  }

#ifdef HSTART
  if (!arg_hierarchical_start || arg_child_charmrun) {
    // Removing nodeprogram from the list
    arg_argv++;
    arg_argc--;
  } else {
    // Removing charmrun from parameters
    arg_argv++;
    arg_argc--;

    arg_argv[arg_argc++] = "++child-charmrun";
    arg_argv[arg_argc] = NULL;
  }
#else
  arg_argv++;
  arg_argc--;
#endif

  if (arg_server_port || arg_server_auth)
    arg_server = 1;

  if (arg_verbose) arg_quiet = 0;

  if (arg_debug || arg_debug_no_pause
#if CMK_USE_SSH
      || arg_in_xterm
#endif
     ) {
    fprintf(stderr, "Charmrun> scalable start disabled under ++debug and ++in-xterm:\n"
                    "NOTE: will make an SSH connection per process launched,"
                    " instead of per physical node.\n");
    arg_scalable_start = 0;
    arg_quiet = 0;
    arg_verbose = 1;
    if (arg_debug || arg_debug_no_pause)
    {
      /*Pass ++debug along to program (used by machine.C)*/
      arg_argv[arg_argc++] = "++debug";
    }
  }
  /* pass ++quiet to program */
  if (arg_quiet) arg_argv[arg_argc++] = "++quiet";

  /* Check for +replay-detail to know we have to load only one single processor
   */
  for (int i = 0; argv[i]; i++) {
    if (0 == strcmp(argv[i], "+replay-detail")) {
      replay_single = 1;
      arg_requested_pes = 1;
    }
  }


#if CMK_USE_SSH
  /* Find the current value of the CONV_RSH variable */
  if (!arg_shell) {
    if (arg_mpiexec)
      arg_shell = "mpiexec";
    else
      arg_shell = getenv_ssh();
  }

#if !defined(_WIN32)
  /* Find the current value of the DISPLAY variable */
  if (!arg_display)
    arg_display = getenv_display_no_tamper();
#endif

  if ((arg_debug || arg_debug_no_pause || arg_in_xterm) && (arg_display == 0)) {
    fprintf(stderr, "ERROR> DISPLAY must be set to use debugging mode\n");
    exit(1);
  }
  if (arg_debug || arg_debug_no_pause)
    arg_timeout = 8 * 60 * 60; /* Wait 8 hours for ++debug */

  /* default debugger is gdb */
  if (!arg_debugger)
#ifdef __APPLE__
    arg_debugger = "lldb";
#else
    arg_debugger = "gdb";
#endif
  /* default xterm is xterm */
  if (!arg_xterm)
    arg_xterm = "xterm";

  arg_mylogin = mylogin();
#endif

  /* find the current directory, absolute version */
  if (getcwd(buf, 1023) == NULL) {
    fprintf(stderr, "charmrun> getcwd() failed!\n");
    exit(1);
  }
  arg_currdir_a = strdup(buf);

  /* find the node-program, absolute version */
  arg_nodeprog_r = argv[1];

  if (arg_nodeprog_r[0] == '-' || arg_nodeprog_r[0] == '+') {
    /*If it starts with - or +, it ain't a node program.
      Chances are, the user screwed up and passed some
      unknown flag to charmrun*/
    fprintf(stderr, "Charmrun does not recognize the flag '%s'.\n", arg_nodeprog_r);
    if (arg_nodeprog_r[0] == '+')
      fprintf(stderr, "Charm++'s flags need to be placed *after* the program name.\n");
    pparam_printdocs();
    exit(1);
  }

#if defined(_WIN32)
  if (argv[1][1] == ':' ||
      argv[1][0] == '\\' && argv[1][1] == '\\') { /*E.g.: "C:\foo\bar.exe*/
#else
  if (argv[1][0] == '/') { /*E.g.: "\foo\bar"*/
#endif
    /*Absolute path to node-program*/
    arg_nodeprog_a = argv[1];
  } else {
    sprintf(buf, "%s%s%s", arg_currdir_a, DIRSEP, arg_nodeprog_r);
    arg_nodeprog_a = strdup(buf);
  }
  if (arg_scalable_start) {
    PRINT(("Charmrun> scalable start enabled. \n"));
  }

#ifdef HSTART
  if (arg_hierarchical_start) {
    PRINT(("Charmrun> Hierarchical scalable start enabled. \n"));
    if (arg_debug || arg_debug_no_pause) {
      fprintf(stderr, "Charmrun> Error: ++hierarchical-start does not support "
                      "debugging mode. \n");
      exit(1);
    }
    if (arg_verbose) {
      fprintf(stderr, "Charmrun> Warning: you have enabled verbose output with "
                      "Hierarchical startup, you may get inconsistent verbose "
                      "outputs. \n++hierarchial-start does not support verbose "
                      "mode. \n");
    }

  } else if (arg_child_charmrun) {
    fprintf(
        stderr,
        "Charmrun> Error: ++child-charmrun is not a user-specified flag. \n");
    exit(1);
  }
#endif

  const int proc_active = proc_per.active();
  const int onewth_active = onewth_per.active();
  if (proc_active || onewth_active || auto_provision)
  {
    if (arg_requested_pes != 0)
    {
      fprintf(stderr, "Charmrun> Error: +p cannot be used with ++(process|oneWth)Per* or ++auto-provision.\n");
      exit(1);
    }

    if (proc_active && arg_requested_nodes > 0)
    {
      fprintf(stderr, "Charmrun> Error: +n/++np cannot be used with ++processPer* or ++auto-provision.\n");
      exit(1);
    }

    if (proc_active && arg_mpiexec)
    {
      fprintf(stderr, "Charmrun> Error: ++mpiexec and ++processPer* cannot be used together.\n");
      exit(1);
    }

    if (proc_active + (auto_provision > 0) > 1)
    {
      fprintf(stderr, "Charmrun> Error: Only one of ++processPer(Host|Socket|Core|PU) or ++auto-provision is allowed.\n");
      exit(1);
    }

#if CMK_SMP
    if (onewth_active + (arg_ppn > 0) + (auto_provision > 0) > 1)
    {
      fprintf(stderr, "Charmrun> Error: Only one of ++oneWthPer(Host|Socket|Core|PU), ++ppn, or ++auto-provision is allowed.\n");
      exit(1);
    }

    using Unit = typename TopologyRequest::Unit;

    const Unit proc_unit = proc_per.unit();
    const Unit onewth_unit = onewth_per.unit();

    if ((onewth_unit == Unit::Host && (proc_unit == Unit::Socket || proc_unit == Unit::Core || proc_unit == Unit::PU)) ||
        (onewth_unit == Unit::Socket && (proc_unit == Unit::Core || proc_unit == Unit::PU)) ||
        (onewth_unit == Unit::Core && proc_unit == Unit::PU))
    {
      fprintf(stderr, "Charmrun> Error: Cannot request processes on a smaller unit than that requested for worker threads.\n");
      exit(1);
    }

    if ((onewth_unit == Unit::Host && proc_unit == Unit::Host && proc_per.host > 1) ||
        (onewth_unit == Unit::Socket && proc_unit == Unit::Socket && proc_per.socket > 1) ||
        (onewth_unit == Unit::Core && proc_unit == Unit::Core && proc_per.core > 1) ||
        (onewth_unit == Unit::PU && proc_unit == Unit::PU && proc_per.pu > 1))
    {
      fprintf(stderr, "Charmrun> Error: Cannot request more processes than worker threads per unit.\n");
      exit(1);
    }
#endif
  }
  else
  {
#if CMK_SMP
    if (arg_requested_pes > 0 && arg_requested_nodes > 0 && arg_ppn > 0 && arg_ppn * arg_requested_nodes != arg_requested_pes)
    {
      fprintf(stderr, "Charmrun> Error: ++np times ++ppn does not equal +p.\n");
      exit(1);
    }

    if (arg_requested_pes > 0 && arg_ppn > 0 && arg_requested_pes % arg_ppn != 0)
    {
      if (arg_ppn > arg_requested_pes)
      {
        arg_ppn = arg_requested_pes;
        fprintf(stderr, "Charmrun> Warning: forced ++ppn = +p = %d\n", arg_ppn);
      }
      else
      {
        fprintf(stderr, "Charmrun> Error: ++ppn (number of PEs per node) does not divide +p (number of PEs).\n");
        exit(1);
      }
    }
#else
    if (arg_requested_pes > 0 && arg_requested_nodes > 0 && arg_requested_pes != arg_requested_nodes)
    {
      fprintf(stderr, "Charmrun> Error: +p and ++np do not agree.\n");
      exit(1);
    }
#endif
  }

#if !CMK_SMP
  if (arg_ppn > 1 || onewth_active)
  {
    fprintf(stderr, "Charmrun> Error: ++oneWthPer(Host|Socket|Core|PU) and ++ppn are only available in SMP mode.\n");
    exit(1);
  }
#endif

  if (auto_provision)
  {
#if CMK_SMP
    proc_per.socket = 1;
    onewth_per.pu = 1;
#else
    proc_per.core = 1;
#endif
  }
  else if (arg_requested_pes <= 0 && arg_requested_nodes <= 0 && arg_ppn <= 0 && !proc_active && !onewth_active)
  {
    PRINT(("Charmrun> No provisioning arguments specified. Running with a single PE.\n"
           "          Use ++auto-provision to fully subscribe resources or +p1 to silence this message.\n"));
  }
}

/****************************************************************************
 *
 * NODETAB:  The nodes file and nodes table.
 *
 ****************************************************************************/

static int portOk = 1;
static const char *nodetab_tempName = NULL;
static char *nodetab_file_find()
{
  char buffer[MAXPATHLEN];

  /* Find a nodes-file as specified by ++nodelist */
  if (arg_nodelist) {
    const char *path = arg_nodelist;
    if (probefile(path))
      return strdup(path);
    fprintf(stderr, "ERROR> No such nodelist file %s\n", path);
    exit(1);
  }
  /* Find a nodes-file as specified by getenv("NODELIST") */
  if (getenv("NODELIST")) {
    char *path = getenv("NODELIST");
    if (path && probefile(path))
      return strdup(path);
    // cppcheck-suppress nullPointer
    fprintf(stderr, "ERROR> Cannot find nodelist file %s\n", path);
    exit(1);
  }
  /* Find a nodes-file by looking under 'nodelist' in the current directory */
  if (probefile("./nodelist"))
    return strdup("./nodelist");
#if defined(_WIN32)
  tmpnam(buffer);
  nodetab_tempName = strdup(buffer);
#else /*UNIX*/
  if (getenv("HOME")) {
    sprintf(buffer, "%s/.nodelist", getenv("HOME"));
  }
#endif
  if (!probefile(buffer)) {
    /*Create a simple nodelist in the user's home*/
    FILE *f = fopen(buffer, "w");
    if (f == NULL) {
      fprintf(stderr, "ERROR> Cannot create a 'nodelist' file.\n");
      exit(1);
    }
    fprintf(f, "group main\nhost localhost\n");
    fclose(f);
  }
  return strdup(buffer);
}

struct nodetab_host
{
  static skt_ip_t resolve(const char *name);

  double speed = 1.0; /*Relative speed of each CPU*/

  const char *name = "SET_H->NAME"; /*Host DNS name*/
#if CMK_USE_SSH
  const char *shell = arg_shell;    /*Ssh to use*/
  const char *debugger = arg_debugger; /*Debugger to use*/
  const char *xterm = arg_xterm;    /*Xterm to use*/
  const char *login = arg_mylogin;    /*User login name to use*/
  const char *passwd = "*";   /*User login password*/
  const char *setup = "*";    /*Commands to execute on login*/
#endif
  char *ext = nullptr;        /* Command suffix */
  pathfixlist pathfixes = nullptr;

  skt_ip_t ip = _skt_invalid_ip;      /*IP address of host*/
  int cpus = 1;     /* # of physical CPUs*/
  int nice = -100;     /* process priority */
//  int forks = 0;    /* number of processes to fork on remote node */

  int processes = 0;

  int hostno = 0;

#ifdef __FAULT__
  bool crashed = false;
#endif
};

skt_ip_t nodetab_host::resolve(const char *name)
{
  skt_ip_t ip = skt_innode_lookup_ip(name);
  if (skt_ip_match(ip, _skt_invalid_ip)) {
    fprintf(stderr, "ERROR> Cannot obtain IP address of %s\n", name);
    exit(1);
  }

  return ip;
}

struct nodetab_process
{
#if CMK_USE_IBVERBS
  ChInfiAddr *qpList = nullptr; /* An array of queue pair identifiers */
  ChInfiAddr *qpData = nullptr;
#endif
#if CMK_USE_IBUD
  ChInfiAddr qp;
#endif

  nodetab_host * host;
  int rank = 0;     /*Rank of this CPU*/

  int ssh_pid = 0;
  SOCKET req_client = -1; /*TCP request sockets for each node*/
  // ^ aka ctrlfd /*Connection to control port*/
  int dataport = -1;  /*UDP port number*/
#ifdef HSTART
  SOCKET charmrun_fds = -1;
#endif

  ChNodeinfo info;

  int num_pus;
  int num_cores;
  int num_sockets;

  int forkstart = 0;

  int PEs = 0;

  int nodeno = 0;

  friend bool operator< (const nodetab_process &, const nodetab_process &);
};

bool operator< (const nodetab_process & a, const nodetab_process & b)
{
  const int a_hostno = a.host->hostno, b_hostno = b.host->hostno;
  return a_hostno < b_hostno || (a_hostno == b_hostno && a.nodeno < b.nodeno);
}

static std::vector<nodetab_host *> host_table;
#ifdef HSTART
static std::vector<nodetab_host *> my_host_table;
#else
# define my_host_table host_table
#endif
static std::vector<nodetab_process> my_process_table;
static std::vector<nodetab_process *> pe_to_process_map;

static const char *nodetab_args(const char *args, nodetab_host *h)
{
  while (*args != 0)
  {
    const char *b1 = skipblanks(args), *e1 = skipstuff(b1);
    const char *b2 = skipblanks(e1), *e2 = skipstuff(b2);

    while (*b1 == '+')
      b1++; /*Skip over "++" on parameters*/

    if (subeqs(b1, e1, "speed"))
      h->speed = atof(b2);
    else if (subeqs(b1, e1, "cpus"))
      h->cpus = atol(b2);
    else if (subeqs(b1, e1, "pathfix"))
    {
      const char *b3 = skipblanks(e2), *e3 = skipstuff(b3);
      args = skipblanks(e3);
      h->pathfixes =
          pathfix_append(substr(b2, e2), substr(b3, e3), h->pathfixes);
      e2 = e3; /* for the skipblanks at the end */
    }
    else if (subeqs(b1, e1, "ext"))
      h->ext = substr(b2, e2);
    else if (subeqs(b1, e1, "nice"))
      h->nice = atoi(b2);
#if CMK_USE_SSH
    else if (subeqs(b1, e1, "login"))
      h->login = substr(b2, e2);
    else if (subeqs(b1, e1, "passwd"))
      h->passwd = substr(b2, e2);
    else if (subeqs(b1, e1, "setup"))
      h->setup = strdup(b2);
    else if (subeqs(b1, e1, "shell"))
      h->shell = substr(b2, e2);
    else if (subeqs(b1, e1, "debugger"))
      h->debugger = substr(b2, e2);
    else if (subeqs(b1, e1, "xterm"))
      h->xterm = substr(b2, e2);
#endif
    else
      return args;

    args = skipblanks(e2);
  }

  return args;
}

/* setup nodetab as localhost only */
static void nodetab_init_for_local()
{

  static const char hostname[] = "127.0.0.1";
  nodetab_host * h = new nodetab_host{};
  h->name = hostname; // should strdup if leaks are fixed
  h->ip = nodetab_host::resolve(hostname);
  host_table.push_back(h);
}

#ifdef HSTART
/* Sets the parent field of hosts to point to their parent charmrun. The root
 * charmrun will create children for all hosts which are parent of at least one
 * other host*/
static int branchfactor;
static int nodes_per_child;
static void nodetab_init_hierarchical_start(void)
{
  TODO;
  branchfactor = ceil(sqrt(nodetab_rank0_size));
  nodes_per_child = round(nodetab_rank0_size * 1.0 / branchfactor);
}
#endif

static void nodetab_init_with_nodelist()
{

  /* Open the NODES_FILE. */
  char *nodesfile = nodetab_file_find();
  if (arg_verbose)
    printf("Charmrun> using %s as nodesfile\n", nodesfile);

  FILE *f;
  if (!(f = fopen(nodesfile, "r"))) {
    fprintf(stderr, "ERROR> Cannot read %s: %s\n", nodesfile, strerror(errno));
    exit(1);
  }
  free(nodesfile);

  nodetab_host global, group;
  int rightgroup = (strcmp(arg_nodegroup, "main") == 0);

  /* Store the previous host so we can make sure we aren't mixing localhost and
   * non-localhost */
  char *prevHostName = NULL;
  char input_line[MAX_LINE_LENGTH];
  std::unordered_map<std::string, nodetab_host *> temp_hosts;
  int lineNo = 1;
  int hostno = 0;

  while (fgets(input_line, sizeof(input_line) - 1, f) != 0) {
    if (input_line[0] == '#')
      continue;
    zap_newline(input_line);
    if (!nodetab_args(input_line, &global)) {
      /*An option line-- also add options to current group*/
      nodetab_args(input_line, &group);
    } else { /*Not an option line*/
      const char *b1 = skipblanks(input_line), *e1 = skipstuff(b1);
      const char *b2 = skipblanks(e1), *e2 = skipstuff(b2);
      const char *b3 = skipblanks(e2);
      if (subeqs(b1, e1, "host")) {
        if (rightgroup) {
          /* check if we have a previous host, if it's different than our
           * current host, and if one of them is localhost */
          if (prevHostName && strcmp(b2, prevHostName) &&
              (!strcmp(b2, "localhost") ||
               !strcmp(prevHostName, "localhost"))) {
            fprintf(stderr, "ERROR> Mixing localhost with other hostnames will "
                            "lead to connection failures.\n");
            fprintf(stderr, "ERROR> The problematic line in group %s is: %s\n",
                    arg_nodegroup, input_line);
            exit(1);
          }

          const std::string hostname = substr(b2, e2);
          auto host_iter = temp_hosts.find(hostname);
          if (host_iter != temp_hosts.end())
          {
            nodetab_host *host = (*host_iter).second;
            nodetab_args(b3, host);
          }
          else
          {
            nodetab_host *host = new nodetab_host{group};
            host->name = strdup(hostname.c_str());
            host->ip = nodetab_host::resolve(hostname.c_str());
            host->hostno = hostno++;
            temp_hosts.insert({hostname, host});
            nodetab_args(b3, host);
          }

          free(prevHostName);
          prevHostName = strdup(b2);
        }
      } else if (subeqs(b1, e1, "group")) {
        group = global;
        nodetab_args(b3, &group);
        rightgroup = subeqs(b2, e2, arg_nodegroup);
      } else if (b1 != b3) {
        fprintf(stderr, "ERROR> unrecognized command in nodesfile:\n");
        fprintf(stderr, "ERROR> %s\n", input_line);
        exit(1);
      }
    }
    lineNo++;
  }
  fclose(f);
  free(prevHostName);
  if (nodetab_tempName != NULL)
    unlink(nodetab_tempName);

  const size_t temp_hosts_size = temp_hosts.size();
  if (temp_hosts_size == 0) {
    fprintf(stderr, "ERROR> No hosts in group %s\n", arg_nodegroup);
    exit(1);
  }

  host_table.resize(temp_hosts_size);
  for (const auto & h_pair : temp_hosts)
  {
    nodetab_host * h = h_pair.second;
    host_table[h->hostno] = h;
  }
}

static void nodetab_init()
{
  /* if arg_local is set, ignore the nodelist file */
  if (arg_local || arg_mpiexec)
    nodetab_init_for_local();
  else
    nodetab_init_with_nodelist();
}

/****************************************************************************
 *
 * Nodeinfo
 *
 * The global list of node PEs, IPs, and port numbers.
 * Stored in ChMachineInt_t format so the table can easily be sent
 * back to the nodes.
 *
 ****************************************************************************/

static void nodeinfo_add(const ChSingleNodeinfo *in, nodetab_process & p)
{
  const int node = ChMessageInt(in->nodeNo);
  if (node != p.nodeno)
    fprintf(stderr, "Charmrun> Warning: Process #%d received ChSingleNodeInfo #%d\n", p.nodeno, node);

  p.info = in->info;
  p.num_pus = ChMessageInt(in->num_pus);
  p.num_cores = ChMessageInt(in->num_cores);
  p.num_sockets = ChMessageInt(in->num_sockets);
}

static void nodeinfo_populate(nodetab_process & p)
{
  ChNodeinfo & i = p.info;
  const int node = p.nodeno;

  i.nodeno = ChMessageInt_new(node);
  i.nPE = ChMessageInt_new(p.PEs);
  i.nProcessesInPhysNode = ChMessageInt_new(p.host->processes);

  if (arg_mpiexec)
    p.host->ip = i.IP; /* get IP */
  else
    i.IP = p.host->ip;

#if !CMK_USE_IBVERBS
  unsigned int dataport = ChMessageInt(i.dataport);
  if (0 == dataport) {
    fprintf(stderr, "Node %d could not initialize network!\n", node);
    exit(1);
  }
  p.dataport = dataport;
  if (arg_verbose) {
    char ips[200];
    skt_print_ip(ips, i.IP);
    printf("Charmrun> client %d connected (IP=%s data_port=%d)\n", node, ips,
           dataport);
  }
#endif
}

/****************************************************************************
 *
 * input handling
 *
 * You can use this module to read the standard input.  It supports
 * one odd function, input_scanf_chars, which is what makes it useful.
 * if you use this module, you may not read stdin yourself.
 *
 * void input_init(void)
 * char *input_gets(void)
 * char *input_scanf_chars(char *fmt)
 *
 ****************************************************************************/

static char *input_buffer;

static void input_extend()
{
  char line[1024];
  int len = input_buffer ? strlen(input_buffer) : 0;
  fflush(stdout);
  if (fgets(line, 1023, stdin) == 0) {
    fprintf(stderr, "end-of-file on stdin");
    exit(1);
  }
  char *new_input_buffer = (char *) realloc(input_buffer, len + strlen(line) + 1);
  if (new_input_buffer == NULL) {
    // could not realloc
    free(input_buffer);
    fprintf(stderr, "Charmrun: Realloc failed");
    exit(1);
  } else {
    input_buffer = new_input_buffer;
  }

  strcpy(input_buffer + len, line);
}

static void input_init() { input_buffer = strdup(""); }

static char *input_extract(int nchars)
{
  char *res = substr(input_buffer, input_buffer + nchars);
  char *tmp =
      substr(input_buffer + nchars, input_buffer + strlen(input_buffer));
  free(input_buffer);
  input_buffer = tmp;
  return res;
}

static char *input_gets()
{
  char *p;
  while (1) {
    p = strchr(input_buffer, '\n');
    if (p)
      break;
    input_extend();
  }
  int len = p - input_buffer;
  char *res = input_extract(len + 1);
  res[len] = 0;
  return res;
}

/*FIXME: I am terrified by this routine. OSL 9/8/00*/
static char *input_scanf_chars(char *fmt)
{
  char buf[8192];
  static int fd;
  static FILE *file;
  fflush(stdout);
  if (file == 0) {
#if CMK_USE_MKSTEMP
    char tmp[128];
    strcpy(tmp, "/tmp/fnordXXXXXX");
    if (mkstemp(tmp) == -1) {
      fprintf(stderr, "charmrun> mkstemp() failed!\n");
      exit(1);
    }
#else
    char *tmp = tmpnam(NULL); /*This was once /tmp/fnord*/
#endif
    unlink(tmp);
    fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
      fprintf(stderr, "cannot open temp file /tmp/fnord");
      exit(1);
    }
    file = fdopen(fd, "r+");
    unlink(tmp);
  }
  int pos;
  while (1) {
    int len = strlen(input_buffer);
    rewind(file);
    fwrite(input_buffer, len, 1, file);
    fflush(file);
    rewind(file);
    if (ftruncate(fd, len)) {
      fprintf(stderr, "charmrun> ftruncate() failed!\n");
      exit(1);
    }
    if (fscanf(file, fmt, buf, buf, buf, buf, buf, buf, buf, buf, buf, buf, buf,
           buf, buf, buf, buf, buf, buf, buf) <= 0) {
      fprintf(stderr, "charmrun> fscanf() failed!\n");
      exit(1);
    }
    pos = ftell(file);
    if (pos < len)
      break;
    input_extend();
  }
  return input_extract(pos);
}

/***************************************************************************
CCS Interface:
  Charmrun forwards CCS requests on to the node-programs' control
sockets.
***************************************************************************/

#if CMK_CCS_AVAILABLE

/*The Ccs Server socket became active--
rec'v the message and respond to the request,
by forwarding the request to the appropriate node.
 */
static void req_ccs_connect(void)
{
  struct {
    ChMessageHeader ch; /*Make a charmrun header*/
    CcsImplHeader hdr;  /*Ccs internal header*/
  } h;
  void *reqData; /*CCS request data*/
  if (0 == CcsServer_recvRequest(&h.hdr, &reqData))
    return; /*Malformed request*/
  int pe = ChMessageInt(h.hdr.pe);
  int reqBytes = ChMessageInt(h.hdr.len);

  if (pe == -1) {
    /*Treat -1 as broadcast and sent to 0 as root of the spanning tree*/
    pe = 0;
  }
  const int pe_count = pe_to_process_map.size();
  if ((pe <= -pe_count || pe >= pe_count) && 0 == replay_single) {
/*Treat out of bound values as errors. Helps detecting bugs*/
/* But when virtualized with Bigemulator, we can have more pes than nodetabs */
/* TODO: We should somehow check boundaries also for bigemulator... */
    if (pe == -pe_count)
      fprintf(stderr, "Invalid processor index in CCS request: are you trying "
                      "to do a broadcast instead?");
    else
      fprintf(stderr, "Invalid processor index in CCS request.");
    CcsServer_sendReply(&h.hdr, 0, 0);
    free(reqData);
    return;
  } else if (pe < -1) {
    /*Treat negative values as multicast to a number of processors specified by
      -pe.
      The pes to multicast to follows sits at the beginning of reqData*/
    reqBytes -= pe * sizeof(ChMessageInt_t);
    pe = ChMessageInt(*(ChMessageInt_t *) reqData);
  }

  if (!check_stdio_header(&h.hdr)) {

#define LOOPBACK 0
#if LOOPBACK /*Immediately reply "there's nothing!" (for performance           \
                testing)*/
    CcsServer_sendReply(&h.hdr, 0, 0);
#else
    int destpe = pe;
    if (replay_single)
      destpe = 0;
    /*Fill out the charmrun header & forward the CCS request*/
    ChMessageHeader_new("req_fw", sizeof(h.hdr) + reqBytes, &h.ch);

    const void *bufs[3];
    size_t lens[3];
    bufs[0] = &h;
    lens[0] = sizeof(h);
    bufs[1] = reqData;
    lens[1] = reqBytes;
    const SOCKET ctrlfd = pe_to_process_map[pe]->req_client;
    skt_sendV(ctrlfd, 2, bufs, lens);

#endif
  }
  free(reqData);
}

/*
Forward the CCS reply (if any) from this client back to the
original network requestor, on the original request socket.
 */
static int req_ccs_reply_fw(ChMessage *msg, SOCKET srcFd)
{
  int len = msg->len; /* bytes of data remaining to receive */

  /* First pull down the CCS header sent by the client. */
  CcsImplHeader hdr;
  skt_recvN(srcFd, &hdr, sizeof(hdr));
  len -= sizeof(hdr);

#define m (4 * 1024)              /* packets of message to recv/send at once */
  if (len < m || hdr.attr.auth) { /* short or authenticated message: grab the
                                     whole thing first */
    void *data = malloc(len);
    skt_recvN(srcFd, data, len);
    CcsServer_sendReply(&hdr, len, data);
    free(data);
  } else { /* long messages: packetize (for pipelined sending; a 2x bandwidth
              improvement!) */
    ChMessageInt_t outLen;
    int destFd; /* destination for data */
    skt_abortFn old = skt_set_abort(reply_abortFn);
    int destErrs = 0;

    destFd = ChMessageInt(hdr.replyFd);
    outLen = ChMessageInt_new(len);
    skt_sendN(destFd, &outLen, sizeof(outLen)); /* first comes the length */
    while (len > 0) {
      char buf[m];
      int r = m;
      if (r > len)
        r = len;
      skt_recvN(srcFd, buf, r);
      if (0 == destErrs) /* don't keep sending to dead clients, but *do* clean
                            out srcFd */
        destErrs |= skt_sendN(destFd, buf, r);
      len -= m;
#undef m
    }
    skt_close(destFd);

    skt_set_abort(old);
  }
  return 0;
}

#else
static int req_ccs_reply_fw(ChMessage *msg, SOCKET srcFd) {}
#endif /*CMK_CCS_AVAILABLE*/

/****************************************************************************
 *
 * REQUEST SERVICER
 *
 * The request servicer accepts connections on a TCP port.  The client
 * sends a sequence of commands (each is one line).  It then closes the
 * connection.  The server must then contact the client, sending replies.
 *
 ****************************************************************************/
/** Macro to switch on the case when charmrun stays up even if
one of the processor crashes*/
/*#define __FAULT__*/

static int req_ending = 0;

/* socket and std streams for the gdb info program */
static int gdb_info_pid = 0;
static int gdb_info_std[3];
static FILE *gdb_stream = NULL;

#define REQ_OK 0
#define REQ_FAILED -1

#ifdef HSTART
static int req_reply(SOCKET fd, const char *type, const char *data, int dataLen);
static int req_reply_child(SOCKET fd, const char *type, const char *data, int dataLen)
{

  int status = req_reply(fd, type, data, dataLen);
  if (status != REQ_OK)
    return status;
  SOCKET clientFd;
  skt_recvN(fd, (char *) &clientFd, sizeof(SOCKET));
  skt_sendN(fd, (const char *) &clientFd, sizeof(fd));
  return status;
}
#endif
/**
 * @brief This is the only place where charmrun talks back to anyone.
 */
static int req_reply(SOCKET fd, const char *type, const char *data, int dataLen)
{
  ChMessageHeader msg;
  if (fd == INVALID_SOCKET)
    return REQ_FAILED;
  ChMessageHeader_new(type, dataLen, &msg);
  skt_sendN(fd, (const char *) &msg, sizeof(msg));
  skt_sendN(fd, data, dataLen);
  return REQ_OK;
}

static void kill_all_compute_nodes(const char *msg, size_t msgSize)
{
  ChMessageHeader hdr;
  ChMessageHeader_new("die", msgSize, &hdr);
  for (const nodetab_process & p : my_process_table)
  {
      skt_sendN(p.req_client, (const char *) &hdr, sizeof(hdr));
      skt_sendN(p.req_client, msg, msgSize);
  }
}

static void kill_all_compute_nodes(const char *msg)
{
  return kill_all_compute_nodes(msg, strlen(msg)+1);
}

template <size_t msgSize>
static inline void kill_all_compute_nodes(const char msg[msgSize])
{
  return kill_all_compute_nodes(msg, msgSize);
}

/* Request handlers:
When a client asks us to do something, these are the
routines that actually respond to the request.
*/
/*Stash this new node's control and data ports.
 */
static int req_handle_initnode(ChMessage *msg, nodetab_process & p)
{
  if (msg->len != sizeof(ChSingleNodeinfo)) {
    fprintf(stderr, "Charmrun: Bad initnode data length. Aborting\n");
    fprintf(stderr, "Charmrun: possibly because: %s.\n", msg->data);
    exit(1);
  }

  nodeinfo_add((ChSingleNodeinfo *) msg->data, p);
  return REQ_OK;
}

#if CMK_USE_IBVERBS || CMK_USE_IBUD
static int req_handle_qplist(ChMessage *msg, nodetab_process & p)
{
#if CMK_USE_IBVERBS
  const int my_process_count = my_process_table.size();
	int qpListSize = (my_process_count-1) * sizeof(ChInfiAddr);

  if (msg->len != qpListSize)
  {
    fprintf(stderr, "Charmrun: Bad qplist data length. Aborting.\n");
    exit(1);
  }

  p.qpList = (ChInfiAddr *) malloc(qpListSize);
  memcpy(p.qpList, msg->data, qpListSize);
#elif CMK_USE_IBUD
  if (msg->len != sizeof(ChInfiAddr))
  {
    fprintf(stderr, "Charmrun: Bad qplist data length. Aborting.\n");
    exit(1);
  }

  p.qp = *(ChInfiAddr *)msg->data;
  printf("Charmrun> client %d lid=%d qpn=%i psn=%i\n", node,
         ChMessageInt(p.qp.lid), ChMessageInt(p.qp.qpn),
         ChMessageInt(p.qp.psn));
#endif
  return REQ_OK;
}
#endif

/**
 * @brief Gets the array of node numbers, IPs, and ports. This is used by the
 * node-programs
 * to talk to one another.
 */
static void req_send_initnodetab_internal(const nodetab_process & destination, int count, int msgSize)
{
  ChMessageHeader hdr;
  ChMessageInt_t nNodes = ChMessageInt_new(count);
  ChMessageInt_t nodeno = ChMessageInt_new(destination.nodeno);
  ChMessageHeader_new("initnodetab", msgSize, &hdr);
  const SOCKET fd = destination.req_client;
  skt_sendN(fd, (const char *) &hdr, sizeof(hdr));
  skt_sendN(fd, (const char *) &nNodes, sizeof(nNodes));
  skt_sendN(fd, (const char *) &nodeno, sizeof(nodeno));
  for (const nodetab_process & p : my_process_table)
    skt_sendN(fd, (const char *) &p.info, sizeof(ChNodeinfo));
}

static void req_send_initnodetab(const nodetab_process & destination)
{
  const int my_process_count = my_process_table.size();
  int msgSize = sizeof(ChMessageInt_t) * ChInitNodetabFields +
                sizeof(ChNodeinfo) * my_process_count;
  req_send_initnodetab_internal(destination, my_process_count, msgSize);
}

#ifdef HSTART
/* Used for fault tolerance with hierarchical start */
static int req_send_initnodetab1(SOCKET fd)
{
  const int my_process_count = my_process_table.size();
  ChMessageHeader hdr;
  ChMessageInt_t nNodes = ChMessageInt_new(my_process_count);
  ChMessageHeader_new("initnttab", sizeof(ChMessageInt_t) +
                                   sizeof(ChNodeinfo) * my_process_count,
                      &hdr);
  skt_sendN(fd, (const char *) &hdr, sizeof(hdr));
  skt_sendN(fd, (const char *) &nNodes, sizeof(nNodes));
  for (const nodetab_process & p : my_process_table)
    skt_sendN(fd, (const char *) p.info, sizeof(ChNodeinfo));

  return REQ_OK;
}
/*Get the array of node numbers, IPs, and ports.
This is used by the node-programs to talk to one another.
*/
static int parent_charmrun_fd = -1;
static int req_handle_initnodedistribution(ChMessage *msg, const nodetab_process & p)
{
  const int nodetab_rank0_size = nodetab_rank0_table.size();
  int nodes_to_fork =
      nodes_per_child; /* rounding should help in better load distribution*/
  int rank0_start = nodetab_rank0_table[client * nodes_per_child];
  int rank0_finish;
  if (client == branchfactor - 1) {
    nodes_to_fork = nodetab_rank0_table.size() - client * nodes_per_child;
    rank0_finish = nodetab_rank0_size;
  } else
    rank0_finish =
        nodetab_rank0_table[client * nodes_per_child + nodes_to_fork];

  ChMessageInt_t *nodemsg = (ChMessageInt_t *) malloc(
      (rank0_finish - rank0_start) * sizeof(ChMessageInt_t));
  for (int k = 0; k < rank0_finish - rank0_start; k++)
    nodemsg[k] = ChMessageInt_new(nodetab_rank0_table[rank0_start + k]);
  ChMessageHeader hdr;
  ChMessageInt_t nNodes = ChMessageInt_new(rank0_finish - rank0_start);
  ChMessageInt_t nTotalNodes = ChMessageInt_new(nodetab_rank0_size);
  ChMessageHeader_new("initnodetab",
                      sizeof(ChMessageInt_t) * 2 +
                          sizeof(ChMessageInt_t) * (rank0_finish - rank0_start),
                      &hdr);
  const SOCKET fd = p.charmrun_fds;
  skt_sendN(fd, (const char *) &hdr, sizeof(hdr));
  skt_sendN(fd, (const char *) &nNodes, sizeof(nNodes));
  skt_sendN(fd, (const char *) &nTotalNodes, sizeof(nTotalNodes));
  skt_sendN(fd, (const char *) nodemsg,
            (rank0_finish - rank0_start) * sizeof(ChMessageInt_t));
  free(nodemsg);
  return REQ_OK;
}

static std::vector<ChSingleNodeinfo> myNodesInfo;
static int send_myNodeInfo_to_parent()
{
  const int nodetab_rank0_size = nodetab_rank0_table.size();
  ChMessageHeader hdr;
  ChMessageInt_t nNodes = ChMessageInt_new(nodetab_rank0_size);
  ChMessageHeader_new("initnodetab",
                      sizeof(ChMessageInt_t) +
                          sizeof(ChSingleNodeinfo) * nodetab_rank0_size,
                      &hdr);
  skt_sendN(parent_charmrun_fd, (const char *) &hdr, sizeof(hdr));
  skt_sendN(parent_charmrun_fd, (const char *) &nNodes, sizeof(nNodes));
  skt_sendN(parent_charmrun_fd, (const char *) myNodesInfo.data(),
            sizeof(ChSingleNodeinfo) * myNodesInfo.size());

  return REQ_OK;
}
static void forward_nodetab_to_children()
{
  /*it just needs to receive and copy the nodetab info if required and send it
   * as it is to its nodes */
  if (!skt_select1(parent_charmrun_fd, 1200 * 1000)) {
    exit(0);
  }
  ChMessage msg;
  ChMessage_recv(parent_charmrun_fd, &msg);

  ChMessageInt_t *nodelistmsg = (ChMessageInt_t *) msg.data;
  int nodetab_Nodes = ChMessageInt(nodelistmsg[0]);
  for (const nodetab_process & p : my_process_table)
  {
    SOCKET fd = p.req_client;
    ChMessageHeader hdr;
    ChMessageInt_t nNodes = ChMessageInt_new(nodetab_Nodes);
    ChMessageHeader_new("initnodetab", sizeof(ChMessageInt_t) +
                                           sizeof(ChNodeinfo) * nodetab_Nodes,
                        &hdr);
    skt_sendN(fd, (const char *) &hdr, sizeof(hdr));
    skt_sendN(fd, (const char *) &nNodes, sizeof(nNodes));
    skt_sendN(fd, (const char *) (nodelistmsg + 1),
              sizeof(ChNodeinfo) * nodetab_Nodes);
  }
}
/*Parent Charmrun receives the nodetab from child and processes it. msg contain
 * array of ChSingleNodeInfo*/
static void receive_nodeset_from_child(ChMessage *msg, SOCKET fd)
{
  ChMessageInt_t *n32 = (ChMessageInt_t *) msg->data;
  int numOfNodes = ChMessageInt(n32[0]);
  ChSingleNodeinfo *childNodeInfo = (ChSingleNodeinfo *) (n32 + 1);
  for (int k = 0; k < numOfNodes; k++)
    nodeinfo_add(childNodeInfo + k, my_process_table[childNodeInfo[k].nodeNo]);
}

static void set_sockets_list(ChMessage *msg, SOCKET fd)
{
  ChMessageInt_t *n32 = (ChMessageInt_t *) msg->data;
  int node_start = ChMessageInt(n32[0]);
  TODO;
  charmrun_fds[node_start / nodes_per_child] = fd;
}
#endif
/* Check this return code from "printf". */
static void checkPrintfError(int err)
{
  if (err < 0) {
    static int warned = 0;
    if (!warned) {
      perror("charmrun WARNING> error in printf");
      warned = 1;
    }
  }
}

static int req_handle_print(ChMessage *msg, SOCKET fd)
{
  checkPrintfError(printf("%s", msg->data));
  checkPrintfError(fflush(stdout));
  write_stdio_duplicate(msg->data);
  return REQ_OK;
}

static int req_handle_printerr(ChMessage *msg, SOCKET fd)
{
  fprintf(stderr, "%s", msg->data);
  fflush(stderr);
  write_stdio_duplicate(msg->data);
  return REQ_OK;
}

static int req_handle_printsyn(ChMessage *msg, SOCKET fd)
{
  checkPrintfError(printf("%s", msg->data));
  checkPrintfError(fflush(stdout));
  write_stdio_duplicate(msg->data);
#ifdef HSTART
  if (arg_hierarchical_start)
    req_reply_child(fd, "printdone", "", 1);
  else
#endif
    req_reply(fd, "printdone", "", 1);
  return REQ_OK;
}

static int req_handle_printerrsyn(ChMessage *msg, SOCKET fd)
{
  fprintf(stderr, "%s", msg->data);
  fflush(stderr);
  write_stdio_duplicate(msg->data);
#ifdef HSTART
  if (arg_hierarchical_start)
    req_reply_child(fd, "printdone", "", 1);
  else
#endif
    req_reply(fd, "printdone", "", 1);
  return REQ_OK;
}

static int _exitcode = 0;

static void finish_set_nodes(std::vector<nodetab_process> &, int start, int stop, bool charmrun_exiting=false);

static int req_handle_ending(ChMessage *msg, SOCKET fd)
{
  req_ending++;

  if (msg->data) {
    int exitcode = atoi(msg->data);
    if (exitcode)
      _exitcode = exitcode;
  }

#if CMK_SHRINK_EXPAND
  // When using shrink-expand, only PE 0 will send an "ending" request.
#else
  if (req_ending == my_process_table.size())
#endif
  {
#if CMK_SHRINK_EXPAND
    ChMessage ackmsg;
    ChMessage_new("realloc_ack", 0, &ackmsg);
    for (const nodetab_process & p : my_process_table)
      ChMessage_send(p.req_client, &ackmsg);
#endif

    for (const nodetab_process & p : my_process_table)
      skt_close(p.req_client);
    if (arg_verbose)
      printf("Charmrun> Graceful exit with exit code %d.\n", _exitcode);
#if CMK_USE_SSH
    if (arg_interactive && !arg_ssh_display)
#else
    if (arg_interactive)
#endif
      finish_set_nodes(my_process_table, 0, 1, true);
    exit(_exitcode);
  }
  return REQ_OK;
}

static int req_handle_barrier(ChMessage *msg, SOCKET fd)
{
  static int barrier_count = 0;
  static int barrier_phase = 0;
  barrier_count++;
#ifdef HSTART
  if (barrier_count == arg_requested_pes)
#else
  if (barrier_count == my_process_table.size())
#endif
  {
    barrier_count = 0;
    barrier_phase++;
    for (const nodetab_process & p : my_process_table)
      if (REQ_OK != req_reply(p.req_client, "barrier", "", 1)) {
        fprintf(stderr, "req_handle_barrier socket error: %d\n", p.nodeno);
        abort();
      }
  }
  return REQ_OK;
}

static int req_handle_barrier0(ChMessage *msg, SOCKET fd)
{
  static int count = 0;
  static SOCKET fd0;
  int pe = atoi(msg->data);
  if (pe == 0)
    fd0 = fd;
  count++;
#ifdef HSTART
  if (count == arg_requested_pes)
#else
  if (count == my_host_table.size())
#endif
  {
    req_reply(fd0, "barrier0", "", 1); /* only send to node 0 */
    count = 0;
  }
  return REQ_OK;
}

static void req_handle_abort(ChMessage *msg, SOCKET fd)
{
  /*fprintf(stderr,"req_handle_abort called \n");*/
  if (msg->len == 0)
    fprintf(stderr, "Aborting!\n");
  else
    fprintf(stderr, "%s\n", msg->data);
  exit(1);
}

static int req_handle_scanf(ChMessage *msg, SOCKET fd)
{
  char *fmt = msg->data;
  fmt[msg->len - 1] = 0;
  char *res = input_scanf_chars(fmt);
  char *p = res;
  while (*p) {
    if (*p == '\n')
      *p = ' ';
    p++;
  }
#ifdef HSTART
  if (arg_hierarchical_start)
    req_reply_child(fd, "scanf-data", res, strlen(res) + 1);
  else
#endif
    req_reply(fd, "scanf-data", res, strlen(res) + 1);
  free(res);
  return REQ_OK;
}

#if CMK_SHRINK_EXPAND
static int req_handle_realloc(ChMessage *msg, SOCKET fd)
{
  printf("Charmrun> Realloc request received\n");

  /* Exec to clear and restart everything, just preserve contents of
   * netstart*/
  int restart_idx = -1, newp_idx = -1, oldp_idx = -1, shrink_expand_idx= -1, charmrun_idx = -1;
  int additional_args = 10;
  for (int i = 0; i < saved_argc; ++i) {
    if (strcmp(saved_argv[i], "+restart") == 0) {
      restart_idx = i;
      additional_args -= 2;
    }
    if(strcmp(saved_argv[i], "++newp") == 0)
    {
      newp_idx = i;
      additional_args -= 2;
    }
    if(strcmp(saved_argv[i], "++oldp") == 0)
    {
      oldp_idx = i;
      additional_args -= 2;
    }
    if(strcmp(saved_argv[i], "++shrinkexpand") == 0)
    {
      shrink_expand_idx = i;
      additional_args -= 1;
    }
    if(strcmp(saved_argv[i], "++charmrun_port") == 0)
    {
      charmrun_idx = i;
      additional_args -= 2;
    }
  }

#if defined __APPLE__
  const char *dir = "/tmp";
#else
  const char *dir = "/dev/shm";
#endif
  for (int i = 0; i < saved_argc; ++i) {
    if (strcmp(saved_argv[i], "+shrinkexpand_basedir") == 0) {
      dir = saved_argv[i+1];
      break;
    }
  }

  const char **ret = (const char **) malloc(sizeof(char *) * (saved_argc + additional_args));

  int newP = ChMessageInt(*(ChMessageInt_t *)msg->data);
  int oldP = arg_requested_pes;
  printf("Charmrun> newp =  %d oldP = %d \n \n \n", newP, oldP);

  for (int i = 0; i < saved_argc; i++) {
    ret[i] = saved_argv[i];
  }

  int index = 0;

  char sp_buffer[50]; // newP buffer
  sprintf(sp_buffer, "%d", newP);

  char sp_buffer1[50]; // oldP buffer
  sprintf(sp_buffer1, "%d", oldP);

  char sp_buffer2[6]; // charmrun port
  sprintf(sp_buffer2, "%d", server_port);

  /* Check that shrink expand parameters don't already exist */

  if(newp_idx == -1)
  {
    ret[saved_argc + index++] = "++newp";
    ret[saved_argc + index++] = sp_buffer;
  }
  else
    ret[newp_idx + 1] = sp_buffer;

  if(oldp_idx == -1)
  {
    ret[saved_argc + index++] = "++oldp";
    ret[saved_argc + index++] = sp_buffer1;
  }
  else
    ret[oldp_idx + 1] = sp_buffer1;

  if(shrink_expand_idx == -1)
  {
    ret[saved_argc + index++] = "++shrinkexpand";
  }

  if(charmrun_idx == -1)
  {
    ret[saved_argc + index++] = "++charmrun_port";
    ret[saved_argc + index++] = sp_buffer2;
  }
  else
    ret[charmrun_idx + 1] = sp_buffer2;

  if (restart_idx == -1) {
    ret[saved_argc + index++] = "+restart";
    ret[saved_argc + index++] = dir;
    ret[saved_argc + index++] = NULL;
  } else {
    ret[restart_idx + 1] = dir;
    ret[saved_argc + index++] = NULL;
  }

  ChMessage ackmsg;
  ChMessage_new("realloc_ack", 0, &ackmsg);
  for (const nodetab_process & p : my_process_table)
    ChMessage_send(p.req_client, &ackmsg);

  skt_close(server_fd);
  skt_close(CcsServer_fd());
  execv(ret[0], (char **)ret);
  printf("Should not be here\n");
  exit(1);

  return REQ_OK;
}
#endif

#ifdef __FAULT__
static void restart_node(nodetab_process &);
static void reconnect_crashed_client(nodetab_process &);
static void announce_crash(const nodetab_process &);

static const nodetab_process * _last_crash;      /* last crashed process */
#ifdef HSTART
static nodetab_process * _crash_charmrun_process; /* last restart socket */
static int crashed_pe_id;
static int restarted_pe_id;
#endif

/**
 * @brief Handles an ACK after a crash. Once it has received all the pending
 * acks, it sends the nodetab
 * table to the crashed node.
 */
static int req_handle_crashack(ChMessage *msg, SOCKET fd)
{
  static int count = 0;
  count++;
#ifdef HSTART
  if (arg_hierarchical_start) {
    if (count == nodetab_rank0_table.size() - 1) {
      /* only after everybody else update its nodetab, can this
         restarted process continue */
      if (arg_verbose)
        PRINT(("Charmrun> Restarted node %d\n", _last_crash->nodeno));
      req_send_initnodetab1(_crash_charmrun_process->req_client);
      _last_crash = nullptr;
      count = 0;
    }
  }

  else

#endif
      if (count == my_process_table.size() - 1) {
    // only after everybody else update its nodetab, can this restarted process
    // continue
    if (arg_verbose)
      PRINT(("Charmrun> Restarted node %d\n", _last_crash->nodeno));
    req_send_initnodetab(*_last_crash);
    _last_crash = nullptr;
    count = 0;
  }
  return 0;
}

#ifdef HSTART
/* send initnode to root*/
static int set_crashed_socket_id(ChMessage *msg, SOCKET fd)
{
  ChSingleNodeinfo *nodeInfo = (ChSingleNodeinfo *) msg->data;
  int nt = ChMessageInt(nodeInfo->nodeNo) - mynodes_start; // TODO: relative nodeNo
  nodeInfo->nodeNo = ChMessageInt_new(nt);
  /* Required for CCS */
  /*Nodetable index for this node*/
  my_process_table[nt].req_client = fd; // TODO: nodeno as index
  TODO;
}

/* Receives new dataport of restarted prcoess	and resends nodetable to
 * everyone*/
static int req_handle_crash(ChMessage *msg, nodetab_process & p)
{
  const SOCKET fd = p.req_client;

  ChMessageInt_t oldpe, newpe;
  skt_recvN(fd, (const char *) &oldpe, sizeof(oldpe));
  skt_recvN(fd, (const char *) &newpe, sizeof(newpe));
  *nodetab_table[ChMessageInt(oldpe)] = *nodetab_table[ChMessageInt(newpe)];

  int status = req_handle_initnode(msg, p);
  _crash_charmrun_process = &p;

  fprintf(stderr, "Root charmrun : Socket %d failed: %s\n", fd,
          _crash_charmrun_process->host->name);
  fflush(stderr);
  ChSingleNodeinfo *nodeInfo = (ChSingleNodeinfo *) msg->data;
  int crashed_node = ChMessageInt(nodeInfo->nodeNo);
  _last_crash = crashed_node;
  switch (status) {
  case REQ_OK:
    break;
  case REQ_FAILED:
    return REQ_FAILED;
  }

  /* Already processed, so send*/
  for (const nodetab_process & p2 : my_process_table)
    req_send_initnodetab(p2);

  /*Anounce crash to all child charmruns*/
  announce_crash(p);
}
#endif

static void crashed_process_common(nodetab_process & p)
{
  skt_close(p.req_client);

  restart_node(p);

  // After the crashed processor has been recreated
  // it connects to Charmrun. That data must now be filled
  // into the node table.
  reconnect_crashed_client(p);
}

static void error_in_req_serve_client(nodetab_process & p)
{
  PRINT(("Charmrun> Process %d (host %s) failed: Socket error\n", p.nodeno, p.host->name));
  fflush(stdout);

#if 0
  // Disabled since sockets do not adequately distinguish host faults from process faults
  p.host->crashed = true;
#endif

  crashed_process_common(p);
}
#endif

static int req_handler_dispatch(ChMessage *msg, nodetab_process & p)
{
  const int replyFd = p.req_client;
  char *cmd = msg->header.type;
  DEBUGF(("Got request '%s'\n", cmd, replyFd));
#if CMK_CCS_AVAILABLE /* CCS *doesn't* want data yet, for faster forwarding */
  if (strcmp(cmd, "reply_fw") == 0)
    return req_ccs_reply_fw(msg, replyFd);
#endif

  /* grab request data */
  int recv_status = ChMessageData_recv(replyFd, msg);
#ifdef __FAULT__
#ifdef HSTART
  if (!arg_hierarchical_start)
#endif
  if (recv_status < 0) {
    error_in_req_serve_client(p);
    return REQ_OK;
  }
#endif

  if (strcmp(cmd, "ping") == 0)
    return REQ_OK;
  else if (strcmp(cmd, "print") == 0)
    return req_handle_print(msg, replyFd);
  else if (strcmp(cmd, "printerr") == 0)
    return req_handle_printerr(msg, replyFd);
  else if (strcmp(cmd, "printsyn") == 0)
    return req_handle_printsyn(msg, replyFd);
  else if (strcmp(cmd, "printerrsyn") == 0)
    return req_handle_printerrsyn(msg, replyFd);
  else if (strcmp(cmd, "scanf") == 0)
    return req_handle_scanf(msg, replyFd);
  else if (strcmp(cmd, "barrier") == 0)
    return req_handle_barrier(msg, replyFd);
  else if (strcmp(cmd, "barrier0") == 0)
    return req_handle_barrier0(msg, replyFd);
  else if (strcmp(cmd, "ending") == 0)
    return req_handle_ending(msg, replyFd);
  else if (strcmp(cmd, "abort") == 0) {
    req_handle_abort(msg, replyFd);
    return REQ_FAILED;
  }
#ifdef __FAULT__
  else if (strcmp(cmd, "crash_ack") == 0)
    return req_handle_crashack(msg, replyFd);
#ifdef HSTART
  else if (strcmp(cmd, "initnode") == 0)
    return req_handle_crash(msg, p);
#endif
#endif
#if CMK_SHRINK_EXPAND
  else if (strcmp(cmd, "realloc") == 0)
    return req_handle_realloc(msg, replyFd);
#endif
  else {
#ifndef __FAULT__
    fprintf(stderr, "Charmrun> Bad control socket request '%s'\n", cmd);
    abort();
    return REQ_OK;
#endif
  }
  return REQ_OK;
}

static void req_serve_client(nodetab_process & p)
{
  DEBUGF(("Getting message from client...\n"));

  ChMessage msg;
  int recv_status = ChMessageHeader_recv(p.req_client, &msg);
#ifdef __FAULT__
#ifdef HSTART
  if (!arg_hierarchical_start && recv_status < 0)
    error_in_req_serve_client(p);
#else
  if (recv_status < 0) {
    error_in_req_serve_client(p);
    return;
  }
#endif
#endif

  DEBUGF(("Message is '%s'\n", msg.header.type));
  int status = req_handler_dispatch(&msg, p);
  switch (status) {
  case REQ_OK:
    break;
  case REQ_FAILED:
    fprintf(stderr, "Charmrun> Error processing control socket request %s\n",
            msg.header.type);
    abort();
    break;
  }
  ChMessage_free(&msg);
}

#ifdef HSTART
static void req_forward_root(nodetab_process & p)
{
  const SOCKET fd = p.req_client;
  ChMessage msg;
  int recv_status = ChMessage_recv(fd, &msg);

  char *cmd = msg.header.type;

#ifdef __FAULT__
  if (recv_status < 0) {
    error_in_req_serve_client(p);
    return;
  }

  /*called from reconnect_crashed_client */
  if (strcmp(cmd, "initnode") == 0) {
    set_crashed_socket_id(&msg, fd);
  }
#endif

  int status = REQ_OK;
  if (strcmp(cmd, "ping") != 0) {
    status = req_reply(parent_charmrun_fd, cmd, msg.data,
                       ChMessageInt(msg.header.len));

    if (strcmp(cmd, "scanf") == 0 || strcmp(cmd, "printsyn") == 0 ||
        strcmp(cmd, "printerrsyn") == 0)
      skt_sendN(parent_charmrun_fd, (const char *) &fd, sizeof(fd));

#ifdef __FAULT__
    if (strcmp(cmd, "initnode") == 0) {
      ChMessageInt_t oldpe = ChMessageInt_new(crashed_pe_id);
      ChMessageInt_t newpe = ChMessageInt_new(restarted_pe_id);
      skt_sendN(parent_charmrun_fd, (const char *) &oldpe, sizeof(oldpe));
      skt_sendN(parent_charmrun_fd, (const char *) &newpe, sizeof(newpe));
    }
#endif
  }

  switch (status) {
  case REQ_OK:
    break;
  case REQ_FAILED:
    abort();
    break;
  }
  ChMessage_free(&msg);
}

static void req_forward_client()
{
  ChMessage msg;
  int recv_status = ChMessage_recv(parent_charmrun_fd, &msg);
  if (recv_status < 0) {

    for (const nodetab_process & p : my_process_table)
      skt_close(p.req_client);
    exit(0);
  }

  char *cmd = msg.header.type;

  if (strcmp(cmd, "barrier") == 0) {
    for (const nodetab_process & p : my_process_table)
      if (REQ_OK != req_reply(p.req_client, cmd, msg.data,
                              ChMessageInt(msg.header.len))) {
        abort();
      }
    return;
  }
#ifdef __FAULT__
  if (strcmp(cmd, "initnodetab") == 0) {
    if (_last_crash == nullptr)
      current_restart_phase++;

    for (const nodetab_process & p : my_process_table)
      if (_last_crash == nullptr)
        if (REQ_OK != req_reply(p.req_client, cmd, msg.data,
                                ChMessageInt(msg.header.len))) {
          abort();
        }
    return;
  }

  if (strcmp(cmd, "crashnode") == 0) {
    for (const nodetab_process & p : my_process_table)
      if (_last_crash == nullptr)
        if (REQ_OK != req_reply(p.req_client, cmd, msg.data,
                                ChMessageInt(msg.header.len))) {
          abort();
        }
    return;
  }
  if (strcmp(cmd, "initnttab") == 0) {
    _last_crash = nullptr;
    if (REQ_OK != req_reply(_last_crash->req_client, "initnodetab",
                            msg.data, ChMessageInt(msg.header.len))) {
      abort();
    }
    return;
  }

#endif

  SOCKET fd;

  /* CCS forward request */
  if (strcmp(cmd, "req_fw") == 0) {
    CcsImplHeader *hdr = (CcsImplHeader *) msg.data;
    int pe = ChMessageInt(hdr->pe);
    fd = nodetab_table[pe]->ctrlfd;
  } else if (strcmp(cmd, "barrier0") == 0) {
    fd = nodetab_table[0]->ctrlfd;
  } else
    skt_recvN(parent_charmrun_fd, (char *) &fd, sizeof(SOCKET));

  int status = req_reply(fd, cmd, msg.data, ChMessageInt(msg.header.len));

  switch (status) {
  case REQ_OK:
    break;
  case REQ_FAILED:
    abort();
    break;
  }
  ChMessage_free(&msg);
}

#endif

static int ignore_socket_errors(SOCKET skt, int c, const char *m)
{ /*Abandon on further socket errors during error shutdown*/

#ifndef __FAULT__
  exit(2);
#endif
  return -1;
}

static nodetab_process & get_process_for_socket(std::vector<nodetab_process> & process_table, SOCKET req_client)
{
  nodetab_process * ptr = nullptr;
  for (nodetab_process & p : process_table)
  {
    if (p.req_client == req_client)
    {
      ptr = &p;
      break;
    }
  }
  if (ptr == nullptr)
  {
    fprintf(stderr, "Charmrun> get_process_for_socket: unknown socket\n");
    exit(1);
  }

  nodetab_process & p = *ptr;
  return p;
}

/*A socket went bad somewhere!  Immediately disconnect,
which kills everybody.
*/
static int socket_error_in_poll(SOCKET skt, int code, const char *msg)
{
  /*commenting it for fault tolerance*/
  /*ifdef it*/
  skt_set_abort(ignore_socket_errors);

  {
    const nodetab_process & p = get_process_for_socket(my_process_table, skt);
    fprintf(stderr, "Charmrun> error on request socket to node %d '%s'--\n"
                    "%s\n",
            p.nodeno, p.host->name, msg);
  }

#ifndef __FAULT__
  for (const nodetab_process & p : my_process_table)
    skt_close(p.req_client);
  exit(1);
#endif
  ftTimer = GetClock();
  return -1;
}

#if CMK_USE_POLL /*poll() version*/
#define CMK_PIPE_DECL(maxn, delayMs)                                           \
  static struct pollfd *fds = NULL;                                            \
  int nFds_sto = 0;                                                            \
  int *nFds = &nFds_sto;                                                       \
  int pollDelayMs = delayMs;                                                   \
  if (fds == NULL)                                                             \
    fds = (struct pollfd *) malloc((maxn) * sizeof(struct pollfd));
#define CMK_PIPE_SUB fds, nFds
#define CMK_PIPE_CALL()                                                        \
  poll(fds, *nFds, pollDelayMs);                                               \
  *nFds = 0

#define CMK_PIPE_PARAM struct pollfd *fds, int *nFds
#define CMK_PIPE_ADDREAD(rd_fd)                                                \
  do {                                                                         \
    fds[*nFds].fd = rd_fd;                                                     \
    fds[*nFds].events = POLLIN;                                                \
    (*nFds)++;                                                                 \
  } while (0)
#define CMK_PIPE_ADDWRITE(wr_fd)                                               \
  do {                                                                         \
    fds[*nFds].fd = wr_fd;                                                     \
    fds[*nFds].events = POLLOUT;                                               \
    (*nFds)++;                                                                 \
  } while (0)
#define CMK_PIPE_CHECKREAD(rd_fd) fds[(*nFds)++].revents &POLLIN
#define CMK_PIPE_CHECKWRITE(wr_fd) fds[(*nFds)++].revents &POLLOUT

#else /*select() version*/

#define CMK_PIPE_DECL(maxn, delayMs)                                           \
  fd_set rfds_sto, wfds_sto;                                                   \
  int nFds = 0;                                                                \
  fd_set *rfds = &rfds_sto, *wfds = &wfds_sto;                                 \
  struct timeval tmo;                                                          \
  FD_ZERO(rfds);                                                               \
  FD_ZERO(wfds);                                                               \
  tmo.tv_sec = delayMs / 1000;                                                 \
  tmo.tv_usec = 1000 * (delayMs % 1000);
#define CMK_PIPE_SUB rfds, wfds
#define CMK_PIPE_CALL() select(FD_SETSIZE, rfds, 0, 0, &tmo)

#define CMK_PIPE_PARAM fd_set *rfds, fd_set *wfds
#define CMK_PIPE_ADDREAD(rd_fd)                                                \
  {                                                                            \
    assert(nFds < FD_SETSIZE);                                                 \
    FD_SET(rd_fd, rfds);                                                       \
    nFds++;                                                                    \
  }
#define CMK_PIPE_ADDWRITE(wr_fd) FD_SET(wr_fd, wfds)
#define CMK_PIPE_CHECKREAD(rd_fd) FD_ISSET(rd_fd, rfds)
#define CMK_PIPE_CHECKWRITE(wr_fd) FD_ISSET(wr_fd, wfds)
#endif

/*
Wait for incoming requests on all client sockets,
and the CCS socket (if present).
*/
static void req_poll()
{
  CMK_PIPE_DECL(my_process_table.size() + 5, 1000);
  for (const nodetab_process & p : my_process_table)
    CMK_PIPE_ADDREAD(p.req_client);
  if (CcsServer_fd() != INVALID_SOCKET)
    CMK_PIPE_ADDREAD(CcsServer_fd());
  if (arg_charmdebug) {
    CMK_PIPE_ADDREAD(0);
    CMK_PIPE_ADDREAD(gdb_info_std[1]);
    CMK_PIPE_ADDREAD(gdb_info_std[2]);
  }

  skt_set_abort(socket_error_in_poll);

  DEBUGF(("Req_poll: Calling select...\n"));
  int status = CMK_PIPE_CALL();
  DEBUGF(("Req_poll: Select returned %d...\n", status));

  if (status == 0)
    return; /*Nothing to do-- timeout*/

  if (status < 0) {
    if (errno == EINTR || errno == EAGAIN)
      return;
    fflush(stdout);
    fflush(stderr);
    socket_error_in_poll(-1, 1359, "Node program terminated unexpectedly!\n");
  }
  for (nodetab_process & p : my_process_table)
  {
    const SOCKET req_client = p.req_client;
    if (CMK_PIPE_CHECKREAD(req_client)) {
      int readcount = 10; /*number of successive reads we serve per socket*/
      /*This client is ready to read*/
      do {
        req_serve_client(p);
        readcount--;
      } while (1 == skt_select1(req_client, 0) && readcount > 0);
    }
  }

  if (CcsServer_fd() != INVALID_SOCKET)
    if (CMK_PIPE_CHECKREAD(CcsServer_fd())) {
      DEBUGF(("Activity on CCS server port...\n"));
      req_ccs_connect();
    }

  if (arg_charmdebug) {
    char buf[2048];
    if (CMK_PIPE_CHECKREAD(0)) {
      int indata = read(0, buf, 5);
      buf[indata] = 0;
      if (indata < 5)
        fprintf(stderr, "Error reading command (%s)\n", buf);
      if (strncmp(buf, "info:", 5) == 0) {
        /* Found info command, forward data to gdb info program */
        char c;
        int num = 0;
        // printf("Command to be forwarded\n");
        while (read(0, &c, 1) != -1) {
          buf[num++] = c;
          if (c == '\n' || num >= 2045) {
            if (write(gdb_info_std[0], buf, num) != num) {
              fprintf(stderr, "charmrun> writing info command to gdb failed!\n");
              exit(1);
            }
            if (c == '\n')
              break;
          }
        }
      }
      // printf("Command from charmdebug: %d(%s)\n",indata,buf);
    }
    /* All streams from gdb are forwarded to the stderr stream through the FILE
       gdb_stream which has been duplicated from stderr */
    /* NOTE: gdb_info_std[2] must be flushed before gdb_info_std[1] because the
       latter contains the string "(gdb) " ending the synchronization. Also the
       std[1] should be read with the else statement. It will not work without.
       */
    if (CMK_PIPE_CHECKREAD(gdb_info_std[2])) {
      int indata = read(gdb_info_std[2], buf, 100);
      /*printf("read data from gdb info stderr %d\n",indata);*/
      if (indata > 0) {
        buf[indata] = 0;
        // printf("printing %s\n",buf);
        // fflush(stdout);
        // fprintf(gdb_stream,"%s",buf);
        fflush(gdb_stream);
      }
    } else if (CMK_PIPE_CHECKREAD(gdb_info_std[1])) {
      int indata = read(gdb_info_std[1], buf, 100);
      /*printf("read data from gdb info stdout %d\n",indata);*/
      if (indata > 0) {
        buf[indata] = 0;
        // printf("printing %s\n",buf);
        // fflush(stdout);
        fprintf(gdb_stream, "%s", buf);
        fflush(gdb_stream);
      }
    }
  }
}

#ifdef HSTART
static void req_poll_hierarchical()
{
  skt_set_abort(socket_error_in_poll);

  struct timeval tmo;
  tmo.tv_sec = 1;
  tmo.tv_usec = 0;
  fd_set rfds;
  FD_ZERO(&rfds); /* clears set of file descriptor */
  for (const nodetab_process & p : my_process_table)
    FD_SET(p.req_client, &rfds); /* adds client sockets to rfds set*/
  if (CcsServer_fd() != INVALID_SOCKET)
    FD_SET(CcsServer_fd(), &rfds);
  if (arg_charmdebug) {
    FD_SET(0, &rfds);
    FD_SET(gdb_info_std[1], &rfds);
    FD_SET(gdb_info_std[2], &rfds);
  }

  if (arg_child_charmrun)
    FD_SET(parent_charmrun_fd, &rfds); /* adds client sockets to rfds set*/
  DEBUGF(("Req_poll: Calling select...\n"));
  int status = select(FD_SETSIZE, &rfds, 0, 0,
                  &tmo); /* FD_SETSIZE is the maximum number of file
                            descriptors that a fd_set object can hold
                            information about, select returns number of
                            polls gathered */
  DEBUGF(("Req_poll: Select returned %d...\n", status));

  if (status == 0)
    return; /*Nothing to do-- timeout*/
  if (status < 0) {
    fflush(stdout);
    fflush(stderr);
    socket_error_in_poll(req_clients[0], 1359, "Node program terminated unexpectedly!\n");
  }
  for (nodetab_process & p : my_process_table)
  {
    const SOCKET req_client = p.req_client;
    if (FD_ISSET(req_client, &rfds)) {
      int readcount = 10; /*number of successive reads we serve per socket*/
      /*This client is ready to read*/
      do {
        if (arg_child_charmrun)
          req_forward_root(p);
        else
          req_serve_client(p);
        readcount--;
      } while (1 == skt_select1(req_client, 0) && readcount > 0);
    }

  if (arg_child_charmrun)
    // Forward from root to clients
    if (FD_ISSET(parent_charmrun_fd, &rfds)) {
      int readcount = 10; /*number of successive reads we serve per socket*/
      do {
        req_forward_client();
        readcount--;
      } while (1 == skt_select1(parent_charmrun_fd, 0) && readcount > 0);
    }

  /*Wait to receive responses and Forward responses */
  if (CcsServer_fd() != INVALID_SOCKET)
    if (FD_ISSET(CcsServer_fd(), &rfds)) {
      DEBUGF(("Activity on CCS server port...\n"));
      req_ccs_connect();
    }

  if (arg_charmdebug) {
    char buf[2048];
    if (FD_ISSET(0, &rfds)) {
      int indata = read(0, buf, 5);
      buf[indata] = 0;
      if (indata < 5)
        fprintf(stderr, "Error reading command (%s)\n", buf);
      if (strncmp(buf, "info:", 5) == 0) {
        /* Found info command, forward data to gdb info program */
        char c;
        int num = 0;
        // printf("Command to be forwarded\n");
        while (read(0, &c, 1) != -1) {
          buf[num++] = c;
          if (c == '\n' || num >= 2045) {
            if (write(gdb_info_std[0], buf, num) != num) {
              fprintf(stderr, "charmrun> writing info command to gdb failed!\n");
              exit(1);
            }
            if (c == '\n')
              break;
          }
        }
      }
      // printf("Command from charmdebug: %d(%s)\n",indata,buf);
    }
    /* All streams from gdb are forwarded to the stderr stream through the FILE
       gdb_stream which has been duplicated from stderr */
    /* NOTE: gdb_info_std[2] must be flushed before gdb_info_std[1] because the
       latter contains the string "(gdb) " ending the synchronization. Also the
       std[1] should be read with the else statement. It will not work without.
       */
    if (FD_ISSET(gdb_info_std[2], &rfds)) {
      int indata = read(gdb_info_std[2], buf, 100);
      /*printf("read data from gdb info stderr %d\n",indata);*/
      if (indata > 0) {
        buf[indata] = 0;
        // printf("printing %s\n",buf);
        // fflush(stdout);
        // fprintf(gdb_stream,"%s",buf);
        fflush(gdb_stream);
      }
    } else if (FD_ISSET(gdb_info_std[1], &rfds)) {
      int indata = read(gdb_info_std[1], buf, 100);
      /*printf("read data from gdb info stdout %d\n",indata);*/
      if (indata > 0) {
        buf[indata] = 0;
        // printf("printing %s\n",buf);
        // fflush(stdout);
        fprintf(gdb_stream, "%s", buf);
        fflush(gdb_stream);
      }
    }
  }
}
#endif

#ifdef HSTART
static skt_ip_t parent_charmrun_IP;
static int parent_charmrun_port;
static int parent_charmrun_pid;
static unsigned int dataport;
static SOCKET dataskt;
static int charmrun_phase = 0;
#endif

static int client_connect_problem(const nodetab_process & p, const char *msg)
{ /*Called when something goes wrong during a client connect*/
  fprintf(stderr, "Charmrun> error attaching to node '%s':\n%s\n", p.host->name, msg);
  exit(1);
  return -1;
}

static int client_connect_problem_skt(SOCKET skt, int code, const char *msg)
{ /* Passed to skt_set_abort */
  const nodetab_process & p = get_process_for_socket(my_process_table, skt);
  return client_connect_problem(p, msg);
}

/** return 1 if connection is opened succesfully with client**/
static SOCKET errorcheck_one_client_connect(void)
{
  static int numClientsConnected = 0;
  unsigned int clientPort; /*These are actually ignored*/
  skt_ip_t clientIP;
  if (arg_verbose)
    printf("Charmrun> Waiting for %d-th client to connect.\n", numClientsConnected);

  if (0 == skt_select1(server_fd, arg_timeout * 1000))
  {
    fprintf(stderr, "Charmrun> Timeout waiting for node-program to connect\n");
    exit(1);
  }

  const SOCKET req_client = skt_accept(server_fd, &clientIP, &clientPort);

  /* FIXME: will this ever be triggered? It seems the skt_abort handler here is
   *        'client_connect_problem', which calls exit(1), so we'd exit
   *        in skt_accept. */
  if (req_client == SOCKET_ERROR)
  {
    fprintf(stderr, "Charmrun> Failure in node accept\n");
    exit(1);
  }
  if (req_client < 0)
  {
    fprintf(stderr, "Charmrun> Warning: errorcheck_one_client_connect: socket < 0\n");
  }

  skt_tcp_no_nagle(req_client);

  ++numClientsConnected;

  return req_client;
}

static nodetab_process & get_process_for_nodeno(std::vector<nodetab_process> & process_table, int nodeno)
{
  nodetab_process * ptr = nullptr;
  for (nodetab_process & p : process_table)
  {
    if (p.nodeno == nodeno)
    {
      ptr = &p;
      break;
    }
  }
  if (ptr == nullptr)
  {
    fprintf(stderr, "Charmrun> get_process_for_nodeno: unknown nodeno %d\n", nodeno);
    exit(1);
  }

  nodetab_process & p = *ptr;
  assert(p.nodeno == nodeno);
  return p;
}

#if CMK_C_INLINE
inline static
#endif
    void
    read_initnode_one_client(nodetab_process & p)
{
  if (!skt_select1(p.req_client, arg_timeout * 1000))
    client_connect_problem(p, "Timeout on IP request");

  ChMessage msg;
  ChMessage_recv(p.req_client, &msg);
  req_handle_initnode(&msg, p);
  ChMessage_free(&msg);
}

#if CMK_IBVERBS_FAST_START
static void req_one_client_partinit_skt(std::vector<nodetab_process> & process_table, const SOCKET req_client)
{
  if (!skt_select1(req_client, arg_timeout * 1000))
  {
    fprintf(stderr, "Charmrun> Timeout on partial init request, socket %d\n", req_client);
    exit(1);
  }

  ChMessage partStartMsg;
  ChMessage_recv(req_client, &partStartMsg);
  assert(strncmp(partStartMsg.header.type, "partinit", 8) == 0);
  int nodeNo = ChMessageInt(*(ChMessageInt_t *) partStartMsg.data);
  ChMessage_free(&partStartMsg);

  nodetab_process & p = get_process_for_nodeno(process_table, nodeNo);
  p.req_client = req_client;
}

static void req_one_client_partinit(std::vector<nodetab_process> & process_table, int index)
{
# ifdef HSTART
  if (arg_hierarchical_start && !arg_child_charmrun && charmrun_phase == 1)
  {
    nodetab_process & p = process_table[index];
    req_one_client_partinit_skt(process_table, p.req_client);
  }
  else
# endif
  {
    const SOCKET req_client = errorcheck_one_client_connect();
    req_one_client_partinit_skt(process_table, req_client);
  }
}
#endif

#ifdef HSTART
/* To keep a global node numbering */
static void add_singlenodeinfo_to_mynodeinfo(ChMessage *msg, SOCKET ctrlfd)
{
  /*add to myNodesInfo */
  ChSingleNodeinfo *nodeInfo = (ChSingleNodeinfo *) msg->data;

  /* need to change nodeNo */
  ChMessageInt_t nodeNo = ChMessageInt_new(
      nodetab_rank0_table[ChMessageInt(nodeInfo->nodeNo) - mynodes_start]);
  myNodesInfo.push_back({nodeNo, nodeInfo->info});

  /* Required for CCS */
  int nt = ChMessageInt(nodeInfo->nodeNo) - mynodes_start; // TODO: relative nodeNo
  nodeInfo->nodeNo = ChMessageInt_new(nt);
  my_process_table[nt].req_client = ctrlfd; // TODO: nodeno as index
}
#endif

static void req_set_client_connect(std::vector<nodetab_process> & process_table, int count)
{
  int curclientend, curclientstart = 0;

  std::queue<SOCKET> open_sockets;

  ChMessage msg;
  msg.len=-1;
#if CMK_USE_IBVERBS && !CMK_IBVERBS_FAST_START
# ifdef HSTART
  if (!(arg_hierarchical_start && !arg_child_charmrun && charmrun_phase == 1))
# endif
  {
    for (int i = 0; i < count; i++)
      open_sockets.push(errorcheck_one_client_connect());
  }
  curclientend = count;
#else
  curclientend = 0;
#endif

  int finished = 0;
  while (finished < count)
  {
/* check server socket for messages */
#if !CMK_USE_IBVERBS || CMK_IBVERBS_FAST_START
    while (curclientstart == curclientend || skt_select1(server_fd, 1) != 0) {
# ifdef HSTART
      if (!(arg_hierarchical_start && !arg_child_charmrun && charmrun_phase == 1))
# endif
        open_sockets.push(errorcheck_one_client_connect());

      curclientend++;
    }
#endif
    /* check appropriate clients for messages */
    while (!open_sockets.empty())
    {
      const SOCKET req_client = open_sockets.front();
      open_sockets.pop();

      if (skt_select1(req_client, 1) != 0)
      {
	if(msg.len!=-1) ChMessage_free(&msg);
        ChMessage_recv(req_client, &msg);

        int nodeNo = ChMessageInt(((ChSingleNodeinfo *)msg.data)->nodeNo);
        nodetab_process & p = get_process_for_nodeno(process_table, nodeNo);
        p.req_client = req_client;

#ifdef HSTART
        if (arg_hierarchical_start)
        {
          if (!arg_child_charmrun) {
            if (charmrun_phase == 1)
              receive_nodeset_from_child(&msg, req_client);
            else
              set_sockets_list(&msg, req_client);
            // here we need to decide based upon the phase
          } else /* hier-start with 2nd leval*/
            add_singlenodeinfo_to_mynodeinfo(&msg, req_client);
        }
        else
#endif
          req_handle_initnode(&msg, p);

        ++finished;
      }
      else
      {
        open_sockets.push(req_client);
      }
    }
  }

  ChMessage_free(&msg);
}

static void send_clients_nodeinfo()
{
  const int my_process_count = my_process_table.size();
  int msgSize = sizeof(ChMessageInt_t) * ChInitNodetabFields +
                sizeof(ChNodeinfo) * my_process_count;

  for (const nodetab_process & p : my_process_table)
  {
    const SOCKET fd = p.req_client;
    req_send_initnodetab_internal(p, my_process_count, msgSize);
  }
}

#if CMK_USE_IBVERBS || CMK_USE_IBUD
static void receive_qplist()
{
#if CMK_USE_IBVERBS && !CMK_IBVERBS_FAST_START
  /* a barrier to make sure infiniband device gets initialized */
  if (my_process_table.size() > 1)
  {
    ChMessage msg;
    for (const nodetab_process & p : my_process_table)
    {
      ChMessage_recv(p.req_client, &msg);
      ChMessage_free(&msg);
    }
    for (const nodetab_process & p : my_process_table)
      req_reply(p.req_client, "barrier", "", 1);
  }
#endif

  for (nodetab_process & p : my_process_table)
  {
    const SOCKET fd = p.req_client;
    if (!skt_select1(p.req_client, arg_timeout * 1000))
      client_connect_problem(p, "Timeout on IP request");

    ChMessage msg;
    ChMessage_recv(p.req_client, &msg);

    req_handle_qplist(&msg, p);

    ChMessage_free(&msg);
  }
}
#endif

#if CMK_USE_IBVERBS
/* Each node has sent the qpn data for all the qpns it has created
   This data needs to be sent to all the other nodes
         This needs to be done for all nodes
**/
static void exchange_qpdata_clients()
{
  const int my_process_count = my_process_table.size();

  for (nodetab_process & p : my_process_table)
    p.qpData =
        (ChInfiAddr *) malloc(sizeof(ChInfiAddr) * my_process_count);

  for (nodetab_process & p1 : my_process_table)
  {
    const int proc = p1.nodeno;
    int count = 0;
    for (nodetab_process & p2 : my_process_table)
    {
      if (&p1 != &p2)
      {
        ChInfiAddr & ia = p2.qpData[proc] = p1.qpList[count];
        ia.nodeno = ChMessageInt_new(proc);
        //			printf("Charmrun> nt %d proc %d lid 0x%x qpn
        // 0x%x
        // psn
        // 0x%x\n",nt,proc,ChMessageInt(p2.qpData[proc].lid),ChMessageInt(p2.qpData[proc].qpn),ChMessageInt(p2.qpData[proc].psn));
        count++;
      }
    }
    free(p1.qpList);
    p1.qpList = nullptr;
  }
}
#endif

#if CMK_USE_IBVERBS || CMK_USE_IBUD
static void send_clients_qpdata()
{
  const int my_process_count = my_process_table.size();
  int qpDataSize = sizeof(ChInfiAddr) * my_process_count;

  for (const nodetab_process & p : my_process_table)
  {
    const SOCKET fd = p.req_client;
    ChMessageHeader hdr;
    ChMessageHeader_new("qpdata", qpDataSize, &hdr);
    skt_sendN(fd, (const char *) &hdr, sizeof(hdr));
#if CMK_USE_IBVERBS
    skt_sendN(fd, (const char *) p.qpData, qpDataSize);
#elif CMK_USE_IBUD
    for (const nodetab_process & p2 : my_process_table)
      skt_sendN(fd, (const char *) &p2.qp, sizeof(ChInfiAddr));
#endif
  }
}
#endif

static struct timeval tim;
#define getthetime(x)                                                          \
  gettimeofday(&tim, NULL);                                                    \
  x = tim.tv_sec + (tim.tv_usec / 1000000.0);
#define getthetime1(x)                                                         \
  gettimeofday(&tim, NULL);                                                    \
  x = tim.tv_sec;

static void req_add_phase2_processes(std::vector<nodetab_process> &);
static void req_all_clients_connected();

/*Wait for all the clients to connect to our server port*/
static void req_client_connect_table(std::vector<nodetab_process> & process_table)
{
#if CMK_IBVERBS_FAST_START
  for (int c = 0, c_end = process_table.size(); c < c_end; ++c)
    req_one_client_partinit(process_table, c);
  for (nodetab_process & p : process_table)
    read_initnode_one_client(p);
#else
  req_set_client_connect(process_table, process_table.size());
#endif
}

static int get_old_style_process_count()
{
  const int p = arg_requested_pes;
  const int np = arg_requested_nodes;
  const int ppn = arg_ppn;

  const bool p_active = (p > 0);
  const bool np_active = (np > 0);
  const bool ppn_active = (ppn > 0);

  if (np_active)
    return np;
  else if (p_active)
    return ppn_active ? (p + ppn - 1) / ppn : p;
  else
    return 1;
}

static int calculated_processes_per_host;

static void req_construct_phase2_processes(std::vector<nodetab_process> & phase2_processes)
{
  const int active_host_count = my_process_table.size(); // phase1_process_count

  int total_processes;

  if (proc_per.active())
  {
    const nodetab_process & p0 = my_process_table[0];

    for (nodetab_process & p : my_process_table)
    {
      if (p.num_pus != p0.num_pus ||
          p.num_cores != p0.num_cores ||
          p.num_sockets != p0.num_sockets)
      {
        fprintf(stderr, "Charmrun> Error: Detected system topology is heterogeneous, please use old-style launch options.\n");
        exit(1);
      }
    }

    using Unit = typename TopologyRequest::Unit;

    int num_processes;
    const Unit proc_unit = proc_per.unit();
    switch (proc_unit)
    {
      case Unit::Host:
        num_processes = proc_per.host;
        break;
      case Unit::Socket:
        num_processes = proc_per.socket * p0.num_sockets;
        break;
      case Unit::Core:
        num_processes = proc_per.core * p0.num_cores;
        break;
      case Unit::PU:
        num_processes = proc_per.pu * p0.num_pus;
        break;
      default:
        num_processes = 1;
        break;
    }

    calculated_processes_per_host = num_processes;
    total_processes = arg_requested_nodes <= 0 ? num_processes * active_host_count : arg_requested_nodes;
  }
  else
  {
    total_processes = get_old_style_process_count();
    calculated_processes_per_host = (total_processes + active_host_count - 1) / active_host_count;
  }

  const int num_new_processes = total_processes - active_host_count;
  const int new_processes_per_host = (num_new_processes + active_host_count - 1) / active_host_count;

  for (nodetab_process & p : my_process_table)
  {
    p.forkstart = active_host_count + p.nodeno * new_processes_per_host;
    p.host->processes = 1;
  }

  for (int i = 0; i < num_new_processes; ++i)
  {
    nodetab_process & src = my_process_table[i % active_host_count];
    phase2_processes.push_back(src);

    nodetab_process & p = phase2_processes.back();
    p.nodeno = src.forkstart + (src.host->processes++ - 1);
  }
}

static void start_nodes_local(const std::vector<nodetab_process> &);
static void start_nodes_ssh(std::vector<nodetab_process> &);
static void finish_nodes(std::vector<nodetab_process> &);

static void req_client_connect(std::vector<nodetab_process> & process_table)
{
  skt_set_abort(client_connect_problem_skt);

  std::vector<nodetab_process> phase2_processes;

  if (arg_mpiexec)
  {
    req_construct_phase2_processes(phase2_processes);
    req_add_phase2_processes(phase2_processes);
    req_client_connect_table(process_table);
    req_all_clients_connected();
    return;
  }

  req_client_connect_table(process_table);
  req_construct_phase2_processes(phase2_processes);

  if (phase2_processes.size() > 0)
  {
    if (!arg_scalable_start)
    {
      if (!arg_local)
      {
#if CMK_SHRINK_EXPAND
        if (!arg_shrinkexpand || (arg_requested_pes > arg_old_pes))
#endif
        {
          assert(!arg_mpiexec);
          start_nodes_ssh(phase2_processes);
        }
#if !CMK_SSH_KILL
#if CMK_USE_SSH
        if (!arg_ssh_display)
#endif
          finish_nodes(phase2_processes);
#endif
      }
      else
      {
        start_nodes_local(phase2_processes);
      }
    }
    else
    {
      // send nodefork packets
      ChMessageHeader hdr;
      ChMessageInt_t mydata[ChInitNodeforkFields];
      ChMessageHeader_new("nodefork", sizeof(mydata), &hdr);
      for (const nodetab_process & p : process_table)
      {
        int numforks = p.host->processes - 1;
        if (numforks <= 0)
          continue;

        if (arg_verbose)
          printf("Charmrun> Instructing host \"%s\" to fork() x %d\n", p.host->name, numforks);

        mydata[0] = ChMessageInt_new(numforks);
        mydata[1] = ChMessageInt_new(p.forkstart);
        skt_sendN(p.req_client, (const char *) &hdr, sizeof(hdr));
        skt_sendN(p.req_client, (const char *) mydata, sizeof(mydata));
      }
    }

    req_client_connect_table(phase2_processes);
  }

  req_add_phase2_processes(phase2_processes);
  req_all_clients_connected();
}

static void req_add_phase2_processes(std::vector<nodetab_process> & phase2_processes)
{
  // add phase-two processes to main table
  my_process_table.insert(my_process_table.end(), phase2_processes.begin(), phase2_processes.end());
}

static void req_all_clients_connected()
{
  if (portOk == 0)
    exit(1);
  if (arg_verbose)
    printf("Charmrun> All clients connected.\n");

  // determine ppn
  int ppn = 1;
#if CMK_SMP
  if (onewth_per.active())
  {
    using Unit = typename TopologyRequest::Unit;

    const nodetab_process & p0 = my_process_table[0];

    int threads_per_host;
    const Unit onewth_unit = onewth_per.unit();
    switch (onewth_unit)
    {
      case Unit::Socket:
        threads_per_host = p0.num_sockets;
        break;
      case Unit::Core:
        threads_per_host = p0.num_cores;
        break;
      case Unit::PU:
        threads_per_host = p0.num_pus;
        break;

      // case Unit::Host:
      default:
        threads_per_host = 1;
        break;
    }

    // account for comm thread, except when space is unavailable
    // assumes that proc_per.xyz == 1, which is enforced in this situation during arg checking
    if (threads_per_host > calculated_processes_per_host && threads_per_host + calculated_processes_per_host > p0.num_pus)
      threads_per_host -= calculated_processes_per_host;

    if (threads_per_host == 0)
      threads_per_host = 1;

    if (threads_per_host < calculated_processes_per_host || threads_per_host % calculated_processes_per_host != 0)
    {
      fprintf(stderr, "Charmrun> Error: Invalid request for %d PEs among %d processes per host.\n",
                      threads_per_host, calculated_processes_per_host);
      kill_all_compute_nodes("Invalid provisioning request");
      exit(1);
    }

    ppn = threads_per_host / calculated_processes_per_host;
  }
  else
  {
    if (arg_ppn > 1)
      ppn = arg_ppn;
    else if (arg_requested_pes > 0 && arg_requested_nodes > 0)
      ppn = arg_requested_pes / arg_requested_nodes;
  }
#endif

  // sort them so that node number locality implies physical locality
  std::stable_sort(my_process_table.begin(), my_process_table.end());

  int newno = 0;
  for (nodetab_process & p : my_process_table)
  {
    // assign new numbering
    p.nodeno = newno++;

    // inform the node of any SMP threads to spawn
    p.PEs = ppn;

    // record each PE's process for our own purposes
    for (int j = 0; j < ppn; ++j)
      pe_to_process_map.push_back(&p);
  }

  for (nodetab_process & p : my_process_table)
    nodeinfo_populate(p);

#ifdef HSTART
  if (arg_hierarchical_start) {
    /* first we need to send data to parent charmrun and then send the nodeinfo
     * to the clients*/
    send_myNodeInfo_to_parent();
    /*then receive from root */
    forward_nodetab_to_children();
  }

  else
#endif
  {
    send_clients_nodeinfo();
#if CMK_USE_IBVERBS || CMK_USE_IBUD
    receive_qplist();
#if CMK_USE_IBVERBS
    exchange_qpdata_clients();
#endif
    send_clients_qpdata();
#endif
  }

  if (arg_verbose)
    printf("Charmrun> IP tables sent.\n");
}

/*Wait for all the clients to connect to our server port, then collect and send
 * nodetable to all */
#ifdef HSTART
static void req_charmrun_connect(void)
{
  //	double t1, t2, t3, t4;

  skt_set_abort(client_connect_problem_skt);

#if CMK_IBVERBS_FAST_START
  for (int c = 0, c_end = my_process_table.size(); c < c_end; ++c)
    req_one_client_partinit(my_process_table, c);
  for (nodetab_process & p : my_process_table)
    read_initnode_one_client(p);
#else
  // if(!arg_child_charmrun) getthetime(t1);

  req_set_client_connect(my_process_table, my_process_table.size());
// if(!arg_child_charmrun)	getthetime(t2);		/* also need to process
// received nodesets JIT */
#endif

  // TODO: two-phase, if applicable (?)

  if (portOk == 0)
    exit(1);
  if (arg_verbose)
    printf("Charmrun> All clients connected.\n");

  // TODO: nodeinfo_populate?

#if CMK_USE_IBVERBS || CMK_USE_IBUD
  send_clients_nodeinfo();
  receive_qplist();
#if CMK_USE_IBVERBS
  exchange_qpdata_clients();
#endif
  send_clients_qpdata();
#else
  for (const nodetab_process & p : my_process_table)
    // add flag to check what level charmrun it is and what phase
    req_handle_initnodedistribution(NULL, p);

  // getthetime(t3);

  /* Now receive the nodetab from child charmruns*/
  charmrun_phase = 1;

  skt_set_abort(client_connect_problem_skt);

  req_set_client_connect(my_process_table, my_process_table.size());

  send_clients_nodeinfo();

// if(!arg_child_charmrun) getthetime(t4);
#endif
  if (arg_verbose)
    printf("Charmrun> IP tables sent.\n");
  // if(!arg_child_charmrun) printf("Time for charmruns connect= %f , sending
  // nodes to fire= %f, node clients connected= %f n ", t2-t1, t3-t2, t4-t3);
}

#endif

static void start_one_node_ssh(nodetab_process & p, const char ** argv = arg_argv);


static void start_nodes_batch_and_connect(std::vector<nodetab_process> & process_table)
{
  int batch = arg_batch_spawn; /* fire several at a time */
  const int process_count = process_table.size();
  int clientstart = 0;
  do
  {
    int clientend = clientstart + batch;
    if (clientend > process_count)
      clientend = process_count;

    for (int c = clientstart; c < clientend; ++c)
      start_one_node_ssh(process_table[c]);

#if !CMK_SSH_KILL
#if CMK_USE_SSH
    /* ssh x11 forwarding will make sure ssh exit */
    if (!arg_ssh_display)
#endif
      finish_set_nodes(process_table, clientstart, clientend);
#endif

    // batch implementation of req_client_connect functionality below this line to end of function

#if CMK_IBVERBS_FAST_START
    for (int c = clientstart; c < clientend; ++c)
      req_one_client_partinit(process_table, c);
#else
    req_set_client_connect(process_table, clientend-clientstart);
#endif

    clientstart = clientend;
  }
  while (clientstart < process_count);

#if CMK_IBVERBS_FAST_START
  for (nodetab_process & p : process_table)
    read_initnode_one_client(p);
#endif
}

static void batch_launch_sequence(std::vector<nodetab_process> & process_table)
{
  skt_set_abort(client_connect_problem_skt);

  start_nodes_batch_and_connect(process_table);

  // batch implementation of req_client_connect functionality below this line to end of function

  std::vector<nodetab_process> phase2_processes;
  req_construct_phase2_processes(phase2_processes);
  if (phase2_processes.size() > 0)
  {
    if (!arg_scalable_start)
    {
      start_nodes_batch_and_connect(phase2_processes);
    }
    else
    {
      // send nodefork packets
      int total = 0;
      ChMessageHeader hdr;
      ChMessageInt_t mydata[ChInitNodeforkFields];
      ChMessageHeader_new("nodefork", sizeof(mydata), &hdr);
      for (const nodetab_process & p : process_table)
      {
        int numforks = p.host->processes - 1;
        if (numforks <= 0)
          continue;

        for (int c = 0; c < numforks; c += arg_batch_spawn)
        {
          const int count = std::min(numforks - c, arg_batch_spawn);

          if (arg_verbose)
            printf("Charmrun> Instructing host \"%s\" to fork() x %d\n", p.host->name, count);

          mydata[0] = ChMessageInt_new(count);
          mydata[1] = ChMessageInt_new(p.forkstart + c);
          skt_sendN(p.req_client, (const char *) &hdr, sizeof(hdr));
          skt_sendN(p.req_client, (const char *) mydata, sizeof(mydata));

#if CMK_IBVERBS_FAST_START
          for (int f = 0; f < count; ++f)
            req_one_client_partinit(phase2_processes, total++);
#else
          req_set_client_connect(phase2_processes, count);
#endif
        }
      }

#if CMK_IBVERBS_FAST_START
      for (nodetab_process & p : phase2_processes)
        read_initnode_one_client(p);
#endif
    }
  }

  req_add_phase2_processes(phase2_processes);
  req_all_clients_connected();
}


/*Start the server socket the clients will connect to.*/
static void req_start_server(void)
{
  skt_ip_t ip = skt_innode_my_ip();
  server_port = 0;
#if CMK_SHRINK_EXPAND
  if (arg_shrinkexpand) { // Need port information
    char *ns = getenv("NETSTART");
    if (ns != 0) { /*Read values set by Charmrun*/
      int node_num, old_charmrun_pid, port;
      char old_charmrun_name[1024 * 1000];
      int nread = sscanf(ns, "%d%s%d%d%d", &node_num, old_charmrun_name,
                         &server_port, &old_charmrun_pid, &port);
      if (nread != 5) {
        fprintf(stderr, "Error parsing NETSTART '%s'\n", ns);
        exit(1);
      }
    }
  }
#endif
  if (arg_local)
    /* local execution, use localhost always */
    strcpy(server_addr, "127.0.0.1");
  else if (arg_charmrunip != NULL)
    /* user specify the IP at +useip */
    strcpy(server_addr, arg_charmrunip);
  else if ((arg_charmrunip = getenv("CHARMRUN_IP")) != NULL)
    /* user specify the env  */
    strcpy(server_addr, arg_charmrunip);
  else if (skt_ip_match(ip, _skt_invalid_ip)) {
    fprintf(stderr, "Charmrun> Warning-- cannot find IP address for your hostname.  "
           "Using loopback.\n");
    strcpy(server_addr, "127.0.0.1");
  } else if (arg_usehostname || skt_ip_match(ip, skt_lookup_ip("127.0.0.1")))
    /*Use symbolic host name as charmrun address*/
    gethostname(server_addr, sizeof(server_addr));
  else
    skt_print_ip(server_addr, ip);

#if CMK_SHRINK_EXPAND
  server_port = arg_charmrun_port;
#else
  server_port = 0;
#endif
  server_fd = skt_server(&server_port);

  if (arg_verbose) {
    printf("Charmrun> Charmrun = %s, port = %d\n", server_addr, server_port);
  }

#if CMK_CCS_AVAILABLE
#ifdef HSTART
  if (!arg_hierarchical_start ||
      (arg_hierarchical_start && !arg_child_charmrun))
#endif
    if (arg_server == 1)
      CcsServer_new(NULL, &arg_server_port, arg_server_auth);
#endif
}

#ifdef HSTART
/* Function copied from machine.C file */
static void parse_netstart(void)
{
  char *ns = getenv("NETSTART");
  if (ns != 0) { /*Read values set by Charmrun*/
    int port;
    char parent_charmrun_name[1024 * 1000];
    int nread = sscanf(ns, "%d%s%d%d%d", &mynodes_start, parent_charmrun_name,
                       &parent_charmrun_port, &parent_charmrun_pid, &port);
    parent_charmrun_IP = skt_lookup_ip(parent_charmrun_name);

    if (nread != 5) {
      fprintf(stderr, "Error parsing NETSTART '%s'\n", ns);
      exit(1);
    }
  }
#if CMK_USE_IBVERBS | CMK_USE_IBUD
  char *cmi_num_nodes = getenv("CmiNumNodes");
  if (cmi_num_nodes != NULL) {
    sscanf(cmi_num_nodes, "%d", &_Cmi_numnodes);
  }
#endif
}

static int hstart_total_hosts;
/* Receive nodes for which I am responsible*/
static void my_nodetab_store(ChMessage *msg)
{
  ChMessageInt_t * nodelistmsg = (ChMessageInt_t *) msg->data;
  const int hstart_hosts_size = ChMessageInt(nodelistmsg[0]);
  hstart_total_hosts = ChMessageInt(nodelistmsg[1]);
  my_host_table.reserve(hstart_hosts_size);
  ChMessageInt_t * hstart_hosts = nodelistmsg + 2;
  for (int k = 0; k < hstart_hosts_size; k++)
    my_host_table.push(host_table[ChMessageInt(hstart_hosts[k])]);
}

/* In hierarchical startup, this function is used by child charmrun to obtains
 * the list of nodes for which it is responsible */
static void nodelist_obtain(void)
{
#if CMK_USE_IBVERBS
# if 0
  {
    int qpListSize = (_Cmi_numnodes-1)*sizeof(ChInfiAddr);
    me.info.qpList = malloc(qpListSize);
    copyInfiAddr(me.info.qpList);
    MACHSTATE1(3,"me.info.qpList created and copied size %d bytes",qpListSize);
    ctrl_sendone_nolock("initnode",(const char *)&me,sizeof(me),(const char *)me.info.qpList,qpListSize);
    free(me.info.qpList);
  }
# endif
#else
  ChMessageHeader hdr;
  ChMessageInt_t node_start = ChMessageInt_new(mynodes_start);
  ChMessageHeader_new("initnodetab", sizeof(ChMessageInt_t), &hdr);
  skt_sendN(parent_charmrun_fd, (const char *) &hdr, sizeof(hdr));
  skt_sendN(parent_charmrun_fd, (const char *) &node_start, sizeof(node_start));
#endif // CMK_USE_IBVERBS

  ChMessage nodelistmsg; /* info about all nodes*/
                         /*Contact charmrun for machine info.*/
  /*We get the other node addresses from a message sent
    back via the charmrun control port.*/
  if (!skt_select1(parent_charmrun_fd, 1200 * 1000)) {
    exit(0);
  }
  ChMessage_recv(parent_charmrun_fd, &nodelistmsg);

  my_nodetab_store(&nodelistmsg);
  ChMessage_free(&nodelistmsg);
}

static void init_mynodes(void)
{
  parse_netstart();
  if (!skt_ip_match(parent_charmrun_IP, _skt_invalid_ip)) {
    dataskt = skt_server(&dataport);
    parent_charmrun_fd =
        skt_connect(parent_charmrun_IP, parent_charmrun_port, 1800);
  } else {
    parent_charmrun_fd = -1;
  }

  nodelist_obtain();
}
#endif

/****************************************************************************
 *
 *  The Main Program
 *
 ****************************************************************************/
static void start_nodes_daemon(std::vector<nodetab_process> &);
static void start_nodes_mpiexec();
#ifdef HSTART
static void start_next_level_charmruns(void);
#endif
static void kill_nodes(void);
static void open_gdb_info(void);
static void read_global_segments_size(void);

static void fast_idleFn(void) { sleep(0); }

static char **main_envp;

int main(int argc, const char **argv, char **envp)
{
  static char s_FROM_CHARMRUN_1[] = "FROM_CHARMRUN=1";
  putenv(s_FROM_CHARMRUN_1);

  srand(time(0));
  skt_init();
  skt_set_idle(fast_idleFn);
/* CrnSrand((int) time(0)); */

  main_envp = envp;
  /* Compute the values of all constants */
  arg_init(argc, argv);
  if (arg_verbose)
    printf("Charmrun> charmrun started...\n");

  start_timer = GetClock();
  /* Initialize the node-table by reading nodesfile */
  nodetab_init();

  if (arg_requested_numhosts > 0)
  {
    if (arg_requested_numhosts > host_table.size())
    {
      fprintf(stderr, "Charmrun> Error: ++numHosts exceeds available host pool.\n");
      exit(1);
    }
    else
      host_table.resize(arg_requested_numhosts);
  }

  if (arg_verbose)
  {
    char ips[200];
    for (const nodetab_host * h : host_table)
    {
      skt_print_ip(ips, h->ip);
      printf("Charmrun> added host \"%s\", IP:%s\n", h->name, ips);
    }
  }

#ifdef HSTART
  if (arg_hierarchical_start)
    nodetab_init_hierarchical_start();
#endif

  /* Start the server port */
  req_start_server();

  /* Initialize the IO module */
  input_init();

#ifdef HSTART
  /* Hierarchical startup*/
  if (arg_child_charmrun) {
    init_mynodes(); /* contacts root charmrun and gets list  of nodes to start*/
  }
  else
    my_host_table = host_table;
#endif

  const int my_host_count = my_host_table.size();
  const int my_initial_process_count = proc_per.active()
                                       ? (arg_requested_nodes > 0 ? std::min(my_host_count, arg_requested_nodes) : my_host_count)
                                       : std::min(my_host_count, get_old_style_process_count());
  my_process_table.resize(my_initial_process_count);
  for (int i = 0; i < my_initial_process_count; ++i)
  {
    nodetab_host * h = my_host_table[i];
    nodetab_process & p = my_process_table[i];
    p.host = h;
    p.nodeno = h->hostno;
  }

  /* start the node processes */
  if (0 != getenv("CONV_DAEMON"))
    start_nodes_daemon(my_process_table);
  else
#if CMK_USE_IBVERBS
    PRINT(("Charmrun> IBVERBS version of charmrun\n"));
#endif

#ifdef HSTART
  /* Hierarchical-startup*/
  if (arg_hierarchical_start) {
    if (!arg_local) {
      if (!arg_child_charmrun) {
        start_next_level_charmruns();
      } else {
        if (!arg_batch_spawn)
          start_nodes_ssh(my_process_table);
        else
          batch_launch_sequence(my_process_table);
      }
    } else
      start_nodes_local(my_process_table);
  }

  /* Normal startup*/
  else

#endif
  {
    if (!arg_local) {
      if (!arg_batch_spawn) {
#if CMK_SHRINK_EXPAND
        //  modified rsh in shrink expand, need to launch only new ones,
        //  preserve some info between new and old
        if (!arg_shrinkexpand || (arg_requested_pes > arg_old_pes))
#endif
        {
          if (arg_mpiexec)
            start_nodes_mpiexec();
          else
            start_nodes_ssh(my_process_table);
        }
      } else
        batch_launch_sequence(my_process_table);
    } else
      start_nodes_local(my_process_table);
  }

  if (arg_charmdebug) {
#if defined(_WIN32)
    /* Gdb stream (and charmdebug) currently valid only with ssh subsystem */
    fprintf(stderr,
            "Charmdebug is supported currently only with the ssh subsystem\n");
    abort();
#else
    /* Open an additional connection to node 0 with a gdb to grab info */
    PRINT(("opening connection with node 0 for info gdb\n"));
    read_global_segments_size();
    open_gdb_info();
    gdb_stream = fdopen(dup(2), "a");
    dup2(1, 2);
#endif
  }

  if (arg_verbose)
    printf("Charmrun> node programs all started\n");

/* Wait for all clients to connect */
#ifdef HSTART
  /* Hierarchical startup*/
  if (arg_hierarchical_start) {
#if !CMK_SSH_KILL
#if CMK_USE_SSH
    if ((!arg_batch_spawn || (!arg_child_charmrun)) && !arg_ssh_display)
#else
    if (!arg_batch_spawn || (!arg_child_charmrun))
#endif
      finish_nodes(my_process_table);
#endif

    if (!arg_child_charmrun)
      req_charmrun_connect();
    else if (!arg_batch_spawn)
      req_client_connect(my_process_table);
  }
  /* Normal startup*/
  else
#endif
  {
#if !CMK_SSH_KILL
#if CMK_USE_SSH
    if (!arg_batch_spawn && !arg_ssh_display)
#else
    if (!arg_batch_spawn)
#endif
      finish_nodes(my_process_table);
#endif
    if (!arg_batch_spawn)
      req_client_connect(my_process_table);
  }
#if CMK_SSH_KILL
#if CMK_USE_SSH
  if (!arg_ssh_display)
#endif
    kill_nodes();
#endif
  if (arg_verbose)
    printf("Charmrun> node programs all connected\n");
  /* report time */
  PRINT(("Charmrun> started all node programs in %.3f seconds.\n",
          GetClock() - start_timer));

/* enter request-service mode */
#ifdef HSTART
  if (arg_hierarchical_start)
    while (1)
      req_poll_hierarchical();
  else
#endif
  {
    if (arg_timelimit == -1)
    {
      while (1)
        req_poll();
    }
    else
    {
      time_t start = time(NULL);
      while (1)
      {
        req_poll();
        time_t end = time(NULL);
        double elapsed = difftime(end, start);
        if (elapsed >= arg_timelimit)
        {
          fprintf(stderr, "Charmrun> Error: Time limit reached\n");

          kill_all_compute_nodes("Time limit reached");
          for (const nodetab_process & p : my_process_table)
            skt_close(p.req_client);
          exit(1);
        }
      }
    }
  }
}

/*This little snippet creates a NETSTART
environment variable entry for the given node #.
It uses the idiotic "return reference to static buffer"
string return idiom.
*/
static char *create_netstart(int node)
{
  static char dest[1536];
  int port = 0;
  if (arg_mpiexec)
    sprintf(dest, "$CmiMyNode %s %d %d %d", server_addr, server_port,
            getpid() & 0x7FFF, port);
  else
    sprintf(dest, "%d %s %d %d %d", node, server_addr, server_port,
            getpid() & 0x7FFF, port);
  return dest;
}

/* The remainder of charmrun is only concerned with starting all
the node-programs, also known as charmrun clients.  We have to
start nodetab_rank0_table.size() processes on the remote machines.
*/

/*Ask the converse daemon running on each machine to start the node-programs.*/
static void start_nodes_daemon(std::vector<nodetab_process> & process_table)
{
  /*Set the parts of the task structure that will be the same for all nodes*/
  /*Figure out the command line arguments (same for all PEs)*/
  char argBuffer[5000] = { '\0' }; /*Buffer to hold assembled program arguments*/
  for (int i = 0; arg_argv[i]; i++) {
    if (arg_verbose)
      printf("Charmrun> packing arg: %s\n", arg_argv[i]);
    strcat(argBuffer, " ");
    strcat(argBuffer, arg_argv[i]);
  }

  taskStruct task;
  task.magic = ChMessageInt_new(DAEMON_MAGIC);

  /*Start up the user program, by sending a message
    to PE 0 on each node.*/
  for (const nodetab_process & p : process_table)
  {
    const nodetab_host * h = p.host;

    char *arg_currdir_r = pathfix(arg_currdir_a, h->pathfixes);
    strcpy(task.cwd, arg_currdir_r);
    free(arg_currdir_r);
    char *arg_nodeprog_r = pathextfix(arg_nodeprog_a, h->pathfixes, h->ext);
    strcpy(task.pgm, arg_nodeprog_r);

    if (arg_verbose)
      printf("Charmrun> Starting node program %d on '%s' as %s.\n", p.nodeno,
             h->name, arg_nodeprog_r);
    free(arg_nodeprog_r);
    sprintf(task.env, "NETSTART=%s", create_netstart(p.nodeno));

    char nodeArgBuffer[5120]; /*Buffer to hold assembled program arguments*/
    char *argBuf;
    if (h->nice != -100) {
      if (arg_verbose)
        printf("Charmrun> +nice %d\n", h->nice);
      sprintf(nodeArgBuffer, "%s +nice %d", argBuffer, h->nice);
      argBuf = nodeArgBuffer;
    } else
      argBuf = argBuffer;
    task.argLength = ChMessageInt_new(strlen(argBuf));

    /*Send request out to remote node*/
    char statusCode = 'N'; /*Default error code-- network problem*/
    int fd = skt_connect(h->ip, DAEMON_IP_PORT, 30);
    if (fd !=
        INVALID_SOCKET) { /*Contact!  Ask the daemon to start the program*/
      skt_sendN(fd, (const char *) &task, sizeof(task));
      skt_sendN(fd, (const char *) argBuf, strlen(argBuf));
      skt_recvN(fd, &statusCode, sizeof(char));
    }
    if (statusCode != 'G') { /*Something went wrong--*/
      fprintf(stderr, "Error '%c' starting remote node program on %s--\n%s\n",
              statusCode, h->name, daemon_status2msg(statusCode));
      exit(1);
    } else if (arg_verbose)
      printf("Charmrun> Node program %d started.\n", p.nodeno);
  }
}

#if defined(_WIN32)
/*Sadly, interprocess communication on Win32 is quite
  different, so we can't use Ssh on win32 yet.
  Fall back to the daemon.*/
static void start_nodes_ssh(std::vector<nodetab_process> & process_table) { start_nodes_daemon(process_table); }
static void finish_nodes(std::vector<nodetab_process> & process_table) {}
static void start_one_node_ssh(nodetab_process & p, const char ** argv) {}
static void start_nodes_mpiexec() {}

static void finish_set_nodes(std::vector<nodetab_process> & process_table, int start, int stop, bool charmrun_exiting) {}

/* simple version of charmrun that avoids the sshd or charmd,   */
/* it spawn the node program just on local machine using exec. */
struct local_nodestart
{
  std::string cmdLine; /*Program command line, including executable name*/

  local_nodestart(const char ** extra_argv = nullptr)
    : cmdLine{pparam_argv[1]}
  {
    append_argv(pparam_argv + 2);
    if (extra_argv != nullptr)
      append_argv(extra_argv);
  }

  void append_argv(const char ** param)
  {
    while (*param) {
      cmdLine += " ";
      cmdLine += *param;
      param++;
    }
  }

  void start(const nodetab_process & p)
  {
    std::string env{"NETSTART="};
    env += create_netstart(p.nodeno);
    env += '\0';

    /* Concatenate all system environment strings */
    const LPTSTR oldEnv = GetEnvironmentStrings();
    LPTSTR oldEnvEnd = oldEnv;
    while (oldEnvEnd[0] != '\0' || oldEnvEnd[1] != '\0')
        ++oldEnvEnd;
    env.append(oldEnv, oldEnvEnd - oldEnv + 1);
    FreeEnvironmentStrings(oldEnv);

    /* Initialise the security attributes for the process
     to be spawned */
    STARTUPINFO si = {0};
    si.cb = sizeof(si);
    if (arg_verbose)
      printf("Charmrun> start %d node program on localhost.\n", p.nodeno);

    PROCESS_INFORMATION pi;
    int ret;
    ret = CreateProcess(NULL,          /* application name */
                        const_cast<char*>(cmdLine.c_str()), /* command line */
                        NULL, /*&sa,*/ /* process SA */
                        NULL, /*&sa,*/ /* thread SA */
                        FALSE,         /* inherit flag */
#if CMK_CHARM4PY
                        // don't disable console output on rank 0 process (need to be able to see python syntax errors, etc)
                        CREATE_NEW_PROCESS_GROUP | (p.nodeno == 0 ? 0 : DETACHED_PROCESS),
#elif 1
                        CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS,
#else
                        CREATE_NEW_PROCESS_GROUP | CREATE_NEW_CONSOLE,
#endif
                        /* creation flags */
                        const_cast<char*>(env.data()), /* environment block */
                        ".",         /* working directory */
                        &si,         /* startup info */
                        &pi);

    if (ret == 0) {
      /*Something went wrong!  Look up the Windows error code*/
      /*
                int error=GetLastError();
                char statusCode=daemon_err2status(error);
                fprintf(logfile,"******************* ERROR *****************\n"
                        "Error in creating process!\n"
                        "Error code = %ld-- %s\n\n\n", error,
                        daemon_status2msg(statusCode));
                fflush(logfile);
      */
      int error = GetLastError();
      fprintf(stderr, "startProcess failed to start process \"%s\" with status: %d\n",
             pparam_argv[1], error);
      exit(1);
    }
  }
};

static void start_nodes_local(const std::vector<nodetab_process> & process_table)
{
  local_nodestart state;

  for (const nodetab_process & p : process_table)
    state.start(p);
}

#else
/*Unix systems can use Ssh normally*/
/********** SSH-ONLY CODE *****************************************/
/*                                                                          */
/* Ssh_etc                                                                  */
/*                                                                          */
/* this starts all the node programs.  It executes fully in the background. */
/*                                                                          */
/****************************************************************************/
#include <sys/wait.h>

extern char **environ;
static void removeEnv(const char *doomedEnv)
{ /*Remove a value from the environment list*/
  char **oe, **ie;
  oe = ie = environ;
  while (*ie != NULL) {
    if (0 != strncmp(*ie, doomedEnv, strlen(doomedEnv)))
      *oe++ = *ie;
    ie++;
  }
  *oe = NULL; /*NULL-terminate list*/
}

static int ssh_fork(const nodetab_process & p, const char *startScript)
{
  const nodetab_host * h = p.host;

  std::vector<const char *> sshargv;

  const char *s = h->shell;
  const char *e = skipstuff(s);
  while (*s) {
    sshargv.push_back(substr(s, e));
    s = skipblanks(e);
    e = skipstuff(s);
  }

  if (p.nodeno == 0 && arg_interactive) {
    // in interactive mode, we use a different ssh command for process 0, that requires
    // the start script to exist on that host, so we scp it there first
    std::vector<const char *> scpargv = {"scp", "-o", "KbdInteractiveAuthentication=no",
                                         "-o", "PasswordAuthentication=no",
                                         "-o", "NoHostAuthenticationForLocalhost=yes"};
    scpargv.push_back(startScript);
    std::string login_and_host;
    login_and_host += h->login;
    login_and_host += "@";
    login_and_host += h->name;
    login_and_host += ":";
    login_and_host += startScript;
    scpargv.push_back(login_and_host.c_str());
    scpargv.push_back((const char *) NULL);

    if (arg_verbose) {
      std::string cmd_str = scpargv[0];
      for (int n = 1; n < scpargv.size() - 1; ++n)
        cmd_str += " " + std::string(scpargv[n]);
      printf("Charmrun> scp command: %s\n", cmd_str.c_str());
    }

    int pid = fork();
    if (pid < 0) {
      perror("ERROR> sending ssh start script to process 0");
      exit(1);
    }
    if (pid == 0) { /* Child process */
      execvp(scpargv[0], const_cast<char **>(&scpargv[0]));
      fprintf(stderr, "Charmrun> Couldn't start scp '%s'!\n", scpargv[0]);
      exit(1);
    } else {
      waitpid(pid, NULL, 0);
    }

    // for ssh connection to process 0, use -t to force tty allocation for interactive session
    sshargv.push_back("-t");
  }

  sshargv.push_back(h->name);
  if (arg_ssh_display)
    sshargv.push_back("-X");
  sshargv.push_back("-l");
  sshargv.push_back(h->login);
  sshargv.push_back("-o");
  sshargv.push_back("KbdInteractiveAuthentication=no");
  sshargv.push_back("-o");
  sshargv.push_back("PasswordAuthentication=no");
  sshargv.push_back("-o");
  sshargv.push_back("NoHostAuthenticationForLocalhost=yes");
  std::string remote_command;
  if (p.nodeno == 0 && arg_interactive) {
    remote_command += "/bin/bash ";
    remote_command += startScript;
    sshargv.push_back(remote_command.c_str());
  } else {
    sshargv.push_back("/bin/bash -f");
  }
  sshargv.push_back((const char *) NULL);

  if (arg_verbose) {
    std::string cmd_str = sshargv[0];
    for (int n = 1; n < sshargv.size()-1; ++n)
      cmd_str += " " + std::string(sshargv[n]);
    printf("Charmrun> Starting %s\n", cmd_str.c_str());
  }

  int pid = fork();
  if (pid < 0) {
    perror("ERROR> starting remote shell");
    exit(1);
  }
  if (pid == 0) { /*Child process*/
    // in interactive mode we don't want to redirect stdin for process 0
    if (p.nodeno != 0 || !arg_interactive) {
      int fdScript = open(startScript, O_RDONLY);
      /**/ unlink(startScript); /**/
      dup2(fdScript, 0);        /*Open script as standard input*/
    }
    // removeEnv("DISPLAY="); /*No DISPLAY disables ssh's slow X11 forwarding*/
    for (int i = 3; i < 1024; i++)
      close(i);
    execvp(sshargv[0], const_cast<char **>(&sshargv[0]));
    fprintf(stderr, "Charmrun> Couldn't find remote shell program '%s'!\n",
            sshargv[0]);
    exit(1);
  }
  if (arg_verbose)
    printf("Charmrun> remote shell (%s:%d) started\n", h->name, p.nodeno);
  return pid;
}

static void fprint_arg(FILE *f, const char **argv)
{
  while (*argv) {
    fprintf(f, " %s", *argv);
    argv++;
  }
}
static void ssh_Find(FILE *f, const char *program, const char *dest)
{
  fprintf(f, "Find %s\n", program);
  fprintf(f, "%s=$loc\n", dest);
}
static void ssh_script(FILE *f, const nodetab_process & p, const char **argv)
{
  const nodetab_host * h = p.host;
  const char *dbg = h->debugger;
  const char *host = h->name;
  const int nodeno = p.nodeno;

  if (arg_mpiexec)
    fprintf(f, "#!/bin/sh\n");

  fprintf(f, /*Echo: prints out status message*/
          "Echo() {\n"
          "  echo 'Charmrun remote shell(%s.%d)>' $*\n"
          "}\n",
          host, nodeno);
  fprintf(f, /*Exit: exits with return code*/
          "Exit() {\n"
          "  if [ $1 -ne 0 ]\n"
          "  then\n"
          "    Echo Exiting with error code $1\n"
          "  fi\n");
#if CMK_SSH_KILL /*End by killing ourselves*/
#if CMK_USE_SSH
  if (!arg_ssh_display)
#endif
    fprintf(f,
            "  sleep 5\n" /*Delay until any error messages are flushed*/
            "  kill -9 $$\n");
#endif           /*Exit normally*/
  fprintf(f,
          "  exit $1\n"
          "}\n");
  fprintf(f, /*Find: locates a binary program in PATH, sets loc*/
          "Find() {\n"
          "  loc=''\n"
          "  for dir in `echo $PATH | sed -e 's/:/ /g'`\n"
          "  do\n"
          "    test -f \"$dir/$1\" && loc=\"$dir/$1\"\n"
          "  done\n"
          "  if [ \"x$loc\" = x ]\n"
          "  then\n"
          "    Echo $1 not found in your PATH \"($PATH)\"--\n"
          "    Echo set your path in your ~/.charmrunrc\n"
          "    Exit 1\n"
          "  fi\n"
          "}\n");

  if (arg_verbose)
    fprintf(f, "Echo 'remote responding...'\n");

  fprintf(f, "test -f \"$HOME/.charmrunrc\" && . \"$HOME/.charmrunrc\"\n");
  /* let's leave DISPLAY untouched and rely on X11 forwarding,
     changing DISPLAY to charmrun does not always work if X11 forwarding
     presents
  */
  if (arg_display && !arg_ssh_display)
    fprintf(f, "DISPLAY='%s';export DISPLAY\n", arg_display);

#ifdef HSTART
  if (arg_child_charmrun)
    fprintf(f, "NETMAGIC=\"%d\";export NETMAGIC\n",
            parent_charmrun_pid & 0x7FFF);
  else
#endif
    fprintf(f, "NETMAGIC=\"%d\";export NETMAGIC\n", getpid() & 0x7FFF);

  if (arg_mpiexec) {
    fprintf(f, "CmiMyNode=$OMPI_COMM_WORLD_RANK\n");
    fprintf(f, "test -z \"$CmiMyNode\" && CmiMyNode=$MPIRUN_RANK\n");
    fprintf(f, "test -z \"$CmiMyNode\" && CmiMyNode=$PMI_RANK\n");
    fprintf(f, "test -z \"$CmiMyNode\" && CmiMyNode=$PMI_ID\n");
    fprintf(f, "test -z \"$CmiMyNode\" && CmiMyNode=$MP_CHILD\n");
    fprintf(f, "test -z \"$CmiMyNode\" && CmiMyNode=$SLURM_PROCID\n");
    fprintf(f, "test -z \"$CmiMyNode\" && (Echo Could not detect rank from "
               "environment ; Exit 1)\n");
    fprintf(f, "export CmiMyNode\n");
  }
#ifdef HSTART
  else if (arg_hierarchical_start && arg_child_charmrun)
    fprintf(f, "CmiMyNode='%d'; export CmiMyNode\n", mynodes_start + nodeno);
#endif
  else
    fprintf(f, "CmiMyNode='%d'; export CmiMyNode\n", nodeno);

  char *netstart;
#ifdef HSTART
  if (arg_hierarchical_start && arg_child_charmrun)
    netstart = create_netstart(mynodes_start + nodeno);
  else
#endif
    netstart = create_netstart(nodeno);
  fprintf(f, "NETSTART=\"%s\";export NETSTART\n", netstart);

  fprintf(f, "CmiMyNodeSize='%d'; export CmiMyNodeSize\n", h->cpus);

  fprintf(f, "CmiMyForks='%d'; export CmiMyForks\n", 0);

  // cpu affinity hints
  using Unit = typename TopologyRequest::Unit;
  switch (proc_per.unit())
  {
    case Unit::Host:
      fprintf(f, "CmiProcessPerHost='%d'; export CmiProcessPerHost\n", proc_per.host);
      break;
    case Unit::Socket:
      fprintf(f, "CmiProcessPerSocket='%d'; export CmiProcessPerSocket\n", proc_per.socket);
      break;
    case Unit::Core:
      fprintf(f, "CmiProcessPerCore='%d'; export CmiProcessPerCore\n", proc_per.core);
      break;
    case Unit::PU:
      fprintf(f, "CmiProcessPerPU='%d'; export CmiProcessPerPU\n", proc_per.pu);
      break;
    default:
      break;
  }
#if CMK_SMP
  switch (onewth_per.unit())
  {
    case Unit::Host:
      fprintf(f, "CmiOneWthPerHost='%d'; export CmiOneWthPerHost\n", 1);
      break;
    case Unit::Socket:
      fprintf(f, "CmiOneWthPerSocket='%d'; export CmiOneWthPerSocket\n", 1);
      break;
    case Unit::Core:
      fprintf(f, "CmiOneWthPerCore='%d'; export CmiOneWthPerCore\n", 1);
      break;
    case Unit::PU:
      fprintf(f, "CmiOneWthPerPU='%d'; export CmiOneWthPerPU\n", 1);
      break;
    default:
      break;
  }
#endif

  if (arg_mpiexec) {
    fprintf(f, "CmiNumNodes=$OMPI_COMM_WORLD_SIZE\n");
    fprintf(f, "test -z \"$CmiNumNodes\" && CmiNumNodes=$MPIRUN_NPROCS\n");
    fprintf(f, "test -z \"$CmiNumNodes\" && CmiNumNodes=$PMI_SIZE\n");
    fprintf(f, "test -z \"$CmiNumNodes\" && CmiNumNodes=$MP_PROCS\n");
    fprintf(f, "test -z \"$CmiNumNodes\" && CmiNumNodes=$SLURM_NTASKS\n");
    fprintf(f, "test -z \"$CmiNumNodes\" && CmiNumNodes=$SLURM_NPROCS\n");
    fprintf(f, "test -z \"$CmiNumNodes\" && (Echo Could not detect node count "
               "from environment ; Exit 1)\n");
    fprintf(f, "export CmiNumNodes\n");
  }
#ifdef HSTART
  else if (arg_hierarchical_start && arg_child_charmrun)
    fprintf(f, "CmiNumNodes='%d'; export CmiNumNodes\n", hstart_total_hosts);
#endif

  else
    fprintf(f, "CmiNumNodes='%d'; export CmiNumNodes\n", (int)my_process_table.size());

#ifdef CMK_GFORTRAN
  fprintf(f, "GFORTRAN_UNBUFFERED_ALL=YES; export GFORTRAN_UNBUFFERED_ALL\n");
#endif

  if (arg_verbose) {
    printf("Charmrun> Sending \"%s\" to client %d.\n", netstart, nodeno);
  }
  fprintf(f,
          "PATH=\"$PATH:/bin:/usr/bin:/usr/X/bin:/usr/X11/bin:/usr/local/bin:"
          "/usr/X11R6/bin:/usr/openwin/bin\"\n");

  /* find the node-program */
  char *arg_nodeprog_r = pathextfix(arg_nodeprog_a, h->pathfixes, h->ext);

  /* find the current directory, relative version */
  char *arg_currdir_r = pathfix(arg_currdir_a, h->pathfixes);

  if (arg_verbose) {
    printf("Charmrun> find the node program \"%s\" at \"%s\" for %d.\n",
           arg_nodeprog_r, arg_currdir_r, nodeno);
  }
  if (arg_debug || arg_debug_no_pause || arg_in_xterm) {
    ssh_Find(f, h->xterm, "F_XTERM");
    if (!arg_ssh_display && !arg_debug_no_xrdb)
      ssh_Find(f, "xrdb", "F_XRDB");
    if (arg_verbose)
      fprintf(f, "Echo 'using xterm' $F_XTERM\n");
  }

  if (arg_debug || arg_debug_no_pause) { /*Look through PATH for debugger*/
    ssh_Find(f, dbg, "F_DBG");
    if (arg_verbose)
      fprintf(f, "Echo 'using debugger' $F_DBG\n");
  }

  if (!arg_ssh_display && !arg_debug_no_xrdb &&
      (arg_debug || arg_debug_no_pause || arg_in_xterm)) {
    /*    if (arg_debug || arg_debug_no_pause || arg_in_xterm) {*/
    fprintf(f, "$F_XRDB -query > /dev/null\n");
    fprintf(f, "if test $? != 0\nthen\n");
    fprintf(f, "  Echo 'Cannot contact X Server '$DISPLAY'.  You probably'\n");
    fprintf(f, "  Echo 'need to run xhost to authorize connections.'\n");
    fprintf(f, "  Echo '(See manual for xhost for security issues)'\n");
    fprintf(f, "  Echo 'Or try ++ssh-display to rely on SSH X11 "
               "forwarding'\n");
    fprintf(f, "  Exit 1\n");
    fprintf(f, "fi\n");
  }

  fprintf(f, "if test ! -x \"%s\"\nthen\n", arg_nodeprog_r);
  fprintf(f, "  Echo 'Cannot locate this node-program: %s'\n", arg_nodeprog_r);
  fprintf(f, "  Exit 1\n");
  fprintf(f, "fi\n");

  fprintf(f, "cd \"%s\"\n", arg_currdir_r);
  fprintf(f, "if test $? = 1\nthen\n");
  fprintf(f, "  Echo 'Cannot propagate this current directory:'\n");
  fprintf(f, "  Echo '%s'\n", arg_currdir_r);
  fprintf(f, "  Exit 1\n");
  fprintf(f, "fi\n");

  if (strcmp(h->setup, "*")) {
    fprintf(f, "%s\n", h->setup);
    fprintf(f, "if test $? = 1\nthen\n");
    fprintf(f, "  Echo 'this initialization command failed:'\n");
    fprintf(f, "  Echo '\"%s\"'\n", h->setup);
    fprintf(f, "  Echo 'edit your nodes file to fix it.'\n");
    fprintf(f, "  Exit 1\n");
    fprintf(f, "fi\n");
  }

  fprintf(f, "rm -f /tmp/charmrun_err.$$\n");
  if (arg_verbose)
    fprintf(f, "Echo 'starting node-program...'\n");
  /* This is the start of the the run-nodeprogram script */
  fprintf(f, "(");

  if (arg_debug || arg_debug_no_pause) {
    if (strcmp(dbg, "gdb") == 0 || strcmp(dbg, "idb") == 0) {
      fprintf(f, "cat > /tmp/charmrun_gdb.$$ << END_OF_SCRIPT\n");
      if (strcmp(dbg, "idb") == 0) {
        fprintf(f, "set \\$cmdset=\"gdb\"\n");
      }
      fprintf(f, "shell /bin/rm -f /tmp/charmrun_gdb.$$\n");
      fprintf(f, "handle SIGPIPE nostop noprint\n");
      fprintf(f, "handle SIGWINCH nostop noprint\n");
      fprintf(f, "handle SIGWAITING nostop noprint\n");
      fprintf(f, "set disable-randomization %s\n", arg_va_rand ? "off" : "on");
      if (arg_debug_commands)
        fprintf(f, "%s\n", arg_debug_commands);
      fprintf(f, "set args");
      fprint_arg(f, argv);
      fprintf(f, "\n");
      if (arg_debug_no_pause)
        fprintf(f, "run\n");
      fprintf(f, "END_OF_SCRIPT\n");
      if (arg_runscript)
        fprintf(f, "\"%s\" ", arg_runscript);
      fprintf(f, "$F_XTERM");
      fprintf(f, " -title 'Node %d (%s)' ", nodeno, h->name);
      if (strcmp(dbg, "idb") == 0)
        fprintf(f, " -e $F_DBG \"%s\" -c /tmp/charmrun_gdb.$$ \n", arg_nodeprog_r);
      else
        fprintf(f, " -e $F_DBG \"%s\" -x /tmp/charmrun_gdb.$$ \n", arg_nodeprog_r);
    } else if (strcmp(dbg, "lldb") == 0) {
      fprintf(f, "cat > /tmp/charmrun_lldb.$$ << END_OF_SCRIPT\n");
      fprintf(f, "platform shell -- /bin/rm -f /tmp/charmrun_lldb.$$\n");
      fprintf(f, "settings set target.disable-aslr %s\n", arg_va_rand ? "false" : "true");
      // must launch before configuring signals, or else:
      // "error: No current process; cannot handle signals until you have a valid process."
      // use -s to stop at the entry point
      fprintf(f, "process launch -X true -s --");
      fprint_arg(f, argv);
      fprintf(f, "\n");
      fprintf(f, "process handle -s false -n false SIGPIPE SIGWINCH\n");
      if (arg_debug_commands)
        fprintf(f, "%s\n", arg_debug_commands);
      if (arg_debug_no_pause)
        fprintf(f, "continue\n");
      else
        fprintf(f, "# Use \"continue\" or \"c\" to begin execution.\n");
      fprintf(f, "END_OF_SCRIPT\n");
      if (arg_runscript)
        fprintf(f, "\"%s\" ", arg_runscript);
      fprintf(f, "$F_XTERM");
      fprintf(f, " -title 'Node %d (%s)' ", nodeno, h->name);
      fprintf(f, " -e $F_DBG \"%s\" -s /tmp/charmrun_lldb.$$ \n", arg_nodeprog_r);
    } else if (strcmp(dbg, "dbx") == 0) {
      fprintf(f, "cat > /tmp/charmrun_dbx.$$ << END_OF_SCRIPT\n");
      fprintf(f, "sh /bin/rm -f /tmp/charmrun_dbx.$$\n");
      fprintf(f, "dbxenv suppress_startup_message 5.0\n");
      fprintf(f, "ignore SIGPOLL\n");
      fprintf(f, "ignore SIGPIPE\n");
      fprintf(f, "ignore SIGWINCH\n");
      fprintf(f, "ignore SIGWAITING\n");
      if (arg_debug_commands)
        fprintf(f, "%s\n", arg_debug_commands);
      fprintf(f, "END_OF_SCRIPT\n");
      if (arg_runscript)
        fprintf(f, "\"%s\" ", arg_runscript);
      fprintf(f, "$F_XTERM");
      fprintf(f, " -title 'Node %d (%s)' ", nodeno, h->name);
      fprintf(f, " -e $F_DBG %s ", arg_debug_no_pause ? "-r" : "");
      if (arg_debug) {
        fprintf(f, "-c \'runargs ");
        fprint_arg(f, argv);
        fprintf(f, "\' ");
      }
      fprintf(f, "-s/tmp/charmrun_dbx.$$ %s", arg_nodeprog_r);
      if (arg_debug_no_pause)
        fprint_arg(f, argv);
      fprintf(f, "\n");
    } else {
      fprintf(stderr, "Unknown debugger: %s.\n Exiting.\n", h->debugger);
    }
  } else if (arg_in_xterm) {
    if (arg_verbose)
      printf("Charmrun> node %d: xterm is %s\n", nodeno, h->xterm);
    fprintf(f, "cat > /tmp/charmrun_inx.$$ << END_OF_SCRIPT\n");
    fprintf(f, "#!/bin/sh\n");
    fprintf(f, "/bin/rm -f /tmp/charmrun_inx.$$\n");
    if (!arg_va_rand)
      fprintf(f, "command -v setarch >/dev/null 2>&1 && SETARCH=\"setarch `uname -m` -R \" || ");
    fprintf(f, "SETARCH=\n");
    fprintf(f, "${SETARCH}%s", arg_nodeprog_r);
    fprint_arg(f, argv);
    fprintf(f, "\n");
    fprintf(f, "echo 'program exited with code '\\$?\n");
    fprintf(f, "read eoln\n");
    fprintf(f, "END_OF_SCRIPT\n");
    fprintf(f, "chmod 700 /tmp/charmrun_inx.$$\n");
    if (arg_runscript)
      fprintf(f, "\"%s\" ", arg_runscript);
    fprintf(f, "$F_XTERM -title 'Node %d (%s)' ", nodeno, h->name);
    fprintf(f, " -sl 5000");
    fprintf(f, " -e /tmp/charmrun_inx.$$\n");
  } else {
    if (!arg_va_rand)
      fprintf(f, "command -v setarch >/dev/null 2>&1 && SETARCH=\"setarch `uname -m` -R \" || ");
    fprintf(f, "SETARCH=\n");
    if (arg_runscript)
      fprintf(f, "\"%s\" ", arg_runscript);
    fprintf(f, "${SETARCH}\"%s\" ", arg_nodeprog_r);
    fprint_arg(f, argv);
    if (h->nice != -100) {
      if (arg_verbose)
        printf("Charmrun> nice -n %d\n", h->nice);
      fprintf(f, " +nice %d ", h->nice);
    }
    fprintf(f, "\nres=$?\n");
    /* If shared libraries fail to load, the program dies without
       calling charmrun back.  Since we *have* to close down stdin/out/err,
       we have to smuggle this failure information out via a file,
       /tmp/charmrun_err.<pid> */
    fprintf(f, "if [ $res -eq 127 ]\n"
               "then\n"
               "  ( \n" /* Re-run, spitting out errors from a subshell: */
               "    \"%s\" \n"
               "    ldd \"%s\"\n"
               "  ) > /tmp/charmrun_err.$$ 2>&1 \n"
               "fi\n",
            arg_nodeprog_r, arg_nodeprog_r);
  }

  /* End the node-program subshell. To minimize the number
     of open ports on the front-end, we must close down ssh;
     to do this, we have to close stdin, stdout, stderr, and
     run the subshell in the background. */
  fprintf(f, ")");
  // in interactive mode, ssh connection to process 0 needs to keep the standard descriptors and run in the foreground
  if (p.nodeno != 0 || !arg_interactive) {
    fprintf(f, " < /dev/null 1> /dev/null 2> /dev/null");
    if (!arg_mpiexec)
      fprintf(f, " &");
  }
  fprintf(f, "\n");

  if (arg_verbose)
    fprintf(f, "Echo 'remote shell phase successful.'\n");
  fprintf(f, /* Check for startup errors: */
          "sleep 1\n"
          "if [ -r /tmp/charmrun_err.$$ ]\n"
          "then\n"
          "  cat /tmp/charmrun_err.$$ \n"
          "  rm -f /tmp/charmrun_err.$$ \n"
          "  Exit 1\n"
          "fi\n");
  fprintf(f, "Exit 0\n");
  free(arg_currdir_r);
}

/* use the command "size" to get information about the position of the ".data"
   and ".bss" segments inside the program memory */
static void read_global_segments_size()
{
  nodetab_host const * h = host_table[0];

  /* find the node-program */
  arg_nodeprog_r =
      pathextfix(arg_nodeprog_a, h->pathfixes, h->ext);

  std::vector<const char *> sshargv;
  sshargv.push_back(h->shell);
  sshargv.push_back(h->name);
  sshargv.push_back("-l");
  sshargv.push_back(h->login);
  char *tmp = (char *) malloc(sizeof(char) * 9 + strlen(arg_nodeprog_r));
  sprintf(tmp, "size -A %s", arg_nodeprog_r);
  sshargv.push_back(tmp);
  sshargv.push_back((const char *) NULL);

  int childPid = fork();
  if (childPid < 0) {
    perror("ERROR> getting the size of the global variables segments");
    exit(1);
  } else if (childPid == 0) {
    /* child process */
    dup2(2, 1);
    /*printf("executing: \"%s\" \"%s\" \"%s\" \"%s\"
     * \"%s\"\n",sshargv[0],sshargv[1],sshargv[2],sshargv[3],sshargv[4]);*/
    execvp(sshargv[0], const_cast<char **>(&sshargv[0]));
    fprintf(stderr, "Charmrun> Couldn't find remote shell program '%s'!\n",
            sshargv[0]);
    exit(1);
  } else {
    /* else we are in the parent */
    free(tmp);
    waitpid(childPid, NULL, 0);
  }
}

/* open a ssh connection with processor 0 and open a gdb session for info */
static void open_gdb_info()
{
  nodetab_host const * h = host_table[0];

  /* find the node-program */
  arg_nodeprog_r =
      pathextfix(arg_nodeprog_a, h->pathfixes, h->ext);

  std::vector<const char *> sshargv;
  sshargv.push_back(h->shell);
  sshargv.push_back(h->name);
  sshargv.push_back("-l");
  sshargv.push_back(h->login);
  char *tmp = (char *) malloc(sizeof(char) * 8 + strlen(arg_nodeprog_r));
  sprintf(tmp, "gdb -q %s", arg_nodeprog_r);
  sshargv.push_back(tmp);
  sshargv.push_back((const char *) NULL);

  int fdin[2];
  int fdout[2];
  int fderr[2];
  if (pipe(fdin) == -1) {
    fprintf(stderr, "charmrun> pipe() failed!\n");
    exit(1);
  }
  if (pipe(fdout) == -1) {
    fprintf(stderr, "charmrun> pipe() failed!\n");
    exit(1);
  }
  if (pipe(fderr) == -1) {
    fprintf(stderr, "charmrun> pipe() failed!\n");
    exit(1);
  }

  gdb_info_pid = fork();
  if (gdb_info_pid < 0) {
    perror("ERROR> starting info gdb");
    exit(1);
  } else if (gdb_info_pid == 0) {
    /* child process */
    close(fdin[1]);
    close(fdout[0]);
    close(fderr[0]);
    PRINT(("executing: \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"\n", sshargv[0],
           sshargv[1], sshargv[2], sshargv[3], sshargv[4]));
    dup2(fdin[0], 0);
    dup2(fdout[1], 1);
    dup2(fderr[1], 2);
    for (int i = 3; i < 1024; i++)
      close(i);
    execvp(sshargv[0], const_cast<char **>(&sshargv[0]));
    fprintf(stderr, "Charmrun> Couldn't find remote shell program '%s'!\n",
            sshargv[0]);
    exit(1);
  }
  /* else we are in the parent */
  free(tmp);
  gdb_info_std[0] = fdin[1];
  gdb_info_std[1] = fdout[0];
  gdb_info_std[2] = fderr[0];
  close(fdin[0]);
  close(fdout[1]);
  close(fderr[1]);
}
#ifdef HSTART
static void start_next_level_charmruns()
{

  const char *nodeprog_name = strrchr(arg_nodeprog_a, '/');
  static char buf[1024];
  sprintf(buf, "%.*s%s%s", (int)(nodeprog_name-arg_nodeprog_a), arg_nodeprog_a, DIRSEP, "charmrun");
  arg_nodeprog_a = strdup(buf);

  int nextIndex = 0;
  int client = 0;
  // while (nextIndex < branchfactor) {
  for (nodetab_process * p : my_process_table)
  {
    TODO; // need to do something more detailed with branchfactor and nodes_per_child

    FILE *f;
    char startScript[200];
    sprintf(startScript, "/tmp/charmrun.%d.%d", getpid(), p.procno);
    f = fopen(startScript, "w");
    if (f == NULL) {
      /* now try current directory */
      sprintf(startScript, "charmrun.%d.%d", getpid(), p.procno);
      f = fopen(startScript, "w");
      if (f == NULL) {
        fprintf(stderr, "Charmrun> Can not write file %s!\n", startScript);
        exit(1);
      }
    }
    ssh_script(f, p, arg_argv);
    fclose(f);

    p.ssh_pid = ssh_fork(p, startScript);
    client += nodes_per_child;
  }
}
#endif

/* returns pid */
static void start_one_node_ssh(nodetab_process & p, const char ** argv)
{
  char startScript[200];
  sprintf(startScript, "/tmp/charmrun.%d.%d", getpid(), p.nodeno);
  FILE *f = fopen(startScript, "w");
  if (f == NULL) {
    /* now try current directory */
    sprintf(startScript, "charmrun.%d.%d", getpid(), p.nodeno);
    f = fopen(startScript, "w");
    if (f == NULL) {
      fprintf(stderr, "Charmrun> Can not write file %s!\n", startScript);
      exit(1);
    }
  }
  ssh_script(f, p, argv);
  fclose(f);

  p.ssh_pid = ssh_fork(p, startScript);
}

static void start_nodes_ssh(std::vector<nodetab_process> & process_table)
{
  for (nodetab_process & p : process_table)
  {
      start_one_node_ssh(p);
  }
}

/* for mpiexec, for once calling mpiexec to start on all nodes  */
static int ssh_fork_one(nodetab_process & p, const char *startScript)
{
  nodetab_host const * h = p.host;

  std::vector<const char *> sshargv;

  const char *s = h->shell;
  const char *e = skipstuff(s);
  while (*s) {
    sshargv.push_back(substr(s, e));
    s = skipblanks(e);
    e = skipstuff(s);
  }

  const int processes = get_old_style_process_count();

  char npes[24];
  if ( ! arg_mpiexec_no_n ) {
    sshargv.push_back("-n");
    sprintf(npes, "%d", processes);
    sshargv.push_back(npes);
  }
  sshargv.push_back((char *) startScript);
  sshargv.push_back((const char *) NULL);
  if (arg_verbose)
    printf("Charmrun> Starting %s %s \n", h->shell, startScript);

  int pid = fork();
  if (pid < 0) {
    perror("ERROR> starting mpiexec");
    exit(1);
  }
  if (pid == 0) { /*Child process*/
    /*  unlink(startScript); */
    // removeEnv("DISPLAY="); /*No DISPLAY disables ssh's slow X11 forwarding*/
    for (int i = 3; i < 1024; i++)
      close(i);
    execvp(sshargv[0], const_cast<char *const *>(&sshargv[0]));
    fprintf(stderr, "Charmrun> Couldn't find mpiexec program '%s'!\n",
            sshargv[0]);
    exit(1);
  }
  if (arg_verbose)
    printf("Charmrun> mpiexec started\n");
  return pid;
}

static void start_nodes_mpiexec()
{
  char startScript[200];
  sprintf(startScript, "./charmrun.%d", getpid());
  FILE *f = fopen(startScript, "w");
  chmod(startScript, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IROTH);
  if (f == NULL) {
    /* now try current directory */
    sprintf(startScript, "./charmrun.%d", getpid());
    f = fopen(startScript, "w");
    if (f == NULL) {
      fprintf(stderr, "Charmrun> Can not write file %s!\n", startScript);
      exit(1);
    }
  }

  nodetab_process & p = my_process_table[0];

  ssh_script(f, p, arg_argv);
  fclose(f);

  ssh_fork_one(p, startScript);
  /* all ssh_pid remain zero: skip finish_nodes */
}

static int finish_one_node(nodetab_process & p, int & retries, const char ** argv = arg_argv)
{
  int status = 0;
  waitpid(p.ssh_pid, &status, 0); /* check if the process is finished */
  if (!WIFEXITED(status))
    return 1;

  const int exitstatus = WEXITSTATUS(status);
  if (!exitstatus)
  { /* good */
    p.ssh_pid = 0; /* process is finished */
    return 0;
  }

  fprintf(stderr, "Charmrun> Error %d returned from remote shell (%s:%d)\n",
          exitstatus, p.host->name, p.nodeno);

  if (exitstatus != 255)
    exit(1);

  if (++retries <= MAX_NUM_RETRIES)
  {
    fprintf(stderr, "Charmrun> Reconnection attempt %d of %d\n",
            retries, MAX_NUM_RETRIES);
    start_one_node_ssh(p, argv);
  }
  else
  {
    fprintf(stderr, "Charmrun> Too many reconnection attempts; bailing out\n");
    exit(1);
  }

  return 2;
}

static void finish_set_nodes(std::vector<nodetab_process> & process_table, int start, int stop, bool charmrun_exiting)
{
  std::vector<int> num_retries(stop - start, 0);
  int done = 0;
  while (!done) {
    done = 1;
    for (int i = start; i < stop; i++) { /* check all nodes */
      nodetab_process & p = process_table[i];
      // Normally, the ssh connections are only needed to start charm on the remote hosts,
      // and they will end when the charm application starts. However, in interactive mode,
      // the ssh connection to process 0 runs until the end of the program, so we don't wait
      // for that process until the end
      if (p.nodeno == 0 && arg_interactive && !charmrun_exiting) continue;
      if (p.ssh_pid != 0) {
        done = 0; /* we are not finished yet */
        finish_one_node(p, num_retries[i - start]);
      }
    }
  }
}

static void finish_nodes(std::vector<nodetab_process> & process_table)
{
#ifdef HSTART
  if (arg_hierarchical_start && !arg_child_charmrun)
    finish_set_nodes(process_table, 0, branchfactor);
  else
#endif
    finish_set_nodes(process_table, 0, process_table.size());
}

static void kill_one_node(nodetab_process & p)
{
  int status = 0;
  if (arg_verbose)
    printf("Charmrun> waiting for remote shell (%s:%d), pid %d\n",
           p.host->name, p.nodeno, p.ssh_pid);
  kill(p.ssh_pid, 9);
  waitpid(p.ssh_pid, &status, 0); /*<- no zombies*/
  p.ssh_pid = 0;
}

static void kill_nodes()
{
  /*Now wait for all the ssh'es to finish*/
  for (nodetab_process & p : my_process_table)
    kill_one_node(p);
}


/* find the absolute path for an executable in the path */
static char *find_abs_path(const char *target)
{
  char *thepath=getenv("PATH");
  char *path=strdup(thepath);
  char *subpath=strtok(path,":");
  char *abspath=(char*) malloc(PATH_MAX + strlen(target) + 2);
  while(subpath!=NULL) {
    strcpy(abspath,subpath);
    strcat(abspath,"/");
    strcat(abspath,target);
    if(probefile(abspath)){
      free(path);
      return abspath;
    }
    subpath=strtok(NULL,":");
  }
  free(abspath);
  free(path);
  return NULL;
}

/* simple version of charmrun that avoids the sshd or charmd,   */
/* it spawn the node program just on local machine using exec. */
struct local_nodestart
{
  int envc;
  char **envp;
  int n;

  const char ** dparamp;
  std::vector<const char *> dparamv;
  std::vector<char *> heapAllocated;

#if CMK_HAS_ADDR_NO_RANDOMIZE
  int persona;
#endif

  local_nodestart(const char ** extra_argv = nullptr)
  {
    char ** env = main_envp;

    /* copy environ and expanded to hold NETSTART and CmiNumNodes */
    for (envc = 0; env[envc]; envc++)
      ;
    int extra = 0;
    const int proc_active = proc_per.active();
    extra += proc_active;
#if CMK_SMP
    const int onewth_active = onewth_per.active();
    extra += onewth_active;
#endif

    envp = (char **) malloc((envc + 3 + extra + 1) * sizeof(void *));
    for (int i = 0; i < envc; i++)
      envp[i] = env[i];
    envp[envc] = (char *) malloc(256);
    envp[envc + 1] = (char *) malloc(256);
    envp[envc + 2] = strdup("FROM_CHARMRUN=1");
    n = 3;
    // cpu affinity hints
    using Unit = typename TopologyRequest::Unit;
    if (proc_active)
    {
      envp[envc + n] = (char *) malloc(256);
      switch (proc_per.unit())
      {
        case Unit::Host:
          sprintf(envp[envc + n], "CmiProcessPerHost=%d", proc_per.host);
          break;
        case Unit::Socket:
          sprintf(envp[envc + n], "CmiProcessPerSocket=%d", proc_per.socket);
          break;
        case Unit::Core:
          sprintf(envp[envc + n], "CmiProcessPerCore=%d", proc_per.core);
          break;
        case Unit::PU:
          sprintf(envp[envc + n], "CmiProcessPerPU=%d", proc_per.pu);
          break;
        default:
          break;
      }
      ++n;
    }
#if CMK_SMP
    if (onewth_active)
    {
      envp[envc + n] = (char *) malloc(256);
      switch (onewth_per.unit())
      {
        case Unit::Host:
          sprintf(envp[envc + n], "CmiOneWthPerHost=%d", 1);
          break;
        case Unit::Socket:
          sprintf(envp[envc + n], "CmiOneWthPerSocket=%d", 1);
          break;
        case Unit::Core:
          sprintf(envp[envc + n], "CmiOneWthPerCore=%d", 1);
          break;
        case Unit::PU:
          sprintf(envp[envc + n], "CmiOneWthPerPU=%d", 1);
          break;
        default:
          break;
      }
      ++n;
    }
#endif
    envp[envc + n] = 0;

    /* insert xterm gdb in front of command line and pass args to gdb */
    if (arg_debug || arg_debug_no_pause || arg_in_xterm)
    {
      char *abs_xterm=find_abs_path(arg_xterm);
      if(!abs_xterm)
      {
        fprintf(stderr, "Charmrun> cannot find xterm for debugging, please add it to your path\n");
        exit(1);
      }

      heapAllocated.push_back(abs_xterm);
      dparamv.push_back(abs_xterm);
      dparamv.push_back("-title");
      dparamv.push_back(pparam_argv[1]);
      dparamv.push_back("-e");

      std::vector<const char *> cparamv;
      if (arg_debug || arg_debug_no_pause)
      {
        const bool isLLDB = strcmp(arg_debugger, "lldb") == 0;
        const char *commandflag = isLLDB ? "-o" : "-ex";
        const char *argsflag = isLLDB ? "--" : "--args";

        cparamv.push_back(arg_debugger);

        if (arg_debug_no_pause)
        {
          cparamv.push_back(commandflag);
          cparamv.push_back("r");
        }

        cparamv.push_back(argsflag);
      }

      for (int i = 1; pparam_argv[i] != nullptr; ++i)
        cparamv.push_back(pparam_argv[i]);

      if (extra_argv != nullptr)
        for (const char ** param = extra_argv; *param != nullptr; ++param)
          cparamv.push_back(*param);

      if (!(arg_debug || arg_debug_no_pause))
        cparamv.push_back("; echo \"program exited with code $?\" ; read eoln");

      char * cparam = cstring_join(cparamv, " ");
      heapAllocated.push_back(cparam);
      dparamv.push_back(cparam);

      if (arg_verbose)
      {
        printf("Charmrun> xterm args:");
        for (const char *p : dparamv)
          printf(" %s", p);
        printf("\n");
      }

      // null terminate your argv or face the wrath of undefined behavior
      dparamv.push_back(nullptr);

      dparamp = dparamv.data();
    }
    else
    {
      if (extra_argv != nullptr)
      {
        for (const char ** param = pparam_argv+1; *param != nullptr; ++param)
          dparamv.push_back(*param);

        for (const char ** param = extra_argv; *param != nullptr; ++param)
          dparamv.push_back(*param);

        dparamv.push_back(nullptr);

        dparamp = dparamv.data();
      }
      else
      {
        dparamp = pparam_argv+1;
      }
    }

#if CMK_HAS_ADDR_NO_RANDOMIZE
    persona = personality(0xffffffff);
    if (arg_va_rand)
      personality(persona & ~ADDR_NO_RANDOMIZE);
    else
      personality(persona | ADDR_NO_RANDOMIZE);
#endif
  }

  void start(const nodetab_process & p)
  {
    if (arg_verbose)
      printf("Charmrun> start %d node program on localhost.\n", p.nodeno);
    sprintf(envp[envc], "NETSTART=%s", create_netstart(p.nodeno));
    sprintf(envp[envc + 1], "CmiNumNodes=%d", 0);

#if CMK_HAS_POSIX_SPAWN
    // We need posix_spawn on macOS because it is the only way to disable ASLR at runtime.
    // There is no harm in using it on any other platform that supports it.

    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);
#if CMK_CHARM4PY
    // don't disable initial output on rank 0 process (need to be able to see python syntax errors, etc)
    if (p.nodeno != 0)
#endif
    {
      posix_spawn_file_actions_addopen(&file_actions, 0, "/dev/null", O_RDWR, 0);
      posix_spawn_file_actions_addopen(&file_actions, 1, "/dev/null", O_RDWR, 0);
      posix_spawn_file_actions_addopen(&file_actions, 2, "/dev/null", O_RDWR, 0);
    }

    posix_spawnattr_t attr;
    short flags;
    posix_spawnattr_init(&attr);
    posix_spawnattr_getflags(&attr, &flags);
#ifdef POSIX_SPAWN_USEVFORK
    flags |= POSIX_SPAWN_USEVFORK;
#endif
#ifdef _POSIX_SPAWN_DISABLE_ASLR
    if (arg_va_rand)
      flags &= ~_POSIX_SPAWN_DISABLE_ASLR;
    else
      flags |= _POSIX_SPAWN_DISABLE_ASLR;
#endif
    posix_spawnattr_setflags(&attr, flags);

    pid_t pid;
    int status = posix_spawn(&pid, dparamp[0], &file_actions, &attr, const_cast<char *const *>(dparamp), envp);

    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&attr);

    if (status != 0) {
      fprintf(stderr, "posix_spawn failed: %s\n", strerror(status));
      exit(1);
    }
#else
    int pid = fork();
    if (pid < 0) {
      fprintf(stderr, "fork failed: %s\n", strerror(errno));
      exit(1);
    }
    if (pid == 0) {
      int fd, fd2 = dup(2);
#if CMK_CHARM4PY
      // don't disable initial output on rank 0 process (need to be able to see python syntax errors, etc)
      if ((p.nodeno != 0) && (-1 != (fd = open("/dev/null", O_RDWR)))) {
#else
      if (-1 != (fd = open("/dev/null", O_RDWR))) {
#endif
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
      }
      int status = execve(dparamp[0],
		      const_cast<char *const *>(dparamp), envp);
      dup2(fd2, 2);
      fprintf(stderr, "execve failed to start process \"%s\": %s\n",
             dparamp[0], strerror(errno));
      kill(getppid(), 9);
      exit(1);
    }
#endif
  }

  ~local_nodestart()
  {
#if CMK_HAS_ADDR_NO_RANDOMIZE
    if (!arg_va_rand)
      personality(persona);
#endif

    for (char * p : heapAllocated)
      free(p);
    for (int i = envc, i_end = envc + n; i < i_end; ++i)
      free(envp[i]);
    free(envp);
  }
};

static void start_nodes_local(const std::vector<nodetab_process> & process_table)
{
  local_nodestart state;

  for (const nodetab_process & p : process_table)
    state.start(p);
}

#ifdef __FAULT__

static int current_restart_phase = 1;

/**
 * @brief Relaunches a program on the crashed node.
 */
static void restart_node(nodetab_process & p)
{
  /** add an argument to the argv of the new process
  so that the restarting processor knows that it
  is a restarting processor */
  int i = 0;
  while (arg_argv[i] != NULL) {
    i++;
  }
  const char **restart_argv = (const char **) malloc(sizeof(char *) * (i + 4));
  i = 0;
  while (arg_argv[i] != NULL) {
    restart_argv[i] = arg_argv[i];
    i++;
  }

  const char ** added_restart_argv = restart_argv + i;
  restart_argv[i] = "+restartaftercrash";
  char phase_str[10];
  sprintf(phase_str, "%d", ++current_restart_phase);
  restart_argv[i + 1] = phase_str;
  restart_argv[i + 2] = "+restartisomalloc";
  restart_argv[i + 3] = NULL;

  /** change the nodetable entry of the crashed
processor to connect it to a new one**/
  static int next_replacement_host = 0;
  const int host_count = host_table.size();
  int hosts_checked = 0;
  while (host_table[next_replacement_host]->crashed)
  {
    ++next_replacement_host;
    next_replacement_host %= host_count;
    if (++hosts_checked == host_count)
    {
      fprintf(stderr, "Charmrun> All hosts crashed, aborting.\n");
      exit(1);
    }
  }
  p.host = host_table[next_replacement_host];
  ++next_replacement_host;
  next_replacement_host %= host_count;

  if (arg_mpiexec)
  {
    fprintf(stderr, "Charmrun> Restarting unsupported with ++mpiexec!\n");
    exit(1);
  }
  else if (!arg_local)
  {
    start_one_node_ssh(p, restart_argv);

#if !CMK_SSH_KILL
#if CMK_USE_SSH
    if (!arg_ssh_display)
#endif
    {
      int retries = 0, unfinished;
      do
        unfinished = finish_one_node(p, retries, restart_argv);
      while (unfinished);
    }
#endif
  }
  else
  {
    local_nodestart state{added_restart_argv};
    state.start(p);
  }

  free(restart_argv);

  if (arg_verbose)
    PRINT(("Charmrun> Finished launching new process in %f s\n",
           GetClock() - ftTimer));
}

/**
 * @brief Reconnects a crashed node. It waits for the I-tuple from the just
 * relaunched program. It also:
 * i) Broadcast the nodetabtable to every other node.
 * ii) Announces the crash to every other node.
 */
static void reconnect_crashed_client(nodetab_process & crashed)
{
  if (0 == skt_select1(server_fd, arg_timeout * 1000)) {
    client_connect_problem(crashed, "Timeout waiting for restarted node-program to connect");
  }

  skt_ip_t clientIP;
  unsigned int clientPort;

  const SOCKET req_client = skt_accept(server_fd, &clientIP, &clientPort);

  if (req_client == SOCKET_ERROR) {
    client_connect_problem(crashed, "Failure in restarted node accept");
    exit(1);
  } else {
    skt_tcp_no_nagle(req_client);

    ChMessage msg;
    if (!skt_select1(req_client, arg_timeout * 1000)) {
      client_connect_problem(crashed, "Timeout on IP request for restarted processor");
    }

#ifdef HSTART
    if (arg_hierarchical_start) {
      req_forward_root(crashed);
      if (_last_crash != nullptr) {
        fprintf(stderr, "ERROR> Charmrun detected multiple crashes.\n");
        exit(1);
      }

      _last_crash = &crashed;
      return;
    }
#endif
    ChMessage_recv(req_client, &msg);
    if (msg.len != sizeof(ChSingleNodeinfo)) {
      fprintf(stderr, "Charmrun: Bad initnode data length. Aborting\n");
      fprintf(stderr, "Charmrun: possibly because: %s.\n", msg.data);
    }

    /** update the nodetab entry corresponding to
    this node, skip the restarted one */
    ChSingleNodeinfo *in = (ChSingleNodeinfo *) msg.data;

    crashed.req_client = req_client;

    nodeinfo_add(in, crashed);

    nodeinfo_populate(crashed);

    for (const nodetab_process & p : my_process_table)
      if (&p != &crashed)
        req_send_initnodetab(p);

    /* tell every one there is a crash */
    announce_crash(crashed);
    if (_last_crash != nullptr) {
      fprintf(stderr, "ERROR> Charmrun detected multiple crashes.\n");
      exit(1);
    }
    _last_crash = &crashed;
    /*holds the restarted process until I got ack back from
      everyone in req_handle_crashack
      now the restarted one can only continue until
      req_handle_crashack calls req_send_initnodetab(socket_index)
      req_send_initnodetab(req_clients[socket_index]); */
    ChMessage_free(&msg);
  }

#if CMK_SSH_KILL
#if CMK_USE_SSH
  if (!arg_ssh_display)
#endif
    kill_one_node(crashed);
#endif
}

/**
 * @brief Sends a message announcing the crash to every other node. This message
 * will be used to
 * trigger fault tolerance methods.
 */
static void announce_crash(const nodetab_process & crashed)
{
  ChMessageHeader hdr;
  ChMessageInt_t crashNo = ChMessageInt_new(crashed.nodeno);
  ChMessageHeader_new("crashnode", sizeof(ChMessageInt_t), &hdr);
  for (const nodetab_process & p : my_process_table)
  {
    if (&p != &crashed)
    {
      skt_sendN(p.req_client, (const char *) &hdr, sizeof(hdr));
      skt_sendN(p.req_client, (const char *) &crashNo,
                sizeof(ChMessageInt_t));
    }
  }
}

#endif

#endif /*CMK_USE_SSH*/
