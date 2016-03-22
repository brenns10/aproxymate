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

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <pthread.h>

#define PORT 58008
#define ADDR "0.0.0.0"

void *proxy_thread(void *client_)
{
  int client = (int) client_;


  close(client);
  return NULL;
}

int main(int argc, char *argv[])
{
  // Open connection socket for accepting connections to the socket.
  int cnx_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (cnx_sock == -1) {
    perror("proxy: socket()");
    exit(EXIT_FAILURE);
  }

  // Initialize the address struct for the address, port we want to bind.
  struct sockaddr_in in;
  in.sin_family = AF_INET;
  in.sin_port = htons(PORT);
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

