#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

// nvcc assumes that all header files are C++ files. Tell it
// that these are C header files
extern "C" {
#include "ipv4.h"
#include "worker-master.h"
#include "util.h"
}

__global__ void
ipv4Gpu(int *req, int *resp, 
	int *tbl_24, int *tbl_long,
	int num_reqs)
{
	int i = blockDim.x * blockIdx.x + threadIdx.x;

	if (i < num_reqs) {
		uint32_t dst_ip = req[i];
		int dst_port = tbl_24[dst_ip >> 8];

		if(dst_port & 0x8000) {
			dst_port = tbl_long[(dst_port & 0x7fff) << 8 | (dst_port & 0xff)];
		}

		resp[i] = dst_port;
	}
}

/**
 * wmq : the worker/master queue for all lcores. Non-NULL iff the lcore
 * 		 is an active worker.
 */
void master_gpu(volatile struct wm_queue *wmq, cudaStream_t my_stream,
	int *h_reqs, int *d_reqs,				/** < Kernel inputs */
	int *h_resps, int *d_resps,				/** < Kernel outputs */
	int *d_tbl_24, int *d_tbl_long,			/** < IPv4 lookup tables */
	int num_workers, int *worker_lcores)
{
	assert(num_workers != 0);
	assert(worker_lcores != NULL);
	
	int i, err;

	/** Variables for batch-size and latency averaging measurements */
	int msr_iter = 0;			// Number of kernel launches
	long long msr_tot_req = 0;	// Total packet serviced by the master
	struct timespec msr_start, msr_end;
	double msr_tot_us = 0;		// Total microseconds over all iterations

	/** The GPU-buffer (h_reqs) start index for an lcore's packets 
	 *	during a kernel launch */
	int req_lo[WM_MAX_LCORE] = {0};

	/** Number of requests that we'll send to the GPU = nb_req.
	 *  We don't need to worry about nb_req overflowing the
	 *  capacity of h_reqs because it fits all WM_MAX_LCORE */
	int nb_req = 0;

	/** <  Value of the queue-head from an lcore during the last iteration*/
	long long prev_head[WM_MAX_LCORE] = {0}, new_head[WM_MAX_LCORE] = {0};
	
	int w_i, w_lid;		/**< A worker-iterator and the worker's lcore-id */
	volatile struct wm_queue *lc_wmq;	/**< Work queue of one worker */

	clock_gettime(CLOCK_REALTIME, &msr_start);

	while(1) {

		/**< Copy all the requests supplied by workers into the 
		  * contiguous h_reqs buffer */
		for(w_i = 0; w_i < num_workers; w_i ++) {
			w_lid = worker_lcores[w_i];		// Don't use w_i after this
			lc_wmq = &wmq[w_lid];
			
			// Snapshot this worker queue's head
			new_head[w_lid] = lc_wmq->head;

			// Note the GPU-buffer extent for this lcore
			req_lo[w_lid] = nb_req;

			// Iterate over the new packets
			for(i = prev_head[w_lid]; i < new_head[w_lid]; i ++) {
				int q_i = i & WM_QUEUE_CAP_;	// Offset in this wrkr's queue
				int req = lc_wmq->reqs[q_i];

				h_reqs[nb_req] = req;			
				nb_req ++;
			}
		}

		if(nb_req == 0) {		// No new packets?
			continue;
		}

		/**< Copy requests to device */
		err = cudaMemcpyAsync(d_reqs, h_reqs, nb_req * sizeof(int), 
			cudaMemcpyHostToDevice, my_stream);
		CPE(err != cudaSuccess, "Failed to copy requests h2d\n");

		/**< Kernel launch */
		int threadsPerBlock = 256;
		int blocksPerGrid = (nb_req + threadsPerBlock - 1) / threadsPerBlock;
	
		ipv4Gpu<<<blocksPerGrid, threadsPerBlock, 0, my_stream>>>(d_reqs, 
			d_resps, d_tbl_24, d_tbl_long, nb_req);
		err = cudaGetLastError();
		CPE(err != cudaSuccess, "Failed to launch ipv4Gpu kernel\n");

		/** < Copy responses from device */
		err = cudaMemcpyAsync(h_resps, d_resps, nb_req * sizeof(int),
			cudaMemcpyDeviceToHost, my_stream);
		CPE(err != cudaSuccess, "Failed to copy responses d2h\n");

		/** < Synchronize all ops */
		cudaStreamSynchronize(my_stream);
		
		/**< Copy the ports back to worker queues */
		for(w_i = 0; w_i < num_workers; w_i ++) {
			w_lid = worker_lcores[w_i];		// Don't use w_i after this

			lc_wmq = &wmq[w_lid];
			for(i = prev_head[w_lid]; i < new_head[w_lid]; i ++) {
	
				/** < Offset in this workers' queue and the GPU-buffer */
				int q_i = i & WM_QUEUE_CAP_;				
				int req_i = req_lo[w_lid] + (i - prev_head[w_lid]);
				lc_wmq->resps[q_i] = h_resps[req_i];
			}

			prev_head[w_lid] = new_head[w_lid];
		
			/** < Update tail for this worker */
			lc_wmq->tail = new_head[w_lid];
		}

		/** < Do some measurements */
		msr_iter ++;
		msr_tot_req += nb_req;
		if(msr_iter == 100000) {
			clock_gettime(CLOCK_REALTIME, &msr_end);
			msr_tot_us = (msr_end.tv_sec - msr_start.tv_sec) * 1000000 +
				(msr_end.tv_nsec - msr_start.tv_nsec) / 1000;

			blue_printf("\tGPU master: average batch size = %lld\n"
				"\t\tAverage time for GPU communication = %f us\n",
				msr_tot_req / msr_iter, msr_tot_us / msr_iter);

			msr_iter = 0;
			msr_tot_req = 0;

			/** < Start the next measurement */
			clock_gettime(CLOCK_REALTIME, &msr_start);
		}

		nb_req = 0;
	}
}

int main(int argc, char **argv)
{
	int c, i, err = cudaSuccess;
	int lcore_mask = -1;
	cudaStream_t my_stream;
	volatile struct wm_queue *wmq;

	/** < CUDA buffers */
	int *h_reqs, *d_reqs;
	int *h_resps, *d_resps;	
	int *d_tbl_24, *d_tbl_long;

	struct dir_ipv4_table *ipv4_table;

	/**< Get the worker lcore mask */
	while ((c = getopt (argc, argv, "c:")) != -1) {
		switch(c) {
			case 'c':
				// atoi() doesn't work for hex representation
				lcore_mask = strtol(optarg, NULL, 16);
				break;
			default:
				blue_printf("\tGPU master: I need coremask. Exiting!\n");
				exit(-1);
		}
	}

	assert(lcore_mask != -1);
	blue_printf("\tGPU master: got lcore_mask: %x\n", lcore_mask);

	/** < Create a CUDA stream */
	err = cudaStreamCreate(&my_stream);
	CPE(err != cudaSuccess, "Failed to create cudaStream\n");

	/** < Allocate hugepages for the shared queues */
	blue_printf("\tGPU master: creating worker-master shm queues\n");
	int wm_queue_bytes = M_2;
	while(wm_queue_bytes < WM_MAX_LCORE * sizeof(struct wm_queue)) {
		wm_queue_bytes += M_2;
	}
	printf("\t\tTotal size of wm_queues = %d hugepages\n", 
		wm_queue_bytes / M_2);
	wmq = (volatile struct wm_queue *) shm_alloc(WM_QUEUE_KEY, wm_queue_bytes);

	/** < Ensure that queue counters are in separate cachelines */
	for(i = 0; i < WM_MAX_LCORE; i ++) {
		uint64_t c1 = (uint64_t) (uintptr_t) &wmq[i].head;
		uint64_t c2 = (uint64_t) (uintptr_t) &wmq[i].tail;
		uint64_t c3 = (uint64_t) (uintptr_t) &wmq[i].sent;

		assert((c1 % 64 == 0) && (c2 % 64 == 0) && (c3 % 64 == 0));
	}

	blue_printf("\tGPU master: creating worker-master shm queues done\n");

	/** < Allocate buffers for requests from all workers*/
	blue_printf("\tGPU master: creating buffers for requests\n");
	int reqs_buf_size = WM_QUEUE_CAP * WM_MAX_LCORE * sizeof(int);
	err = cudaMallocHost((void **) &h_reqs, reqs_buf_size);
	CPE(err != cudaSuccess, "Failed to cudaMallocHost req buffer\n");
	err = cudaMalloc((void **) &d_reqs, reqs_buf_size);
	CPE(err != cudaSuccess, "Failed to cudaMalloc req buffer\n");

	/** < Allocate buffers for responses for all workers */
	blue_printf("\tGPU master: creating buffers for responses\n");
	int resps_buf_size = WM_QUEUE_CAP * WM_MAX_LCORE * sizeof(int);
	err = cudaMallocHost((void **) &h_resps, resps_buf_size);
	CPE(err != cudaSuccess, "Failed to cudaMallocHost resp buffers\n");
	err = cudaMalloc((void **) &d_resps, resps_buf_size);
	CPE(err != cudaSuccess, "Failed to cudaMalloc resp buffers\n");

	/** < Create the IPv4 cache and copy it over */
	blue_printf("\tGPU master: creating IPv4 lookup table\n");

	ipv4_table = (struct dir_ipv4_table *) malloc(sizeof(struct dir_ipv4_table));
	dir_ipv4_init(ipv4_table, IPv4_PORT_MASK);

	int tbl_24_bytes = (1 << 24) * sizeof(int);
	int tbl_long_bytes = IPv4_TABLE_LONG_CAP * sizeof(int);
	
	blue_printf("\tGPU master: alloc-ing DIR-24 tables on device\n");
	err = cudaMalloc((void **) &d_tbl_24, tbl_24_bytes);
	CPE(err != cudaSuccess, "Failed to cudaMalloc tbl_24\n");
	cudaMemcpy(d_tbl_24, ipv4_table->tbl_24, tbl_24_bytes, 
		cudaMemcpyHostToDevice);

	err = cudaMalloc((void **) &d_tbl_long, tbl_long_bytes);
	CPE(err != cudaSuccess, "Failed to cudaMalloc tbl_long\n");
	cudaMemcpy(d_tbl_long, ipv4_table->tbl_long, tbl_long_bytes, 
		cudaMemcpyHostToDevice);

	int num_workers = bitcount(lcore_mask);
	int *worker_lcores = get_active_bits(lcore_mask);
	
	/** < Launch the GPU master */
	blue_printf("\tGPU master: launching GPU code\n");
	master_gpu(wmq, my_stream,
		h_reqs, d_reqs, 
		h_resps, d_resps, 
		d_tbl_24, d_tbl_long,
		num_workers, worker_lcores);
	
}