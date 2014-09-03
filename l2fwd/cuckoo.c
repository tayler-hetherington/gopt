#include "cuckoo.h"

#define COMPUTE 1

uint32_t hash(uint32_t u)
{
	int i, ret = u;
	for(i = 0; i < COMPUTE; i ++) {
		ret =  CityHash32((char *) &ret, 4);
	}
	return ret;
}

void cuckoo_init(int **entries, struct cuckoo_slot **ht_index)
{
	int i;

	printf("Initializing cuckoo index of size = %lu bytes\n", 
		HASH_INDEX_N * sizeof(struct cuckoo_slot));

	int sid = shmget(HASH_INDEX_KEY, HASH_INDEX_N * sizeof(struct cuckoo_slot), 
		IPC_CREAT | 0666 | SHM_HUGETLB);

	if(sid < 0) {
		printf("Could not create cuckoo hash index\n");
		exit(-1);
	}

	*ht_index = shmat(sid, 0, 0);

	// Allocate the packets and put them into the hash index randomly
	printf("Putting entries into hash index randomly\n");
	*entries = malloc(NUM_ENTRIES * sizeof(int));

	for(i = 0; i < NUM_ENTRIES; i++) {
		int K = rand();
		(*entries)[i] = K;
		
		// With 1/2 probability, put into 1st bucket
		int hash_bucket_i = 0;
		
		// The 2nd hash function for key K is CITYHASH(K + 1)
		if(rand() % 2 == 0) {
			hash_bucket_i = hash(K) & HASH_INDEX_N_;
		} else {
			hash_bucket_i = hash(K + 1) & HASH_INDEX_N_;
		}

		// The value for key K is K + i
		(*ht_index)[hash_bucket_i].key = K;
		(*ht_index)[hash_bucket_i].value = K + i;
	}
}