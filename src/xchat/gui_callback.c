/* gui_callback.c: send callbacks to the GUI */

#include <stdio.h>
#include <string.h>

#include "lib/util.h"
#include "lib/pipemsg.h"
#include "lib/keys.h"
#include "gui_socket.h"
#include "xcommon.h"

static void gui_callback_message_received (const char * peer,
                                           const char * message,
                                           const char * desc,
                                           uint64_t seq, time_t mtime,
                                           int broadcast, int gui_sock)
{
/* format: code, 1-byte broadcast, 8-byte sequence, 8-byte time,
           then null_terminated peer, message, and description */
  size_t string_alloc = strlen (peer) + strlen (message) + strlen (desc) + 3;
#define RECEIVED_MESSAGE_HEADER_SIZE	18
  size_t alloc = RECEIVED_MESSAGE_HEADER_SIZE + string_alloc;
  char * reply = malloc_or_fail (alloc, "gui_callback_message_received");
  reply [0] = GUI_CALLBACK_MESSAGE_RECEIVED;
  reply [1] = broadcast;
  writeb64 (reply + 2, seq);
  writeb64 (reply + 10, mtime);
  char * p = reply + 18;
  strcpy (p, peer);
  p += strlen (peer) + 1;
  strcpy (p, message);
  p += strlen (message) + 1;
  strcpy (p, desc);
  p += strlen (desc) + 1;
  gui_send_buffer (gui_sock, reply, alloc);
  free (reply);
#undef RECEIVED_MESSAGE_HEADER_SIZE
}

static void gui_callback_message_acked (const char * peer, uint64_t ack,
                                        int gui_sock)
{
/* format: code, 8-byte ack, null-terminated peer */
  size_t string_alloc = strlen (peer) + 1;
#define RECEIVED_ACK_HEADER_SIZE			9
  size_t alloc = RECEIVED_ACK_HEADER_SIZE + string_alloc;
  char * reply = malloc_or_fail (alloc, "gui_callback_message_received");
  reply [0] = GUI_CALLBACK_MESSAGE_ACKED;
  writeb64 (reply + 1, ack);
  strcpy (reply + RECEIVED_ACK_HEADER_SIZE, peer);
  gui_send_buffer (gui_sock, reply, alloc);
  free (reply);
#undef RECEIVED_ACK_HEADER_SIZE
}

static void gui_callback_created (int code, const char * peer, int gui_sock)
{
/* format: code, null_terminated peer */
  size_t alloc = 1 + strlen (peer) + 1;
  char * reply = malloc_or_fail (alloc, "gui_callback_message_received");
  reply [0] = code;
  strcpy (reply + 1, peer);
  gui_send_buffer (gui_sock, reply, alloc);
  free (reply);
}

void gui_socket_main_loop (int gui_sock, int allnet_sock, pd p)
{
  int rcvd = 0;
  char * packet;
  int pipe;
  unsigned int pri;
  int timeout = 100;      /* sleep up to 1/10 second */
  char * old_contact = NULL;
  keyset old_kset = -1;
  while ((rcvd = receive_pipe_message_any (p, timeout, &packet, &pipe, &pri))
         >= 0) {
    int verified, duplicate, broadcast;
    uint64_t seq;
    char * peer;
    keyset kset;
    char * desc;
    char * message;
    struct allnet_ack_info acks;
    time_t mtime = 0;
    int mlen = handle_packet (allnet_sock, packet, rcvd, pri,
                              &peer, &kset, &acks, &message, &desc,
                              &verified, &seq, &mtime,
                              &duplicate, &broadcast);
    if ((mlen > 0) && (verified) && (! duplicate)) {
      if (! duplicate) {
        if (is_visible (peer))
          gui_callback_message_received (peer, message, desc, seq,
                                         mtime, broadcast, gui_sock);
        char ** groups = NULL;
        int ngroups = member_of_groups_recursive (peer, &groups);
        int ig;
        for (ig = 0; ig < ngroups; ig++) {
          if (is_visible (groups [ig]))
            gui_callback_message_received (groups [ig], message, desc, seq,
                                           mtime, broadcast, gui_sock);
        }
        if (groups != NULL)
          free (groups);
      }
      if ((! broadcast) &&
          ((old_contact == NULL) ||
           (strcmp (old_contact, peer) != 0) || (old_kset != kset))) {
        request_and_resend (allnet_sock, peer, kset, 1);
        if (old_contact != NULL)
          free (old_contact);
        old_contact = peer;
        old_kset = kset;
      } else { /* same peer or broadcast, do nothing */
        free (peer);
      }
      free (message);
      if (! broadcast)
        free (desc);
    } else if (mlen == -1) {   /* confirm successful key exchange */
      gui_callback_created (GUI_CALLBACK_CONTACT_CREATED, peer, gui_sock);
    } else if (mlen == -2) {   /* confirm successful subscription */
      gui_callback_created (GUI_CALLBACK_SUBSCRIPTION_COMPLETE, peer, gui_sock);
    }
    /* handle_packet may have changed what has and has not been acked */
    int i;
    for (i = 0; i < acks.num_acks; i++) {
      gui_callback_message_acked (acks.peers [i], acks.acks [i], gui_sock);
      free (acks.peers [i]);
    }
  }
  printf ("xchat_socket pipe closed, exiting\n");
}
