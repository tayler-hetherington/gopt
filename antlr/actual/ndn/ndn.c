#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "city.h"
#include "ndn.h"
#include "util.h"

/**< Create a mutable prefix from a URL */
char *ndn_get_prefix(const char *url, int len)
{
	char *prefix = malloc(len + 1);
	memcpy(prefix, url, len);
	prefix[len] = 0;
	return prefix;
}

/**< Check if a prefix exists in the NDN hash table. If the existing prefix
  *  is non-terminal, and this prefix is terminal, then update the existing
  *  prefix's log entry (set is_terminal = 1 and a valid dst port).
  *
  *  To use this function just to test is a prefix is present (without any
  *  updates to the log, call with is_terminal = 0.
  *
  *  Returns 1 if the prefix was found, and 0 otherwise. */
int ndn_contains(const char *url, int len,
	int is_terminal, int dst_port_id, struct ndn_ht *ht)
{
	/**< A prefix ends with a '/', so it contains at least 2 characters */
	assert(len >= 2);

	int i;
	int bkt_num, bkt_1, bkt_2;
	uint16_t tag = (uint16_t) url[len - 2];	/**< url[len - 1] is '/' */

	struct ndn_bucket *ht_index = ht->ht_index;
	ULL *slot;
	uint8_t *ht_log = ht->ht_log;

	/**< Test the two candidate buckets */
	for(bkt_num = 1; bkt_num <= 2; bkt_num ++) {

		/**< Get the slot array for this bucket */
		if(bkt_num == 1) {
			bkt_1 = CityHash64(url, len) & NDN_NUM_BKT_;
			slot = ht_index[bkt_1].slot;
		} else {
			bkt_2 = (bkt_1 ^ CityHash64((char *) &tag, 2)) & NDN_NUM_BKT_;
			slot = ht_index[bkt_2].slot;
		}

		/**< Now, "slot" points to an ndn_bucket. Find a valid slot with 
		  *  a matching tag. */
		for(i = 0; i < 8; i ++) {
			int slot_offset = NDN_SLOT_TO_OFFSET(slot[i]);
			uint16_t slot_tag = NDN_SLOT_TO_TAG(slot[i]);
			uint8_t *log_ptr = &ht_log[slot_offset];
	
			if(slot_offset != 0 && slot_tag == tag) {
				uint8_t prefix_len = log_ptr[0];

				if(prefix_len == (uint8_t) len && 
					memcmp(url, &log_ptr[3], len) == 0) {

					/**< Should we upgrade this prefix to "terminal"? */
					if(log_ptr[1] == 0 && is_terminal == 1) {
						log_ptr[1] = 1;
						log_ptr[2] = dst_port_id;
					}

					return 1;
				}
			}
		}
	}

	return 0;
}

/**< Insert a prefix (specified by "url" and "len") into the NDN hash table. 
  *  Returns 0 on success and -1 on failure. */
int ndn_ht_insert(const char *url, int len, 
	int is_terminal, int dst_port_id, struct ndn_ht *ht) 
{
	/**< A prefix ends with a '/', so it contains at least 2 characters */
	assert(len >= 2);

	assert(is_terminal == 0 || is_terminal == 1);
	assert(dst_port_id < NDN_MAX_ETHPORTS);
	assert(len <= NDN_MAX_URL_LENGTH);

	if(ndn_contains(url, len, is_terminal, dst_port_id, ht)) {
		return 0;
	}

	int i;
	int bkt_num, bkt_1, bkt_2;
	uint16_t tag = (uint16_t) url[len - 2];	/**< url[len - 1] is '/' */

	struct ndn_bucket *ht_index = ht->ht_index;
	ULL *slot;
	uint8_t *ht_log = ht->ht_log;

	/**< Test the two candidate buckets */
	for(bkt_num = 1; bkt_num <= 2; bkt_num ++) {

		/**< Get the slot array for this bucket */
		if(bkt_num == 1) {
			bkt_1 = CityHash64(url, len) & NDN_NUM_BKT_;
			slot = ht_index[bkt_1].slot;
		} else {
			bkt_2 = (bkt_1 ^ CityHash64((char *) &tag, 2)) & NDN_NUM_BKT_;
			slot = ht_index[bkt_2].slot;
		}

		/**< Now, "slot" points to an ndn_bucket */
		for(i = 0; i < 8; i ++) {
			int slot_offset = NDN_SLOT_TO_OFFSET(slot[i]);
	
			/**< Found an empty slot */
			if(slot_offset == 0) {
				int insert_offset = ht->log_head;
				assert(insert_offset + NDN_LOG_HEADROOM < NDN_LOG_CAP);

				/**< Initialize the slot */
				slot[i] = ((ULL) tag << 48) | insert_offset;

				/**< We write "len" bytes and 3 bytes of metadata to the log */
				ht->log_head += (3 + len);

				/**< Actually write the prefix and metadata to the log */
				ht_log[insert_offset] = len;
				ht_log[insert_offset + 1] = is_terminal;
				ht_log[insert_offset + 2] = dst_port_id;
				memcpy(&ht_log[insert_offset + 3], url, len);

				return 0;
			}
		}
	}

	printf("\tUnable to insert URL: %s\n", url);
	return -1;
}

void ndn_init(const char *urls_file, int portmask, struct ndn_ht *ht)
{
	int nb_urls = 0;
	char url[NDN_MAX_URL_LENGTH] = {0};
	int shm_flags = IPC_CREAT | 0666 | SHM_HUGETLB;

	int index_size = (int) (NDN_NUM_BKT * sizeof(struct ndn_bucket));
	int log_size = (int) (NDN_LOG_CAP * sizeof(uint8_t));

	int num_active_ports = bitcount(portmask);
	int *port_arr = get_active_bits(portmask);

	/**< Allocate the hash index and URL log, and zero-out the log head */
	red_printf("Initializing NDN hash of size = %lu bytes\n", index_size);
	int index_sid = shmget(NDN_HT_INDEX_KEY, index_size, shm_flags);
	assert(index_sid >= 0);
	ht->ht_index = shmat(index_sid, 0, 0);
	memset((char *) ht->ht_index, 0, index_size);

	red_printf("Initializing NDN URL log of size = %lu bytes\n", log_size);
	int log_sid = shmget(NDN_HT_LOG_KEY, log_size, shm_flags);
	assert(log_sid >= 0);
	ht->ht_log = shmat(log_sid, 0, 0);
	memset((char *) ht->ht_log, 0, log_size);

	/**< In any slot, log_head >= 1 means that it is a valid slot */
	ht->log_head = 1;

	FILE *url_fp = fopen(urls_file, "r");
	assert(url_fp != NULL);
	int nb_fail = 0;

	while(1) {

		/**< Read a new URL from the file and check if its valid */
		fscanf(url_fp, "%s", url);
		if(url[0] == 0) {
			break;
		}

		assert(url[NDN_MAX_URL_LENGTH - 1] == 0);

		/**< The destination port for this URL */
		int dst_port_id = port_arr[rand() % num_active_ports];

		int i;
		for(i = 0; i < NDN_MAX_URL_LENGTH; i ++) {
			if(url[i] == '/') {
				/**< Non-terminal prefixes have an invalid dst port */
				if(ndn_ht_insert(url, i + 1, 0, -1, ht) != 0) {
					nb_fail ++;
				}
			} else if(url[i] == 0) {
				/**< All inserted prefixes must end with '/'. This is required
				  *  for correctness during lookups. */
				url[i] = '/';
				if(ndn_ht_insert(url, i + 1, 1, dst_port_id, ht) != 0) {
					nb_fail ++;
				}
				break;
			}
		}
		
		memset(url, 0, NDN_MAX_URL_LENGTH * sizeof(char));
		nb_urls ++;

		if((nb_urls & K_512_) == 0) {
			printf("Total urls = %d. Fails = %d\n", nb_urls, nb_fail);
		}
	}

	red_printf("Total urls = %d. Fails = %d.\n", nb_urls, nb_fail);
	red_printf("Total log memory used = %d bytes\n", ht->log_head);

}

/**< Check if all the URLs in "urls_file" are inserted in the hash table */
void ndn_check(const char *urls_file, struct ndn_ht *ht)
{
	int nb_urls = 0;
	char url[NDN_MAX_URL_LENGTH] = {0};

	FILE *url_fp = fopen(urls_file, "r");
	assert(url_fp != NULL);

	while(1) {

		/**< Read a new URL from the file and check if its valid */
		fscanf(url_fp, "%s", url);
		if(url[0] == 0) {
			break;
		}

		assert(url[NDN_MAX_URL_LENGTH - 1] == 0);

		int i;
		for(i = 0; i < NDN_MAX_URL_LENGTH; i ++) {
			/**< While calling ndn_contains(), use is_terminal = 0 to avoid
			  *  overwriting any log entry */
			if(url[i] == '/') {
				if(ndn_contains(url, i + 1, 0, -1, ht) == 0) {
					printf("Prefix %s absent.\n", ndn_get_prefix(url, i + 1));
					assert(0);
				}
			} else if(url[i] == 0) {
				url[i] = '/';
				if(ndn_contains(url, i + 1, 0, -1, ht) == 0) {
					printf("Prefix %s absent.\n", ndn_get_prefix(url, i + 1));
					assert(0);
				}
				break;
			}
		}
		
		memset(url, 0, NDN_MAX_URL_LENGTH);
		nb_urls ++;

		if((nb_urls & K_512_) == 0) {
			printf("Checked %d URLs.\n", nb_urls);
		}
	}
}

/**< Return the number of URLs in a file */
int ndn_get_num_urls(const char *urls_file)
{
	int nb_urls = 0;
	FILE *url_fp = fopen(urls_file, "r");
	assert(url_fp != NULL);

	char url[NDN_MAX_URL_LENGTH] = {0};

	while(1) {
		/**< Read a new URL from the file and check if its valid */
		fscanf(url_fp, "%s", url);
		if(url[0] == 0) {
			break;
		}

		/**< As we're only counting URLs, no need to zero out all bytes */
		url[0] = 0;
		nb_urls ++;
	}

	return nb_urls;
}


/**< Put all the URLs in a linear array with fixed sized slots */
struct ndn_linear_url *ndn_get_url_array(const char *urls_file)
{
	int i;
	int nb_urls = ndn_get_num_urls(urls_file);
	struct ndn_linear_url *url_arr = 
		malloc(nb_urls * sizeof(struct ndn_linear_url));
	memset(url_arr, 0, nb_urls * sizeof(struct ndn_linear_url));

	char url[NDN_MAX_URL_LENGTH] = {0};
	FILE *url_fp = fopen(urls_file, "r");
	assert(url_fp != NULL);

	for(i = 0; i < nb_urls; i ++) {
		fscanf(url_fp, "%s", url);
		if(url[0] == 0) {
			break;
		}
		assert(url[NDN_MAX_URL_LENGTH - 1] == 0);

		int url_len = strlen(url);

		memcpy((char *) &url_arr[i], url, url_len);
		memset(url, 0, NDN_MAX_URL_LENGTH);
	}

	return url_arr;
}

/**< Print some useful stats for the URLs in this file */
void ndn_print_url_stats(const char *urls_file)
{
	int i;

	/**< Maximum number of components = 5 */
	int components_stats[NDN_MAX_URL_LENGTH + 1] = {0};
	char url[NDN_MAX_URL_LENGTH] = {0};

	FILE *url_fp = fopen(urls_file, "r");
	assert(url_fp != NULL);

	while(1) {
		fscanf(url_fp, "%s", url);
		if(url[0] == 0) {
			break;
		}
		assert(url[NDN_MAX_URL_LENGTH - 1] == 0);

		int num_components = ndn_num_components(url);
		assert(num_components <= NDN_MAX_COMPONENTS);
		components_stats[num_components] ++;
	
		memset(url, 0, NDN_MAX_URL_LENGTH);
	}

	red_printf("URL stats:\n");
	for(i = 0; i <= NDN_MAX_COMPONENTS; i ++) {
		printf("%d URLs have %d components\n", components_stats[i], i);
	}
}

/**< Count the number of components in this URL */
inline int ndn_num_components(const char *url)
{
	int i, num_slash = 0;
	for(i = 0; i < NDN_MAX_URL_LENGTH; i ++) {
		if(url[i] == '/') {
			num_slash ++;
		}

		if(url[i] == 0) {
			break;
		}
	}

	return num_slash + 1;
}