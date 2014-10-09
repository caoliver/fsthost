/*
    Copyright (C) 2004 Paul Davis <paul@linuxaudiosystems.com> 
                       Torben Hohn <torbenh@informatik.uni-bremen.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <semaphore.h>
#include <signal.h>

#include "jfst/jfst.h"
#include "xmldb/info.h"

#define VERSION "1.5.5.1"
#ifdef __x86_64__
#define ARCH "64"
#else
#define ARCH "32"
#endif
#define APPNAME "fsthost"
#define APPNAME_ARCH APPNAME ARCH

/* jvstproto.c */
bool jvst_proto_init ( JackVST* jvst );
bool jvst_proto_close ( JackVST* jvst );

/* gtk.c */
#ifndef NO_GTK
extern void gtk_gui_init (int* argc, char** argv[]);
extern int gtk_gui_start (JackVST * jvst);
extern void gtk_gui_quit();
#endif

/* lash.c */
#ifdef HAVE_LASH
extern void jvst_lash_init(JackVST *jvst, int* argc, char** argv[]);
#endif

GMainLoop* glib_main_loop;
volatile JackVST *jvst_first = NULL;

void jvst_quit(JackVST* jvst) {
#ifdef NO_GTK
	g_main_loop_quit(glib_main_loop);
#else
	if (jvst->with_editor == WITH_EDITOR_NO) {
		g_main_loop_quit(glib_main_loop);
	} else {
		gtk_gui_quit();
	}
#endif
}

static void signal_handler (int signum) {
	JackVST *jvst = (JackVST*) jvst_first;

	switch(signum) {
	case SIGINT:
		puts("Caught signal to terminate (SIGINT)");
		g_idle_add( (GSourceFunc) jvst_quit, jvst);
		break;
	case SIGUSR1:
		puts("Caught signal to save state (SIGUSR1)");
		jvst_save_state(jvst, jvst->default_state_file);
		break;
	case SIGUSR2:
		puts("Caught signal to open editor (SIGUSR2)");
		g_idle_add( (GSourceFunc) fst_run_editor, jvst->fst);
		break;
	}
}

static bool idle ( JackVST* jvst ) {
	if ( ! jvst_idle ( jvst, APPNAME_ARCH ) ) {
		jvst_quit(jvst);
		return FALSE;
	}
	return TRUE;
}

#ifdef NO_GTK
static void edit_close_handler ( void* arg ) {
	JackVST* jvst = (JackVST*) arg;
	g_idle_add( (GSourceFunc) jvst_quit, jvst);
}
#endif

static void cmdline2arg(int *argc, char ***pargv, LPSTR cmdline) {
	LPWSTR* szArgList = CommandLineToArgvW(GetCommandLineW(), argc);
	if (!szArgList) {
		fputs("Unable to parse command line", stderr);
		*argc = -1;
		return;
	}

	char** argv = malloc(*argc * sizeof(char*));
	unsigned short i;
	for (i=0; i < *argc; ++i) {
		int nsize = WideCharToMultiByte(CP_UNIXCP, 0, szArgList[i], -1, NULL, 0, NULL, NULL);
		
		argv[i] = malloc( nsize );
		WideCharToMultiByte(CP_UNIXCP, 0, szArgList[i], -1, (LPSTR) argv[i], nsize, NULL, NULL);
	}
	LocalFree(szArgList);
	argv[0] = (char*) APPNAME_ARCH; // Force APP name
	*pargv = argv;
}

static void usage(char* appname) {
	const char* format = "%-20s%s\n";

	fprintf(stderr, "\nUsage: %s [ options ] <plugin>\n", appname);
	fprintf(stderr, "  or\n");
	fprintf(stderr, "Usage: %s -L [ -d <xml_db_info> ]\n", appname);
	fprintf(stderr, "  or\n");
	fprintf(stderr, "Usage: %s -g [ -d <xml_db_info> ] <path_for_add_to_db>\n", appname);
	fprintf(stderr, "  or\n");
	fprintf(stderr, "Usage: %s -s <FPS state file>\n\n", appname);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, format, "-g", "Create/Update XML info DB.");
	fprintf(stderr, format, "-L", "List plugins from XML info DB.");
	fprintf(stderr, format, "-d xml_db_path", "Custom path to XML DB");
	fprintf(stderr, format, "-b", "Start in bypass mode");
	fprintf(stderr, format, "-n", "Disable Editor and GTK GUI");
	fprintf(stderr, format, "-N", "Notify changes by SysEx");
#ifndef NO_GTK
	fprintf(stderr, format, "-e", "Hide Editor");
#endif
	fprintf(stderr, format, "-s <state_file>", "Load <state_file>");
	fprintf(stderr, format, "-S <port>", "Start CTRL server on port <port>. Use 0 for random.");
	fprintf(stderr, format, "-c <client_name>", "Jack Client name");
	fprintf(stderr, format, "-A", "Set plugin port names as aliases");
	fprintf(stderr, format, "-k channel", "MIDI Channel (0: all, 17: none)");
	fprintf(stderr, format, "-i num_in", "Jack number In ports");
	fprintf(stderr, format, "-j <connect_to>", "Connect Audio Out to <connect_to>");
	fprintf(stderr, format, "-l", "save state to state_file on SIGUSR1 (require -s)");
	fprintf(stderr, format, "-m mode_midi_cc", "Bypass/Resume MIDI CC (default: 122)");
	fprintf(stderr, format, "-p", "Connect MIDI In port to all physical");
	fprintf(stderr, format, "-P", "Self MIDI Program Change handling");
	fprintf(stderr, format, "-o num_out", "Jack number Out ports");
	fprintf(stderr, format, "-T", "Separate threads");
	fprintf(stderr, format, "-u uuid", "JackSession UUID");
	fprintf(stderr, format, "-U SysExID", "SysEx ID (1-127). 0 is default (do not use it)");
	fprintf(stderr, format, "-V", "Disable Volume control / filtering CC7 messages");
}

struct SepThread {
	JackVST* jvst;
	const char* plug_spec;
	sem_t sem;
	bool loaded;
	bool state_can_fail;
};

static DWORD WINAPI
sep_thread ( LPVOID arg ) {
	struct SepThread* st = (struct SepThread*) arg;

	fst_set_thread_priority ( "SepThread", ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL );

	bool loaded = st->loaded = jvst_load ( st->jvst, st->plug_spec, true, st->state_can_fail );

	sem_post( &st->sem );

	if ( loaded ) fst_event_loop();

	return 0;
}

bool jvst_load_sep_th (JackVST* jvst, const char* plug_spec, bool want_state_and_amc, bool state_can_fail) {

	struct SepThread st;
	st.jvst = jvst;
	st.plug_spec = plug_spec;
	st.state_can_fail = state_can_fail;
	sem_init( &st.sem, 0, 0 );
	CreateThread( NULL, 0, sep_thread, &st, 0, 0 );

	sem_wait ( &st.sem );
	sem_destroy ( &st.sem );

	return st.loaded;
}

int WINAPI
WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR cmdline, int cmdshow) {
	int		argc = -1;
	char**		argv = NULL;
	int32_t		opt_maxIns = -1;
	int32_t		opt_maxOuts = -1;
	bool		opt_generate_dbinfo = false;
	bool		opt_list_plugins = false;
	bool		sigusr1_save_state = false;
	bool		want_midi_physical = false;
	bool		separate_threads = false;
	bool		serv = false;
	const char*	connect_to = NULL;
	const char*	custom_path = NULL;

	printf("FSTHost Version: %s (%s)\n", VERSION, ARCH "bit");

	JackVST*	jvst = jvst_new();
	jvst_first = jvst;

	// Handle FSTHOST_GUI environment
	char* menv = getenv("FSTHOST_GUI");
	if ( menv ) jvst->with_editor = strtol(menv, NULL, 10);

	// Handle FSTHOST_THREADS environment
	menv = getenv("FSTHOST_THREADS");
	if ( menv ) separate_threads = true;

        // Parse command line options
	cmdline2arg(&argc, &argv, cmdline);
	short c;
	while ( (c = getopt (argc, argv, "Abd:egs:S:c:k:i:j:lLnNm:pPo:Tu:U:V")) != -1) {
		switch (c) {
			case 'A': jvst->want_port_aliases = true; break;
			case 'b': jvst->bypassed = true; break;
			case 'd': jvst->dbinfo_file = optarg; break;
			case 'e': jvst->with_editor = WITH_EDITOR_HIDE; break;
			case 'g': opt_generate_dbinfo = true; break;
			case 'L': opt_list_plugins = true; break;
			case 's': jvst->default_state_file = optarg; break;
			case 'S': serv=true; jvst->ctrl_port_number = strtol(optarg,NULL,10); break;
			case 'c': jvst->client_name = optarg; break;
			case 'k': midi_filter_one_channel_set(&jvst->channel, strtol(optarg, NULL, 10)); break;
			case 'i': opt_maxIns = strtol(optarg, NULL, 10); break;
			case 'j': connect_to = optarg; break;
			case 'l': sigusr1_save_state = true; break;
			case 'p': want_midi_physical = true; break;
			case 'P': jvst->midi_pc = MIDI_PC_SELF; break; /* used but not enabled */
			case 'o': opt_maxOuts = strtol(optarg, NULL, 10); break;
			case 'n': jvst->with_editor = WITH_EDITOR_NO; break;
			case 'N': jvst->sysex_want_notify = true; break;
			case 'm': jvst->want_state_cc = strtol(optarg, NULL, 10); break;
			case 'T': separate_threads = true;
			case 'u': jvst->uuid = optarg; break;
			case 'U': jvst_sysex_set_uuid( jvst, strtol(optarg, NULL, 10) ); break;
			case 'V': jvst->volume = -1; break;
			default: usage (argv[0]); return 1;
		}
	}

	/* If use want to list plugins then abandon other tasks */
	if (opt_list_plugins) return fst_info_list ( jvst->dbinfo_file );

	/* We have more arguments than getops options */
	if (optind < argc) custom_path = argv[optind];

	/* If NULL then Generate using VST_PATH */
	if (opt_generate_dbinfo) {
		int ret = fst_info_update ( jvst->dbinfo_file, custom_path );
		if (ret > 0) usage ( argv[0] );
		return ret;
	}

	/* Load plugin - in this thread or dedicated */
	bool loaded;
	if ( separate_threads ) {
		loaded = jvst_load_sep_th ( jvst, custom_path, true, sigusr1_save_state );
	} else {
		loaded = jvst_load ( jvst, custom_path, true, sigusr1_save_state );
	}

	/* Well .. Are we loaded plugin ? */
	if (! loaded) {
		usage ( argv[0] );
		return 1;
	}

	// Set Thread policy - usefull only with WineRT/LPA patch
	//fst_set_thread_priority ( "Main", REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL );
	fst_set_thread_priority ( "Main", ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL );

	// Init Jack
	if ( ! jvst_init ( jvst, opt_maxIns, opt_maxOuts ) )
		return 1;

	/* Socket stuff */
	if ( serv && ! jvst_proto_init(jvst) ) {
		jvst_close(jvst);
		return 1;
	}

	// Handling signals
	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = &signal_handler;
	sigaction(SIGINT, &sa, NULL); // SIGINT for clean quit
	sigaction(SIGUSR2, &sa, NULL);// SIGUSR2 open editor

	// SIGUSR1 for save state ( ladish support )
	if (sigusr1_save_state && jvst->default_state_file)
		sigaction(SIGUSR1, &sa, NULL);

#ifdef HAVE_LASH
	jvst_lash_init(jvst, &argc, &argv);
#endif

	// Activate plugin
	if (! jvst->bypassed) fst_call ( jvst->fst, RESUME );

	puts("Jack Activate");
	jack_activate(jvst->client);

	// Init Glib main event loop
	glib_main_loop = g_main_loop_new(NULL, FALSE);
	g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, 750, (GSourceFunc) idle, jvst, NULL);

	// Autoconnect AUDIO on start
	if (connect_to) jvst_connect_audio(jvst, connect_to);

	// Autoconnect MIDI on start
	if (want_midi_physical) jvst_connect_midi_to_physical(jvst);

	// Add FST event callback to Gblib main loop
	if ( ! separate_threads )
		g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, 100, (GSourceFunc) fst_event_callback, NULL, NULL);

#ifdef NO_GTK
	// Create GTK or GlibMain thread
	if (jvst->with_editor != WITH_EDITOR_NO) {
		puts("Run Editor");
		fst_set_window_close_callback( jvst->fst, edit_close_handler, jvst );
		fst_run_editor (jvst->fst, false);
	} else {
		puts("GUI Disabled - start GlibMainLoop");
	}
	g_main_loop_run ( glib_main_loop );
#else
	// Create GTK or GlibMain thread
	if (jvst->with_editor != WITH_EDITOR_NO) {
		puts( "Start GUI" );
		gtk_gui_init(&argc, &argv);
		gtk_gui_start(jvst);
	} else {
		puts("GUI Disabled - start GlibMainLoop");
		g_main_loop_run ( glib_main_loop );
	}
#endif

	puts("Jack Deactivate");
	jack_deactivate ( jvst->client );

	/* Close CTRL socket */
	jvst_proto_close ( jvst );

	jvst_close(jvst);

	puts("Game Over");

	return 0;
}
