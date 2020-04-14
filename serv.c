#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>    //write
#include <sys/socket.h>
#include <arpa/inet.h> //inet_addr
#include <sys/un.h>
#include <stdlib.h>

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

int serv_get_client ( int socket_desc ) {
	//Accept and incoming connection
	puts("Waiting for incoming connections...");
     
	//accept connection from an incoming client
	int client_sock = accept(socket_desc, NULL, NULL);
	if (client_sock < 0) {
		perror("accept failed");
		return 0;
	}
	puts("Connection accepted");

	return client_sock;
}

static void strip_trailing ( char* string, char chr ) {
	char* c = strrchr ( string, chr );
	if ( c ) *c = '\0';
}

bool serv_send_client_data ( int client_sock, char* msg, int msg_len ) {
	char data[msg_len + 2];
	snprintf ( data, sizeof data, "%s\n", msg );
	int len = sizeof(data) - 1;
	int write_size = write ( client_sock , data, len);
	return ( write_size == len ) ? true : false;
}

bool serv_client_get_data ( int client_sock, char* msg, int msg_max_len ) {
	//Receive a message from client
	int read_size = recv (client_sock , msg , msg_max_len, 0);
	if (read_size == 0) {
		puts("Client disconnected");
		close ( client_sock );
		return false;
	} else if (read_size < 0) {
		perror("recv failed");
		close ( client_sock );
		return false;
	}
	msg[read_size] = '\0'; /* make sure there is end */

	strip_trailing ( msg, '\n' );
	strip_trailing ( msg, '\r' );

	return true;
}

void serv_close_socket ( int socket_desc ) {
	close ( socket_desc );
	if (cleanup_sock)
	    unlink(server_un.sun_path);
}
