/* WLAN transport: TCP server. The PSP is the client (psp/source/wlan.c):
   it connects to NET_PORT(+offset), sends a 32-byte password, then the same
   frame protocol runs over the socket. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "pspdisp.h"

static int listenfd = -1;
static int connfd = -1;

static int t_write(const void *data, int len)
{
  const char *p = data; int left = len;
  while (left > 0) {
    ssize_t n = send(connfd, p, left, MSG_NOSIGNAL);
    if (n <= 0) return -1;          /* incl. EINTR: end link so we can exit */
    p += n; left -= n;
  }
  return 0;
}

static int t_read(void *data, int len, unsigned tmo)
{
  struct timeval tv = { tmo / 1000, (tmo % 1000) * 1000 };
  setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  char *p = data; int left = len;
  while (left > 0) {
    ssize_t n = recv(connfd, p, left, 0);
    if (n <= 0) return -1;          /* peer closed, timeout, or EINTR -> exit */
    p += n; left -= n;
  }
  return 0;
}

static bool t_open(void)
{
  if (listenfd < 0) {
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) return false;
    int one = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(g_opt.tcp_port);
    if (bind(listenfd, (struct sockaddr *)&a, sizeof a) != 0) {
      fprintf(stderr, "tcp: bind :%d failed: %s\n", g_opt.tcp_port, strerror(errno));
      close(listenfd); listenfd = -1; return false;
    }
    listen(listenfd, 1);
    printf("tcp: waiting for PSP on port %d ...\n", g_opt.tcp_port);
  }

  struct sockaddr_in peer; socklen_t pl = sizeof peer;
  connfd = accept(listenfd, (struct sockaddr *)&peer, &pl);
  if (connfd < 0) return false;

  int one = 1;
  setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  /* PSP sends a 32-byte password immediately after connecting. */
  char pw[NET_PASSWORD_LEN];
  if (t_read(pw, sizeof pw, 3000) != 0) { close(connfd); connfd = -1; return false; }
  if (memcmp(pw, g_opt.password, NET_PASSWORD_LEN) != 0) {
    fprintf(stderr, "tcp: password mismatch, dropping client\n");
    close(connfd); connfd = -1; return false;
  }
  printf("tcp: PSP connected from %s\n", inet_ntoa(peer.sin_addr));
  return true;
}

static void t_close(void)
{
  if (connfd >= 0) { close(connfd); connfd = -1; }
}

static transport_backend backend = {
  .name = "tcp", .open = t_open, .write = t_write, .read = t_read,
  .close = t_close, .full_response = false,
};
transport_backend *transport_tcp(void) { return &backend; }
