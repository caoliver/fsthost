#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <glib.h>
#include <ctype.h>
#include "jackvst.h"

#define ACK "<OK>"
#define NAK "<FAIL>"

/* serv.c */
int serv_get_sock ( uint16_t port );
int serv_get_client ( int socket_desc );
bool serv_send_client_data ( int client_sock, char* msg, int msg_len );
bool serv_client_get_data ( int client_sock, char* msg, int msg_max_len );
void serv_close_socket ( int socket_desc );

/* jfst.c */
void jvst_quit(JackVST* jvst);

static int serv_fd = 0;

static void list_programs ( JackVST* jvst, int client_sock ) {
	FST* fst = jvst->fst;
	int32_t i;
	for ( i = 0; i < fst->plugin->numPrograms; i++ ) {
		/* VST standard says that progName is 24 bytes but some plugs use more characters */
		char progName[64];
		if ( fst->vst_version >= 2 ) {
		    sprintf(progName, "%4d: ", i);
			fst_get_program_name(fst, i, &progName[6], sizeof(progName));
		} else {
			/* FIXME:
			So what ? nasty plugin want that we iterate around all presets ?
			no way ! We don't have time for this
			*/
			sprintf ( progName, "preset %d", i );
		}
		serv_send_client_data ( client_sock, progName, strlen(progName) );
        }
}

static void get_program ( JackVST* jvst, int client_sock ) {
	char msg[16];
	sprintf( msg, "PROGRAM:%d", jvst->fst->current_program );
	serv_send_client_data ( client_sock, msg, strlen(msg) );
}

enum PROTO_CMD {
	CMD_UNKNOWN,
	CMD_EDITOR,
	CMD_LIST_PROGRAMS,
	CMD_LIST_PARAMS,
	CMD_LIST_MIDI_MAP,
	CMD_GET_PROGRAM,
	CMD_SET_PROGRAM,
	CMD_GET_CHANNEL,
	CMD_SET_CHANNEL,
	CMD_MIDI_LEARN,
	CMD_SET_VOLUME,
	CMD_GET_VOLUME,
	CMD_SUSPEND,
	CMD_RESUME,
	CMD_LOAD,
	CMD_SAVE,
	CMD_HELP,
	CMD_KILL
};

struct PROTO_MAP {
	enum PROTO_CMD key;
	const char* name;
};

static struct PROTO_MAP proto_string_map[] = {
	{ CMD_EDITOR, "editor" },
	{ CMD_LIST_PROGRAMS, "list_programs" },
	{ CMD_LIST_PARAMS, "list_params" },	// TODO
	{ CMD_LIST_MIDI_MAP, "list_midi_map" },	// TODO
	{ CMD_GET_PROGRAM, "get_program" },
	{ CMD_SET_PROGRAM, "set_program" },
	{ CMD_GET_CHANNEL, "get_channel" },
	{ CMD_SET_CHANNEL, "set_channel" },
	{ CMD_MIDI_LEARN, "midi_learn" },
	{ CMD_SET_VOLUME, "set_volume" },
	{ CMD_GET_VOLUME, "get_volume" },
	{ CMD_SUSPEND, "suspend" },
	{ CMD_RESUME, "resume" },
	{ CMD_LOAD, "load" },
	{ CMD_SAVE, "save" },
	{ CMD_HELP, "help" },
	{ CMD_KILL, "kill" },
	{ CMD_UNKNOWN, NULL }
};

static enum PROTO_CMD proto_lookup ( const char* name ) {
	short i;
	for ( i = 0; proto_string_map[i].key != CMD_UNKNOWN; i++ ) {
		if ( ! strcasecmp( proto_string_map[i].name, name ) )
			return proto_string_map[i].key;
	}
	return CMD_UNKNOWN;
}

static char *nexttoken(char **str)
{
    char *cursor = *str;
    while (isspace(*cursor)) cursor++;
    if (!*cursor) return NULL;

    enum { qnormal, qbackslash, qoctal } state = qnormal;
    char *out = *str;
    char *tokstart = *str;
    char ch;
    char accum;
    int octct;

    while (ch = *cursor++) {
	switch (state) {
	case qnormal:
	    if (ch == '\\') {
		state = qbackslash;
		continue;
	    }
	    if (isspace(ch)) {
		*str = cursor;
		*out = 0;
		return tokstart;
	    }
	    *out++ = ch;
	    continue;
	case qbackslash:
		switch (ch) {
		case 'n': ch = '\n'; break;
		case 'r': ch = '\r'; break;
		case 't': ch = '\t'; break;
		case 'v': ch = '\v'; break;
		case 'f': ch = '\f'; break;
		default:
		    if (ch >= '0' && ch <= '7') {
			state = qoctal;
			accum = ch-'0';
			octct = 1;
			continue;
		    }
		}
	    *out++ = ch;
	    state = qnormal;
	    continue;
	case qoctal:
	    if (ch < '0' || ch > '7' || octct == 3) {
		*out++ = accum;
		cursor--;
		state = qnormal;
	    } else {
		accum = accum<<3 | ch-'0';
		octct++;
	    }
	}
    }
    if (state == qoctal)
	*out++ = accum;
    *str = cursor - 1;
    *out = 0;
    return tokstart != out ? tokstart : NULL;
}

static bool jvst_proto_client_dispatch ( JackVST* jvst, int client_sock ) {
        char *result = ACK;
        char msg[64];
        if ( ! serv_client_get_data ( client_sock, msg, sizeof msg ) )
		return false;
	bool do_update = false;

	printf ( "GOT MSG: %s\n", msg );

	char *msgptr = msg;
	char *cmdtok = nexttoken(&msgptr);
	char *param = cmdtok ? nexttoken(&msgptr) : NULL;
	char outbuf[64];

	if (cmdtok) {
	    switch ( proto_lookup ( cmdtok ) ) {
	    case CMD_SAVE:
		if (!jvst_save_state(jvst, param))
		    result = NAK;
		break;
	    case CMD_LOAD:
		if (jvst_load_state(jvst, param))
		    do_update = true;
		else
		    result = NAK;
		break;
	    case CMD_EDITOR:
		if (param && !strcasecmp(param, "open"))
		    fst_run_editor ( jvst->fst );
		else if (param && !strcasecmp(param, "close"))
		    fst_call ( jvst->fst, EDITOR_CLOSE );
		else {
		    puts ( "Need param: open|close" );
		    result = NAK;
		}
		break;
	    case CMD_MIDI_LEARN:
		do_update = true;
		if ( param && !strcasecmp(param, "start") ) {
		    jvst->midi_learn_CC = jvst->midi_learn_PARAM = -1;
		    jvst->midi_learn = TRUE;
		} else if ( param && !strcasecmp(param,"stop") ) {
		    jvst->midi_learn = FALSE;
		} else {
			puts ( "Need param: start|stop" );
			result = NAK;
			do_update = false;
		}
		break;
	    case CMD_GET_CHANNEL:
		snprintf(outbuf, 63, "CHANNEL:%d\n",
			 jvst->channel.redirect->channel);
		serv_send_client_data ( client_sock, outbuf, strlen(outbuf));
		break;
	    case CMD_SET_CHANNEL:
		if (param) {
		    midi_filter_one_channel_set(&jvst->channel,
						strtol(param, NULL, 10));
		    do_update = true;
		}
		break;
	    case CMD_GET_VOLUME:
		snprintf(outbuf, 63, "VOLUME:%d\n", jvst_get_volume(jvst));
		serv_send_client_data ( client_sock, outbuf, strlen(outbuf));
		break;
	    case CMD_SET_VOLUME:
		if (param) {
		    jvst_set_volume(jvst, strtol(param, NULL, 10));
		    do_update = true;
		}
		break;
	    case CMD_LIST_PROGRAMS:
		list_programs ( jvst, client_sock );
		break;
	    case CMD_GET_PROGRAM:
		get_program ( jvst, client_sock );
		break;
	    case CMD_SET_PROGRAM: {
		int pgm = param ? strtol(param, NULL, 10) : 0;
		fst_program_change ( jvst->fst, pgm >= 0 ? pgm : 0 ) ;
		do_update = true;
	    }
		break;
	    case CMD_SUSPEND:
		jvst_bypass ( jvst, true );
		do_update = true;
		break;
	    case CMD_RESUME:
		jvst_bypass ( jvst, false );
		do_update = true;
		break;
	    case CMD_KILL:
		jvst_quit ( jvst );
		break;
	    case CMD_UNKNOWN:
	    default:
		printf ( "Unknown command: %s\n", msg );
		result = NAK;
	    }
	}
	// Is this a race?
	if (do_update)
	    idle_cb(jvst);

	// Send ACK
	serv_send_client_data ( client_sock, result, strlen(result) );

	return true;
}

static bool handle_client_connection (GIOChannel *source, GIOCondition condition, gpointer data ) {
	JackVST* jvst = (JackVST*) data;

	int client_fd = g_io_channel_unix_get_fd ( source );
	bool ok = jvst_proto_client_dispatch ( jvst, client_fd );

	return ok;
}

static bool handle_server_connection (GIOChannel *source, GIOCondition condition, gpointer data ) {
	JackVST* jvst = (JackVST*) data;

	int serv_fd = g_io_channel_unix_get_fd ( source );
	int client_fd = serv_get_client ( serv_fd );

	/* Watch client socket */
	GIOChannel* channel = g_io_channel_unix_new ( client_fd );
	g_io_add_watch_full(
		channel,
		G_PRIORITY_DEFAULT_IDLE,
		G_IO_IN,
		(GIOFunc) handle_client_connection,
		jvst, NULL
	);

	return true;
}

/* Public functions */
bool jvst_proto_init ( JackVST* jvst ) {
	puts ( "Starting JVST PROTO control server ..." );
	serv_fd = serv_get_sock ( jvst->ctrl_port_number );
	if ( ! serv_fd ) {
		fst_error ( "Cannot create CTRL socket :(" );
		return false;
	}

	/* Watch server socket */
	GIOChannel* channel = g_io_channel_unix_new(serv_fd);
	g_io_add_watch_full (
		channel,
		G_PRIORITY_DEFAULT_IDLE,
		G_IO_IN,
		(GIOFunc) handle_server_connection,
		jvst, NULL
	);
	g_io_channel_unref(channel);

	return true;
}

bool jvst_proto_close ( JackVST* jvst ) {
	if ( serv_fd ) serv_close_socket ( serv_fd );
	return true;
}
