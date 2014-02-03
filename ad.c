/* ad.c: main allnet daemon to forward allnet messages */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "packet.h"
#include "mgmt.h"
#include "lib/pipemsg.h"
#include "social.h"
#include "lib/priority.h"
#include "lib/log.h"
#include "lib/util.h"

#define PROCESS_PACKET_DROP	1
#define PROCESS_PACKET_LOCAL	2
#define PROCESS_PACKET_ALL	3

static int process_mgmt (char * message, int msize, int is_local,
                         int * priority)
{
  /* last time we received a trace that was not forwarded, or 0 for none */
  static time_t trace_received = 0;
  /* if sent from local, use the priority they gave us */
  /* else set priority to the lowest possible.  Generally the right thing */
  /* to do unless we know better (and doesn't affect local delivery). */
  if (! is_local)
    *priority = EPSILON;

  struct allnet_header * hp = (struct allnet_header *) message;
  int hs = ALLNET_AFTER_HEADER (hp->transport, msize);
  if (msize < hs + sizeof (struct allnet_mgmt_header))
    return PROCESS_PACKET_DROP;
  struct allnet_mgmt_header * ahm = 
    (struct allnet_mgmt_header *) (message + hs);
  switch (ahm->mgmt_type) {
  case ALLNET_MGMT_BEACON:
  case ALLNET_MGMT_BEACON_REPLY:
  case ALLNET_MGMT_BEACON_GRANT:
    return PROCESS_PACKET_DROP;   /* do not forward beacons */
  case ALLNET_MGMT_PEER_REQUEST:
  case ALLNET_MGMT_PEERS:
  case ALLNET_MGMT_DHT:
    return PROCESS_PACKET_LOCAL;  /* forward to local daemons only */
  case ALLNET_MGMT_TRACE_REQ:
    if (is_local) {
      trace_received = 0;
      return PROCESS_PACKET_ALL;    /* forward as a normal data packet */
    }
    if ((trace_received != 0) && ((time (NULL) - trace_received) > 10)) {
      /* either trace process died or something else failed -- just forward */
      printf ("warning: last unforwarded trace at %ld, now %ld\n",
              trace_received, time (NULL));
      return PROCESS_PACKET_ALL;    /* forward as a normal data packet */
    }
    /* trace is not local, forward to the trace server */
    trace_received = time (NULL);
    return PROCESS_PACKET_LOCAL;  /* forward to local daemons only */
  case ALLNET_MGMT_TRACE_REPLY:
    return PROCESS_PACKET_ALL;    /* forward to all with very low priority */
#if 0
  case ALLNET_MGMT_TRACE_PATH:
    return PROCESS_PACKET_ALL;    /* forward to all with very low priority */
#endif /* 0 */
  default:
    printf ("unknown management message type %d\n", ahm->mgmt_type);
    *priority = EPSILON;
    return PROCESS_PACKET_ALL;   /* forward unknown management packets */
  }
}

/* return 0 to drop the packet (do nothing), 1 to process as a request packet,
 * 2 to forward only to local destinations, and 3 to forward everywhere */
/* if returning 3, fills in priority */
static int process_packet (char * packet, int size, int is_local,
                           struct social_info * soc, int * priority)
{
  if (! is_valid_message (packet, size))
    return PROCESS_PACKET_DROP;
  static time_t trace_received = 0;

/* skip the hop count in the hash, since it changes at each hop */
#define HEADER_SKIP	3
  /* have we received this packet in the last minute?  if so, drop it */
  int time = record_packet_time (packet + HEADER_SKIP, size - HEADER_SKIP, 0);
#undef HEADER_SKIP
  if ((time > 0) && (time < 60)) {
    if (is_local)
      return PROCESS_PACKET_LOCAL;  /* should be OK to forward locally */
    snprintf (log_buf, LOG_SIZE, 
              "packet received in the last %d seconds, dropping\n", time);
    log_print ();
    return PROCESS_PACKET_DROP;     /* duplicate, ignore */
  }

  struct allnet_header * ah = (struct allnet_header *) packet;
  if (ah->message_type == ALLNET_TYPE_MGMT) {     /* AllNet management */
    printf ("calling process_mgmt (%p, %d, %d, %p/%d)\n",
            packet, size, is_local, priority, *priority);
    int r = process_mgmt (packet, size, is_local, priority);
    printf ("done calling process_mgmt, result %d, p %d\n", r, *priority);
    return r;
  }

  if (is_local)
    return PROCESS_PACKET_ALL;

  /* before forwarding, increment the number of hops seen */
  if (ah->hops < 255)   /* do not increment 255 to 0 */
    ah->hops++;
  snprintf (log_buf, LOG_SIZE, "forwarding packet with %d hops\n", ah->hops);
  log_print ();

  if (ah->hops >= ah->max_hops)   /* reached hop count */
  /* no matter what it is, only forward locally, i.e. to alocal and acache */
    return PROCESS_PACKET_LOCAL;

  /* compute a forwarding priority for non-local packets */
  *priority = compute_priority (is_local, size, ah->src_nbits, ah->dst_nbits,
                                ah->hops, ah->max_hops, UNKNOWN_SOCIAL_TIER,
                                largest_rate ());
  if (ah->sig_algo == ALLNET_SIGTYPE_NONE)
    return PROCESS_PACKET_ALL;
  int sig_size = (packet [size - 2] & 0xff) << 8 + (packet [size - 1] & 0xff);
  if (ALLNET_HEADER_SIZE + sig_size + 2 <= size)
    return PROCESS_PACKET_ALL;
  char * sig = packet + (size - 2 - sig_size);
  char * verify = packet + ALLNET_HEADER_SIZE; 
  int vsize = size - (ALLNET_HEADER_SIZE + sig_size + 2);
  int valid;
  int social_distance =
       social_connection (soc, verify, vsize, ah->source, ah->src_nbits,
                          ah->sig_algo, sig, sig_size, &valid);
  if (! valid)
    return PROCESS_PACKET_ALL;
  int rate_fraction = track_rate (ah->source, ah->src_nbits, size);
  *priority = compute_priority (is_local, size, ah->src_nbits, ah->dst_nbits,
                                ah->hops, ah->max_hops, social_distance,
                                rate_fraction);

  /* send each of the packets, with its priority, to each of the pipes */
  return PROCESS_PACKET_ALL;
}

static void send_all (char * packet, int psize, int priority,
                      int * write_pipes, int nwrite, char * desc)
{
  int n = snprintf (log_buf, LOG_SIZE, "send_all (%s) sending to %d pipes: ",
                    desc, nwrite);
  int i;
  for (i = 0; i < nwrite; i++)
    n += snprintf (log_buf + n, LOG_SIZE - n, "%d, ", write_pipes [i]);
  log_print ();
  for (i = 0; i < nwrite; i++) {
    if (! send_pipe_message (write_pipes [i], packet, psize, priority)) {
      snprintf (log_buf, LOG_SIZE, "write_pipes [%d] = %d is no longer valid\n",
                i, write_pipes [i]);
      log_print ();
    }
  }
}

/* runs forever, and only returns in case of error. */
/* the first read_pipe and the first write_pipe are from/to alocal.
 * the second read_pipe and write_pipe are from/to acache
 * the third read_pipe and write_pipe are from/to aip
 * there may or may not be more pipes, but they should generally be the
 * same number, even though the code only explicitly refers to the first
 * three and doesn't require the same number of read and write pipes
 */
static void main_loop (int * read_pipes, int nread,
                       int * write_pipes, int nwrite,
                       int update_seconds, int max_social_bytes, int max_checks)
{
  int i;
  for (i = 0; i < nread; i++)
    add_pipe (read_pipes [i]);
snprintf (log_buf, LOG_SIZE, "ad calling init_social\n"); log_print ();
  struct social_info * soc = init_social (max_social_bytes, max_checks);
snprintf (log_buf, LOG_SIZE, "ad calling update_social\n"); log_print ();
  time_t next_update = update_social (soc, update_seconds);
snprintf (log_buf, LOG_SIZE, "ad finished update_social\n"); log_print ();

  while (1) {
    /* read messages from each of the pipes */
    char * packet = NULL;
    int from_pipe;
    int priority = EPSILON; /* incoming priorities ignored unless from local */
    int psize = receive_pipe_message_any (PIPE_MESSAGE_WAIT_FOREVER,
                                          &packet, &from_pipe, &priority);
snprintf (log_buf, LOG_SIZE, "ad received %d, fd %d\n", psize, from_pipe);
log_print ();
printf ("ad received %d, fd %d, packet %p\n", psize, from_pipe, packet);
    if (psize <= 0) { /* for now exit */
      snprintf (log_buf, LOG_SIZE,
                "error: received %d from receive_pipe_message_any, pipe %d\n%s",
                psize, from_pipe, "  exiting\n");
      log_print ();
      exit (1);
    }
    /* packets generated by alocal and acache are local */
    int is_local = ((from_pipe == read_pipes [0]) ||
                    (from_pipe == read_pipes [1]));
printf ("calling process_packet (%p, %d, %d, %p, %p/%d)\n",
        packet, psize, is_local, soc, &priority, priority);
    int p = process_packet (packet, psize, is_local, soc, &priority);
printf ("done calling process_packet, result %d\n", p);
    switch (p) {
    case PROCESS_PACKET_ALL:
      log_packet ("sending to all", packet, psize);
      send_all (packet, psize, priority, write_pipes, nwrite, "all");
      break;
    /* all the rest are not forwarded, so priority does not matter */
    case PROCESS_PACKET_LOCAL:   /* send only to alocal and acache */ 
      log_packet ("sending to alocal and acache", packet, psize);
      send_all (packet, psize, 0, write_pipes, 2, "local");
      break;
    case PROCESS_PACKET_DROP:    /* do not forward */
      log_packet ("dropping packet", packet, psize);
      /* do nothing */
      break;
    }
    free (packet);  /* was allocated by receive_pipe_message_any */

    /* about once every next_update seconds, re-read social connections */
    if (time (NULL) >= next_update)
      next_update = update_social (soc, update_seconds);
printf ("ad loop complete\n");
  }
}

/* arguments are: the number of pipes, then pairs of read and write file
 * file descriptors (ints) for each pipe, from/to alocal, acache, aip.
 * any additional pipes will again be pairs from/to each abc.
 */
int main (int argc, char ** argv)
{
  init_log ("ad");
  if (argc < 2) {
    printf ("need to have at least the number of read and write pipes\n");
    return -1;
  }
  int npipes = atoi (argv [1]);
  if (npipes < 3) {
    printf ("%d pipes, at least 3 needed\n", npipes);
    return -1;
  }
  if (argc != 2 * npipes + 2) {
    printf ("%d arguments, expected 2 + %d for %d pipes\n",
            argc, 2 * npipes, npipes);
    return -1;
  }
  if (argc < 7) {
    printf ("need to have at least 3 each read and write pipes\n");
    return -1;
  }
  snprintf (log_buf, LOG_SIZE, "AllNet (ad) version %d\n", ALLNET_VERSION);
  log_print ();
  /* allocate both read and write pipes at once, then point write_pipes
   * to the middle of the allocated array
   */
  int * read_pipes  = malloc (sizeof (int) * npipes * 2);
  if (read_pipes == NULL) {
    printf ("allocation error in ad main\n");
    return -1;
  }
  int * write_pipes = read_pipes + npipes;

  int i;
  for (i = 0; i < npipes; i++) {
    read_pipes  [i] = atoi (argv [2 + 2 * i    ]);
    write_pipes [i] = atoi (argv [2 + 2 * i + 1]);
  }
  for (i = 0; i < npipes; i++) {
    snprintf (log_buf, LOG_SIZE, "read_pipes [%d] = %d\n", i, read_pipes [i]);
    log_print ();
  }
  for (i = 0; i < npipes; i++) {
    snprintf (log_buf, LOG_SIZE, "write_pipes [%d] = %d\n", i, write_pipes [i]);
    log_print ();
  }
  main_loop (read_pipes, npipes, write_pipes, npipes, 30, 30000, 5);
  snprintf (log_buf, LOG_SIZE, "ad error: main loop returned, exiting\n");
  log_print ();
}

