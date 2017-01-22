#include <unistd.h>
#include <stdlib.h>
#include "background.h"
#include "libmemory.h"

/**
 * This defines how log we sleep between loop ticks
 * in microseconds
 */
#define PERIODIC_TIME_USEC 250000

/**
 * Based on the PERIODIC_TIME_USEC, this should
 * convert seconds to tick counts. One tick occurs
 * each PERIODIC_TIME_USEC interval
 */
#define SEC_TO_TICKS(sec) ((sec * 4))

/*
* After how many background operations should we force a client
* checkpoint. This allows the vacuum thread to make progress even
* if we have a very slow background task
*/
#define PERIODIC_CHECKPOINT 16

static void* flush_thread_main(void *in);
static void* unmap_thread_main(void *in);
static void* memory_check_thread_main(void *in);
typedef struct {
    bloom_config *config;
    bloom_filtmgr *mgr;
    int *should_run;
} background_thread_args;

/**
 * Helper macro to pack and unpack the arguments
 * to the thread, and free the memory.
 */
# define PACK_ARGS() {                  \
    args = malloc(sizeof(background_thread_args));  \
    args->config = config;              \
    args->mgr = mgr;                    \
    args->should_run = should_run;      \
}
# define UNPACK_ARGS() {                \
    background_thread_args *args = in;  \
    config = args->config;              \
    mgr = args->mgr;                    \
    should_run = args->should_run;      \
    free(args);                         \
}


/**
 * Starts a flushing thread which on every
 * configured flush interval, flushes all the filters.
 * @arg config The configuration
 * @arg mgr The filter manager to use
 * @arg should_run Pointer to an integer that is set to 0 to
 * indicate the thread should exit.
 * @arg t The output thread
 * @return 1 if the thread was started
 */
int start_flush_thread(bloom_config *config, bloom_filtmgr *mgr, int *should_run, pthread_t *t) {
    // Return if we are not scheduled
    if(config->flush_interval <= 0) {
        return 0;
    }

    // Start thread
    background_thread_args *args;
    PACK_ARGS();
    pthread_create(t, NULL, flush_thread_main, args);
    return 1;
}

/**
 * Starts a cold unmap thread which on every
 * cold interval unmaps cold filters.
 * @arg config The configuration
 * @arg mgr The filter manager to use
 * @arg should_run Pointer to an integer that is set to 0 to
 * indicate the thread should exit.
 * @arg t The output thread
 * @return 1 if the thread was started
 */
int start_cold_unmap_thread(bloom_config *config, bloom_filtmgr *mgr, int *should_run, pthread_t *t) {
    // Return if we are not scheduled
    if(config->cold_interval <= 0) {
        return 0;
    }

    // Start thread
    background_thread_args *args;
    PACK_ARGS();
    pthread_create(t, NULL, unmap_thread_main, args);
    return 1;
}

/**
* Starts a memory policing thread which on every
* check interval compares the resident memory size
* of the app, and unmaps filters if the memory size
* exceeds the max allowed percentage of RAM
* @arg config The configuration
* @arg mgr The filter manager to use
* @arg should_run Pointer to an integer that is set to 0
*   to indicate the thread should exit.
* @arg t The output thread
* @return 1 if the thread was started
*/
int start_memory_check_thread(bloom_config *config, bloom_filtmgr *mgr, int *should_run, pthread_t *t) {
    // Return if we are not scheduled
    if(config->memory_check_interval <= 0){
        return 0;
    }

    // Start thread
    background_thread_args *args;
    PACK_ARGS();
    pthread_create(t, NULL, memory_check_thread_main, args);
    return 1;

}

static void* flush_thread_main(void *in) {
    bloom_config *config;
    bloom_filtmgr *mgr;
    int *should_run;
    UNPACK_ARGS();

    // Perform the initial checkpoint with the manager
    filtmgr_client_checkpoint(mgr);

    syslog(LOG_INFO, "Flush thread started. Interval: %d seconds.", config->flush_interval);
    unsigned int ticks = 0;
    while (*should_run) {
        usleep(PERIODIC_TIME_USEC);
        filtmgr_client_checkpoint(mgr);
        if ((++ticks % SEC_TO_TICKS(config->flush_interval)) == 0 && *should_run) {
            // List all the filters
            syslog(LOG_INFO, "Scheduled flush started.");
            bloom_filter_list_head *head;
            int res = filtmgr_list_filters(mgr, NULL, &head);
            if (res != 0) {
                syslog(LOG_WARNING, "Failed to list filters for flushing!");
                continue;
            }

            // Flush all, ignore errors since
            // filters might get deleted in the process
            bloom_filter_list *node = head->head;
            unsigned int cmds = 0;
            while (node) {
                filtmgr_flush_filter(mgr, node->filter_name);
                if (!(++cmds % PERIODIC_CHECKPOINT)) filtmgr_client_checkpoint(mgr);
                node = node->next;
            }

            // Cleanup
            filtmgr_cleanup_list(head);
        }
    }
    return NULL;
}

static void* unmap_thread_main(void *in) {
    bloom_config *config;
    bloom_filtmgr *mgr;
    int *should_run;
    UNPACK_ARGS();

    // Perform the initial checkpoint with the manager
    filtmgr_client_checkpoint(mgr);

    syslog(LOG_INFO, "Cold unmap thread started. Interval: %d seconds.", config->cold_interval);
    unsigned int ticks = 0;
    while (*should_run) {
        usleep(PERIODIC_TIME_USEC);
        filtmgr_client_checkpoint(mgr);
        if ((++ticks % SEC_TO_TICKS(config->cold_interval)) == 0 && *should_run) {
            // List the cold filters
            syslog(LOG_INFO, "Cold unmap started.");
            bloom_filter_list_head *head;
            int res = filtmgr_list_cold_filters(mgr, &head);
            if (res != 0) {
                continue;
            }

            // Close the filters, save memory
            syslog(LOG_INFO, "Cold filter count: %d", head->size);
            bloom_filter_list *node = head->head;
            unsigned int cmds = 0;
            while (node) {
                syslog(LOG_INFO, "Unmapping filter '%s' for being cold.", node->filter_name);
                filtmgr_unmap_filter(mgr, node->filter_name);
                if (!(++cmds % PERIODIC_CHECKPOINT)) filtmgr_client_checkpoint(mgr);
                node = node->next;
            }

            // Cleanup
            filtmgr_cleanup_list(head);
        }
    }
    return NULL;
}

static void* memory_check_thread_main(void *in) {
    bloom_config *config;
    bloom_filtmgr *mgr;
    int *should_run;
    UNPACK_ARGS();

    size_t all_memory = getMemorySize();
    size_t max_memory = (size_t)(config->max_memory_percent * all_memory * 0.01);
    size_t safe_memory = (size_t)(config->safe_memory_percent * all_memory * 0.01);
    size_t current_memory;

    // Perform the initial checkpoint with the mamager
    filtmgr_client_checkpoint(mgr);

    syslog(LOG_INFO, "Memory check thread started. Interval: %d seconds.", config->memory_check_interval);
    unsigned int ticks = 0;
    while (*should_run) {
        usleep(PERIODIC_TIME_USEC);
        filtmgr_client_checkpoint(mgr);
        if ((++ticks % SEC_TO_TICKS(config->memory_check_interval)) == 0 && *should_run) {
            // check RAM, compare to available and max.
            current_memory = getCurrentRSS();
            if (current_memory > max_memory) {
                syslog(LOG_INFO, "Max memory exceeded. Unmapping filters to reclaim RAM.");
                bloom_filter_list_head *head;
                int res = filtmgr_list_filters(mgr, NULL, &head);
                if (res != 0) {
                    syslog(LOG_WARNING, "Failed to list filters for flushing!");
                    continue;
                }
                bloom_filter_list *node = head->head;

                unsigned int cmds = 0;

                while (current_memory > safe_memory){
                    // start flushing filters until you're back below the safe-water mark
                    syslog(LOG_INFO, "Unmapping filter '%s' to free RAM.", node->filter_name);
                    /**
                    * Combines the unmap and the flush into one operation.
                    * Acquires the lock on the filter at the beginning to ensure
                    * no race conditions apply to writing a filter that's in the
                    * process of being dumped.
                    *
                    * @arg filter_name The name of the filter to operate on
                    * @return 0 on success, -1 if the filter does not exist
                    */
                    filtmgr_flush_and_unmap_filter(mgr, node->filter_name);


                    if (!(++cmds % PERIODIC_CHECKPOINT)) filtmgr_client_checkpoint(mgr);

                    current_memory = getCurrentRSS();
                    node = node->next;
                }

                filtmgr_cleanup_list(head);
            }
        }

    }
    return NULL;

}