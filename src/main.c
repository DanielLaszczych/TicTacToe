#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "debug.h"
#include "protocol.h"
#include "server.h"
#include "client_registry.h"
#include "player_registry.h"
#include "jeux_globals.h"
#include "semaphore.h"
#include "csapp.h"

#ifdef DEBUG
int _debug_packets_ = 1;
#endif

volatile sig_atomic_t done = 0;

static void terminate(int status);

void sighup_handler(int sig, siginfo_t *info, void *context) {
    done = 1;
}

// void sigint_handler(int sig, siginfo_t *info, void *context) {
//     done = 2;
// }

/*
 * "Jeux" game server.
 *
 * Usage: jeux <port>
 */
int main(int argc, char* argv[]){
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = sighup_handler;
    act.sa_flags = SA_SIGINFO;
    sigaction(SIGHUP, &act, NULL);
    // struct sigaction act2;
    // sigemptyset(&act2.sa_mask);
    // act2.sa_sigaction = sigint_handler;
    // act2.sa_flags = SA_SIGINFO;
    // sigaction(SIGINT, &act2, NULL);
    struct sigaction act3;
    sigemptyset(&act3.sa_mask);
    act3.sa_handler = SIG_IGN;
    act3.sa_flags = -1;
    sigaction(SIGPIPE, &act3, NULL);
    // Option processing should be performed here.
    // Option '-p <port>' is required in order to specify the port number
    // on which the server should listen.
    int opt;
    char *port = NULL;
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
        case 'p':
            port = optarg;
            break;
       default: /* '?' */
            fprintf(stdout, "Usage: bin/jeux -p <port>\n");
            exit(EXIT_SUCCESS);
       }
    }

    if (port == NULL) {
        fprintf(stdout, "Usage: bin/jeux -p <port>\n");
        exit(EXIT_SUCCESS);
    }
    // Perform required initializations of the client_registry and
    // player_registry.
    client_registry = creg_init();
    player_registry = preg_init();

    // TODO: Set up the server socket and enter a loop to accept connections
    // on this socket.  For each connection, a thread should be started to
    // run function jeux_client_service().  In addition, you should install
    // a SIGHUP handler, so that receipt of SIGHUP will perform a clean
    // shutdown of the server.
    int listenfd, *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    listenfd = Open_listenfd(port);
    debug("%ld: Jeux server listening on port %s", pthread_self(), port);
    done = 0;
    while(!done) {
        clientlen = sizeof(struct sockaddr_storage);
        connfdp = malloc(sizeof(int));
        *connfdp = accept(listenfd, (SA *)&clientaddr, &clientlen);
        if (*connfdp != -1) {
            pthread_create(&tid, NULL, jeux_client_service, connfdp);
        }
    }
    free(connfdp);
    if (done == 1) {
        terminate(EXIT_SUCCESS);
    }
    // fprintf(stderr, "You have to finish implementing main() "
	   //  "before the Jeux server will function.\n");

    terminate(EXIT_FAILURE);
}

/*
 * Function called to cleanly shut down the server.
 */
void terminate(int status) {
    // Shutdown all client connections.
    // This will trigger the eventual termination of service threads.
    creg_shutdown_all(client_registry);
    debug("%ld: Waiting for service threads to terminate...", pthread_self());
    creg_wait_for_empty(client_registry);
    debug("%ld: All service threads terminated.", pthread_self());

    // Finalize modules.
    creg_fini(client_registry);
    preg_fini(player_registry);

    debug("%ld: Jeux server terminating", pthread_self());
    exit(status);
}
