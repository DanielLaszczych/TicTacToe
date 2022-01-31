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

#include "protocol.h"
#include "csapp.h"
#include "debug.h"

/*
 * Send a packet, which consists of a fixed-size header followed by an
 * optional associated data payload.
 *
 * @param fd  The file descriptor on which packet is to be sent.
 * @param hdr  The fixed-size packet header, with multi-byte fields
 *   in network byte order
 * @param data  The data payload, or NULL, if there is none.
 * @return  0 in case of successful transmission, -1 otherwise.
 *   In the latter case, errno is set to indicate the error.
 *
 * All multi-byte fields in the packet are assumed to be in network byte order.
 */
int proto_send_packet(int fd, JEUX_PACKET_HEADER *hdr, void *data) {
	int returnedBytes = write(fd, hdr, sizeof(*hdr));
	if (returnedBytes == -1) {
		return -1;
	}
	if (ntohs(hdr->size) > 0 && data != NULL) {
		returnedBytes = write(fd, data, ntohs(hdr->size));
		if (returnedBytes == -1) {
			return -1;
		}
	}
	return 0;
}

/*
 * Receive a packet, blocking until one is available.
 *
 * @param fd  The file descriptor from which the packet is to be received.
 * @param hdr  Pointer to caller-supplied storage for the fixed-size
 *   packet header.
 * @param datap  Pointer to a variable into which to store a pointer to any
 *   payload received.
 * @return  0 in case of successful reception, -1 otherwise.  In the
 *   latter case, errno is set to indicate the error.
 *
 * The returned packet has all multi-byte fields in network byte order.
 * If the returned payload pointer is non-NULL, then the caller has the
 * responsibility of freeing that storage.
 */
int proto_recv_packet(int fd, JEUX_PACKET_HEADER *hdr, void **payloadp) {
	*payloadp = NULL;
	hdr->size = ntohs(hdr->size);
	hdr->timestamp_sec = ntohl(hdr->timestamp_sec);
	hdr->timestamp_nsec = ntohl(hdr->timestamp_nsec);
	int returnedBytes = read(fd, hdr, sizeof(*hdr));
	if (returnedBytes == -1) {
		return -1;
	}
	if (returnedBytes == 0) {
		debug("EOF on fd: %d", fd);
		return -1;
	}
	if (hdr->size > 0) {
		*payloadp = calloc(hdr->size + 1, sizeof(char));
		returnedBytes = read(fd, *payloadp, hdr->size);
		if (returnedBytes == -1) {
			return -1;
		}
	} else {
		*payloadp = NULL;
	}
	return 0;
}