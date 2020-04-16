#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <glib.h>
#include <ctype.h>
#include "jackvst.h"

#define ACK "<OK>"
#define NAK "<FAIL>"

/* serv.c */
int serv_get_sock ( const char * );
int serv_get_client ( int socket_desc );
bool serv_send_client_data ( int client_sock, char* msg, int msg_len );
bool serv_client_get_data ( int client_sock, char* msg, int msg_max_len );
void serv_close_socket ( int socket_desc );

/* jfst.c */
void jvst_quit(JackVST* jvst);

static int serv_fd = 0;
bool send_ack;

// b64 encoding avoiding special OSC chars.
static char encode_table[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
"abcdefghijklmnopqrstuvwxyz"
"0123456789"
"+-";

static uint32_t decode_table[128];

static __attribute__((constructor)) void build_b64_decode_table()
{
    int i = 0;

    for (i = 63; i >= 0; i--)
        decode_table[encode_table[i]] = i;
}

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

#define FST_MAX_PARAM_NAME 32

struct __attribute__ ((packed)) b64template {
    float current;
    uint16_t index;
    float minimum;
    float maximum;
    uint8_t pad;
};

#define TEMPLATE_END ((sizeof(struct b64template)+2)/3 * 4)

static void encode_template(struct b64template *tpl, char *dst)
{
    uint8_t *src = (uint8_t *)tpl;
    
    for (int i=0; i < sizeof(*tpl); i+=3, src += 3) {
	uint32_t q = src[2] | (src[1] | (src[0] << 8)) << 8;
	*dst++ = encode_table[q >> 18];
	*dst++ = encode_table[q >> 12 & 63];
	*dst++ = encode_table[q >> 6 & 63];
	*dst++ = encode_table[q & 63];
    }
}

static void list_params ( JackVST *jvst, int client_sock, bool raw ) {
	char paramName[FST_MAX_PARAM_NAME];
	char msg[80];

	send_ack = !raw;

	FST* fst = jvst->fst;
	for ( int i = 0; i < fst->plugin->numParams; i++ ) {
		fst_call_dispatcher(fst, effGetParamName, i, 0, paramName, 0);
		float parm = fst->plugin->getParameter(fst->plugin, i);
		if (raw)
		    snprintf(msg, sizeof(msg), "%d:%s = 0x%X",
			     i, paramName, *(uint32_t *)&parm);
		else
		    snprintf(msg, sizeof(msg), "%d:%s = %f",
			     i, paramName, parm);
		serv_send_client_data ( client_sock, msg, strlen(msg) );
        }
}

static void list_params_b64(JackVST* jvst, int client_sock, bool brief ) {
    char paramName[FST_MAX_PARAM_NAME];
    char msg[80];
    char out[TEMPLATE_END + 1];
    out[TEMPLATE_END] = 0;

    FST* fst = jvst->fst;
    for ( int ix = 0; ix < fst->plugin->numParams; ix++ ) {
	fst_call_dispatcher(fst, effGetParamName, ix, 0, paramName, 0);
	float parm = fst->plugin->getParameter(fst->plugin, ix);
	for (int i = strlen(paramName); --i >= 0;)
	    if (isspace(paramName[i]))
		paramName[i] = '_';
	struct b64template tmpl = { parm, ix, 0, 1, 0 };
	encode_template(&tmpl, out);
	if (brief)
	    snprintf(msg, sizeof(msg), "%.8s %s", out, paramName);
	else
	    snprintf(msg, sizeof(msg), "%.8s %s %s", out, &out[8], paramName);

	serv_send_client_data ( client_sock, msg, strlen(msg) );
    }
    serv_send_client_data ( client_sock, "#FINI", 5 );
}

static void list_midi_map ( JackVST *jvst, int client_sock ) {
	char name[FST_MAX_PARAM_NAME];
	char msg[80];

	FST* fst = jvst->fst;
	uint8_t cc;
	for (cc = 0; cc < 128; cc++) {
		int32_t paramIndex = jvst->midi_map[cc];
		if ( paramIndex < 0 || paramIndex >= fst->plugin->numParams )
			continue;

		fst_call_dispatcher( fst, effGetParamName, paramIndex, 0, name, 0 );
		snprintf(msg, sizeof(msg), "%d:%s = %f", cc, name,
			 fst->plugin->getParameter(fst->plugin, paramIndex));
		serv_send_client_data ( client_sock, msg, strlen(msg) );
	}
}

static void get_midi_map(JackVST *jvst, int cc_num, int client_sock)
{
    char name[FST_MAX_PARAM_NAME];
    char msg[80];

    FST* fst = jvst->fst;
    cc_num = cc_num < 0 ? 0 : cc_num > 127 ? 127 : cc_num;
    int32_t paramIndex = jvst->midi_map[cc_num];
    if ( paramIndex < 0 || paramIndex >= fst->plugin->numParams ) {
	snprintf(msg, sizeof(msg), "%d -> UNASSIGNED", cc_num);
    } else {
	fst_call_dispatcher( fst, effGetParamName, paramIndex, 0, name, 0 );
	snprintf(msg, sizeof(msg), "%d -> %d:%s", cc_num, paramIndex, name);
    }
    serv_send_client_data ( client_sock, msg, strlen(msg) );
}

static void set_midi_map(JackVST *jvst, int cc_num, int parm_no,
			 int client_sock)
{
    FST* fst = jvst->fst;
    cc_num = cc_num < 0 ? 0 : cc_num > 127 ? 127 : cc_num;
    if (parm_no >= -1 && parm_no < fst->plugin->numParams)
	jvst->midi_map[cc_num] = parm_no;
}

static void set_param_helper( JackVST *jvst, int ix, float parmval)
{
    FST* fst = jvst->fst;
    ix = ix < 0 ? 0 : ix >= fst->plugin->numParams ? fst->plugin->numParams : ix;
    parmval = parmval < 0 ? 0 : parmval > 1 ? 1 : parmval;
    fst->plugin->setParameter(fst->plugin, ix, parmval);
}

static void set_param( JackVST *jvst, int ix, char *value, int client_sock ) {
	float parmval;
	if (value[0] == '0' && value[1] == 'x') {
	    uint32_t uintval = strtoul(value, NULL, 0);
	    send_ack = false;
	    parmval = *(float *)&uintval;
	} else
	    parmval = strtof(value, NULL);
	set_param_helper(jvst, ix, parmval);
}

static void set_param_b64(JackVST *jvst, const char *b64str)
{
    struct parts {
	float value;
	uint16_t param;
    } parmspec;

    if (strlen(b64str) != 8)
	return;

    uint8_t *pun = (uint8_t *)&parmspec, *src = b64str;
    uint32_t q = decode_table[src[3]] |
        (decode_table[src[2]] |
         (decode_table[src[1]] |
          decode_table[src[0]] << 6) << 6) << 6;
    pun[0] = q >> 16;
    pun[1] = q >> 8 & 0xff;
    pun[2] = q & 0xff;
    q = decode_table[src[7]] |
        (decode_table[src[6]] |
         (decode_table[src[5]] |
          decode_table[src[4]] << 6) << 6) << 6;
    pun[3] = q >> 16;
    pun[4] = q >> 8 & 0xff;
    pun[5] = q & 0xff;
    set_param_helper(jvst, parmspec.param, parmspec.value);
}

static void get_param( JackVST *jvst, int ix, int client_sock, bool raw ) {
    	char name[FST_MAX_PARAM_NAME];
	char msg[80];

	send_ack = !raw;

    	FST* fst = jvst->fst;
	if (ix < 0 && ix >= fst->plugin->numParams)
	    return;
	fst_call_dispatcher( fst, effGetParamName, ix, 0, name, 0 );
	float parm = fst->plugin->getParameter(fst->plugin, ix);
	if (raw)
	    snprintf(msg, sizeof(msg), "%d = 0x%X",
			     ix, *(uint32_t *)&parm);
	else
	    snprintf(msg, sizeof(msg), "%d:%s = %f", ix, name, parm);
	serv_send_client_data ( client_sock, msg, strlen(msg) );
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
	CMD_GET_MIDI_MAP,
	CMD_SET_MIDI_MAP,
	CMD_GET_PROGRAM,
	CMD_SET_PROGRAM,
	CMD_GET_PARAM,
	CMD_SET_PARAM,
	CMD_GET_CHANNEL,
	CMD_SET_CHANNEL,
	CMD_MIDI_LEARN,
	CMD_SET_VOLUME,
	CMD_GET_VOLUME,
	CMD_SET_TRANSPOSE,
	CMD_GET_TRANSPOSE,
	CMD_SUSPEND,
	CMD_RESUME,
	CMD_LOAD,
	CMD_SAVE,
	CMD_HELP,
	CMD_QUIT,
	CMD_KILL
};

struct PROTO_MAP {
	enum PROTO_CMD key;
	const char* name;
};

static struct PROTO_MAP proto_string_map[] = {
	{ CMD_EDITOR, "editor" },
	{ CMD_LIST_PROGRAMS, "list_programs" },
	{ CMD_LIST_PARAMS, "list_params" },
	{ CMD_LIST_MIDI_MAP, "list_midi_map" },
	{ CMD_GET_MIDI_MAP, "get_midi_map" },
	{ CMD_SET_MIDI_MAP, "set_midi_map" },
	{ CMD_GET_PROGRAM, "get_program" },
	{ CMD_SET_PROGRAM, "set_program" },
	{ CMD_GET_PARAM, "get_param" },
	{ CMD_SET_PARAM, "set_param" },
	{ CMD_GET_CHANNEL, "get_channel" },
	{ CMD_SET_CHANNEL, "set_channel" },
	{ CMD_MIDI_LEARN, "midi_learn" },
	{ CMD_SET_VOLUME, "set_volume" },
	{ CMD_GET_VOLUME, "get_volume" },
	{ CMD_SET_TRANSPOSE, "set_transpose" },
	{ CMD_GET_TRANSPOSE, "get_transpose" },
	{ CMD_SUSPEND, "suspend" },
	{ CMD_RESUME, "resume" },
	{ CMD_LOAD, "load" },
	{ CMD_SAVE, "save" },
	{ CMD_HELP, "help" },
	{ CMD_QUIT, "quit" },
	{ CMD_KILL, "kill" },
	{ CMD_UNKNOWN, NULL }
};

static void dohelp(int client_sock)
{
    char msg[256] = "";
	short i;
	for ( i = 0; proto_string_map[i].key != CMD_UNKNOWN; i++ ) {
		strcat( msg, proto_string_map[i].name );
		strcat( msg, " " );
	}
	serv_send_client_data ( client_sock, msg, strlen(msg) );
}

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
	send_ack = true;

//	printf ( "GOT MSG: %s\n", msg );

	char *p = msg;
	char *token;

	char *cmdtok = "";
	char *param = "";
	char *param2 = "";

	if (token = nexttoken(&p)) {
	    cmdtok = token;
	    if (token = nexttoken(&p)) {
		param = token;
		if (token = nexttoken(&p))
		    param2 = token;
	    }
	}


	char outbuf[64];
	unsigned int num_arg;

	if (cmdtok) {
	    if (*cmdtok == ':') {
		set_param_b64(jvst, cmdtok+1);
		return;
	    }

	    if (*cmdtok == '#') {
		list_params_b64(jvst, client_sock, cmdtok[1] == 0);
		return;
	    }

	    switch ( proto_lookup ( cmdtok ) ) {
	    case CMD_QUIT:
		close ( client_sock );
		return false;
	    case CMD_LIST_PARAMS:
		list_params(jvst, client_sock, param[0]);
		break;
	    case CMD_LIST_MIDI_MAP:
		list_midi_map(jvst, client_sock);
		break;
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
	    case CMD_GET_PARAM:
		num_arg = strtol(param, NULL, 10);
		get_param(jvst, num_arg, client_sock, param2[0]);
		break;
	    case CMD_SET_PARAM: {
		num_arg = strtol(param, NULL, 10);
		set_param(jvst, num_arg, param2[0] ? param2 : "0",
			  client_sock);
		break;
	    }
	    case CMD_GET_MIDI_MAP:
		num_arg = strtol(param, NULL, 10);
		get_midi_map(jvst, num_arg, client_sock);
		break;
	    case CMD_SET_MIDI_MAP: {
		num_arg = strtol(param, NULL, 10);
		set_midi_map(jvst, num_arg,
			     strtol(param2 ? param2 : "-1", NULL, 10),
			     client_sock);
		break;
	    }
	    case CMD_GET_CHANNEL:
		snprintf(outbuf, sizeof(outbuf)-1, "CHANNEL:%d\n",
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
		snprintf(outbuf, sizeof(outbuf)-1, "VOLUME:%d\n",
			 jvst_get_volume(jvst));
		serv_send_client_data ( client_sock, outbuf, strlen(outbuf));
		break;
	    case CMD_SET_VOLUME:
		if (param) {
		    jvst_set_volume(jvst, strtol(param, NULL, 10));
		    do_update = true;
		}
		break;
	    case CMD_GET_TRANSPOSE:
		snprintf(outbuf, sizeof(outbuf)-1, "TRANSPOSE:%d\n",
			 midi_filter_transposition_get(jvst->transposition));
		serv_send_client_data ( client_sock, outbuf, strlen(outbuf));
		break;
	    case CMD_SET_TRANSPOSE:
		if (param) {
		    int trnspos = strtol(param, NULL, 10);
		    trnspos =
			trnspos < -36 ? -36 : trnspos > 36 ? 36 : trnspos;
		    midi_filter_transposition_set(jvst->transposition,
						  trnspos);
		    refresh_transposition_spin(jvst);
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
	    case CMD_HELP:
		dohelp(client_sock);
		break;
	    case CMD_UNKNOWN:
	    default:
		printf ( "Unknown command: %s\n", cmdtok );
		result = NAK;
	    }
	}

	// Send ACK
	if (send_ack)
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
	serv_fd = serv_get_sock ( jvst->ctrl_port_name );
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
