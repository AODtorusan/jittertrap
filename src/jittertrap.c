#include <sys/socket.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include "fossa.h"
#include "frozen.h"
#include "stats_thread.h"
#include "netem.h"

static char *g_iface;
static const char *s_http_port = "8000";
static struct ns_serve_http_opts s_http_server_opts = { "static_content/" };

struct ns_connection *nc;

static void
print_ns_str(const struct ns_str *s)
{
  char *ss = malloc(s->len + 1);
  memcpy(ss, s->p, s->len);
  ss[s->len] = 0;
  printf("%s\n", ss);
  free(ss);
}

static void
print_websocket_message(const struct websocket_message *m)
{
  char *s = malloc(m->size+1);
  memcpy(s, m->data, m->size);
  s[m->size] = 0;
  printf("websocket_message: [%s]\n", s);
  free(s);
}

static bool
match_msg_type(const struct json_token *tok, const char *r)
{
  if (!tok) return false;
  if (tok->type != JSON_TYPE_STRING) return false;
  return (strncmp(tok->ptr, r, tok->len) == 0);
}

static char*
quote_string(const char * const s)
{
  char *outs;
  outs = malloc(strlen(s) + 3);
  sprintf(outs,"\"%s\"", s);
  return outs;
}

static char*
json_arr_alloc()
{
  char *buf = malloc(3);
  buf[0] = '[';
  buf[1] = ']';
  buf[2] = 0;
  return buf;
}

static void
json_arr_append(char **arr, const char * const word)
{
  char *quoted_word;
  assert(arr);
  int buf_len = strlen(*arr);
  assert(word);
  quoted_word = quote_string(word);
  int word_len = strlen(quoted_word);

  /* comma, space, nul term */
  *arr = realloc(*arr, buf_len + word_len + 2 + 1);

  if (buf_len >= 3) {
    memcpy((*arr)+buf_len -1, ", ", 2);
    buf_len += 2;
  }
  memcpy(*arr+buf_len -1, quoted_word, word_len);
  free(quoted_word);
  (*arr)[buf_len + word_len -1] = ']';
  (*arr)[buf_len + word_len] = 0;
}

static char*
list_ifaces()
{
  char *json_ifaces = json_arr_alloc();
  char **ifaces = netem_list_ifaces();
  char **i = ifaces;

  do {
    json_arr_append(&json_ifaces, *i);
    free(*i);
    i++;
  } while (*i);
  free(ifaces);

  char *head = "{\"ifaces\":";
  char *tail = "}";
  char *msg = malloc(strlen(head) + strlen(json_ifaces) + strlen(tail) + 1);
  *msg = 0;
  strncat(msg, head, strlen(head));
  strncat(msg, json_ifaces, strlen(json_ifaces));
  strncat(msg, tail, strlen(tail));
  free(json_ifaces);
  return msg;
}

static void
handle_websocket_list_ifaces(struct ns_connection *nc)
{
  char *buf = list_ifaces();
  printf("matched list_ifaces. ifaces:[%s]\n", buf);
  ns_send_websocket_frame(nc, WEBSOCKET_OP_TEXT, buf, strlen(buf));
  free(buf);
}

static void
handle_websocket_dev_select(struct json_token *tok)
{
  printf("switching to iface: %.*s\n", tok->len, tok->ptr);
  g_iface = malloc(tok->len+1);
  memcpy(g_iface, tok->ptr, tok->len);
  g_iface[tok->len] = 0;
  stats_monitor_iface(g_iface);
}

static void
handle_websocket_get_netem(struct ns_connection *nc, struct json_token *tok)
{
  struct netem_params p;
  char *iface = malloc(tok->len+1);
  printf("get netem for iface: %.*s\n", tok->len, tok->ptr);
  memcpy(iface, tok->ptr, tok->len);
  iface[tok->len] = 0;
  if (0 != netem_get_params(iface, &p)) {
    fprintf(stderr, "couldn't get netem parameters.\n");
    free(iface);
    return;
  }

  char *template =
    "{\"netem_params\":"
    "{\"iface\":\"%.10s\", \"delay\":%3.0d, \"jitter\":%3.0d, \"loss\":%3.1d}}";
  char msg[200] = {0};
  sprintf(msg, template, p.iface, p.delay, p.jitter, p.loss);
  printf("%s\n",msg);
  ns_send_websocket_frame(nc, WEBSOCKET_OP_TEXT, msg, strlen(msg));
  free(iface);
}

static bool
parse_int(char *str, long *l)
{
  char *endptr;
  errno = 0;    /* To distinguish success/failure after call */

  long val = strtol(str, &endptr, 10);

  /* Check for various possible errors */
  if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
       || (errno != 0 && val == 0)) {
    perror("strtol");
    return false;
  }

  if (endptr == str) {
    fprintf(stderr, "No digits were found\n");
    return false;
  }

  /* success! */
  *l = val;
  return true;
}

static void
json_token_to_string(struct json_token *tok, char **str)
{
  if (!*str) {
    *str = malloc(tok->len+1);
  } else {
    *str = realloc(*str, tok->len+1);
  }
  memcpy(*str, tok->ptr, tok->len);
  (*str)[tok->len] = 0;
}

static void
handle_websocket_set_netem(struct ns_connection *nc,
                           struct json_token *t_dev,
                           struct json_token *t_delay,
                           struct json_token *t_jitter,
                           struct json_token *t_loss)
{
  char *s = NULL;
  char *dev = NULL;
  long delay, jitter, loss;

  json_token_to_string(t_dev, &dev);
  printf("set_netem: dev: %s, ", dev);

  json_token_to_string(t_delay, &s);
  if (!parse_int(s, &delay)) {
    fprintf(stderr, "couldn't parse delay\n");
    free(s);
    return;
  }
  printf("delay: %ld, ", delay);

  json_token_to_string(t_jitter, &s);
  if (!parse_int(s, &jitter)) {
    fprintf(stderr, "couldn't parse jitter\n");
    free(s);
    return;
  }
  printf("jitter: %ld, ", jitter);

  json_token_to_string(t_loss, &s);
  if (!parse_int(s, &loss)) {
    fprintf(stderr, "couldn't parse loss\n");
    free(s);
    return;
  }
  printf("loss: %ld\n", loss);
  printf("\n\n");

  netem_update(dev, delay, jitter, loss);
  handle_websocket_get_netem(nc, t_dev);
  free(s);
  free(dev);
}


static void
handle_websocket_message(struct ns_connection *nc,
                         const struct websocket_message *m)
{
  print_websocket_message(m);
  struct json_token *arr, *tok;

  arr = parse_json2((const char*)m->data, m->size);
  if (!arr) {
    return; /* not valid JSON. */
  }

/* expected json looks like:
 * {'msg':'list_ifaces'}
 *     OR
 * {'msg': 'get_netem', 'dev': 'eth0' }
 */

  const char *key = "msg";
  tok = find_json_token(arr, key);
  if (tok) {
    if (match_msg_type(tok, "list_ifaces")) {
      handle_websocket_list_ifaces(nc);
    } else if (match_msg_type(tok, "dev_select")) {
      tok = find_json_token(arr, "dev");
      handle_websocket_dev_select(tok);
    } else if (match_msg_type(tok, "get_netem")) {
      tok = find_json_token(arr, "dev");
      handle_websocket_get_netem(nc, tok);
    } else if (match_msg_type(tok, "set_netem")) {
      handle_websocket_set_netem(nc,
                                 find_json_token(arr, "dev"),
                                 find_json_token(arr, "delay"),
                                 find_json_token(arr, "jitter"),
                                 find_json_token(arr, "loss"));
    }
  }

  free(arr);
}

static int
is_websocket(struct ns_connection *c)
{
  return c->flags & NSF_IS_WEBSOCKET;
}

static void
print_peer_name(struct ns_connection *c)
{
  struct sockaddr_storage addr;
  char ipstr[INET6_ADDRSTRLEN];
  socklen_t len = sizeof addr;

  getpeername(c->sock, (struct sockaddr*)&addr, &len);

  /* deal with both IPv4 and IPv6: */
  if (addr.ss_family == AF_INET) {
    struct sockaddr_in *s = (struct sockaddr_in *)&addr;
    inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
  } else { /* AF_INET6 */
    struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
    inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);
  }
  printf("Peer IP address: %s\n", ipstr);
}

static void
ev_handler(struct ns_connection *nc, int ev, void *ev_data)
{
  struct http_message *hm = (struct http_message *) ev_data;
  struct websocket_message *wm = (struct websocket_message *) ev_data;

  switch (ev) {
    case NS_HTTP_REQUEST:
      /* keep this simple. no REST. serve only index.html.
       * everything else is done through websockets.*/
      print_ns_str(&hm->message);
      ns_serve_http(nc, hm, s_http_server_opts);
      break;
    case NS_WEBSOCKET_HANDSHAKE_DONE:
      print_peer_name(nc);
      break;
    case NS_WEBSOCKET_FRAME:
      handle_websocket_message(nc, wm);
      break;
    case NS_CLOSE:
      if (is_websocket(nc)) {
        /* TODO: if nobody is listening, stop threads. */
        ;
      }
      break;
    default:
      break;
  }
}

/* callback for the real-time stats thread. */
void stats_event_handler(struct byte_counts *counts)
{
  struct ns_connection *c;

  for (c = ns_next(nc->mgr, NULL); c != NULL; c = ns_next(nc->mgr, c)) {
    if (is_websocket(c)) {
      /*
      printf("stats event. timestamp:[%lld] tx-bytes:[%lld] tx-delta:[%d]\n",
             counts->timestamp, counts->tx_bytes, counts->tx_bytes_delta );
       */
      ns_printf_websocket_frame(c,
                                WEBSOCKET_OP_TEXT,
                                "{\"stats\": {"
                                "\"iface\": \"%s\","
                                "\"rx-bytes\":%lld,"
                                "\"tx-bytes\":%lld,"
                                "\"rx-delta\":%d,"
                                "\"tx-delta\":%d"
                                "}}",
                                g_iface,
                                counts->rx_bytes,
                                counts->tx_bytes,
                                counts->rx_bytes_delta,
                                counts->tx_bytes_delta);
    }
  }
}

int main(int argc, char *argv[])
{
  struct ns_mgr mgr;
  struct ns_bind_opts opts = { .flags = TCP_NODELAY };

  ns_mgr_init(&mgr, NULL);
  nc = ns_bind_opt(&mgr, s_http_port, ev_handler, opts);
  ns_set_protocol_http_websocket(nc);
  mgr.hexdump_file = argc > 1 ? argv[1] : NULL;  /* Allow hexdump debug */

  if (netem_init() < 0) {
    fprintf(stderr, "Couldn't initialise netlink for netem module.\n");
    return -1;
  }

  stats_monitor_iface("lo");
  stats_thread_init(stats_event_handler);

  printf("Starting server on port %s\n", s_http_port);
  for (;;) {
    ns_mgr_poll(&mgr, 1000);
  }
  ns_mgr_free(&mgr);

  return 0;
}
