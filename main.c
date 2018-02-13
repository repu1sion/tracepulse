//gcc main.c -o tracepulse -ltrace && ./tracepulse
//gcc main.c -o tracepulse -ltrace && sudo ./tracepulse 4

//combiner: we use combiner_ordered. so output data stored in ordered way.
//	    there are 3 combiner types: ordered, unordered, sorted.
//hasher:   3 hashers: balanced, unidirectional, bidirectional, we use balanced one,
//	    so the data spread packets across the threads in a balanced way

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libtrace_parallel.h>	//resides just in /usr/local/include

#define DEBUG

#ifdef DEBUG
 #define debug(x...) printf(x)
#else
 #define debug(x...)
#endif

typedef struct libtrace_thread_t libtrace_thread_t;

//local storage for each processing thread. should be allocated for every thread
struct t_store
{
	uint64_t pkts;		//received packets
	uint64_t bytes;		//received bytes
};

//storage for reporter thread (the only one)
struct r_store
{
	uint64_t pkts;		//received packets
	uint64_t bytes;		//received bytes
};


// PROCESSING THREADS CALLBACKS
// -----------------------------------------------------------------------------
//start callback function
static void* start_cb(libtrace_t *trace, libtrace_thread_t *thread, void *global)
{
	/* Create and initialise a counter struct */
	struct t_store *ts = (struct t_store*)malloc(sizeof(struct t_store));
	if (!ts)
	{
		printf("<error: can't allocate ram for thread storage!>\n");
		return NULL;
	}
	memset(ts, 0x0, sizeof(struct t_store));

	return ts;
}

static void stop_cb(libtrace_t *trace, libtrace_thread_t *thread, void *global, void *tls) 
{
	struct t_store *ts = (struct t_store*)tls;
	libtrace_generic_t gen;

	gen.ptr = ts;
	//XXX: 0 - order for result, 0 - no order, but we need to add ordering by timestamp!
	//could be needed RESULT_PACKET
	//Inside it calls:
	//libtrace->combiner.publish(libtrace, t->perpkt_num, &libtrace->combiner, &res);
	//trace_publish_result(trace, thread, 0, gen, RESULT_USER);
}

// the packet callback
static libtrace_packet_t* packet_cb(libtrace_t *trace, libtrace_thread_t *thread,
				    void *global, void *tls, libtrace_packet_t *packet)
{
	int payloadlen = 0;
	struct t_store *ts = (struct t_store*)tls;
	struct libtrace_thread_t *lt = (struct libtrace_thread_t *)thread;

	payloadlen = trace_get_payload_length(packet);
	
	ts->pkts++;
	ts->bytes += payloadlen;

	debug(/*"thread #%d:*/ "pkts: %lu, bytes: %lu \n", /*lt->perpkt_num,*/ ts->pkts, ts->bytes);

        // forwarding the packet to the reporter
        trace_publish_result(trace, thread, 0, (libtrace_generic_t){.pkt = packet}, RESULT_PACKET);

	//by returning NULL we say to libtrace that we are keeping the packet
	return NULL;
}
// -----------------------------------------------------------------------------

// REPORTER THREAD CALLBACKS
// -----------------------------------------------------------------------------
/* Starting callback for the reporter thread */
static void *start_reporter_cb(libtrace_t *trace, libtrace_thread_t *thread, void *global) 
{
	debug("%s()\n", __func__);

	struct r_store *rs = (struct r_store*)malloc(sizeof(struct r_store));
	if (!rs)
	{
		printf("<error: can't allocate ram for thread storage!>\n");
		return NULL;
	}
	memset(rs, 0x0, sizeof(struct r_store));

	return rs;
}

// The result callback is invoked for each result that reaches the reporter thread
// (so anytime when someone calls trace_publish_result()
static void result_reporter_cb(libtrace_t *trace, libtrace_thread_t *sender,
        		       void *global, void *tls, libtrace_result_t *result)
{
	struct r_store *rs = (struct r_store*)tls;
	libtrace_packet_t *pkt;
	int payloadlen = 0;

	debug("%s()\n", __func__);

	pkt = (libtrace_packet_t *)result->value.pkt;
	if (pkt)
	{
		payloadlen = trace_get_payload_length(pkt);
		rs->pkts++;
		rs->bytes += payloadlen;
		debug("pkt in reporter, len: %d, total pkts: %lu, total bytes: %lu \n", 
			payloadlen, rs->pkts, rs->bytes);
	}
	
	//XXX - implement writing to file
        if (result->type == RESULT_PACKET)
                trace_free_packet(trace, result->value.pkt);

}

//called once in the end for reporter thread?
static void stop_reporter_cb(libtrace_t *trace, libtrace_thread_t *thread, 
			     void *global, void *tls) 
{
	struct r_store *rs = (struct r_store*)tls;

	debug("%s()\n", __func__);
}


// -----------------------------------------------------------------------------
int init()
{
	printf("init\n");

	return 0;
}

int scrot()
{
	int rv = 0;
	char cmd[512] = {0};

/*
	strcpy(cmd, "scrot ");
	strcat(cmd, IMG_NAME);

	rv = system(cmd);
	printf("scrot execution value is: %d\n", rv);
*/
	return rv;
}


int main(int argc, char *argv[])
{
	int rv = 0;
	int threads_num = 1;		//1 thread by default
	libtrace_t *input;
	char *uri = "ring:eth0";	//XXX - why ring?
	libtrace_callback_set_t *processing = NULL, *reporter = NULL;

	//rv = init();
	if (argc > 1)
	{
		threads_num = atoi(argv[1]);
	}

	//we create 2 callback sets: for processing and reporter threads
	processing = trace_create_callback_set();
	trace_set_starting_cb(processing, start_cb);
	trace_set_stopping_cb(processing, stop_cb);
	trace_set_packet_cb(processing, packet_cb);

	reporter = trace_create_callback_set();
	trace_set_starting_cb(reporter, start_reporter_cb);
	trace_set_stopping_cb(reporter, stop_reporter_cb);
	trace_set_result_cb(reporter, result_reporter_cb);

	/* Create the input trace object */
	input = trace_create(uri);
	if (trace_is_err(input)) 
	{
		trace_perror(input, "error creating trace");
		return 1;
	}

	/* Set the number of processing threads to use.
	If not set, libtrace will create one thread for each core it detects on your system. */
	printf("set %d threads \n", threads_num);
	trace_set_perpkt_threads(input, threads_num);

	//there are 3 possible combiners: ordered, unordered, sorted. we use ordered.
	trace_set_combiner(input, &combiner_ordered, (libtrace_generic_t){0});	//XXX - strange syntax

	/* Try to balance our load across all processing threads. If
	we were doing flow analysis, we should use 
	HASHER_BIDIRECTIONAL instead to ensure that all packets for
	a given flow end up on the same processing thread. */
	trace_set_hasher(input, HASHER_BALANCE, NULL, NULL);

	/* Start the parallel trace using our callback sets. The NULL 
	* parameter here is where we can provide global data for the
	input trace -- we don't need any in this example.
	Second param is global data available for all callbacks 
	Third param - callback set for processing threads
	Fourth param - callback set for reporter thread */
	if (trace_pstart(input, NULL, processing, reporter)) 
	{
		trace_perror(input, "Starting parallel trace");
		return 1;
	}

	/* This will wait for all the threads to complete */
	trace_join(input);

	/* Clean up everything that we've created */
	trace_destroy(input);
	trace_destroy_callback_set(processing);
	trace_destroy_callback_set(reporter);

	return rv;
}
