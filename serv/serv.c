#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h> //inet_addr
#include <poll.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "log/log.h"
#include "serv.h"

#define INFO log_info
#define DEBUG log_debug
#define ERROR log_error

static bool cleanup_sock;

static void strip_trailing ( char* string, char chr ) {
	char* c = strrchr ( string, chr );
	if ( c ) *c = '\0';
}

static void serv_client_close ( ServClient* client ) {
	if ( client->fd != -1 ) {
		close ( client->fd );
		client->fd = -1;
	}
	client->closed = true;
}

static void serv_client_get_data ( ServClient* client ) {
	char msg[64];

	//Receive a message from client
	ssize_t read_size = read (client->fd, msg, sizeof msg );
	if (read_size == 0) {
		INFO("Client disconnected");
		serv_client_close( client );
		return;
	} else if (read_size < 0) {
		ERROR("recv failed FD:%d", client->fd);
		serv_client_close( client );
		return;
	} else {
		// Message normalize
		msg[read_size] = '\0'; /* make sure there is end */
		strip_trailing ( msg, '\n' );
		strip_trailing ( msg, '\r' );
	}

	if ( ! client->callback(client, msg) )
		serv_client_close( client );
}

static int serv_get_client_fd ( Serv* serv ) {
	//Accept and incoming connection
	INFO("Waiting for incoming connections...");
     
	//accept connection from an incoming client
	int client_sock = accept(serv->fd, NULL, NULL);
	if (client_sock < 0) {
		ERROR("accept failed");
		return 0;
	}
	INFO("Connection accepted");

	return client_sock;
}

static void serv_client_open ( Serv* serv ) {
	/* Find space for new client */
	int i;
	for ( i = 0; i < SERV_MAX_CLIENTS; i++ ) {
		if ( serv->clients[i].fd == -1 ) {
			int fd = serv_get_client_fd( serv );
			if ( ! fd ) return;

			ServClient* client = &serv->clients[i];
			client->fd = fd;
			client->closed = false;
			client->data = NULL;
			return;
		}
	}
}

struct sockaddr_un server_un;

Serv* serv_init ( const char *name, serv_client_callback cb ) {
    int fd ;
    
    if (*name && strspn(name, "0123456789") != strlen(name)) {
	fd = socket(AF_UNIX , SOCK_STREAM , 0);
	if (fd == -1) {
	    ERROR("Could not create socket");
	    return NULL;
	}
	INFO("Socket created");
	socklen_t addrlen = strlen(name);
	if (addrlen > sizeof(server_un.sun_path))
	    addrlen = sizeof(server_un.sun_path) - 1;
	memcpy(server_un.sun_path, name, addrlen);
	server_un.sun_family = AF_UNIX;
	unlink(server_un.sun_path);
	if (bind(fd, (struct sockaddr *)&server_un,
		 sizeof(server_un)) != 0) {
	    ERROR("bind failed. Error");
	    close(fd);
	    return NULL;
	}
	cleanup_sock = true;
	INFO("Serv start on port: %s", server_un.sun_path);
    } else {
	fd = socket(AF_INET , SOCK_STREAM , 0);
	if (fd == -1) {
	    ERROR("Could not create socket");
	    return NULL;
	}
	INFO("Socket created");
	struct sockaddr_in server;
	socklen_t addrlen = sizeof(server);
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl( INADDR_ANY );
	server.sin_port = htons( atoi(name) );
	int ofc = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &ofc, sizeof ofc);
 
	if ( bind (fd,(struct sockaddr *) &server , addrlen) < 0 ) {
	    ERROR("bind failed. Error");
	    close(fd);
	    return NULL;
	}
	getsockname ( fd, (struct sockaddr *) &server , &addrlen );
	INFO("Serv start on port: %d", ntohs(server.sin_port));
    }

    listen(fd, SERV_MAX_CLIENTS);

    Serv* serv = malloc( sizeof(Serv) );
    serv->fd = fd;

    // Init clients
    int i;
    for ( i = 0; i < SERV_MAX_CLIENTS; i++ ) {
	ServClient* client = &serv->clients[i];
	client->fd = -1;
	client->number = i;
	client->callback = cb;
	client->closed = true;
	client->data = NULL;
    }

    return serv;
}

bool serv_client_send_data ( ServClient* client, const char* msg ) {
	size_t msg_len = strlen(msg);
	char data[msg_len + 2];
	sprintf ( data, "%s\n", msg );
	size_t len = sizeof(data) - 1;
	ssize_t write_size = write ( client->fd, data, len );
	return ( write_size == len ) ? true : false;
}

void serv_poll (Serv* serv) {
	struct pollfd fds[SERV_POLL_SIZE];

	int i;
	for ( i = 0; i < SERV_POLL_SIZE; i++ ) {
		if ( i == 0 ) {
			fds[i].fd = serv->fd;
		} else {
			fds[i].fd = serv->clients[i-1].fd;
		}

		fds[i].events = POLLIN;
		fds[i].revents = 0;
	}

	//wait for an activity on one of the sockets, don't block
	int activity = poll(fds, SERV_POLL_SIZE, 0);
	if (activity < 1) return;

	for ( i = 0; i < SERV_POLL_SIZE; i++ ) {
		if ( fds[i].revents != POLLIN) {
		    if ( fds[i].revents != 0 ){
			if (fds[i].revents & POLLHUP) {
			    for ( int j = 1; j < SERV_POLL_SIZE; j++ )
				if (serv->clients[j-1].fd == fds[i].fd)
				    serv_client_close(&serv->clients[j-1]);
			}
			ERROR("FDS: %d, Err revents = %d", i, fds[i].revents);
		    }
			continue;
		}

		// Server socket i.e. new client
		if ( i == 0 ) {
			serv_client_open(serv);
		// Client socket
		} else {
			serv_client_get_data( &( serv->clients[i-1] ) );
		}
	}
}

void serv_close (Serv* serv) {
	// Close all clients
	int i;
	for ( i = 0; i < SERV_MAX_CLIENTS; i ++ )
		serv_client_close( &serv->clients[i] );

	// Close server
	close ( serv->fd );
	if (cleanup_sock)
	    unlink(server_un.sun_path);
	free(serv);
}
