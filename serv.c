#include <stdbool.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>    //write
#include <sys/socket.h>
#include <arpa/inet.h> //inet_addr
#include <sys/un.h>
#include <stdlib.h>
#include "linefile.h"

static struct sockaddr_un server_un;
static bool cleanup_sock;

int serv_get_sock ( const char *sockname ) {
    int socket_desc;
    if ( *sockname && strspn(sockname, "0123456789") != strlen(sockname) ) {
	socket_desc = socket(AF_UNIX , SOCK_STREAM , 0);
	if (socket_desc < 0) goto err0;
	socklen_t addrlen = strlen(sockname);
	puts("Socket created");
	if (addrlen > sizeof(server_un.sun_path))
	    addrlen = sizeof(server_un.sun_path) - 1;
	memcpy(server_un.sun_path, sockname, addrlen);
	server_un.sun_family = AF_UNIX;
	unlink(server_un.sun_path);
	if (bind(socket_desc, (struct sockaddr *)&server_un,
		 sizeof(server_un)) != 0)
	    goto err1;
	cleanup_sock = true;
	printf ("bind done: port %s\n", server_un.sun_path);
    } else {
	socket_desc = socket(AF_INET , SOCK_STREAM , 0);
	if (socket_desc < 0) goto err0;
	puts("Socket created");
	struct sockaddr_in server;
	socklen_t addrlen = sizeof(server);
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl( INADDR_ANY );
	uint16_t port = atoi(sockname);
	server.sin_port = htons( port );
	int ofc = 1;
	setsockopt(socket_desc, SOL_SOCKET, SO_REUSEADDR, &ofc, sizeof ofc);
	if (bind (socket_desc,(struct sockaddr *) &server , addrlen) < 0)
	    goto err1;
	getsockname ( socket_desc, (struct sockaddr *) &server , &addrlen );
	printf ("bind done, port: %d\n", ntohs( server.sin_port ) );
    }

    listen(socket_desc , 3);
    return socket_desc;
    
err0:
    printf("Could not create socket");
    return 0;
err1:
    close(socket_desc);
    printf("Bind failed. Error");
    return 0;
}


static struct linefile *make_linefile(int fd, void *jvst)
{
    struct linefile *file = malloc(sizeof(*file));
    memset(file, 0, sizeof(*file));
    file->fd = fd;
    file->jvst = jvst;
    return file;
}

struct linefile *serv_get_client ( int socket_desc, void *jvst ) {
	//Accept and incoming connection
	puts("Waiting for incoming connections...");
     
	//accept connection from an incoming client
	int client_sock = accept(socket_desc, NULL, NULL);
	if (client_sock < 0) {
		perror("accept failed");
		return 0;
	}
	puts("Connection accepted");

	return make_linefile(client_sock, jvst);
}

bool serv_send_client_data ( int client_sock, char* msg, int msg_len ) {
	char data[msg_len + 2];
	snprintf ( data, sizeof data, "%s\n", msg );
	int len = sizeof(data) - 1;
	int write_size = write ( client_sock , data, len);
	return ( write_size == len ) ? true : false;
}

char *serv_client_nextline(struct linefile *file) {
    fd_set read_set;
    struct timeval timeout = {0, 0};
    char *start;
    int actual;
    switch (file->state) {
    case linefile_q_init:
    case linefile_q_readagain:
	FD_ZERO(&read_set);
	FD_SET(file->fd, &read_set);
	if (select(file->fd+1, &read_set, NULL, NULL, &timeout) < 1) {
	    file->state = linefile_q_init;
	    return NULL;
	}
	actual = read(file->fd, (char *)file->buf+file->nextix,
			  LINEFILE_BUFSIZE - file->nextix);
	if (actual <= 0) {
	    close(file->fd);
	    file->state = linefile_q_exit;
	    return NULL;
	}
	file->nextix += actual;
	file->end = (char *)file->buf - 1;
	file->newline_seen = false;
	file->state = linefile_q_scanagain;
    case linefile_q_scanagain:
	start = file->end + 1;
	if (file->end = strchr(start, '\n')) {
	    file->newline_seen = true;
	    if (file->skipping) {
		file->skipping = false;
		return NULL;
	    }
	    *file->end = 0;
	    return start;
	}
	file->end = (char *)file->buf - 1;
	if (file->newline_seen) {
	    file->nextix -= start - (char *)file->buf;
	    memmove(file->buf, start, file->nextix);
	} else if (file->nextix == LINEFILE_BUFSIZE) {
	    file->nextix = 0;
	    file->skipping = true;
	}
	file->state = linefile_q_readagain;
	return NULL;
    }
}

void serv_close_socket ( int socket_desc ) {
	close ( socket_desc );
	if (cleanup_sock)
	    unlink(server_un.sun_path);
}
