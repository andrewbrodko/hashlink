/*
 * Copyright (C)2015-2019 Haxe Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <hl.h>
#include <hlmodule.h>

#define MAX_STACK_SIZE (8 << 20)
#define MAX_STACK_COUNT 2048

HL_API double hl_sys_time( void );
HL_API void *hl_gc_threads_info( void );
HL_API void hl_setup_profiler( void *, void * );
int hl_module_capture_stack_range( void *stack_top, void **stack_ptr, void **out, int size );
uchar *hl_module_resolve_symbol_full( void *addr, uchar *out, int *outSize, int **r_debug_addr );

typedef struct {
	int count;
	bool stopping_world;
	hl_thread_info **threads;
} hl_gc_threads;

typedef struct _thread_handle thread_handle;
typedef struct _profile_data profile_data;

struct _thread_handle {
	int tid;
#	ifdef HL_WIN_DESKTOP
	HANDLE h;
#	endif
	hl_thread_info *inf;
	thread_handle *next;
};

struct _profile_data {
	int currentPos;
	int dataSize;
	unsigned char *data;
	profile_data *next;
};

typedef struct {
	profile_data *r;
	int pos;
} profile_reader;

static struct {
	int sample_count;
	volatile int profiling_pause;
	volatile bool stopLoop;
	volatile bool waitLoop;
	thread_handle *handles;
	void **tmpMemory;
	void *stackOut[MAX_STACK_COUNT];
	profile_data *record;
	profile_data *first_record;
} data = {0};

static void *get_thread_stackptr( thread_handle *t, void **eip ) {
#ifdef HL_WIN_DESKTOP
	CONTEXT c;
	c.ContextFlags = CONTEXT_CONTROL;
	if( !GetThreadContext(t->h,&c) ) return NULL;
#	ifdef HL_64
	*eip = (void*)c.Rip;
	return (void*)c.Rsp;
#	else
	*eip = (void*)c.Eip;
	return (void*)c.Esp;
#	endif
#else
	return NULL;
#endif
}

static void thread_data_init( thread_handle *t ) {
#ifdef HL_WIN
	t->h = OpenThread(THREAD_ALL_ACCESS,FALSE, t->tid);
#endif
}

static void thread_data_free( thread_handle *t ) {
#ifdef HL_WIN
	CloseHandle(t->h);
#endif
}

static bool pause_thread( thread_handle *t, bool b ) {
#ifdef HL_WIN
	if( b )
		return (int)SuspendThread(t->h) >= 0;
	else {
		ResumeThread(t->h);
		return true;
	}
#else
	return false;
#endif
}

static void record_data( void *ptr, int size ) {
	profile_data *r = data.record;
	if( !r || r->currentPos + size > r->dataSize ) {
		r = malloc(sizeof(profile_data));
		r->currentPos = 0;
		r->dataSize = 1 << 20;
		r->data = malloc(r->dataSize);
		r->next = NULL;
		if( data.record )
			data.record->next = r;
		else
			data.first_record = r;
		data.record = r;
		fflush(stdout);
	}
	memcpy(r->data + r->currentPos, ptr, size);
	r->currentPos += size;
}

static void read_thread_data( thread_handle *t ) {
	if( !pause_thread(t,true) )
		return;
	void *eip;
	void *stack = get_thread_stackptr(t,&eip);
	if( !stack ) {
		pause_thread(t,false);
		return;
	}

	int size = (int)((unsigned char*)t->inf->stack_top - (unsigned char*)stack);
	if( size > MAX_STACK_SIZE-32 ) size = MAX_STACK_SIZE-32;
	memcpy(data.tmpMemory + 2,stack,size);
	pause_thread(t, false);
	data.tmpMemory[0] = eip;
	data.tmpMemory[1] = stack;
	size += sizeof(void*) * 2;

	int count = hl_module_capture_stack_range((char*)data.tmpMemory+size, (void**)data.tmpMemory, data.stackOut, MAX_STACK_COUNT);
	int eventId = count | 0x80000000;
	double time = hl_sys_time();
	record_data(&time,sizeof(double));
	record_data(&t->tid,sizeof(int));
	record_data(&eventId,sizeof(int));
	record_data(data.stackOut,sizeof(void*)*count);
}

static void hl_profile_loop( void *_ ) {
	double wait_time = 1. / data.sample_count;
	double next = hl_sys_time();
	data.tmpMemory = malloc(MAX_STACK_SIZE);
	while( !data.stopLoop ) {
		if( hl_sys_time() < next || data.profiling_pause ) {
			data.waitLoop = false;
			continue;
		}
		hl_gc_threads *threads = (hl_gc_threads*)hl_gc_threads_info();
		int i;
		thread_handle *prev = NULL;
		thread_handle *cur = data.handles;
		for(i=0;i<threads->count;i++) {
			hl_thread_info *t = threads->threads[i];
			if( !cur || cur->tid != t->thread_id ) {
				thread_handle *h = malloc(sizeof(thread_handle));
				h->tid = t->thread_id;
				h->inf = t;
				thread_data_init(h);
				h->next = cur;
				cur = h;
				if( prev == NULL ) data.handles = h; else prev->next = h;
			}
			read_thread_data(cur);
			prev = cur;
			cur = cur->next;
		}
		if( prev ) prev->next = NULL; else data.handles = NULL;
		while( cur != NULL ) {
			thread_data_free(cur);
			free(cur);
			cur = cur->next;
		}
		next += wait_time;
	}
}

static void profile_event( int code, vbyte *data, int dataLen );

void hl_profile_start( int sample_count ) {
#	if defined(HL_THREADS) && defined(HL_WIN_DESKTOP)
	data.sample_count = sample_count;
	hl_thread_start(hl_profile_loop,NULL,false);
	hl_setup_profiler(profile_event,hl_profile_end);
#	endif
}

static bool read_profile_data( profile_reader *r, void *ptr, int size ) {
	while( size ) {
		if( r->r == NULL ) return false;
		int bytes = r->r->currentPos - r->pos;
		if( bytes > size ) bytes = size;
		memcpy(ptr, r->r->data + r->pos, bytes);
		size -= bytes;
		r->pos += bytes;
		if( r->pos == r->r->currentPos ) {
			r->r = r->r->next;
			r->pos = 0;
		}
	}
	return true;
}

static void profile_dump() {
	if( !data.first_record ) return;
	
	data.profiling_pause++;
	printf("Writing profiling data...\n");
	fflush(stdout);

	FILE *f = fopen("hlprofile.dump","wb");
	int version = HL_VERSION;
	fwrite("PROF",1,4,f);
	fwrite(&version,1,4,f);
	fwrite(&data.sample_count,1,4,f);
	profile_reader r;
	r.r = data.first_record;
	r.pos = 0;
	int samples = 0;
	while( true ) {
		double time;
		int i, tid, eventId;
		if( !read_profile_data(&r,&time, sizeof(double)) ) break;
		read_profile_data(&r,&tid,sizeof(int));
		read_profile_data(&r,&eventId,sizeof(int));
		fwrite(&time,1,8,f);
		fwrite(&tid,1,4,f);
		fwrite(&eventId,1,4,f);
		if( eventId < 0 ) {
			int count = eventId & 0x7FFFFFFF;
			read_profile_data(&r,data.stackOut,sizeof(void*)*count);
			for(i=0;i<count;i++) {
				uchar outStr[256];
				int outSize = 256;
				int *debug_addr = NULL;
				if( hl_module_resolve_symbol_full(data.stackOut[i],outStr,&outSize,&debug_addr) == NULL ) {
					int bad = -1;
					fwrite(&bad,1,4,f);
				} else {
					fwrite(debug_addr,1,8,f);
					if( (debug_addr[0] & 0x80000000) == 0 ) {
						debug_addr[0] |= 0x80000000;
						fwrite(&outSize,1,4,f);
						fwrite(outStr,1,outSize*sizeof(uchar),f);
					}
				}
			}
			samples++;
		} else {
			int size;
			read_profile_data(&r,&size, sizeof(int));
			fwrite(&size,1,4,f);
			while( size ) {
				int k = size > MAX_STACK_SIZE ? MAX_STACK_SIZE : size;
				read_profile_data(&r,data.tmpMemory,k);
				fwrite(data.tmpMemory,1,k,f);
				size -= k;
			}
		}
	}
	fclose(f);
	printf("%d profile samples saved\n", samples);
	data.profiling_pause--;
}

void hl_profile_end() {
	profile_dump();
	data.stopLoop = true;
}

static void profile_event( int code, vbyte *ptr, int dataLen ) {
	switch( code ) {
	case -1:
		data.profiling_pause++;
		break;
	case -2:
		data.profiling_pause--;
		break;
	case -3:
		profile_dump();
		break;
	default:
		if( code < 0 ) return;
		data.profiling_pause++;
		data.waitLoop = true;
		while( data.waitLoop ) {}
		double time = hl_sys_time();
		record_data(&time,sizeof(double));
		record_data(&hl_get_thread()->thread_id,sizeof(int));
		record_data(&code,sizeof(int));
		record_data(&dataLen,sizeof(int));
		record_data(ptr,dataLen);
		data.profiling_pause--;
		break;
	}
}

