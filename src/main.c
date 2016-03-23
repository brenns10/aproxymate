/***************************************************************************//**

  @file         main.c

  @author       Stephen Brennan

  @date         Created Tuesday, 22 March 2016

  @brief        Main file for C proxy.

  @copyright    Copyright (c) 2016, Stephen Brennan.  Released under the Revised
                BSD License.  See LICENSE.txt for details.

*******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <assert.h>
#include <signal.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <pthread.h>

#include "libstephen/cb.h"
#include "libstephen/ht.h"

/*
  A few good macros.
 */
#define ADDR "0.0.0.0"
#define BUFSIZE 4096
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) >= (y) ? (x) : (y))

/*
  Type declarations.
 */
struct http_message {
  char *buffer;
  char *first[3];
  smb_ht headers;
};

enum body_type {
  NONE, CONTENT_LENGTH, CHUNKED, IDENTITY
};

/*
  Convenience functions for sockets.
 */
char *read_line(int socket)
{
  char c;
  int state = 0;
  cbuf cb;
  cb_init(&cb, 256);

  // Loop until we've read a line.
  while (state != 2) {
    if (read(socket, &c, 1) <= 0) {
      perror("proxy: read()");
      cb_destroy(&cb);
      return NULL;
    }
    cb_append(&cb, c);

    // Update state
    if (c == '\r' && state == 0) state++;
    else if (c == '\n' && state == 1) state++;
    else state = 0;
  }

  cb_trim(&cb);
  return cb.buf;
}

char *socket_read_message(int socket)
{
  char c;
  int state = 0;
  cbuf cb;
  cb_init(&cb, 256);

  // Loop until we've read \r\n\r\n
  while (state != 4) {
    if (read(socket, &c, 1) <= 0) {
      perror("proxy: read()");
      cb_destroy(&cb);
      return NULL;
    }
    cb_append(&cb, c);

    // Update state
    if (c == '\r' && (state == 0 || state == 2)) state++;
    else if (c == '\n' && (state == 1 || state == 3)) state++;
    else state = 0;
  }

  cb_trim(&cb);
  return cb.buf;
}

int write_buffer(int socket, char *buf, int len)
{
  int written = 0;
  while (written < len) {
    int rv = write(socket, buf + written, len - written);
    if (rv < 0) {
      return -1;
    }
    written += rv;
  }
  return 0;
}

int write_string(int socket, char *string)
{
  return write_buffer(socket, string, strlen(string));
}

void send_nbytes(int from, int to, int nbytes)
{
  printf("send_nbytes\n");
  char *bytes = malloc(BUFSIZE);
  int forwarded = 0;
  while (forwarded < nbytes) {
    int amount = read(from, bytes, MIN(nbytes-forwarded, BUFSIZE));
    if (amount == -1) {
      perror("proxy: read()");
      exit(EXIT_FAILURE);
    }
    write_buffer(to, bytes, amount);
    forwarded += amount;
    printf("%d/%d\n", forwarded, nbytes);
  }
}

/*
  Message parsing.
 */
bool char_in_set(char c, char *set)
{
  while (*set != '\0') {
    if (c == *set) return true;
    set++;
  }
  return false;
}

char *trim(char *string, char *characters)
{
  size_t i, start_idx, end_idx;
  size_t string_len;
  string_len = strlen(string);

  // locate the first character not in the set
  for (i = 0; i < string_len; i++) {
    if (!char_in_set(string[i], characters)) break;
  }
  start_idx = i;

  // Locate the last character not in the set.
  for (i = string_len - 1; i > start_idx; i++) {
    if (!char_in_set(string[i], characters)) break;
  }
  end_idx = i + 1; // end index is the one we replace with \0

  string[end_idx] = '\0';
  return string + start_idx;
}

void parse_http_message_headers(struct http_message *msg)
{
  char *curr = msg->buffer, *newline, *colon, *value;

  printf("Parsing message headers...\n");
  // First, find the end of the request/status line.
  newline = strstr(curr, "\r\n");
  *newline = '\0';

  // Tokenize first line.
  msg->first[0] = msg->buffer;
  msg->first[1] = strchr(curr, ' ') + 1;
  *(msg->first[1]-1) = '\0';
  msg->first[2] = strchr(msg->first[1], ' ') + 1;
  *(msg->first[2]-1) = '\0';
  printf("  First line tokens: \"%s\" \"%s\" \"%s\"\n", msg->first[0],
         msg->first[1], msg->first[2]);

  // Move the current pointer onwards.
  curr = newline + 2;

  // Now, parse headers:
  while (strstr(curr, "\r\n") != curr) {
    // Find the colon of the header.
    colon = strchr(curr, ':');
    *colon = '\0';

    // Find the end of the header line.
    newline = strstr(colon + 1, "\r\n");
    *newline = '\0';

    // Save the key and value.
    value = colon + 1;
    value = trim(value, " \t");
    printf("  parsed: %s: %s\n", curr, value);
    ht_insert(&msg->headers, PTR(curr), PTR(value));
    curr = newline + 2;
  }
  printf("DONE!\n");
}

char *normalize_url(struct http_message *req)
{
  printf("Normalizing URL %s\n", req->first[1]);
  char prefix[] = "http://";
  if (strncmp(req->first[1], prefix, strlen(prefix)) == 0) {
    // Identify where the host string starts.
    char *host_start = req->first[1] + 7;

    // And then where the remainder of the URL is.
    char *slash = strchr(host_start, '/');
    if (slash == NULL) {
      // if there's no slash, it must be an empty URL.  We'll change it to /
      // later.  Right now, we just pretend it's the empty string.
      slash = strchr(host_start, '\0');
    }

    // Length of the hostname part.
    int host_length = slash-host_start;

    // Move the hostname back a bit in the buffer, and null-terminate it.
    memmove(req->first[1], host_start, host_length);
    req->first[1][host_length] = '\0';
    host_start = req->first[1];

    // If we had an empty url, make it slash.
    if (*slash == '\0') {
      slash--;
      *slash = '/';
    }

    req->first[1] = slash;
    printf("Result is %s, host is %s\n", req->first[1], host_start);
    return host_start;
  }
    printf("Result is %s\n", req->first[1]);
  return NULL;
}

enum body_type message_body_type(struct http_message *msg)
{
  smb_status status = SMB_SUCCESS;
  if (ht_contains(&msg->headers, PTR("Content-Length"))) {
    return CONTENT_LENGTH;
  } else if (ht_contains(&msg->headers, PTR("Transfer-Encoding"))) {
    char *encoding = ht_get(&msg->headers, PTR("Transfer-Encoding"), &status).data_ptr;
    assert(status == SMB_SUCCESS);
    if (strcmp(encoding, "identity") == 0) {
      return IDENTITY;
    } else {
      return CHUNKED;
    }
  }
  return NONE;
}

void forward_chunked(int from, int to)
{
  printf("forward_chunked\n");
  char *line;
  int size;
  for (;;) {
    line = read_line(from);
    sscanf(line, "%x", &size);
    write_string(to, line);
    printf("chunk: %s", line);
    printf("aka: %d\n", size);
    free(line);
    if (size == 0) break;
    send_nbytes(from, to, size + 2);
  }
  write_string(to, "\r\n");
}

char *build_headers(struct http_message *msg)
{
  smb_status status = SMB_SUCCESS;
  char *key, *value;
  cbuf cb;
  cb_init(&cb, 1024);
  cb_concat(&cb, msg->first[0]);
  cb_append(&cb, ' ');
  cb_concat(&cb, msg->first[1]);
  cb_append(&cb, ' ');
  cb_concat(&cb, msg->first[2]);
  cb_concat(&cb, "\r\n");

  smb_iter it = ht_get_iter(&msg->headers);
  while (it.has_next(&it)) {
    key = it.next(&it, &status).data_ptr;
    assert(status == SMB_SUCCESS);
    value = ht_get(&msg->headers, PTR(key), &status).data_ptr;
    assert(status == SMB_SUCCESS);
    cb_concat(&cb, key);
    cb_concat(&cb, ": ");
    cb_concat(&cb, value);
    cb_concat(&cb, "\r\n");
  }

  cb_concat(&cb, "\r\n");
  cb_trim(&cb);
  printf("built headers:\"%s\"\n", cb.buf);
  return cb.buf;
}

void forward_message(struct http_message *msg, int from, int to)
{
  printf("forward_message()\n");
  // First the headers.
  char *headers = build_headers(msg);
  write_string(to, headers);

  // Now the body.
  enum body_type type = message_body_type(msg);
  smb_status status = SMB_SUCCESS;
  int size;
  char *size_string;
  switch (type) {
  case CONTENT_LENGTH:
    size_string = ht_get(&msg->headers, PTR("Content-Length"), &status).data_ptr;
    sscanf(size_string, "%d", &size);
    send_nbytes(from, to, size);
    break;
  case CHUNKED:
    forward_chunked(from, to);
    break;
  case IDENTITY:
    // TODO
    fprintf(stderr, "error: haven't implemented chunked transfer-encoding\n");
    exit(EXIT_FAILURE);
    break;
  case NONE:
    // explicitly do nothing :D
    break;
  }
}

int connect_to(char *host, uint16_t port)
{
  // Lookup address of host.
  struct hostent *hostent = gethostbyname(host);
  if (hostent == NULL) {
    herror("proxy: gethostbyname()");
    exit(EXIT_FAILURE);
  }

  // Create socket to talk to server
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server == -1) {
    perror("proxy: socket()");
    exit(EXIT_FAILURE);
  }

  // Create socket address.
  struct sockaddr_in in;
  in.sin_family = AF_INET;
  in.sin_port = htons(port);
  memcpy(&in.sin_addr, hostent->h_addr_list[0], hostent->h_length); // finger cross

  // And we connect.
  if (connect(server, (struct sockaddr*)&in, sizeof(in)) == -1) {
    perror("proxy: connect()");
    exit(EXIT_FAILURE);
  }

  return server;
}

void *proxy_thread(void *client_)
{
  int client = (int) client_;
  struct http_message req, rsp;
  smb_status status = SMB_SUCCESS;
  char *hostname;
  bool keep_alive = true;

  while (keep_alive) {
    keep_alive = false;

    // Read start of message and parse headers.
    printf("reading message from the client\n");
    req.buffer = socket_read_message(client);
    printf("raw message:\"%s\"\n", req.buffer);
    ht_init(&req.headers, ht_string_hash, data_compare_string);
    printf("parsing message headers\n");
    parse_http_message_headers(&req);

    // Do connection management.
    printf("updating some headers\n");
    if (ht_contains(&req.headers, PTR("Connection")) &&
        strcmp(ht_get(&req.headers, PTR("Connection"), &status).data_ptr, "keep-alive") == 0) {
      keep_alive = true;
    } else  if (ht_contains(&req.headers, PTR("Proxy-Connection")) &&
                strcmp(ht_get(&req.headers, PTR("Proxy-Connection"), &status).data_ptr, "keep-alive") == 0) {
      keep_alive = true;
      ht_remove(&req.headers, PTR("Proxy-Connection"), &status);
    }
    ht_insert(&req.headers, PTR("Connection"), PTR("close"));

    // Get hostname and URL.
    hostname = normalize_url(&req);
    if (hostname == NULL) {
      hostname = ht_get(&req.headers, PTR("Host"), &status).data_ptr;
    } else {
      ht_insert(&req.headers, PTR("Host"), PTR(hostname));
    }
    printf("Final hostname is: %s\n", hostname);

    // Open connection to server.
    printf("Connecting to the server.\n");
    int server = connect_to(hostname, 80);

    // Now forward the request.
    printf("Forwarding message to server.\n");
    forward_message(&req, client, server);

    // Now we read the start of the response.
    printf("Reading response.\n");
    rsp.buffer = socket_read_message(server);
    printf("raw message:\"%s\"\n", rsp.buffer);
    ht_init(&rsp.headers, ht_string_hash, data_compare_string);
    printf("Parsing headers of response.\n");
    parse_http_message_headers(&rsp);

    // More connection management.
    if (keep_alive) {
      ht_insert(&rsp.headers, PTR("Connection"), PTR("keep-alive"));
    } else {
      ht_insert(&rsp.headers, PTR("Connection"), PTR("close"));
    }

    // Forward the response back.
    printf("Forwarding message back to client.\n");
    forward_message(&rsp, server, client);

    // Always close the server socket.
    close(server);
  }

  printf("thread terminating\n");
  close(client);
  return NULL;
}

int cnx_sock;
void cleanup_socket(int sig)
{
  (void)sig; //unused
  close(cnx_sock);
}

int main(int argc, char *argv[])
{
  (void)argc; //unused
  (void)argv; //unused

  if (argc < 2) {
    fprintf(stderr, "usage: %s PORT\n", argv[0]);
    exit(EXIT_FAILURE);
  }
  int port;
  sscanf(argv[1], "%d", &port);

  // Close the server socket when interrupted.
  signal(SIGINT, &cleanup_socket);

  // Open connection socket for accepting connections to the socket.
  cnx_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (cnx_sock == -1) {
    perror("proxy: socket()");
    exit(EXIT_FAILURE);
  }

  // Initialize the address struct for the address, port we want to bind.
  struct sockaddr_in in;
  in.sin_family = AF_INET;
  in.sin_port = htons(port);
  inet_aton(ADDR, &in.sin_addr);

  // Bind the socket to a particular port.
  if (bind(cnx_sock, (struct sockaddr *)&in, sizeof(in)) == -1) {
    perror("proxy: bind()");
    close(cnx_sock);
    exit(EXIT_FAILURE);
  }

  // Instruct socket to listen  for conections.
  if (listen(cnx_sock, 0) == -1) {
    perror("proxy: listen()");
    close(cnx_sock);
    exit(EXIT_FAILURE);
  }

  // Create a buffer to hold thread info, which we can dynamically expand.  This
  // is a bad design for scaling, since the memory requirement scales linearly
  // as we add threads.  But for an initial test, this is fine. (TODO: address
  // when prototyping complete).
  size_t threads_alloc = 1024;
  size_t threads_inited = 0;
  pthread_t *threads = calloc(threads_alloc, sizeof(pthread_t));

  // Initialie attributes for our threads.
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) {
    perror("proxy: pthread_attr_init()");
    free(threads);
    close(cnx_sock);
    exit(EXIT_FAILURE);
  }

  // Loop accepting connections and spawning threads.
  for (;;) {
    // Accept an incoming connection.
    int client_sock = accept(cnx_sock, NULL, NULL);
    if (client_sock == -1) {
      perror("proxy: accept()");
      break;
    }

    // TODO: realloc buffer if necessary

    // Create and start a thread to handle this connection.
    if (0 != pthread_create(&threads[threads_inited++], &attributes,
                            proxy_thread, (void*) client_sock)) {
      perror("proxy: pthread_create()");
      break;
    }
  }

  // Join all threads.
  for (size_t i = 0; i < threads_inited; i++) {
    pthread_join(threads[i], NULL);
  }

  // Finally, we're done.
  free(threads);
  close(cnx_sock);
  return 0;
}

