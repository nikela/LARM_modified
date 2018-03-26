#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#ifdef _PAPI_
#include <papi.h>
#endif
#ifdef _OPENMP
#include <omp.h>
#endif
#include <hwloc.h>
#include "sampling.h"
#include "list.h"

static unsigned         BYTES = 8;
static unsigned         FLOPS = 1;
static FILE *           output_file = NULL;
static hwloc_topology_t topology;
static unsigned         reduction_depth;
static list             samples;
static uint64_t *       bindings;

struct roofline_sample{
#ifdef _PAPI_
  int                    eventset;    /* Eventset of this sample used to read events */
  long long              values[4];   /* Values to be store on counter read */
#endif
  int                    last_thread; /* A counter atomically incremented to know when the last thread has to stop timer */  
  uint64_t               s_nano;      /* Timestamp in nanoseconds where the sampling started */
  uint64_t               e_nano;      /* Timestamp in nanoseconds where the sampling ended */  
  uint64_t               flops;       /* The amount of flops computed */
  uint64_t               bytes;       /* The amount of bytes of type transfered */
  hwloc_obj_t            location;    /* Where the thread is taking samples */
  int                    n_threads;   /* The number of threads counting on this sample */  
};

static char * roofline_cat_info(const char * info){
  size_t len = 2;
  char * env_info = getenv("LARM_INFO");
  char * ret = NULL;
  if(info != NULL) len += strlen(info);
  if(env_info != NULL) len += strlen(env_info);
  ret = malloc(len);
  memset(ret, 0 ,len);
  if(info != NULL && env_info != NULL)snprintf(ret, len, "%s_%s", info, env_info);
  else if(info != NULL)snprintf(ret, len, "%s", info);
  else if(env_info != NULL)snprintf(ret, len, "%s", env_info);
  return ret;
}

static int roofline_hwloc_obj_snprintf(const hwloc_obj_t obj, char * info_in, const size_t n){
  int nc;
  memset(info_in,0,n);
  /* Special case for MCDRAM */
  if(obj->type == HWLOC_OBJ_NUMANODE && obj->subtype != NULL && !strcmp(obj->subtype, "MCDRAM"))
    nc = snprintf(info_in, n, "%s", obj->subtype);
  else nc = hwloc_obj_type_snprintf(info_in, n, obj, 0);
  nc += snprintf(info_in+nc,n-nc,":%d",obj->logical_index);
  return nc;
}

#ifdef _PAPI_
static void
PAPI_handle_error(const int err)
{
  if(err!=0)
    fprintf(stderr,"PAPI error %d: ",err);
  switch(err){
  case PAPI_EINVAL:
    fprintf(stderr,"Invalid argument.\n");
    break;
  case PAPI_ENOINIT:
    fprintf(stderr,"PAPI library is not initialized.\n");
    break;
  case PAPI_ENOMEM:
    fprintf(stderr,"Insufficient memory.\n");
    break;
  case PAPI_EISRUN:
    fprintf(stderr,"Eventset is already_counting events.\n");
    break;
  case PAPI_ECNFLCT:
    fprintf(stderr,"This event cannot be counted simultaneously with another event in the monitor eventset.\n");
    break;
  case PAPI_ENOEVNT:
    fprintf(stderr,"This event is not available on the underlying hardware.\n");
    break;
  case PAPI_ESYS:
    fprintf(stderr, "A system or C library call failed inside PAPI, errno:%s\n",strerror(errno)); 
    break;
  case PAPI_ENOEVST:
    fprintf(stderr,"The EventSet specified does not exist.\n");
    break;
  case PAPI_ECMP:
    fprintf(stderr,"This component does not support the underlying hardware.\n");
    break;
  case PAPI_ENOCMP:
    fprintf(stderr,"Argument is not a valid component. PAPI_ENOCMP\n");
    break;
  case PAPI_EBUG:
    fprintf(stderr,"Internal error, please send mail to the developers.\n");
    break;
  default:
    perror("");
    break;
  }
}

#define PAPI_call_check(call,check_against, ...) do{	\
    int err = call;					\
    if(err!=check_against){				\
      fprintf(stderr, __VA_ARGS__);			\
      PAPI_handle_error(err);				\
      exit(EXIT_FAILURE);				\
    }							\
  } while(0)

void roofline_sampling_eventset_init(const hwloc_obj_t PU, int * eventset){
#ifdef _OPENMP
#pragma omp critical
    {
#endif /* _OPENMP */
      
      *eventset = PAPI_NULL;      
      PAPI_call_check(PAPI_create_eventset(eventset), PAPI_OK, "PAPI eventset initialization failed\n");
      /* assign eventset to a component */
      PAPI_call_check(PAPI_assign_eventset_component(*eventset, 0), PAPI_OK, "Failed to assign eventset to commponent: ");
      /* bind eventset to cpu */
      PAPI_option_t cpu_option;
      cpu_option.cpu.eventset=*eventset;
      cpu_option.cpu.cpu_num = PU->os_index;
      PAPI_call_check(PAPI_set_opt(PAPI_CPU_ATTACH,&cpu_option), PAPI_OK, "Failed to bind eventset to cpu: ");
      
      PAPI_call_check(PAPI_add_named_event(*eventset, "FP_ARITH:SCALAR_DOUBLE"), PAPI_OK, "Failed to add FP_ARITH:SCALAR_DOUBLE event\n");
      if(BYTES == 16)
	PAPI_call_check(PAPI_add_named_event(*eventset, "FP_ARITH:128B_PACKED_DOUBLE"), PAPI_OK, "Failed to add FP_ARITH:128B_PACKED_DOUBLE event\n");
      else if(BYTES == 32)
	PAPI_call_check(PAPI_add_named_event(*eventset, "FP_ARITH:256B_PACKED_DOUBLE"), PAPI_OK, "Failed to add FP_ARITH:256B_PACKED_DOUBLE event\n");
      PAPI_call_check(PAPI_add_named_event(*eventset, "MEM_UOPS_RETIRED:ALL_STORES"), PAPI_OK, "Failed to add MEM_UOPS_RETIRED:ALL_STORES event\n");
      PAPI_call_check(PAPI_add_named_event(*eventset, "MEM_UOPS_RETIRED:ALL_LOADS"), PAPI_OK, "Failed to add MEM_UOPS_RETIRED:ALL_LOADS event\n");

#ifdef _OPENMP
    }
#endif /* _OPENMP */
}

#endif /* _PAPI_ */

static void roofline_sample_reset(struct roofline_sample * s){
  s->s_nano = 0;
  s->e_nano = 0;  
  s->flops = 0;
  s->bytes = 0;
  s->n_threads = 0;
#ifdef _PAPI_
  PAPI_reset(s->eventset);
#endif /* _PAPI_ */
}

#define roofline_MIN(a,b) ((a)<(b)?(a):(b))
#define roofline_MAX(a,b) ((a)>(b)?(a):(b))

static struct roofline_sample * roofline_sample_accumulate(struct roofline_sample * out,
							   const struct roofline_sample * with){
#ifdef _OPENMP_
#pragma omp critical
  {
#endif
    /* Keep timing of the slowest */
    long out_time = out->e_nano - out->s_nano;
    long with_time = with->e_nano - with->s_nano;  
    out->s_nano     = out_time > with_time ? out->s_nano : with->s_nano;
    out->e_nano     = out_time > with_time ? out->e_nano : with->e_nano;  
    /* But accumulate bytes */
    out->bytes      += with->bytes;
    out->flops      += with->flops;
    out->n_threads  += with->n_threads;
    return out;
#ifdef _OPENMP_
#pragma
  }
#endif  
}

static struct roofline_sample * new_roofline_sample(const hwloc_obj_t PU){
  struct roofline_sample * s = malloc(sizeof(struct roofline_sample));
  if(s==NULL){perror("malloc"); return NULL;}
  s->s_nano = 0;
  s->e_nano = 0;  
  s->flops = 0;
  s->bytes = 0;
  s->location = PU;
  s->n_threads=0;
  s->last_thread = 0;
  while(s->location->depth>reduction_depth){s->location = s->location->parent;}
#ifdef _PAPI_
  roofline_sampling_eventset_init(PU, &(s->eventset));
#endif /* _PAPI_ */
  return s;
}

static void delete_roofline_sample(struct roofline_sample * s){
#ifdef _PAPI_
  PAPI_destroy_eventset(&(s->eventset));
#endif
  free(s);
}

static void roofline_sample_print(const struct roofline_sample * s, const char * info)
{
  char location[16];
  roofline_hwloc_obj_snprintf(s->location, location, sizeof(location));
  char * info_cat = roofline_cat_info(info);
  
  fprintf(output_file, "%16s %16lu %16lu %16lu %10d %10s %s\n",
  	  location,
  	  (s->e_nano-s->s_nano),
  	  s->bytes,
  	  s->flops,
  	  s->n_threads,
  	  "APP",
  	  info_cat);
}

static inline void roofline_print_header(){
  fprintf(output_file, "%16s %16s %16s %16s %10s %10s %s\n",
	  "Location", "Nanoseconds", "Bytes", "Flops", "n_threads", "type", "info");
}

static int roofline_reduction_depth(const enum roofline_location reduction_location){
  int depth = 0;
  switch(reduction_location){
  case ROOFLINE_MACHINE:
    depth = hwloc_get_type_depth(topology, HWLOC_OBJ_MACHINE);
    break;
  case ROOFLINE_NUMA:
    depth = hwloc_get_type_depth(topology, HWLOC_OBJ_NUMANODE);
    break;
  case ROOFLINE_CORE:
    depth = hwloc_get_type_depth(topology, HWLOC_OBJ_CORE);
    break;
  default:
    depth = hwloc_get_type_depth(topology, HWLOC_OBJ_NUMANODE);
    break;
  }
  
  if(depth<0){return 0;}
  return depth;
}

void roofline_sampling_init(const char * output, const int append_output,
			    const enum roofline_location reduction_location){

  /* Open output */
  int print_header = 0;
  char * mode = "a";
  
  if(output == NULL){
    output_file = stdout;
    print_header = 1;
  } else {
    /* Check file existence */
    if(access( output, F_OK ) == -1 || !append_output ){
      print_header = 1;
      mode = "w+";
    }
    if((output_file = fopen(output, mode)) == NULL){
      perror("fopen");
      exit(EXIT_FAILURE);
    }
  }

  /* Initialize topology */
  if(hwloc_topology_init(&topology) ==-1){
    fprintf(stderr, "hwloc_topology_init failed.\n");
    exit(EXIT_FAILURE);
  }
  hwloc_topology_set_icache_types_filter(topology, HWLOC_TYPE_FILTER_KEEP_ALL);
  if(hwloc_topology_load(topology) ==-1){
    fprintf(stderr, "hwloc_topology_load failed.\n");
    exit(EXIT_FAILURE);
  }

  /* Check whether flops counted will be AVX or SSE */
  int eax = 0, ebx = 0, ecx = 0, edx = 0;
  /* Check SSE */
  eax = 1;
  __asm__ __volatile__ ("CPUID\n\t": "=c" (ecx), "=d" (edx): "a" (eax));
  if ((edx & 1 << 25) || (edx & 1 << 26)) {BYTES = 16; FLOPS=2;}
  /* Check AVX */
  if ((ecx & 1 << 28) || (edx & 1 << 26)) {BYTES = 32; FLOPS=4;}
  eax = 7; ecx = 0;
  __asm__ __volatile__ ("CPUID\n\t": "=b" (ebx), "=c" (ecx): "a" (eax), "c" (ecx));
  if ((ebx & 1 << 5)) {BYTES = 32; FLOPS=4;}
  /* AVX512. Not checked */
  if ((ebx & 1 << 16)) {BYTES = 64; FLOPS=8;}
  
  /* Initialize bindings array */
  unsigned i, max_threads;
#ifdef _OPENMP
  #pragma omp parallel
  max_threads = omp_get_max_threads();
#else
  max_threads = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
#endif
  bindings = malloc(sizeof(*bindings)*max_threads);
  for(i=0;i<max_threads;i++){bindings[i] = 0;}
		    
#ifdef _PAPI_
  /* Initialize PAPI library */
  PAPI_call_check(PAPI_library_init(PAPI_VER_CURRENT), PAPI_VER_CURRENT, "PAPI version mismatch\n");
  PAPI_call_check(PAPI_is_initialized(), PAPI_LOW_LEVEL_INITED, "PAPI initialization failed\n");
#endif

  /* Match reduction level */
  reduction_depth = roofline_reduction_depth(reduction_location);

  /* Create one sample per PU */
  struct roofline_sample * s;
  unsigned n_PU = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
  hwloc_obj_t first_PU, obj = NULL;  
  samples = new_list(sizeof(s), n_PU, (void(*)(void*))delete_roofline_sample);
  for(i=0; i<n_PU; i++){
    obj = hwloc_get_obj_by_type(topology, HWLOC_OBJ_PU, i);
    s = new_roofline_sample(obj);
    while(obj && obj->depth != reduction_depth){obj = obj->parent;}
    s->location = obj;
    list_push(samples, s);
  }
  
  /* Store a sublist of sample in each node at reduction level */
  obj=NULL;
  while((obj=hwloc_get_next_obj_by_depth(topology, reduction_depth, obj)) != NULL){
    if(obj->arity == 0){continue;}
    n_PU = hwloc_get_nbobjs_inside_cpuset_by_type(topology, obj->cpuset, HWLOC_OBJ_PU);
    first_PU = obj; while(first_PU->type!=HWLOC_OBJ_PU){first_PU = first_PU->first_child;}    
    obj->userdata = sub_list(samples, first_PU->logical_index, n_PU);    
  }
 
  /* Passed initialization then print header */
  if(print_header){roofline_print_header();}
}

void roofline_sampling_fini(){
  hwloc_obj_t Node = NULL;
  while((Node=hwloc_get_next_obj_by_depth(topology, reduction_depth, Node)) != NULL){
    if(Node->arity == 0){continue;}
    delete_list(Node->userdata);
  }
  free(bindings);
  delete_list(samples);  
  hwloc_topology_destroy(topology);
  fclose(output_file);
}

static void roofline_samples_reduce(list l, hwloc_obj_t location, const char * info){
  struct roofline_sample s;
  s.s_nano = 0;
  s.e_nano = 0;
  s.flops = 0;
  s.bytes = 0;
  s.location = location;
  s.n_threads = 0;
  list_reduce(l, &s, (void*(*)(void*,void*))roofline_sample_accumulate);
  roofline_sample_print(&s, info);
}

static struct roofline_sample * roofline_sampling_caller(const int id){
  if(id > 0){return list_get(samples, bindings[id]);}
  long cpu = -1;

  /* Check if calling thread is bound on a PU */
  hwloc_cpuset_t binding = hwloc_bitmap_alloc();
  if(hwloc_get_cpubind(topology, binding, HWLOC_CPUBIND_THREAD) == -1){perror("get_cpubind");}
  else if(hwloc_bitmap_weight(binding) == 1){cpu = hwloc_bitmap_first(binding);}
  hwloc_bitmap_free(binding);
  
  if(cpu == -1){ if((cpu = sched_getcpu()) == -1){perror("getcpu");} }

  if(cpu != -1){
    hwloc_obj_t PU = hwloc_get_pu_obj_by_os_index(topology, (int)cpu);
    bindings[-id] = PU->logical_index;
    return list_get(samples, PU->logical_index);
  }

  return NULL;
}

#ifdef _PAPI_
static void * roofline_sequential_sampling_start(__attribute__ ((unused)) long flops,__attribute__ ((unused)) long bytes, int tid)
#else
  static void * roofline_sequential_sampling_start(long flops, long bytes, int tid)
#endif
{
  struct timespec t;
  struct roofline_sample * s;
  
  /* Initialize sample structure */
  s = roofline_sampling_caller(-tid);
  if(s == NULL){return NULL;}

  /* Increment the number of threads modifying the sample */
  __sync_fetch_and_add(&s->n_threads, 1);
#ifdef _OPENMP
#pragma omp barrier
#endif

  /* Start PAPI eventset or set the number of flops and bytes */
#ifndef _PAPI_
  s->bytes = bytes;
  s->flops = flops;
#endif
  if(!__sync_fetch_and_add(&s->last_thread, 1)){
#ifdef _PAPI_    
    PAPI_start(s->eventset);
#endif
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    s->s_nano = t.tv_nsec + 1e9*t.tv_sec;
  }
    
  return (void*)s;
}

#ifdef _OPENMP
void * roofline_sampling_start(const int parallel, const long flops, const long bytes)
#else
  void * roofline_sampling_start(__attribute__ ((unused)) int parallel, long flops, long bytes)
#endif
{
  struct roofline_sample * ret = NULL;
#ifdef _OPENMP
  if(parallel && !omp_in_parallel()){
#pragma omp parallel
    {
      roofline_sequential_sampling_start(flops/omp_get_num_threads(), bytes/omp_get_num_threads(), omp_get_thread_num());
    }
  } else {
    ret = roofline_sequential_sampling_start(flops, bytes, omp_get_thread_num());
  }
#else
  ret = roofline_sequential_sampling_start(flops, bytes, 0);
#endif
  return ret;
}


static void roofline_sequential_sampling_stop(void *sample, const char* info){
  if(sample == NULL){return;}
  struct roofline_sample * s = (struct roofline_sample*)sample;
  struct timespec t;  
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
#ifdef _PAPI_
  if(__sync_fetch_and_sub(&s->last_thread, 1) == 1){
    PAPI_stop(s->eventset, s->values);
    s->e_nano = t.tv_nsec + 1e9*t.tv_sec;
    s->flops = s->values[0] + FLOPS * s->values[1];
    unsigned long muops = s->values[3]+s->values[2];
    unsigned long fuops = s->values[0] + s->values[1];
    if(fuops > 0){
      s->bytes = (8*muops/fuops)*(FLOPS*s->values[1] + s->values[0]);
    } else {
      s->bytes = 8*muops;
    }
/* #pragma omp critical */
/*     { */
/*       printf("%s:%d: load_muops=%ld, store_muops=%ld, PD(%d)=%ld, SD=%ld, AI=%f, GFlops/s=%f\n", */
/* 	     hwloc_type_name(s->location->type), */
/* 	     s->location->logical_index, */
/* 	     s->values[3], */
/* 	     s->values[2], */
/* 	     FLOPS,	     	      */
/* 	     s->values[1], */
/* 	     s->values[0], */
/* 	     (float)s->flops/(float)s->bytes, */
/* 	     (float)s->flops/(float)(s->e_nano-s->s_nano)); */
/*     } */
  }
#else
  if(__sync_fetch_and_sub(&s->last_thread, 1) == 1){ s->e_nano = t.tv_nsec + 1e9*t.tv_sec; }
#endif

#ifdef _OPENMP 
#pragma omp barrier
#pragma omp single
  {
#endif
    hwloc_obj_t Node = NULL;
    while((Node=hwloc_get_next_obj_by_depth(topology, reduction_depth, Node)) != NULL){
      if(Node->arity == 0){continue;}
      roofline_samples_reduce(Node->userdata, Node, info);
    }
    list_apply(samples, roofline_sample_reset);
#ifdef _OPENMP    
  }
#endif
}

void roofline_sampling_stop(void *sample, const char* info){
#ifdef _OPENMP
  if(sample == NULL){
    if(!omp_in_parallel()){
#pragma omp parallel
      roofline_sequential_sampling_stop(roofline_sampling_caller(omp_get_thread_num()), info);
    } else {
      roofline_sequential_sampling_stop(roofline_sampling_caller(omp_get_thread_num()), info);
    }
  } else {
    roofline_sequential_sampling_stop(sample, info);
  }
#else
  roofline_sequential_sampling_stop(sample, info);
#endif
}

