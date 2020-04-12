#include <stdio.h>
#include <stdarg.h>
#include <strings.h>
#include <ctype.h>

#include "jfst/node.h"
#include "log/log.h"

#define ACK "<OK>"
#define NAK "<FAIL>"

static Serv* serv = NULL;

/* fsthost.c */
extern void fsthost_quit();

static void jfst_send_fmt ( JFST* jfst, ServClient* serv_client, const char* fmt, ... ) {
	va_list ap;
	char msg[512];

	va_start (ap, fmt);
	vsnprintf (msg, sizeof msg, fmt, ap);
	serv_client_send_data ( serv_client, msg );
	va_end (ap);
}

static void list_programs ( JFST* jfst, ServClient* serv_client ) {
	char progName[FST_MAX_PROG_NAME];

	FST* fst = jfst->fst;
	int32_t i;
	for ( i = 0; i < fst_num_presets(fst); i++ ) {
		fst_get_program_name(fst, i, progName, sizeof(progName));
		jfst_send_fmt(jfst, serv_client, "%d:%s", i, progName);
        }
}

static void list_params ( JFST* jfst, ServClient* serv_client, bool raw ) {
	char paramName[FST_MAX_PARAM_NAME];

	FST* fst = jfst->fst;
	int32_t i;
	for ( i = 0; i < fst_num_params(fst); i++ ) {
		fst_call_dispatcher ( fst, effGetParamName, i, 0, paramName, 0 );
		float parm = fst->plugin->getParameter(fst->plugin, i);
		if (raw) {
		    jfst_send_fmt(jfst, serv_client, "%d:%s = 0x%X",
				  i, paramName, *(uint32_t *)&parm);
		} else
		    jfst_send_fmt(jfst, serv_client, "%d:%s = %f",
				  i, paramName, parm);
        }
}

static void list_midi_map ( JFST* jfst, ServClient* serv_client ) {
	char name[FST_MAX_PARAM_NAME];

	MidiLearn* ml = &jfst->midi_learn;
	FST* fst = jfst->fst;
	uint8_t cc;
	for (cc = 0; cc < 128; cc++) {
		int32_t paramIndex = ml->map[cc];
		if ( paramIndex < 0 || paramIndex >= fst_num_params(fst) )
			continue;

		fst_call_dispatcher( fst, effGetParamName, paramIndex, 0, name, 0 );
		jfst_send_fmt(jfst, serv_client, "%d:%s = %f", cc, name,
		    fst->plugin->getParameter(fst->plugin, paramIndex));
	}
}

static void get_midi_map(JFST *jfst, int cc_num, ServClient *serv_client)
{
    char name[FST_MAX_PARAM_NAME];

    MidiLearn* ml = &jfst->midi_learn;
    FST* fst = jfst->fst;
    cc_num = cc_num < 0 ? 0 : cc_num > 127 ? 127 : cc_num;
    int32_t paramIndex = ml->map[cc_num];
    if ( paramIndex < 0 || paramIndex >= fst_num_params(fst) ) {
	jfst_send_fmt(jfst, serv_client, "%d -> UNASSIGNED", cc_num);
    } else {
	fst_call_dispatcher( fst, effGetParamName, paramIndex, 0, name, 0 );
	jfst_send_fmt(jfst, serv_client, "%d -> %d:%s", cc_num, paramIndex,
		      name);
    }
}

static void set_midi_map(JFST *jfst, int cc_num, int parm_no)
{
    FST* fst = jfst->fst;
    MidiLearn* ml = &jfst->midi_learn;
    cc_num = cc_num < 0 ? 0 : cc_num > 127 ? 127 : cc_num;
    if (parm_no >= -1 && parm_no < fst->plugin->numParams)
	ml->map[cc_num] = parm_no;
}


static void get_program ( JFST* jfst, ServClient* serv_client ) {
	jfst_send_fmt( jfst, serv_client, "PROGRAM:%d", fst_get_program(jfst->fst) );
}

static void get_param ( JFST* jfst, int parm_no, ServClient* serv_client,
    bool raw) {
    char name[FST_MAX_PARAM_NAME];
    FST* fst = jfst->fst;
    parm_no = parm_no < 0 ? 0 : parm_no >= fst_num_params(fst) ?
	fst_num_params(fst) : parm_no;
    fst_call_dispatcher( fst, effGetParamName, parm_no, 0, name, 0 );
    float parm = fst->plugin->getParameter(fst->plugin, parm_no);
    if  (raw)
	jfst_send_fmt(jfst, serv_client, "%d = 0x%X",
		      parm_no, *(uint32_t *)&parm);
    else
	jfst_send_fmt(jfst, serv_client, "%d:%s = %f", parm_no, name, parm);
}

static void set_param_helper( JFST *jfst, int ix, float parmval)
{
    FST* fst = jfst->fst;
    ix = ix < 0 ? 0 : ix > fst_num_params(fst) ? fst_num_params(fst) : ix;
    parmval = parmval < 0 ? 0 : parmval > 1 ? 1 : parmval;
    fst->plugin->setParameter(fst->plugin, ix, parmval);
}

static void set_param ( JFST * jfst, int parm_no, char *value) {
    FST* fst = jfst->fst;
    float parmval;
    if (value[0] == '0' && value[1] == 'x') {
	uint32_t parm_int = strtoul(value, NULL, 0);
	parmval = *(float *)&parm_int;
    } else
	parmval = strtof(value, NULL);
    set_param_helper (jfst, parm_no, parmval);
}

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
    pun[0] = q >> 16 & 0xff;
    pun[1] = q >> 8 & 0xff;
    pun[2] = q & 0xff;
    q = decode_table[src[7]] |
        (decode_table[src[6]] |
         (decode_table[src[5]] |
          decode_table[src[4]] << 6) << 6) << 6;
    pun[3] = q >> 16 & 0xff;
    pun[4] = q >> 8 & 0xff;
    pun[5] = q & 0xff;
    set_param_helper(jfst, parmspec.param, parmspec.value);
}
static void get_channel ( JFST* jfst, ServClient* serv_client ) {
	jfst_send_fmt( jfst, serv_client, "CHANNEL:%d", midi_filter_one_channel_get(&jfst->channel) );
}

static void get_volume ( JFST* jfst, ServClient* serv_client ) {
	jfst_send_fmt( jfst, serv_client, "VOLUME:%d", jfst_get_volume(jfst) );
}

static void get_transpose ( JFST* jfst, ServClient* serv_client ) {
	jfst_send_fmt( jfst, serv_client, "TRANSPOSE:%d",
		       midi_filter_transposition_get(jfst->transposition) );
}

static void set_transpose ( JFST* jfst, int trnspos, ServClient* serv_client ) {
    trnspos =  trnspos < -36 ? -36 : trnspos > 36 ? 36 : trnspos;
    midi_filter_transposition_set(jfst->transposition, trnspos);
}

typedef enum {
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
	CMD_GET_TRANSPOSE,
	CMD_SET_TRANSPOSE,
	CMD_SUSPEND,
	CMD_RESUME,
	CMD_LOAD,
	CMD_SAVE,
	CMD_HELP,
	CMD_QUIT,
	CMD_KILL
} PROTO_CMD;

struct PROTO_MAP {
	PROTO_CMD key;
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

static PROTO_CMD proto_lookup ( const char* name ) {
	short i;
	for ( i = 0; proto_string_map[i].key != CMD_UNKNOWN; i++ ) {
		if ( ! strcasecmp( proto_string_map[i].name, name ) )
			return proto_string_map[i].key;
	}
	return CMD_UNKNOWN;
}

typedef struct {
	const char* cmd;
	const char* value;
	const char* value2;
	PROTO_CMD proto_cmd;
	JFST* jfst;
	/* return */
	unsigned int ack;
	bool quit;
	bool done;
} CMD;

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

void msg2cmd( char* msg, CMD* cmd ) {
	char* p = msg;
	char *token;

	cmd->cmd = "";
	cmd->value = "";
	cmd->value2 = "";

	if (token = nexttoken(&p)) {
	    cmd->cmd = token;
	    if (token = nexttoken(&p)) {
		cmd->value = token;
		if (token = nexttoken(&p))
		    cmd->value2 = token;
	    }
	}

	if (*cmd->cmd != ':')
	    cmd->proto_cmd = proto_lookup( cmd->cmd );
	cmd->ack = false;
	cmd->quit = false;
	cmd->done = false;
	cmd->jfst = NULL;
}

static void help( ServClient* serv_client ) {
	char msg[256] = "";
	short i;
	for ( i = 0; proto_string_map[i].key != CMD_UNKNOWN; i++ ) {
		strcat( msg, proto_string_map[i].name );
		strcat( msg, " " );
	}
	serv_client_send_data ( serv_client, msg );
}

void jfst_proto_dispatch( ServClient* serv_client, CMD* cmd ) {
	JFST* jfst = cmd->jfst;
	const char* value = cmd->value;
	cmd->done = cmd->ack = true;

	if (*cmd->cmd == ':') {
	    set_param_b64(jfst, cmd->cmd+1);
	    cmd->ack = 2;
	    return;
	}

	switch ( cmd->proto_cmd ) {
	case CMD_EDITOR:
		if ( !strcasecmp(value, "open") ) {
			fst_run_editor ( jfst->fst, false );
		} else if ( !strcasecmp(value,"close") ) {
			fst_call ( jfst->fst, EDITOR_CLOSE );
		} else {
			puts ( "Need value: open|close" );
			cmd->ack = false;
		}
		break;
	case CMD_LIST_PROGRAMS:
		list_programs ( jfst, serv_client );
		break;
	case CMD_LIST_PARAMS:
	        list_params ( jfst, serv_client, *value );
		if (*value)
		    cmd->ack = 2;
		break;
	case CMD_LIST_MIDI_MAP:
		list_midi_map( jfst, serv_client );
		break;
	case CMD_GET_PROGRAM:
		get_program ( jfst, serv_client );
		break;
	case CMD_SET_PROGRAM:
		fst_set_program ( jfst->fst, strtol(value, NULL, 10) );
		break;
	case CMD_GET_PARAM:
	        get_param ( jfst, strtol(value, NULL, 10), serv_client,
			    *cmd->value2 );
		if (*cmd->value2)
		    cmd->ack = 2;
		break;
	case CMD_SET_PARAM:
	        set_param(jfst, strtol(value, NULL, 10), cmd->value2);
		if (cmd->value2[0] == '0' && cmd->value2[1] == 'x')
		    cmd->ack = 2;
		break;
	case CMD_GET_MIDI_MAP:
	        get_midi_map ( jfst, strtol(value, NULL, 10), serv_client );
		break;
	case CMD_SET_MIDI_MAP:
	        set_midi_map(jfst, strtol(value, NULL, 10),
			  cmd->value2[0] ?
			     strtol(cmd->value2, NULL, 10) : -1);
		break;
	case CMD_GET_CHANNEL:
		get_channel ( jfst, serv_client );
		break;
	case CMD_SET_CHANNEL:
		midi_filter_one_channel_set( &jfst->channel, strtol(value, NULL, 10) );
		break;
	case CMD_GET_TRANSPOSE:
	        get_transpose( jfst, serv_client );
		break;
	case CMD_SET_TRANSPOSE:
	        set_transpose( jfst, strtol(value, NULL, 10), serv_client );
		break;
	case CMD_MIDI_LEARN:
		if ( !strcasecmp(value, "start") ) {
			jfst_midi_learn(jfst, true );
		} else if ( !strcasecmp(value,"stop") ) {
			jfst_midi_learn(jfst, false);
		} else {
			puts ( "Need value: start|stop" );
			cmd->ack = false;
		}
		break;
	case CMD_SET_VOLUME:
		jfst_set_volume(jfst, strtol(value, NULL, 10));
		break;
	case CMD_GET_VOLUME:
		get_volume(jfst, serv_client);
		break;
	case CMD_SUSPEND:
		jfst_bypass ( jfst, true );
		break;
	case CMD_RESUME:
		jfst_bypass ( jfst, false );
		break;
	case CMD_LOAD:
		cmd->ack = jfst_load_state ( jfst, value );
		break;
	case CMD_SAVE:
		cmd->ack = jfst_save_state ( jfst, value );
		break;
	default:
		cmd->ack = cmd->done = false;
	}
}

/******************** SERV ***********************************/

void fsthost_proto_dispatch ( ServClient* serv_client, CMD* cmd ) {
	cmd->ack = cmd->done = true;

	switch ( cmd->proto_cmd ) {
	case CMD_QUIT:
		cmd->quit = true;
		break;
	case CMD_HELP:
		help( serv_client );
		break;
	case CMD_UNKNOWN:
		log_error ( "GOT INVALID CMD: %s", cmd->proto_cmd );
		cmd->ack = false;
		break;
	case CMD_KILL:
		// TODO: close only one plugin
		fsthost_quit();
		break;

	default:
		cmd->ack = cmd->done = false;
	}
}

static bool handle_client_callback ( ServClient* serv_client, char* msg ) {
	CMD cmd;
	msg2cmd( msg, &cmd );

	fsthost_proto_dispatch( serv_client, &cmd );
	if ( cmd.done ) goto quit;

	JFST_NODE* jn = jfst_node_get_first();
	cmd.jfst = jn->jfst;
	jfst_proto_dispatch( serv_client, &cmd );

quit:
	if ( cmd.done ) log_debug ( "GOT VALID MSG: %s", msg );

	// Send ACK / NAK
	if (cmd.ack != 2)
	    serv_client_send_data ( serv_client, (cmd.ack) ? ACK : NAK );

	return ( ! cmd.quit );
}

/* Public functions */
bool fsthost_proto_init ( const char * ctrl_port_name ) {
	log_info ( "Starting PROTO control server" );
	serv = serv_init ( ctrl_port_name, handle_client_callback );
	if ( ! serv ) {
		log_error ( "Cannot create PROTO socket :(" );
		return false;
	}
	return true;
}

void proto_poll() {
	if ( ! serv ) return;
	serv_poll(serv);
}

void proto_close() {
	if ( ! serv ) return;
	log_info( "Stopping PROTO control server" );
	serv_close (serv);
}
