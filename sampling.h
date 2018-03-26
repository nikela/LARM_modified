#ifndef SAMPLING_H
#define SAMPLING_H

/**
 * Underneath the abstraction, samples are collected on Processing Elements, and accumulated on nodes of the topology.
 * Possible location to reduce samples are the topology root (ROOFLINE_MACHINE), 
 * the NUMA domains of the machine (ROOFLINE_NUMA),
 * or the Cores of the machine (ROOFLINE_CORE).
 **/
enum roofline_location{ROOFLINE_MACHINE, ROOFLINE_NUMA, ROOFLINE_CORE};

/**
 * Initialize the sampling library.
 * This function internally initialize a topology where samples are stored, the PAPI library.
 * Each processing unit of the topology own a sample containing flops and bytes count and is updated 
 * by threads calling roofline_sampling_start() from this processing unit.
 * If the library is compiled with openmp enabled, it is thread safe, and thread synchronization occures 
 * on roofline_sampling_stop() call. 
 * @arg output: The file where to write the output on call to sampling stop.
 * @arg append_output: if output file already exists, and \p apppend_output is TRUE, 
 *                     then results are appended to output without printing header.
 * @arg reduction_location: A topology level type giving the depth of nodes where samples are accumulated.  
 **/
void   roofline_sampling_init(const char * output, int append_output,
			      const enum roofline_location reduction_location);

/**
 * Destroy structures initialized on call to roofline_sampling_init.
 **/
void   roofline_sampling_fini ();

/**
 * Initialize a sampling structure and start counting flops, bytes and time.
 * This function and its sibling roofline_sampling_stop() must be called from the same scope.
 * Both can be called from a parallel or a sequential region.
 * If the library is compiled with PAPI support, flops and bytes are acquired with hardware counters.
 * If compiled with openmp support, a barrier occures during this call.
 *
 * If call is made from a parallel region with PAPI:
 *    Then, values are measured by each processing element then accumulated into each parent reduction locations.
 * If call is made from a sequential region with PAPI:
 *    Then the user can choose to record a single processing element by setting the parameter \p force_parallel to 0 or 
 *    record in parallel the whole machine anyway with the same parameter set to a non-zero value.
 * If call is made without PAPI:
 *    Then the parameters \p flops and \p bytes are used to set the flops and bytes results in the following fashion:
 * If call is made from a sequential region without PAPI:
 *    Then \p flops and \p bytes are assumed to be the total of all threads results for the scope of the call, and to be uniformely
 *    spread among threads.
 * If call is made from a parallel region with PAPI:
 *    Then \p flops and \p bytes are assumed to be worth the calling thread only.
 * @arg force_parallel: a flag to tell if we have to count in parallel even if called from a sequential region
 * @arg flops: by hand flop count.
 * @arg bytes: by hand byte count.
 * @return a pointer to an opaque struct holding the current sample. 
 *         This sample pointer must be provided to roofline_sampling_stop().
 **/
void * roofline_sampling_start(int force_parallel, long flops, long bytes);

/**
 * Stop sampling roofline metrics and output results for the whole machine. 
 * If compiled with openmp support, a barrier occures during this call.
 * Result presentation is as follow:
 * reduction_location Nanoseconds Bytes Flops n_threads type info.
 * Results are accumulated by location set in roofline_sampling_init().
 * Nanoseconds is the time between roofline_sampling_start() and roofline_sampling_stop() calls.
 * Bytes and Flops are counted between roofline_sampling_start() and roofline_sampling_stop() calls, 
 * whether with hardware counters or with manually provided values.
 * n_threads is the number of threads actually used in a parallel region by the library.
 * Additionnally, the environment variable "LARM_INFO" is read on each roofline_sampling_stop() call, 
 * and is appended to the info column of the sample result.
 * @arg sample: The structure provided on roofline_sampling_start() call.
 * @arg info: an additional information to be written into info column.
 **/
void   roofline_sampling_stop (void * sample, const char* info);

#endif /*  SAMPLING_H */
