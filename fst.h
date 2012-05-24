#ifndef __fst_fst_h__
#define __fst_fst_h__

#include <setjmp.h>
#include <signal.h>
#include <pthread.h>

#include <windows.h>
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

#include <vestige/aeffectx.h>

typedef struct _FST FST;
typedef struct _FSTHandle FSTHandle;
typedef struct _FSTInfo FSTInfo;
typedef struct _FXHeader FXHeader;

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

typedef struct AEffect * (*main_entry_t)(audioMasterCallback);

struct _FSTHandle
{
    void*    dll;
    char*    name;
    char*    path; /* ptr returned from strdup() etc. */
    //struct AEffect* (*main_entry)(audioMasterCallback);
    main_entry_t main_entry;

    int plugincnt;
};

enum EventCall {
	RESET,
	DISPATCHER,
	EDITOR_OPEN,
	EDITOR_CLOSE,
	PROGRAM_CHANGE
};

struct _FST 
{
    struct	AEffect*    plugin;
    void*       window; /* win32 HWND */
    int         xid;    /* X11 XWindow */
    FSTHandle*  handle;
    int 	width;
    int 	height;
    int         wantIdle;

    enum EventCall event_call;

    int		want_program;
    int         current_program;
    float      *want_params;
    float      *set_params;

    int            midi_map[128];
    volatile int   midi_learn;
    volatile int   midi_learn_CC;
    volatile int   midi_learn_PARAM;

    int         dispatcher_opcode;
    int         dispatcher_index;
    int         dispatcher_val;
    void *	dispatcher_ptr;
    float	dispatcher_opt;
    int		dispatcher_retval;

    struct _FST* next;
    pthread_mutex_t lock;
    pthread_mutex_t event_call_lock;
    pthread_cond_t  program_change;
    pthread_cond_t  window_status_change;
    pthread_cond_t  plugin_dispatcher_called;
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

#ifdef __cplusplus
extern "C" {
#endif

extern int        start_gui_event_loop (HMODULE hInst);

extern FSTHandle* fst_load (const char * );
extern int        fst_unload (FSTHandle*);

extern FST*       fst_instantiate (FSTHandle*, audioMasterCallback amc, void* userptr);
extern void       fst_close (FST*);

extern int   fst_run_editor (FST*);
extern void  fst_destroy_editor (FST*);

extern FSTInfo *fst_get_info (char *dllpathname);
extern void fst_free_info (FSTInfo *info);
extern void fst_event_loop_remove_plugin (FST* fst);
extern int fst_call_dispatcher(FST *fst, int opcode, int index, int val, void *ptr, float opt );

/**
 * Load a plugin state from a file.
 */
extern int fst_load_fps (FST * fst, char * filename);
extern int fst_load_fxfile (FST * fst, char * filename);

/**
 * Save a plugin state to a file.
 */
extern int fst_save_state (FST * fst, char * filename);

#ifdef __cplusplus
}
#endif

#endif /* __fst_fst_h__ */
