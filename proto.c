#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <glib.h>
#include <ctype.h>
#include "linefile.h"
#include "jfst/node.h"
#include "log/log.h"

#define NAK "<FAIL>"

/* serv.c */
int serv_get_sock ( const char * );
struct linefile *serv_get_client ( int socket_desc );
bool serv_send_client_data ( int client_sock, char* msg, int msg_len );
bool serv_client_get_data ( int client_sock, char* msg, int msg_max_len );
void serv_close_socket ( int socket_desc );

/* jfst.c */
void fsthost_quit();

static int serv_fd = 0;

// b64 encoding avoiding special OSC chars.
static char encode_table[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
"abcdefghijklmnopqrstuvwxyz"
"0123456789"
"+/";

static uint32_t decode_table[128];

static __attribute__((constructor)) void build_b64_decode_table()
{
    int i = 0;

    for (i = 63; i >= 0; i--)
        decode_table[encode_table[i]] = i;
}

static void list_programs ( JFST *jfst, int client_sock ) {
	char progName[64];
	FST* fst = jfst->fst;
	
	for ( int32_t i = 0; i < fst_num_presets(fst); i++ ) {
	    sprintf(progName, "%4d: ", i);
	    fst_get_program_name(fst, i, &progName[6], sizeof(progName));
	    serv_send_client_data ( client_sock, progName, strlen(progName) );
        }
	serv_send_client_data ( client_sock, "##", 2 );
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

static void list_params ( JFST *jfst, int client_sock, bool raw ) {
	char paramName[FST_MAX_PARAM_NAME];
	char msg[80];

	FST* fst = jfst->fst;
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
	serv_send_client_data ( client_sock, "##", 2);
}

static void list_params_b64( JFST *jfst, int client_sock, bool brief ) {
    char paramName[FST_MAX_PARAM_NAME];
    char msg[80];
    char out[TEMPLATE_END + 1];
    out[TEMPLATE_END] = 0;

    FST* fst = jfst->fst;
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
    serv_send_client_data ( client_sock, "##", 2 );
}

static void list_midi_map ( JFST *jfst, int client_sock ) {
	char name[FST_MAX_PARAM_NAME];
	char msg[80];

	MidiLearn* ml = &jfst->midi_learn;
	FST* fst = jfst->fst;
	uint8_t cc;
	for (cc = 0; cc < 128; cc++) {
		int32_t paramIndex = ml->map[cc];
		if ( paramIndex < 0 || paramIndex >= fst_num_params(fst) )
			continue;

		fst_call_dispatcher( fst, effGetParamName, paramIndex, 0, name, 0 );
		snprintf(msg, sizeof(msg), "%d:%s = %f", cc, name,
			 fst->plugin->getParameter(fst->plugin, paramIndex));
		serv_send_client_data ( client_sock, msg, strlen(msg) );
	}
	serv_send_client_data ( client_sock, "##", 2 );
}

static void get_midi_map(JFST *jfst, int cc_num, int client_sock)
{
    char name[FST_MAX_PARAM_NAME];
    char msg[80];

    MidiLearn* ml = &jfst->midi_learn;
    FST* fst = jfst->fst;
    cc_num = cc_num < 0 ? 0 : cc_num > 127 ? 127 : cc_num;
    int32_t paramIndex = ml->map[cc_num];
    if ( paramIndex < 0 || paramIndex >= fst->plugin->numParams ) {
	snprintf(msg, sizeof(msg), "%d -> UNASSIGNED", cc_num);
    } else {
	fst_call_dispatcher( fst, effGetParamName, paramIndex, 0, name, 0 );
	snprintf(msg, sizeof(msg), "%d -> %d:%s", cc_num, paramIndex, name);
    }
    serv_send_client_data ( client_sock, msg, strlen(msg) );
}

static void set_midi_map(JFST *jfst, int cc_num, int parm_no,
			 int client_sock)
{
    FST* fst = jfst->fst;
    MidiLearn* ml = &jfst->midi_learn;
    cc_num = cc_num < 0 ? 0 : cc_num > 127 ? 127 : cc_num;
    if (parm_no >= -1 && parm_no < fst->plugin->numParams)
	ml->map[cc_num] = parm_no;
}

static void set_param_helper( JFST *jfst, int ix, float parmval)
{
    FST* fst = jfst->fst;
    ix = ix < 0 ? 0 : ix >= fst->plugin->numParams ? fst->plugin->numParams-1 : ix;
    parmval = parmval < 0 ? 0 : parmval > 1 ? 1 : parmval;
    fst->plugin->setParameter(fst->plugin, ix, parmval);
}

static void set_param( JFST *jfst, int ix, char *value, int client_sock ) {
	float parmval;
	if (value[0] == '0' && value[1] == 'x') {
	    uint32_t uintval = strtoul(value, NULL, 0);
	    parmval = *(float *)&uintval;
	} else
	    parmval = strtof(value, NULL);
	set_param_helper(jfst, ix, parmval);
}

static void set_param_b64(JFST *jfst, const char *b64str)
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
    set_param_helper(jfst, parmspec.param, parmspec.value);
}

static void get_param( JFST *jfst, int ix, int client_sock, bool raw ) {
    	char name[FST_MAX_PARAM_NAME];
	char msg[80];
    	FST* fst = jfst->fst;
	if (ix < 0 || ix >= fst_num_params(fst)) {
	    serv_send_client_data ( client_sock, NAK, strlen(NAK) );
	    return;
	}
	fst_call_dispatcher( fst, effGetParamName, ix, 0, name, 0 );
	float parm = fst->plugin->getParameter(fst->plugin, ix);
	if (raw)
	    snprintf(msg, sizeof(msg), "%d = 0x%X",
			     ix, *(uint32_t *)&parm);
	else
	    snprintf(msg, sizeof(msg), "%d:%s = %f", ix, name, parm);
	serv_send_client_data ( client_sock, msg, strlen(msg) );
}

static void get_program ( JFST* jfst, int client_sock ) {
	char msg[16];
	sprintf( msg, "PROGRAM:%d", fst_get_program(jfst->fst) );
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

static bool jfst_proto_client_dispatch ( char *msg, int client_sock,
					 JFST* jfst) {
	bool failed = false;

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
		set_param_b64(jfst, cmdtok+1);
		return true;
	    }

	    if (*cmdtok == '#') {
		list_params_b64(jfst, client_sock, cmdtok[1] == 0);
		return true;
	    }

	    switch ( proto_lookup ( cmdtok ) ) {
	    case CMD_QUIT:
		return false;
	    case CMD_LIST_PARAMS:
		list_params(jfst, client_sock, param[0]);
		break;
	    case CMD_LIST_MIDI_MAP:
		list_midi_map(jfst, client_sock);
		break;
	    case CMD_SAVE:
		if (!jfst_save_state(jfst, param))
		    failed = true;
		break;
	    case CMD_LOAD:
		if (!jfst_load_state(jfst, param))
		    failed = true;
		break;
	    case CMD_EDITOR:
		if (param && !strcasecmp(param, "open"))
		    fst_run_editor ( jfst->fst, false );
		else if (param && !strcasecmp(param, "close"))
		    fst_call ( jfst->fst, EDITOR_CLOSE );
		else {
		    puts ( "Need param: open|close" );
		    failed = true;
		}
		break;
	    case CMD_MIDI_LEARN:
		if ( param && !strcasecmp(param, "start") ) {
		    jfst_midi_learn(jfst, true );
		} else if ( param && !strcasecmp(param,"stop") ) {
		    jfst_midi_learn(jfst, false );
		} else {
			puts ( "Need param: start|stop" );
			failed = true;
		}
		break;
	    case CMD_GET_PARAM:
		num_arg = strtol(param, NULL, 10);
		get_param(jfst, num_arg, client_sock, param2[0]);
		break;
	    case CMD_SET_PARAM: {
		num_arg = strtol(param, NULL, 10);
		set_param(jfst, num_arg, param2[0] ? param2 : "0",
			  client_sock);
		break;
	    }
	    case CMD_GET_MIDI_MAP:
		num_arg = strtol(param, NULL, 10);
		get_midi_map(jfst, num_arg, client_sock);
		break;
	    case CMD_SET_MIDI_MAP: {
		num_arg = strtol(param, NULL, 10);
		set_midi_map(jfst, num_arg,
			     strtol(param2 ? param2 : "-1", NULL, 10),
			     client_sock);
		break;
	    }
	    case CMD_GET_CHANNEL:
		snprintf(outbuf, sizeof(outbuf)-1, "CHANNEL:%d\n",
			 midi_filter_one_channel_get(&jfst->channel));
		serv_send_client_data ( client_sock, outbuf, strlen(outbuf));
		break;
	    case CMD_SET_CHANNEL:
		if (param) {
		    midi_filter_one_channel_set(&jfst->channel,
						strtol(param, NULL, 10));
		}
		break;
	    case CMD_GET_VOLUME:
		snprintf(outbuf, sizeof(outbuf)-1, "VOLUME:%d\n",
			 jfst_get_volume(jfst));
		serv_send_client_data ( client_sock, outbuf, strlen(outbuf));
		break;
	    case CMD_SET_VOLUME:
		if (param)
		    jfst_set_volume(jfst, strtol(param, NULL, 10));
		break;
	    case CMD_GET_TRANSPOSE:
		snprintf(outbuf, sizeof(outbuf)-1, "TRANSPOSE:%d\n",
			 midi_filter_transposition_get(jfst->transposition));
		serv_send_client_data ( client_sock, outbuf, strlen(outbuf));
		break;
	    case CMD_SET_TRANSPOSE:
		if (param) {
		    int trnspos = strtol(param, NULL, 10);
		    trnspos =
			trnspos < -36 ? -36 : trnspos > 36 ? 36 : trnspos;
		    midi_filter_transposition_set(jfst->transposition,
						  trnspos);
		}
		break;
	    case CMD_LIST_PROGRAMS:
		list_programs ( jfst, client_sock );
		break;
	    case CMD_GET_PROGRAM:
		get_program ( jfst, client_sock );
		break;
	    case CMD_SET_PROGRAM: {
		int pgm = param ? strtol(param, NULL, 10) : 0;
		fst_set_program ( jfst->fst, pgm >= 0 ? pgm : 0 ) ;
	    }
		break;
	    case CMD_SUSPEND:
		jfst_bypass ( jfst, true );
		break;
	    case CMD_RESUME:
		jfst_bypass ( jfst, false );
		break;
	    case CMD_KILL:
		fsthost_quit ();
		break;
	    case CMD_HELP:
		dohelp(client_sock);
		break;
	    case CMD_UNKNOWN:
	    default:
		printf ( "Unknown command: %s\n", cmdtok );
		failed = true;
	    }
	}

	if (failed)
	    serv_send_client_data ( client_sock, NAK, strlen(NAK) );

	return true;
}

static bool handle_client_connection (GIOChannel *source, GIOCondition condition, gpointer data ) {
    bool do_quit;
    struct linefile *file = (struct linefile *)data;
    JFST* jfst = jfst_node_get_first()->jfst;
    
    while (1) {
	char *msg = serv_client_nextline(file);
	if (msg) {
	    if (do_quit = !jfst_proto_client_dispatch(msg, file->fd, jfst)) {
		g_io_channel_shutdown(file->channel, false, NULL);
		break;
	    }
	} else if (do_quit = file->state == linefile_q_exit) {
	    g_io_channel_shutdown(file->channel, false, NULL);
	    break;
	} else if (file->state == linefile_q_init) break;
    }
    if (do_quit)
	free(file);
    return !do_quit;
}

static bool handle_server_connection (GIOChannel *source, GIOCondition condition, gpointer data ) {
	int serv_fd = g_io_channel_unix_get_fd ( source );

	struct linefile *file = serv_get_client ( serv_fd );
	if (file != NULL) {
	    /* Watch client socket */
	    GIOChannel* channel = g_io_channel_unix_new ( file->fd );
	    file->channel = channel;
	    g_io_add_watch_full(
		channel,
		G_PRIORITY_DEFAULT_IDLE,
		G_IO_IN,
		(GIOFunc) handle_client_connection,
		file, NULL
		);
	}

	return true;
}

/* Public functions */
bool fsthost_proto_init ( const char * ctrl_port_name ) {
	log_info ( "Starting PROTO control server" );
	serv_fd = serv_get_sock ( ctrl_port_name );
	if ( ! serv_fd ) {
		log_error ( "Cannot create PROTO socket :(" );
		return false;
	}

	/* Watch server socket */
	GIOChannel* channel = g_io_channel_unix_new(serv_fd);
	g_io_add_watch_full (
		channel,
		G_PRIORITY_DEFAULT_IDLE,
		G_IO_IN,
		(GIOFunc) handle_server_connection,
		NULL, NULL
	);
	g_io_channel_unref(channel);

	return true;
}

bool proto_close () {
	if ( serv_fd ) serv_close_socket ( serv_fd );
	log_info( "Stopping PROTO control server" );
	return true;
}
