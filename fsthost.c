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
#include <semaphore.h>
#include <signal.h>
#include <windows.h>

#include "log/log.h"
#include "jfst/jfst.h"
#include "serv/serv.h"
#include "xmldb/info.h"

#ifndef NO_GTK
#include "gtk/gjfst.h"
#endif

#define VERSION "1.5.5"
#ifdef __x86_64__
#define ARCH "64"
#else
#define ARCH "32"
#endif
#define APPNAME "fsthost"
#define APPNAME_ARCH APPNAME ARCH

#define STR_NO_CONNECT "!"

/* lash.c */
#ifdef HAVE_LASH
extern void jfst_lash_init(JFST *jfst, int* argc, char** argv[]);
extern bool jfst_lash_idle(JFST *jfst);
#endif

volatile JFST *jfst_first = NULL;
volatile bool quit = false;
volatile bool open_editor = false;
volatile bool separate_threads = false;
volatile bool sigusr1_save_state = false;

void jfst_quit(JFST* jfst) {
	quit = true;

#ifndef NO_GTK
	if (jfst->with_editor != WITH_EDITOR_NO)
		gjfst_quit();
#endif
}

static void signal_handler (int signum) {
	JFST *jfst = (JFST*) jfst_first;

	switch(signum) {
	case SIGINT:
		log_info("Caught signal to terminate (SIGINT)");
		jfst_quit(jfst);
		break;
	case SIGTERM:
		log_info("Caught signal to terminate (SIGTERM)");
		jfst_quit(jfst);
		break;
	case SIGUSR1:
		log_info("Caught signal to save state (SIGUSR1)");
		if (sigusr1_save_state && jfst->default_state_file) {
			jfst_save_state(jfst, jfst->default_state_file);
		} else {
			log_info ( "SIGUSR1 - skipped ( no \"-l\" option )" );
		}
		break;
	case SIGUSR2:
		log_info("Caught signal to open editor (SIGUSR2)");
		open_editor = true;
		break;
	}
}

bool fsthost_idle () {
	if ( ! jfst_first ) return true;
	JFST* jfst = (JFST*) jfst_first;

	Changes change = jfst_idle ( jfst );
	if ( change & CHANGE_QUIT ) {
		jfst_quit(jfst);
		return false;
	}

	serv_poll( change );

	if ( open_editor ) {
		open_editor = false;
		fst_run_editor( jfst->fst, false );
	}

#ifdef HAVE_LASH
	if ( ! jfst_lash_idle(jfst) ) {
		jfst_quit(jfst);
		return false;
	}
#endif

	if ( separate_threads ) return true;

	if ( ! fst_event_callback() ) {
		jfst_quit(jfst);
		return false;
	}

	return true;
}

#ifdef NO_GTK
static void edit_close_handler ( void* arg ) {
	JFST* jfst = (JFST*) arg;
	jfst_quit(jfst);
}
#endif

static void cmdline2arg(int *argc, char ***pargv, LPSTR cmdline) {
	LPWSTR* szArgList = CommandLineToArgvW(GetCommandLineW(), argc);
	if (!szArgList) {
		log_error("Unable to parse command line");
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
	fprintf(stderr, format, "-S <port>", "Start CTRL server on port <port>. Use 0 for random.");
	fprintf(stderr, format, "-s <state_file>", "Load <state_file>");
	fprintf(stderr, format, "-c <client_name>", "Jack Client name");
	fprintf(stderr, format, "-A", "Set plugin port names as aliases");
	fprintf(stderr, format, "-k channel", "MIDI Channel (0: all, 17: none)");
	fprintf(stderr, format, "-i num_in", "Jack number In ports");
	fprintf(stderr, format, "-j <connect_to>", "Connect Audio Out to <connect_to>. " STR_NO_CONNECT " for no connect");
	fprintf(stderr, format, "-l", "save state to state_file on SIGUSR1 (require -s)");
	fprintf(stderr, format, "-m mode_midi_cc", "Bypass/Resume MIDI CC (default: 122)");
	fprintf(stderr, format, "-p", "Disable connecting MIDI In port to all physical");
	fprintf(stderr, format, "-P", "Self MIDI Program Change handling");
	fprintf(stderr, format, "-o num_out", "Jack number Out ports");
	fprintf(stderr, format, "-T", "Separate threads");
	fprintf(stderr, format, "-u uuid", "JackSession UUID");
	fprintf(stderr, format, "-U SysExID", "SysEx ID (1-127). 0 is default (do not use it)");
	fprintf(stderr, format, "-v", "Verbose");
	fprintf(stderr, format, "-V", "Disable Volume control / filtering CC7 messages");
}

struct SepThread {
	JFST* jfst;
	const char* plug_spec;
	sem_t sem;
	bool loaded;
	bool state_can_fail;
};

static DWORD WINAPI
sep_thread ( LPVOID arg ) {
	struct SepThread* st = (struct SepThread*) arg;

	fst_set_thread_priority ( "SepThread", ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL );

	bool loaded = st->loaded = jfst_load ( st->jfst, st->plug_spec, true, st->state_can_fail );

	sem_post( &st->sem );

	if ( ! loaded ) return 0;

	while ( fst_event_callback() ) {
		usleep ( 30000 );
	}

	jfst_quit(st->jfst);

	return 0;
}

bool jfst_load_sep_th (JFST* jfst, const char* plug_spec, bool want_state_and_amc, bool state_can_fail) {
	struct SepThread st;
	st.jfst = jfst;
	st.plug_spec = plug_spec;
	st.state_can_fail = state_can_fail;
	sem_init( &st.sem, 0, 0 );
	CreateThread( NULL, 0, sep_thread, &st, 0, 0 );

	sem_wait ( &st.sem );
	sem_destroy ( &st.sem );

	return st.loaded;
}

static inline void
main_loop( JFST* jfst ) {
	log_info("GUI Disabled - start MainLoop");
	while ( ! quit ) {
		if ( ! fsthost_idle() )
			break;

		usleep ( 100000 );
	}
}

int WINAPI
WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR cmdline, int cmdshow) {
	int		argc = -1;
	char**		argv = NULL;
	int32_t		opt_maxIns = -1;
	int32_t		opt_maxOuts = -1;
	bool		opt_generate_dbinfo = false;
	bool		opt_list_plugins = false;
	const char*	connect_to = NULL;
	const char*	custom_path = NULL;
	bool		serv = false;
	LogLevel	log_level = LOG_INFO;

	JFST*	jfst = jfst_new( APPNAME_ARCH );
	jfst_first = jfst;

	// Handle FSTHOST_GUI environment
	char* menv = getenv("FSTHOST_GUI");
	if ( menv ) jfst->with_editor = strtol(menv, NULL, 10);

	// Handle FSTHOST_THREADS environment
	menv = getenv("FSTHOST_THREADS");
	if ( menv ) separate_threads = true;

        // Parse command line options
	cmdline2arg(&argc, &argv, cmdline);
	short c;
	while ( (c = getopt (argc, argv, "Abd:egs:S:c:k:i:j:lLnNm:pPo:Tu:U:vV")) != -1) {
		switch (c) {
			case 'A': jfst->want_port_aliases = true; break;
			case 'b': jfst->bypassed = true; break;
			case 'd': jfst->dbinfo_file = optarg; break;
			case 'e': jfst->with_editor = WITH_EDITOR_HIDE; break;
			case 'g': opt_generate_dbinfo = true; break;
			case 'L': opt_list_plugins = true; break;
			case 's': jfst->default_state_file = optarg; break;
			case 'S': serv=true; jfst->ctrl_port_number = strtol(optarg,NULL,10); break;
			case 'c': jfst->client_name = optarg; break;
			case 'k': midi_filter_one_channel_set(&jfst->channel, strtol(optarg, NULL, 10)); break;
			case 'i': opt_maxIns = strtol(optarg, NULL, 10); break;
			case 'j': connect_to = optarg; break;
			case 'l': sigusr1_save_state = true; break;
			case 'p': jfst->want_auto_midi_physical = false; break;
			case 'P': jfst->midi_pc = MIDI_PC_SELF; break; /* used but not enabled */
			case 'o': opt_maxOuts = strtol(optarg, NULL, 10); break;
			case 'n': jfst->with_editor = WITH_EDITOR_NO; break;
			case 'N': jfst->sysex_want_notify = true; break;
			case 'm': jfst->want_state_cc = strtol(optarg, NULL, 10); break;
			case 'T': separate_threads = true;
			case 'u': jfst->uuid = optarg; break;
			case 'U': jfst_sysex_set_uuid( jfst, strtol(optarg, NULL, 10) ); break;
			case 'v': log_level = LOG_DEBUG; break;
			case 'V': jfst->volume = -1; break;
			default: usage (argv[0]); return 1;
		}
	}

	log_init ( log_level, NULL, NULL );
	log_info( "FSTHost Version: %s (%s)", VERSION, ARCH "bit" );

	/* Under Jack Session Manager Control "-p -j !" is forced */
	if ( getenv("SESSION_DIR") ) {
		jfst->want_auto_midi_physical = false;
		connect_to = STR_NO_CONNECT;
	}

	/* If use want to list plugins then abandon other tasks */
	if (opt_list_plugins) return fst_info_list ( jfst->dbinfo_file, ARCH );

	/* We have more arguments than getops options */
	if (optind < argc) custom_path = argv[optind];

	/* If NULL then Generate using VST_PATH */
	if (opt_generate_dbinfo) {
		int ret = fst_info_update ( jfst->dbinfo_file, custom_path );
		if (ret > 0) usage ( argv[0] );
		return ret;
	}

	/* Load plugin - in this thread or dedicated */
	bool loaded;
	if ( separate_threads ) {
		loaded = jfst_load_sep_th ( jfst, custom_path, true, sigusr1_save_state );
	} else {
		loaded = jfst_load ( jfst, custom_path, true, sigusr1_save_state );
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
	if ( ! jfst_init ( jfst, opt_maxIns, opt_maxOuts ) )
		return 1;

	/* Socket stuff */
	if ( serv && ! jfst_proto_init(jfst) ) {
		jfst_close(jfst);
		return 1;
	}

	// Handling signals
	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = &signal_handler;
	sigaction(SIGINT, &sa, NULL); // SIGINT for clean quit
	sigaction(SIGTERM, &sa, NULL); // SIGTERM for clean quit
	sigaction(SIGUSR2, &sa, NULL);// SIGUSR2 open editor
	sigaction(SIGUSR1, &sa, NULL); // SIGUSR1 for save state ( ladish support )

#ifdef HAVE_LASH
	jfst_lash_init(jfst, &argc, &argv);
#endif
	// Activate plugin
	if (! jfst->bypassed) fst_call ( jfst->fst, RESUME );

	log_info( "Jack Activate" );
	jack_activate(jfst->client);

	// Autoconnect AUDIO on start
	jfst_connect_audio(jfst, connect_to);

#ifdef NO_GTK
	// Create GTK or GlibMain thread
	if (jfst->with_editor != WITH_EDITOR_NO) {
		log_info( "Run Editor" );
		fst_set_window_close_callback( jfst->fst, edit_close_handler, jfst );
		fst_run_editor (jfst->fst, false);
	}
	main_loop( jfst );
#else
	// Create GTK or GlibMain thread
	if (jfst->with_editor != WITH_EDITOR_NO) {
		log_info( "Start GUI" );
		gjfst_init(&argc, &argv);
		gjfst_add ( jfst );
		gjfst_start();
		gjfst_free ( jfst );
	} else {
		main_loop( jfst );
	}
#endif
	log_info( "Jack Deactivate" );
	jack_deactivate ( jfst->client );

	/* Close CTRL socket */
	if ( serv ) {
		log_info( "Stopping JFST control" );
		serv_close();
	}

	jfst_close(jfst);

	log_info( "Game Over" );

	return 0;
}
