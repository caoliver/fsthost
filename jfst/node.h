#ifndef __node_h__
#define __node_h__

#include "jfst.h"

typedef struct _JFST_NODE {
	struct _JFST_NODE* next;
	JFST* jfst;
} JFST_NODE;

JFST_NODE* jfst_node_get_first();
JFST_NODE* jfst_node_new( const char* appname );
void jfst_node_free_all();

#endif /* __node_h__ */
