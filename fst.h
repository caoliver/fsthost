#ifndef __fst_fst_h__
#define __fst_fst_h__

#include <stdio.h>
#include <setjmp.h>
#include <pthread.h>
#include <stdbool.h>

#include "vestige/aeffectx.h"
#include "amc.h"

/**
 * Display FST error message.
 *
 * Set via fst_set_error_function(), otherwise a FST-provided
 * default will print @a msg (plus a newline) to stderr.
 *
 * @param msg error message text (no newline at end).
 */
extern void (*fst_error_callback)(const char *msg);

/**
 * Set the @ref fst_error_callback for error message display.
 *
 * The FST library provides two built-in callbacks for this purpose:
 * default_fst_error_callback() and silent_fst_error_callback().
 */
void fst_set_error_function (void (*func)(const char *));

typedef struct _FST FST;
typedef struct _FSTHandle FSTHandle;
//typedef struct _FSTInfo FSTInfo;
typedef struct _FXHeader FXHeader;

/*
struct _FSTInfo 
{
    char *name;
    int UniqueID;
    char *Category;
    
    int numInputs;
    int numOutputs;
    int numParams;

    int wantMidi;
    int wantEvents;
    int hasEditor;
    int canProcessReplacing; // what do we need this for ?

    // i think we should save the parameter Info Stuff soon.
    // struct VstParameterInfo *infos;
    char **ParamNames;
    char **ParamLabels;
};
*/

typedef AEffect* (VSTCALLBACK *main_entry_t)(audioMasterCallback);

struct _FSTHandle
{
    void*		dll;
    char*		name;
    char*		path;
    main_entry_t	main_entry;
};

struct FSTDispatcher {
	int	opcode;
	int	index;
	int	val;
	void*	ptr;
	float	opt;
	int	retval;
};

enum EventCall {
	RESET,
	CLOSE,
	SUSPEND,
	RESUME,
	DISPATCHER,
	EDITOR_OPEN,
	EDITOR_CLOSE,
	PROGRAM_CHANGE
};

struct _FST 
{
	AEffect*		plugin;
	FSTHandle*		handle;
	AMC*			amc;
	struct _FST*		next;

	char*			name;
	enum EventCall		event_call;
	struct FSTDispatcher*	dispatcher;

	bool			editor_popup;
	void*			window; /* win32 HWND */
	void*			xid;    /* X11 XWindow */
	int			width;
	int			height;
	bool			wantResize;
	bool			wantIdle;

	bool			program_changed;
	short			want_program;
	short			current_program;

	unsigned short		vst_version;
	bool			isSynth;
	bool			canReceiveVstEvents;
	bool			canReceiveVstMidiEvent;
	bool			canSendVstEvents;
	bool			canSendVstMidiEvent;

	/* This is needed only audioMasterGetTime - but we don't know how long plugin want to use it */
	struct VstTimeInfo	timeInfo;

	pthread_mutex_t		lock;
	pthread_mutex_t		event_call_lock;
	pthread_cond_t		event_called;
};

enum FxFileType {
	FXBANK		= 0,
	FXPROGRAM	= 1
};

struct _FXHeader {
        unsigned int chunkMagic;
        unsigned int byteSize;
        unsigned int fxMagic;
        unsigned int version;
        unsigned int fxID;
        unsigned int fxVersion;
        unsigned int numPrograms;
};

void fst_error (const char *fmt, ...);

FSTHandle* fst_load (const char * path );
bool fst_unload (FSTHandle*);
FST* fst_open (FSTHandle*);
FST* fst_load_open (const char* path);
void fst_close (FST*);

void fst_event_loop();
bool fst_event_callback();

void fst_call (FST *fst, enum EventCall type);
int  fst_call_dispatcher (FST *fst, int opcode, int index, int val, void *ptr, float opt );
void fst_program_change (FST *fst, short want_program);
bool fst_get_program_name (FST *fst, short program, char* name, size_t size);
bool fst_set_program_name (FST *fst, const char* name);

bool fst_run_editor (FST*, bool popup);
bool fst_show_editor (FST *fst);

/* Support for FXB/FXP files (fxb.c) */
int fst_load_fxfile ( FST *fst, const char *filename );
int fst_save_fxfile (FST * fst, const char * filename, enum FxFileType fileType);

/* Support for XML Database (info.c) */
int fst_info_update(const char *dbpath, const char *fst_path);

#endif /* __fst_fst_h__ */
