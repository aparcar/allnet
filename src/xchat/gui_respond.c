/* gui_respond.c: respond to requests from the GUI */

#if defined(WIN32) || defined(WIN64)
#ifndef WINDOWS_ENVIRONMENT
#define WINDOWS_ENVIRONMENT
#define WINDOWS_ENVIRONMENT
#endif /* WINDOWS_ENVIRONMENT */
#endif /* WIN32 || WIN64 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>

#ifdef WINDOWS_ENVIRONMENT
#include <windows.h>
#endif /* WINDOWS_ENVIRONMENT */

#include "lib/packet.h"
#include "lib/util.h"
#include "lib/pipemsg.h"
#include "lib/keys.h"
#include "lib/mgmt.h"
#include "lib/trace_util.h"
#include "xcommon.h"
#include "store.h"
#include "gui_socket.h"

static int send_bytes (int sock, char *buffer, int64_t length)
{
  while (length > 0) {   /* inefficient implementation for now */
    if (write (sock, buffer, 1) != 1) {
      perror ("gui.c send_bytes");
      return 0;
    }
    buffer++;
    length--;
  }
  return 1;              /* success */
}

static int receive_bytes (int sock, char *buffer, int64_t length)
{
  while (length > 0) {
    /* get one byte at a time -- for now, inefficient implementation */
    if (read (sock, buffer, 1) != 1) {
      if (errno != ENOENT)  /* ENOENT when the socket is closed */
        perror ("gui_respond.c receive_bytes");
      return 0;
    }
    buffer++;
    length--;
  }
  return 1;              /* success */
}

/* returns 1 for success or 0 for failure */
/* also called from gui_callback.c, so the mutex is global */
int gui_send_buffer (int sock, char *buffer, int64_t length)
{
  if (length < 1)
    return 0;
  char length_buf [8];
  writeb64 (length_buf, length);
  int result = 0;
  /* use a mutex to ensure only one message is sent at a time */
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock (&mutex);
  if ((send_bytes (sock, length_buf, 8)) &&
      (send_bytes (sock, buffer, length)))
    result = 1;
  pthread_mutex_unlock (&mutex);
  return result;
}

static int64_t receive_buffer (int sock, char **buffer)
{
  char length_buf [8];
  if (! receive_bytes (sock, length_buf, 8))
    return 0;
  int64_t length = readb64 (length_buf);
  if (length < 1)
    return 0;
  *buffer = malloc_or_fail (length, "gui.c receive_buffer");
  if (! receive_bytes (sock, *buffer, length))
    return 0;
  return length;
}

static size_t size_of_string_array (char ** array, int count)
{
  size_t result = 0;
  int i;
  for (i = 0; i < count; i++)
    result += (strlen (array [i]) + 1);
  return result;
}

static int copy_string_array (char * dest, size_t dsize,
                              char ** array, int count)
{
  if ((dsize < 1) || (count <= 0))
    return 0;
  if (dsize < size_of_string_array (array, count)) /* inefficient but sane */
    return 0;
  int i;
  for (i = 0; i < count; i++) {
    strcpy (dest, array [i]);
    dest += strlen (array [i]) + 1;
  }
  return 1;
}

static void gui_send_string_array (int code, char ** array, int count, int sock,
                                   const char * caller)
{
/* format: code, 64-bit number of strings, null-terminated strings */
#define STRING_ARRAY_HEADER_SIZE	9
  size_t string_alloc = size_of_string_array (array, count);
  size_t alloc = STRING_ARRAY_HEADER_SIZE + string_alloc;
  char * reply = malloc_or_fail (alloc, caller);
  reply [0] = code;
  writeb64 (reply + 1, count);
  if ((count > 0) && (string_alloc > 0)) {
    copy_string_array (reply + STRING_ARRAY_HEADER_SIZE, string_alloc,
                       array, count);
  }
  gui_send_buffer (sock, reply, alloc);
  free (reply);
#undef STRING_ARRAY_HEADER_SIZE
}

/* send all the contacts to the gui, null-separated */
static void gui_contacts (int sock)
{
/* format: code, 64-bit number of contacts, null-terminated contacts */
  char ** contacts = NULL;
  int nc = all_contacts (&contacts);
  gui_send_string_array (GUI_CONTACTS, contacts, nc, sock, "gui_contacts");
  free (contacts);
}

/* send all the subscriptions to the gui, null-separated */
static void gui_subscriptions (int sock)
{
/* format: code, 64-bit number of senders, null-terminated contacts */
  struct bc_key_info * bki = NULL;
  int nb = get_other_keys (&bki);
  char ** senders = malloc_or_fail (nb * sizeof (char *), "gui_subscriptions");
  int i;
  for (i = 0; i < nb; i++)
    senders [i] = bki [i].identifier;
  gui_send_string_array (GUI_SUBSCRIPTIONS, senders, nb, sock,
                         "gui_subscriptions");
  free (senders);
}

/* dynamically allocates contact, must be freed */
static char * contact_name_from_buffer (char * message, int64_t length)
{
  if (length > 0) {
    char * contact = malloc_or_fail (length + 1, "contact_name_from_buffer");
    memcpy (contact, message, length);
    contact [length] = '\0';   /* null terminate if necessary */
    return contact;
  }
  return NULL;
}

/* send a 1 if a contact exists, or a 0 otherwise */
static void gui_contact_exists (char * message, int64_t length, int sock)
{
/* message format: contact name (not null terminated) */
/* reply format: 1-byte code, 1-byte response */
  char reply [2];
  reply [0] = GUI_CONTACT_EXISTS;
  reply [1] = 0;   /* does not exist */
  if (length > 0) {
    char * contact = contact_name_from_buffer (message, length);
    if (num_keysets (contact) > 0)
      reply [1] = 1;   /* success */
    free (contact);
  }
  gui_send_buffer (sock, reply, sizeof (reply));
}

/* send a 1 if a contact exists and is a group, or a 0 otherwise */
static void gui_contact_is_group (char * message, int64_t length, int sock)
{
/* message format: contact name (not null terminated) */
/* reply format: 1-byte code, 1-byte response */
  char reply [2];
  reply [0] = GUI_CONTACT_IS_GROUP;
  reply [1] = 0;   /* does not exist */
  if (length > 0) {
    char * contact = contact_name_from_buffer (message, length);
    if ((num_keysets (contact) > 0) && (is_group (contact)))
      reply [1] = 1;   /* success */
    free (contact);
  }
  gui_send_buffer (sock, reply, sizeof (reply));
}

/* send a 1 if a contact exists and has a peer key, or a 0 otherwise */
static void gui_contact_has_peer_key (char * message, int64_t length, int sock)
{
/* message format: contact name (not null terminated) */
/* reply format: 1-byte code, 1-byte response */
  char reply [2];
  reply [0] = GUI_CONTACT_IS_GROUP;
  reply [1] = 0;   /* by default, no peer key */
  if (length > 0) {
    char * contact = contact_name_from_buffer (message, length);
    keyset * keys = NULL;
    int nk = all_keys (contact, &keys);
    int ik;
    for (ik = 0; ik < nk; ik++) {
      allnet_rsa_pubkey k;  /* do not free */
      if (get_contact_pubkey (keys [ik], &k) > 0)
        reply [1] = 1;   /* has key */
    }
    if (keys != NULL)
      free (keys);
    free (contact);
  }
  gui_send_buffer (sock, reply, sizeof (reply));
}

/* create a group, sending a 1 or a 0 as response */
static void gui_create_group (char * message, int64_t length, int sock)
{
/* message format: group name (not null terminated) */
/* reply format: 1-byte code, 1-byte response */
  char reply [2];
  reply [0] = GUI_CREATE_GROUP;
  reply [1] = 0;   /* failure */
  if (length > 0) {
    char * contact = contact_name_from_buffer (message, length);
    if (create_group (contact))
      reply [1] = 1;   /* success */
    free (contact);
  }
  gui_send_buffer (sock, reply, sizeof (reply));
}

static void gui_members (unsigned int code, char * message, int64_t length,
                         int gui_sock, int recursive)
{
/* message format: group name (not null terminated) */
/* format: code, 64-bit number of members, null-terminated member names */
  if (length > 0) {
    char * contact = contact_name_from_buffer (message, length);
    char ** members = NULL;
    int count = group_membership (contact, &members);
    gui_send_string_array (GUI_CONTACTS, members, count,
                           gui_sock, "gui_members");
    free (contact);
    if (members != NULL)
      free (members);
  } else {
    char reply [9];
    reply [0] = code;
    writeb64 (reply + 1, 0);
    gui_send_buffer (gui_sock, reply, sizeof (reply));
  }
}

static void gui_member_of (unsigned int code, char * message, int64_t length,
                           int gui_sock, int recursive)
{
/* message format: contact name (not null terminated) */
/* format: code, 64-bit number of groups, null-terminated group names */
  if (length > 0) {
    char * contact = contact_name_from_buffer (message, length);
    char ** members = NULL;
    int count = (recursive ? member_of_groups_recursive (contact, &members)
                           : member_of_groups (contact, &members));
    gui_send_string_array (GUI_CONTACTS, members, count,
                           gui_sock, "gui_member_of");
    free (contact);
    if (members != NULL)
      free (members);
  } else {
    char reply [9];
    reply [0] = code;
    writeb64 (reply + 1, 0);
    gui_send_buffer (gui_sock, reply, sizeof (reply));
  }
}

static void gui_rename_contact (char * message, int64_t length, int gui_sock)
{
/* message format: old contact name, new contact name both null terminated */
/* reply format: 1-byte code, 1-byte response */
  char reply [2];
  reply [0] = GUI_RENAME_CONTACT;
  reply [1] = 0;   /* failure */
  if (length >= 4) {   /* shortest is two single-character names, null-term */
    char * old = message;
    size_t offset = strlen (message) + 1;  /* should be index of new name */
    if (offset + 1 < length) { /* room for new name, plus null termination */
      char * new = message + offset;
      if ((strlen (old) > 0) && (strlen (new) > 0) && (rename (old, new)))
        reply [1] = 1;    /* success */
    }
  }
  gui_send_buffer (gui_sock, reply, sizeof (reply));
}

static void gui_variable (char * message, int64_t length, int op, int gui_sock)
{
/* message format: variable code, contact name (not null terminated) */
/* reply format: 1-byte code, 1-byte response */
  char reply [2];
  reply [0] = GUI_QUERY_VARIABLE;
  if (op == 0)
    reply [0] = GUI_UNSET_VARIABLE;
  else if (op == 1)
    reply [0] = GUI_SET_VARIABLE;
  reply [1] = 0;   /* not set, or failure */
  if (length > 1) {
    int code = message [0];
    char * contact = contact_name_from_buffer (message + 1, length - 1);
    if (num_keysets (contact) > 0) {  /* contact exists */
      switch (code) {
      case GUI_VARIABLE_VISIBLE:
        if (op == -1)       /* query */
          reply [1] = is_visible (contact);
        else if (op == 0)   /* make invisible */
          reply [1] = make_invisible (contact);
        else if (op == 1)   /* make visible */
          reply [1] = make_visible (contact);
        else
          printf ("GUI_VARIABLE_VISIBLE: unknown op %d\n", op);
        break;
      case GUI_VARIABLE_NOTIFY:
        if (op == -1)       /* query */
          reply [1] = (contact_file_get (contact, "no_notify", NULL) < 0);
        else if (op == 0)   /* cancel notifications by creating no_notify */
          reply [1] = (contact_file_write (contact, "no_notify", "", 0) == 1);
        else if (op == 1) { /* make notifiable by deleting "no_notify" if any */
          contact_file_delete (contact, "no_notify");
          reply [1] = 1;
        } else
          printf ("GUI_VARIABLE_NOTIFY: unknown op %d\n", op);
        break;
      case GUI_VARIABLE_SAVING_MESSAGES:
        if (op == -1)       /* query */
          reply [1] = (contact_file_get (contact, "no_saving", NULL) < 0);
        else if (op == 0)   /* cancel message saving by creating no_saving */
          reply [1] = (contact_file_write (contact, "no_saving", "", 0) == 1);
        else if (op == 1) { /* save by deleting "no_saving" if any */
          contact_file_delete (contact, "no_saving");
          reply [1] = 1;
        } else
          printf ("GUI_VARIABLE_SAVING_MESSAGES: unknown op %d\n", op);
        break;
      case GUI_VARIABLE_COMPLETE:
        if (op == -1)       /* query */
          reply [1] = (contact_file_get (contact, "exchange", NULL) < 0);
        else if (op == 1) { /* delete exchange file if any */
          contact_file_delete (contact, "exchange");
          reply [1] = 1;
        } else
          printf ("GUI_VARIABLE_COMPLETE: unknown op %d\n", op);
        break;
      default:
        break;
      }
    }
    free (contact);
  }
  gui_send_buffer (gui_sock, reply, sizeof (reply));
}

static void gui_send_result_messages (int code,
                                      struct message_store_info * msgs,
                                      int count, int sock)
{
/* format: code, 64-bit number of messages, then the messages
   each message has type, sequence, number of missing prior sequence
   numbers, time sent, timezone sent, time received, and
   null-terminated message contents.
   type                1 byte     byte  0      1 sent, 2 sent+acked, 3 received
   sequence            8 bytes    bytes 1..8
   missing             8 bytes    bytes 9..16  0 for sent messages
   time_sent           8 bytes    bytes 17..24
   timezone            2 bytes    bytes 25..26
   time_received       8 bytes    bytes 27..34
   message             n+1 bytes  bytes 35..
 */
#define MESSAGE_ARRAY_HEADER_SIZE	9
#define MESSAGE_HEADER_SIZE	35
  size_t message_alloc = 0;
  int i;
  for (i = 0; i < count; i++)
    message_alloc += (MESSAGE_HEADER_SIZE + strlen (msgs [i].message) + 1);
  size_t alloc = MESSAGE_ARRAY_HEADER_SIZE + message_alloc;
  char * reply = malloc_or_fail (alloc, "gui_send_messages");
  reply [0] = code;
  writeb64 (reply + 1, count);
  char * dest = reply + MESSAGE_ARRAY_HEADER_SIZE;
  for (i = 0; i < count; i++) {
    if (msgs [i].msg_type == MSG_TYPE_RCVD)
      dest [0] = 3;
    else if (msgs [i].message_has_been_acked)
      dest [0] = 2;
    else
      dest [0] = 1;
    writeb64 (dest + 1, msgs [i].seq);
    writeb64 (dest + 9, 0);
    if (msgs [i].msg_type == MSG_TYPE_RCVD)
      writeb64 (dest + 9, msgs [i].prev_missing);
    writeb64 (dest + 17, msgs [i].time);
    writeb16 (dest + 25, msgs [i].tz_min);
    writeb64 (dest + 27, msgs [i].rcvd_time);
    strcpy (dest + MESSAGE_HEADER_SIZE, msgs [i].message);
    dest += MESSAGE_HEADER_SIZE + strlen (msgs [i].message) + 1;
  }
  gui_send_buffer (sock, reply, alloc);
  free (reply);
#undef MESSAGE_HEADER_SIZE
#undef MESSAGE_ARRAY_HEADER_SIZE
}

static void gui_get_messages (char * message, int64_t length, int gui_sock)
{
/* message format: 64-bit max, contact name (not null terminated) */
/* reply format: 1-byte code, 64-bit number of messages, messages each
 * in the format shown under gui_send_result_messages */
  char reply_header [9];
  reply_header [0] = GUI_GET_MESSAGES;
  writeb64 (reply_header + 1, 0);
  if (length >= 9) {
    int64_t max = readb64 (message);
    message += 8;
    length -= 8;
    char * contact = contact_name_from_buffer (message, length);
    if ((max > 0) && (num_keysets (contact) > 0)) {  /* contact exists */
      /* for now, use list_all_messages.  Later, modify list_all_messages
       * to accept a maximum number of messages */
      struct message_store_info * msgs = NULL;
      int num_alloc = 0;
      int num_used = 0;
      if (list_all_messages (contact, &msgs, &num_alloc, &num_used)) {
        int nresult = num_used;
        if (nresult > max)
          nresult = max;
        gui_send_result_messages (GUI_GET_MESSAGES, msgs, nresult, gui_sock);
        free_all_messages (msgs, num_used);
        return;
      }
    }
  }
  /* if we didn't reply above, something went wrong.  Send 0 messages */
  gui_send_buffer (gui_sock, reply_header, sizeof (reply_header));
}

static void gui_send_message (char * message, int64_t length, int broadcast,
                             int gui_sock, int allnet_sock)
{
/* message format: contact name and message, both null terminated */
/* reply format: 1-byte code, 64-bit sequence number (0 in case of error) */
  char reply_header [9];
  reply_header [0] = GUI_SEND_MESSAGE;
  writeb64 (reply_header + 1, 0);
  if (length >= 4) {
    char * contact = message;
    size_t offset = strlen (message) + 1;  /* should be index of message */
    if (offset + 1 < length) { /* room for message, plus null termination */
      char * to_send = message + offset;
      if ((strlen (to_send) > 0) && (strlen (contact) > 0) &&
          (num_keysets (contact) > 0)) {  /* contact and message exist */
        if (broadcast) {
          printf ("sending broadcast messages not implemented yet\n");
        } else {
          writeb64 (reply_header + 1, 
                    send_data_message (allnet_sock, contact,
                                       to_send, strlen (to_send)));
        }
      }
    }
  }
  gui_send_buffer (gui_sock, reply_header, sizeof (reply_header));
}

static void gui_init_key_exchange (char * message, int64_t length,
                                   int gui_sock, int allnet_sock)
{
/* message format: 1-byte hop count, contact name and one or two secrets,
 * all null terminated */
/* reply format: 1-byte code, 1-byte result: 1 for success, 0 failure */
  char reply_header [2];
  reply_header [0] = GUI_KEY_EXCHANGE;
  reply_header [1] = 0;  /* failure */
  int hops = * ((unsigned char *)message);
  char * contact = message + 1;
  if (length > 1) {
    if (length <= 1 + strlen (contact) + 1 + 2)
      printf ("gui_init_key_exchange error: length %" PRId64
              ", contact %s (%zd)\n", length, contact, strlen (contact));
    char * secret1 = contact + (strlen (contact) + 1);
    char * secret2 = NULL;
    if (length > (1 + strlen (contact) + 1 + strlen (secret1) + 1))
      secret2 = contact + (strlen (contact) + 1 + strlen (secret1) + 1);
    reply_header [1] =
      create_contact_send_key (allnet_sock, contact, secret1, secret2, hops);
  }
  gui_send_buffer (gui_sock, reply_header, sizeof (reply_header));
}

static void gui_subscribe (char * message, int64_t length,
                           int gui_sock, int allnet_sock)
{
/* message format: ahra, not null-terminated */
/* reply format: 1-byte code, 1-byte result: 1 for success, 0 failure */
  char reply_header [2];
  reply_header [0] = GUI_SUBSCRIBE;
  reply_header [1] = 0;  /* failure */
  if (length > 0) {
    char * ahra = contact_name_from_buffer (message, length);
    reply_header [1] = subscribe_broadcast (allnet_sock, ahra);
  }
  gui_send_buffer (gui_sock, reply_header, sizeof (reply_header));
}

static void gui_trace (char * message, int64_t length,
                       int gui_sock, int allnet_sock)
{
/* message format: 1-byte nhops, 1-byte nbits, 1-byte record intermediates,
   8-byte address */
/* reply format: 1-byte code, 16-byte trace ID (all 0s for failure) */
printf ("gui_trace called\n");
  char reply_header [1 + MESSAGE_ID_SIZE];
  reply_header [0] = GUI_TRACE;
  memset (reply_header + 1, 0, sizeof (reply_header) - 1);
  if (length >= 3 + ADDRESS_SIZE) {
    int nhops = ((unsigned char *)message) [0];
    int nbits = ((unsigned char *)message) [1];
    int inter =                   message  [2];
    unsigned char addr [ADDRESS_SIZE];
    memcpy (addr, message + 3, ADDRESS_SIZE);
    if (! start_trace (allnet_sock, addr, nbits, nhops, inter,
                       reply_header + 1))
      memset (reply_header + 1, 0, sizeof (reply_header) - 1);
  }
  gui_send_buffer (gui_sock, reply_header, sizeof (reply_header));
}

static void gui_busy_wait (int gui_sock, int allnet_sock)
{
/* reply format: 1-byte code */
  do_request_and_resend (allnet_sock); 
  char reply_header [1];
  reply_header [0] = GUI_BUSY_WAIT;
  gui_send_buffer (gui_sock, reply_header, sizeof (reply_header));
}

static void interpret_from_gui (char * message, int64_t length,
                                int gui_sock, int allnet_sock)
{
  switch ((unsigned char) (message [0])) {
  case GUI_CONTACTS:
    gui_contacts (gui_sock);
    break;
  case GUI_SUBSCRIPTIONS:
    gui_subscriptions (gui_sock);
    break;
  case GUI_CONTACT_EXISTS:
    gui_contact_exists (message + 1, length - 1, gui_sock);
    break;
  case GUI_CONTACT_IS_GROUP:
    gui_contact_is_group (message + 1, length - 1, gui_sock);
    break;
  case GUI_HAS_PEER_KEY:
    gui_contact_has_peer_key (message + 1, length - 1, gui_sock);
    break;

  case GUI_CREATE_GROUP:
    gui_create_group (message + 1, length - 1, gui_sock);
    break;
  case GUI_MEMBERS :
    gui_members (message [0], message + 1, length - 1, gui_sock, 0);
    break;
  case GUI_MEMBERS_RECURSIVE:
    gui_members (message [0], message + 1, length - 1, gui_sock, 1);
    break;
  case GUI_MEMBER_OF_GROUPS :
    gui_member_of (message [0], message + 1, length - 1, gui_sock, 0);
    break;
  case GUI_MEMBER_OF_GROUPS_RECURSIVE:
    gui_member_of (message [0], message + 1, length - 1, gui_sock, 1);
    break;

  case GUI_RENAME_CONTACT:
    gui_rename_contact (message + 1, length - 1, gui_sock);
    break;

  case GUI_QUERY_VARIABLE:
    gui_variable (message + 1, length - 1, -1, gui_sock);
    break;
  case GUI_SET_VARIABLE:
    gui_variable (message + 1, length - 1, 1, gui_sock);
    break;
  case GUI_UNSET_VARIABLE:
    gui_variable (message + 1, length - 1, 0, gui_sock);
    break;

  case GUI_GET_MESSAGES:
    gui_get_messages (message + 1, length - 1, gui_sock);
    break;
  case GUI_SEND_MESSAGE:
    gui_send_message (message + 1, length - 1, 0, gui_sock, allnet_sock);
    break;
  case GUI_SEND_BROADCAST:
    gui_send_message (message + 1, length - 1, 1, gui_sock, allnet_sock);
    break;

  case GUI_KEY_EXCHANGE:
    gui_init_key_exchange (message + 1, length - 1, gui_sock, allnet_sock);
    break;
  case GUI_SUBSCRIBE:
    gui_subscribe (message + 1, length - 1, gui_sock, allnet_sock);
    break;
  case GUI_TRACE:
    gui_trace (message + 1, length - 1, gui_sock, allnet_sock);
    break;

  case GUI_BUSY_WAIT:
    gui_busy_wait (gui_sock, allnet_sock); 
    break;

  default:
    printf ("command from GUI has unknown code %d\n", message [0]); 
    break;
  }
}

void * gui_respond_thread (void * arg)
{
  int * socks = (int*)arg;
  int gui_sock = socks [0];
  int allnet_sock = socks [1];
  free (arg);
  printf ("gui_respond_thread (%d, %d) started\n", gui_sock, allnet_sock);
  char * message = NULL;
  int64_t mlen = 0;
  while ((mlen = receive_buffer (gui_sock, &message)) > 0) {
    interpret_from_gui (message, mlen, gui_sock, allnet_sock);
    free (message);
    message = NULL;
  }
#ifdef DEBUG_PRINT
  printf ("gui_respond_thread socket closed, receive thread exiting\n");
#endif /* DEBUG_PRINT */
  stop_chat_and_exit (0);
  return NULL;
}

