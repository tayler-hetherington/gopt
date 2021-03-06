#include<stdio.h>
#include<stdlib.h>
#include<pthread.h>
#include<papi.h>
#include<time.h>

#include "fpp.h"
#include "rand-walk.h"

struct node *nodes;

long long sum = 0;

// batch_index must be declared outside process_batch
int batch_index = 0;
void process_batch(struct node *nodes)
{
	int next_nbh[BATCH_SIZE];
	int i[BATCH_SIZE];
	struct node *cur_node[BATCH_SIZE];

	int I = 0;			// batch index
	void *batch_rips[BATCH_SIZE];		// goto targets
	int iMask = 0;		// No packet is done yet

	int temp_index;
	for(temp_index = 0; temp_index < BATCH_SIZE; temp_index ++) {
		batch_rips[temp_index] = &&fpp_start;
	}

fpp_start:

        cur_node[I] = &nodes[I];
        
        for(i[I] = 0; i[I] < STEPS; i[I] ++) {
            FPP_PSS(cur_node[I], fpp_label_1);
fpp_label_1:

            sum += cur_node[I]->id;
            
            /** < Compute the next neighbor */
            next_nbh[I] = -1;
            while(next_nbh[I] < 0) {
                next_nbh[I] = rand() % 7;
            }
            
            cur_node[I] = (struct node *) nodes[I].neighbors[next_nbh[I]];
        }
        
fpp_end:
    batch_rips[I] = &&fpp_end;
    iMask = FPP_SET(iMask, I); 
    if(iMask == (1 << BATCH_SIZE) - 1) {
        return;
    }
    I = (I + 1) & BATCH_SIZE_;
    goto *batch_rips[I];

}

int main(int argc, char **argv)
{
	int i;

	/** < Variables for PAPI */
	float real_time, proc_time, ipc;
	long long ins;
	int retval;

	red_printf("main: Initializing nodes for random walk\n");
	rand_walk_init(&nodes);

	red_printf("main: Starting random walks\n");
	/** < Init PAPI_TOT_INS and PAPI_TOT_CYC counters */
	if((retval = PAPI_ipc(&real_time, &proc_time, &ins, &ipc)) < PAPI_OK) {    
		printf("PAPI error: retval: %d\n", retval);
		exit(1);
	}

	/** < Do a random-walk from every node in the graph */
	for(i = 0; i < NUM_NODES; i += BATCH_SIZE) {
		process_batch(&nodes[i]);
	}

	if((retval = PAPI_ipc(&real_time, &proc_time, &ins, &ipc)) < PAPI_OK) {    
		printf("PAPI error: retval: %d\n", retval);
		exit(1);
	}

	red_printf("Time = %.4f, rate = %.2f sum = %lld\n"
		"Instructions = %lld, IPC = %f\n",
		real_time, NUM_NODES / real_time, sum,
		ins, ipc);

	return 0;
}
