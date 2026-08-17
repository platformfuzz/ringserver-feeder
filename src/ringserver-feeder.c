/***************************************************************************
 * ringserver-feeder.c
 *
 * SeedLink client that forwards miniSEED to multiple DataLink servers using
 * the same packet ID on each (DataLink 1.1 WRITE flag I).
 *
 * Derived from EarthScope slink2dali (Apache-2.0).
 ***************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include <libdali.h>
#include <libmseed.h>
#include <libslink.h>

#define PACKAGE "ringserver-feeder"
#define VERSION "0.1.0"

typedef struct DLTarget_s
{
  char *address;
  DLCP *conn;
} DLTarget;

static short int verbose = 0;
static int stateint      = 0;
static char *netcode     = 0;
static char *statefile   = 0;
static char *pktidfile   = 0;
static char *lockfile    = 0;
static int writeack      = 0;
static int dialup        = 0;
static int keepalive     = 300;
static int netto         = 600;
static int netdly        = 30;

static SLCD *slconn;
static DLTarget *targets = NULL;
static int targetcount   = 0;
static int lockfd        = -1;
static uint64_t nextpktid = 1;

static int sendrecord (char *record, int reclen);
static int connect_targets (void);
static int parameter_proc (int argcount, char **argvec);
static char *getoptval (int argcount, char **argvec, int argopt);
static void term_handler (int sig);
static void print_timelogc (const char *msg);
static void print_timelog (char *msg);
static void usage (void);
static void apply_env (void);
static int parse_hosts (const char *csv);
static int load_pktid (void);
static int save_pktid (uint64_t pktid);
static int acquire_lock (const char *path);

int
main (int argc, char **argv)
{
  SLpacket *slpack;
  int seqnum;
  int ptype;
  int packetcnt = 0;

  char *type[] = {"Data", "Detection", "Calibration", "Timing",
                  "Message", "General", "Request", "Info",
                  "Info (terminated)", "KeepAlive"};

  struct sigaction sa;

  sa.sa_flags = SA_RESTART;
  sigemptyset (&sa.sa_mask);
  sa.sa_handler = term_handler;
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGQUIT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);
  sa.sa_handler = SIG_IGN;
  sigaction (SIGHUP, &sa, NULL);
  sigaction (SIGPIPE, &sa, NULL);

  apply_env ();

  if (parameter_proc (argc, argv) < 0)
  {
    fprintf (stderr, "Argument processing failed\n");
    fprintf (stderr, "Try '-h' for detailed help\n");
    return -1;
  }

  if (lockfile && acquire_lock (lockfile) < 0)
  {
    sl_log (2, 0, "Another feeder holds %s\n", lockfile);
    return -1;
  }

  if (load_pktid () < 0)
    return -1;

  if (connect_targets () < 0)
  {
    sl_log (2, 0, "Error connecting to one or more DataLink targets\n");
    return -1;
  }

  while (sl_collect (slconn, &slpack))
  {
    ptype  = sl_packettype (slpack);
    seqnum = sl_sequence (slpack);

    if (verbose > 1)
    {
      if (ptype == SLKEEP)
        sl_log (1, 0, "Keep alive packet received\n");
      else
        sl_log (1, 0, "Received %s packet, SeedLink sequence %d\n",
                type[ptype], seqnum);
    }

    if (ptype >= SLDATA && ptype < SLNUM)
    {
      while (sendrecord ((char *)&slpack->msrecord, SLRECSIZE))
      {
        if (verbose)
          sl_log (1, 0, "Re-connecting to DataLink target(s)\n");

        for (int i = 0; i < targetcount; i++)
        {
          if (targets[i].conn->link != -1)
            dl_disconnect (targets[i].conn);
        }

        if (connect_targets () < 0)
        {
          sl_log (2, 0, "Error re-connecting to DataLink target(s), sleeping 10 seconds\n");
          sleep (10);
        }

        if (slconn->terminate)
          break;
      }

      packetcnt++;
    }

    if (statefile && stateint)
    {
      if (++packetcnt >= stateint)
      {
        sl_savestate (slconn, statefile);
        packetcnt = 0;
      }
    }
  }

  if (slconn->link != -1)
    sl_disconnect (slconn);

  for (int i = 0; i < targetcount; i++)
  {
    if (targets[i].conn->link != -1)
      dl_disconnect (targets[i].conn);
  }

  if (statefile)
    sl_savestate (slconn, statefile);

  if (lockfd >= 0)
  {
    flock (lockfd, LOCK_UN);
    close (lockfd);
  }

  return 0;
}

static int
connect_targets (void)
{
  for (int i = 0; i < targetcount; i++)
  {
    if (targets[i].conn->link >= 0)
      continue;

    if (dl_connect (targets[i].conn) < 0)
    {
      sl_log (2, 0, "Error connecting to DataLink server %s\n", targets[i].address);
      return -1;
    }
  }

  return 0;
}

static int
sendrecord (char *record, int reclen)
{
  static MSRecord *msr = NULL;
  hptime_t endtime;
  char streamid[100];
  int rv;
  uint64_t pktid;

  if (netcode)
    ms_strncpopen (((struct fsdh_s *)record)->network, netcode, 2);

  if ((rv = msr_unpack (record, reclen, &msr, 0, 0)) != MS_NOERROR)
  {
    ms_recsrcname (record, streamid, 0);
    sl_log (2, 0, "Error unpacking %s: %s", streamid, ms_errorstr (rv));
    return -1;
  }

  msr_srcname (msr, streamid, 0);
  strcat (streamid, "/MSEED");
  endtime = msr_endtime (msr);

  if (nextpktid > LIBDALI_PKTID_MAXIMUM)
  {
    sl_log (2, 0, "Packet ID overflow\n");
    return -1;
  }

  pktid = nextpktid++;

  for (int i = 0; i < targetcount; i++)
  {
    DLTarget *target = &targets[i];

    if (target->conn->link < 0 && connect_targets () < 0)
      return -1;

    if (dl_write_id (target->conn, record, (size_t)reclen, streamid,
                     msr->starttime, endtime, pktid, writeack) < 0)
    {
      dl_disconnect (target->conn);
      return -1;
    }
  }

  if (save_pktid (pktid) < 0)
    sl_log (2, 0, "Warning: failed to save packet ID state\n");

  return 0;
}

static void
apply_env (void)
{
  const char *value;

  if ((value = getenv ("FEEDER_VERBOSE")) && value[0])
    verbose = (short int)atoi (value);

  if ((value = getenv ("FEEDER_STATE_FILE")) && value[0])
    statefile = strdup (value);

  if ((value = getenv ("FEEDER_PKTID_FILE")) && value[0])
    pktidfile = strdup (value);

  if ((value = getenv ("FEEDER_LOCK_FILE")) && value[0])
    lockfile = strdup (value);

  if ((value = getenv ("FEEDER_RECONNECT_SECONDS")) && value[0])
    netdly = atoi (value);

  if ((value = getenv ("FEEDER_NETWORK_TIMEOUT")) && value[0])
    netto = atoi (value);

  if ((value = getenv ("FEEDER_KEEPALIVE_SECONDS")) && value[0])
    keepalive = atoi (value);

  if ((value = getenv ("FEEDER_DIALUP")) && value[0] && strcmp (value, "0") != 0)
    dialup = 1;

  if ((value = getenv ("FEEDER_WRITE_ACK")) && value[0] && strcmp (value, "0") != 0)
    writeack = 1;
}

static int
parse_hosts (const char *csv)
{
  char *copy;
  char *cursor;
  char *token;
  int count = 0;

  if (!csv || !csv[0])
    return -1;

  copy = strdup (csv);
  if (!copy)
    return -1;

  for (token = strtok_r (copy, ",", &cursor); token; token = strtok_r (NULL, ",", &cursor))
  {
    while (*token == ' ')
      token++;

    if (*token == '\0')
      continue;

    targets = realloc (targets, (size_t)(count + 1) * sizeof (DLTarget));
    if (!targets)
    {
      free (copy);
      return -1;
    }

    targets[count].address = strdup (token);
    targets[count].conn    = dl_newdlcp (targets[count].address, PACKAGE);
    if (!targets[count].address || !targets[count].conn)
    {
      free (copy);
      return -1;
    }

    count++;
  }

  free (copy);
  targetcount = count;
  return (count > 0) ? 0 : -1;
}

static int
load_pktid (void)
{
  FILE *fp;
  uint64_t value;

  if (!pktidfile)
    return 0;

  fp = fopen (pktidfile, "r");
  if (!fp)
    return 0;

  if (fscanf (fp, "%" SCNu64, &value) == 1 && value > 0)
    nextpktid = value + 1;

  fclose (fp);
  return 0;
}

static int
save_pktid (uint64_t pktid)
{
  FILE *fp;

  if (!pktidfile)
    return 0;

  fp = fopen (pktidfile, "w");
  if (!fp)
    return -1;

  fprintf (fp, "%" PRIu64 "\n", pktid);
  fclose (fp);
  return 0;
}

static int
acquire_lock (const char *path)
{
  lockfd = open (path, O_RDWR | O_CREAT, 0644);
  if (lockfd < 0)
    return -1;

  if (flock (lockfd, LOCK_EX | LOCK_NB) < 0)
  {
    close (lockfd);
    lockfd = -1;
    return -1;
  }

  return 0;
}

static int
parameter_proc (int argcount, char **argvec)
{
  const char *value;
  char *sladdress = 0;
  char *dlhosts   = 0;
  int error       = 0;
  char *streamfile  = 0;
  char *multiselect = 0;
  char *selectors   = 0;
  char *timewin     = 0;
  char *tptr;
  SLstrlist *timelist;

  if ((value = getenv ("FEEDER_SEEDLINK_HOST")) && value[0] && !sladdress)
    sladdress = strdup (value);

  if ((value = getenv ("FEEDER_DATALINK_HOSTS")) && value[0] && !dlhosts)
    dlhosts = strdup (value);

  if ((value = getenv ("FEEDER_SELECTORS")) && value[0] && !selectors)
    selectors = strdup (value);

  if ((value = getenv ("FEEDER_STREAMS")) && value[0] && !multiselect)
    multiselect = strdup (value);

  if (!pktidfile && (value = getenv ("FEEDER_PKTID_FILE")) && value[0])
    pktidfile = strdup (value);

  if (!lockfile && (value = getenv ("FEEDER_LOCK_FILE")) && value[0])
    lockfile = strdup (value);

  if (!statefile && (value = getenv ("FEEDER_STATE_FILE")) && value[0])
    statefile = strdup (value);

  if (!pktidfile)
    pktidfile = strdup ("/data/pktid.state");

  if (!lockfile)
    lockfile = strdup ("/data/feeder.lock");

  for (optind = 1; optind < argcount; optind++)
  {
    if (strcmp (argvec[optind], "-V") == 0)
    {
      fprintf (stderr, "%s version: %s\n", PACKAGE, VERSION);
      exit (0);
    }
    else if (strcmp (argvec[optind], "-h") == 0)
    {
      usage ();
      exit (0);
    }
    else if (strncmp (argvec[optind], "-v", 2) == 0)
    {
      verbose += (short int)strspn (&argvec[optind][1], "v");
    }
    else if (strcmp (argvec[optind], "-l") == 0)
      streamfile = getoptval (argcount, argvec, optind++);
    else if (strcmp (argvec[optind], "-s") == 0)
      selectors = getoptval (argcount, argvec, optind++);
    else if (strcmp (argvec[optind], "-S") == 0)
      multiselect = getoptval (argcount, argvec, optind++);
    else if (strcmp (argvec[optind], "-d") == 0)
      dialup = 1;
    else if (strcmp (argvec[optind], "-N") == 0)
      netcode = getoptval (argcount, argvec, optind++);
    else if (strcmp (argvec[optind], "-x") == 0)
      statefile = getoptval (argcount, argvec, optind++);
    else if (strcmp (argvec[optind], "-tw") == 0)
      timewin = getoptval (argcount, argvec, optind++);
    else if (strcmp (argvec[optind], "-nt") == 0)
      netto = atoi (getoptval (argcount, argvec, optind++));
    else if (strcmp (argvec[optind], "-nd") == 0)
      netdly = atoi (getoptval (argcount, argvec, optind++));
    else if (strcmp (argvec[optind], "-k") == 0)
      keepalive = atoi (getoptval (argcount, argvec, optind++));
    else if (strncmp (argvec[optind], "-", 1) == 0)
    {
      fprintf (stderr, "Unknown option: %s\n", argvec[optind]);
      exit (1);
    }
    else if (!sladdress)
      sladdress = argvec[optind];
    else if (!dlhosts)
      dlhosts = argvec[optind];
    else
    {
      fprintf (stderr, "Unknown option: %s\n", argvec[optind]);
      exit (1);
    }
  }

  if (!sladdress)
  {
    fprintf (stderr, "No SeedLink server specified (FEEDER_SEEDLINK_HOST)\n");
    error = 1;
  }

  if (!dlhosts)
  {
    fprintf (stderr, "No DataLink targets specified (FEEDER_DATALINK_HOSTS)\n");
    error = 1;
  }

  if (error)
  {
    usage ();
    exit (1);
  }

  if (!(slconn = sl_newslcd ()))
  {
    fprintf (stderr, "Cannot allocate SeedLink descriptor\n");
    exit (1);
  }

  slconn->sladdr    = sladdress;
  slconn->netto     = netto;
  slconn->netdly    = netdly;
  slconn->keepalive = keepalive;
  if (dialup)
    slconn->dialup = 1;

  if (parse_hosts (dlhosts) < 0)
  {
    fprintf (stderr, "Invalid DataLink target list\n");
    exit (1);
  }

  sl_loginit (verbose, &print_timelogc, NULL, &print_timelogc, NULL);
  dl_loginit (verbose, &print_timelog, "", &print_timelog, "");
  setvbuf (stdout, NULL, _IOLBF, 0);
  sl_log (1, 0, "%s version: %s\n", PACKAGE, VERSION);

  if (streamfile)
    sl_read_streamlist (slconn, streamfile, selectors);

  if (timewin)
  {
    if (strchr (timewin, ':') == NULL)
    {
      sl_log (2, 0, "time window not in begin:[end] format\n");
      return -1;
    }

    if (sl_strparse (timewin, ":", &timelist) > 2)
    {
      sl_log (2, 0, "time window not in begin:[end] format\n");
      sl_strparse (NULL, NULL, &timelist);
      return -1;
    }

    if (strlen (timelist->element) == 0)
    {
      sl_log (2, 0, "time window must specify a begin time\n");
      sl_strparse (NULL, NULL, &timelist);
      return -1;
    }

    slconn->begin_time = strdup (timelist->element);
    timelist           = timelist->next;

    if (timelist != 0)
    {
      slconn->end_time = strdup (timelist->element);
      if (timelist->next != 0)
      {
        sl_log (2, 0, "malformed time window specification\n");
        sl_strparse (NULL, NULL, &timelist);
        return -1;
      }
    }

    sl_strparse (NULL, NULL, &timelist);
  }

  if (multiselect)
  {
    if (sl_parse_streamlist (slconn, multiselect, selectors) == -1)
      return -1;
  }
  else if (!streamfile)
    sl_setuniparams (slconn, selectors, -1, 0);

  if (statefile)
  {
    if ((tptr = strchr (statefile, ':')) != NULL)
    {
      char *tail;

      *tptr++ = '\0';
      stateint = (unsigned int)strtoul (tptr, &tail, 0);

      if (*tail || stateint > 1000000000U)
      {
        sl_log (2, 0, "state saving interval specified incorrectly\n");
        return -1;
      }
    }

    if (sl_recoverstate (slconn, statefile) < 0)
      sl_log (2, 0, "state recovery failed\n");
  }

  return 0;
}

static char *
getoptval (int argcount, char **argvec, int argopt)
{
  if (argvec == NULL || argvec[argopt] == NULL)
  {
    fprintf (stderr, "getoptval(): NULL option requested\n");
    exit (1);
  }

  if ((argopt + 1) < argcount && *argvec[argopt + 1] != '-')
    return argvec[argopt + 1];

  fprintf (stderr, "Option %s requires a value\n", argvec[argopt]);
  exit (1);
}

static void
term_handler (int sig)
{
  (void)sig;
  sl_terminate (slconn);
}

static void
print_timelogc (const char *msg)
{
  char timestr[100];
  time_t loc_time;

  time (&loc_time);
  strcpy (timestr, asctime (localtime (&loc_time)));
  timestr[strlen (timestr) - 1] = '\0';
  fprintf (stdout, "%s - %s", timestr, msg);
}

static void
print_timelog (char *msg)
{
  print_timelogc (msg);
}

static void
usage (void)
{
  fprintf (stderr, "%s version %s\n\n", PACKAGE, VERSION);
  fprintf (stderr,
           "Usage: %s [options]\n\n"
           "Environment:\n"
           "  FEEDER_SEEDLINK_HOST     upstream SeedLink host:port\n"
           "  FEEDER_DATALINK_HOSTS    comma-separated DataLink host:port list\n"
           "  FEEDER_STATE_FILE        SeedLink resume state (slink2dali -x)\n"
           "  FEEDER_PKTID_FILE        last written packet ID (default /data/pktid.state)\n"
           "  FEEDER_LOCK_FILE         single-writer lock (default /data/feeder.lock)\n"
           "  FEEDER_SELECTORS         default SeedLink selectors (-s)\n"
           "  FEEDER_STREAMS           multi-station list (-S)\n\n"
           "Options match slink2dali where applicable (-l, -s, -S, -x, -nd, -nt, -k).\n\n",
           PACKAGE);
}
