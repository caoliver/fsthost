#ifndef __serv_h__
#define __serv_h__

#include <stdint.h>
#include <stdbool.h>

#define SERV_MAX_CLIENTS 3
#define SERV_POLL_SIZE SERV_MAX_CLIENTS + 1

typedef struct _ServClient ServClient;
typedef bool (*serv_client_callback) ( ServClient* client, char* msg );

#define LINEFILE_BUFSIZE 1024

enum linefile_state {
    linefile_q_init,
    linefile_q_exit,
    linefile_q_scanagain,
    linefile_q_readagain };

struct _ServClient {
	int fd; /* descriptior */
	int number; /* TODO: userfull only for news (?!?) */
	serv_client_callback callback;
	void* data;
	bool closed;

        char buf[LINEFILE_BUFSIZE+1];
	bool skipping;
	bool newline_seen;
	enum linefile_state state;
	int nextix;
	char *end;
};

typedef struct _Serv {
	int fd; /* descriptor */
	void* serv_usr_data;
	ServClient clients[SERV_MAX_CLIENTS];
} Serv;

Serv* serv_init ( const char *name, serv_client_callback cb );
void serv_close (Serv* serv);
void serv_poll (Serv* serv);
bool serv_client_send_data ( ServClient* client, const char* msg );

#endif /* __serv_h__ */
