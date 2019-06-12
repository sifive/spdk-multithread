#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"

/* A singly-linked list of the first active namespaces of all the controllers */
struct ns_list {
    struct spdk_nvme_ns *current;
    struct spdk_nvme_ctrlr *ctrlr;
    struct ns_list *next;
};

static struct spdk_env_opts opts;

static struct ns_list *n_list = NULL;

/* Callback function after probe for NVME controller is completed */
static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, 
        struct spdk_nvme_ctrlr_opts *opts) {
	printf("Attaching to %s\n", trid->traddr);
	return true;
}

/* Callback function after attachment to an NVME controller, store the first activate namespace encountered */
static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
        struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts) {
    const struct spdk_nvme_ctrlr_data *data = spdk_nvme_ctrlr_get_data(ctrlr);
    printf("Number of namespaces is %d\n", data->nn);
    int ns_id;
    if ((ns_id = spdk_nvme_ctrlr_get_first_active_ns(ctrlr)) == 0) {
        fprintf(stderr, "There is no active namespace on this controller\n"); 
        return;
    }

    struct ns_list *ns_node = (struct ns_list *) malloc(sizeof(struct ns_list));
    if (ns_node == NULL) {
        fprintf(stderr, "malloc to new node in the singly linked list of namespaces failed");
        return;
    }

    /* Append new namespace to the singly linked list */
    ns_node->ctrlr = ctrlr;
    ns_node->current = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);
    ns_node->next = n_list;
    n_list = ns_node;

    return;
}

/* task provides an abstraction for the workload to be read and written */
struct task {
    struct spdk_nvme_ns *ns;
    struct spdk_nvme_ctrlr *ctrlr;
    char *buffer;
    bool is_completed;
    bool is_on_cmb;
    struct spdk_nvme_qpair *qpair;
};

/* String to be written to nvme and read back to stdout */
static const char str[] = "Hello World!";

/* Callback function on read complete */
static void read_complete(void *cb_ctx, const struct spdk_nvme_cpl *cpl_ctx) {
    struct task *iotask = cb_ctx;

    /* Print the content of the read buffer to stdout */
    printf("%s", iotask->buffer);

    /* Free the memory allocated for the read buffer */
    if (iotask->is_on_cmb) {
        spdk_nvme_ctrlr_free_cmb_io_buffer(iotask->ctrlr, iotask->buffer, sizeof(iotask->buffer));
    } else {
        spdk_free(iotask->buffer);
    }

    /* Indicate to the poller that the io routine is completed */
    iotask->is_completed = true;
}

/* Callback function on write complete */
static void write_complete(void *cb_ctx, const struct spdk_nvme_cpl *cpl_ctx) {
    struct task *iotask = cb_ctx;
    
    /* Free the buffer allocated to the write command */
    if (iotask->is_on_cmb) {
        spdk_nvme_ctrlr_free_cmb_io_buffer(iotask->ctrlr, iotask->buffer, sizeof(iotask->buffer));
    } else {
        spdk_free(iotask->buffer);
    }

    /* Allocate read buffer on either host or cmb */
    if ((iotask->buffer = spdk_nvme_ctrlr_alloc_cmb_io_buffer(iotask->ctrlr, sizeof(str))) == NULL) {
        iotask->buffer = spdk_malloc(sizeof(str), 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
        iotask->is_on_cmb = false;
    } else {
        iotask->is_on_cmb = true;
    }

    /* Submit read command to the queue pair */
    if (spdk_nvme_ns_cmd_read(iotask->ns, iotask->qpair, iotask->buffer, 0, 1, read_complete, iotask, 0)) {
        fprintf(stderr, "Failure in submitting a read commandi");
        return;
    }
}

static struct ns_list *head;

/* Free the singly list, detach the nvme controller */
static void cleanup(void) {
    while (head != NULL) {
        struct ns_list *next = head->next;
        if (spdk_nvme_detach(head->ctrlr)) {
            fprintf(stderr, "Error in detaching nvme controller");
        }
        free(head);
        head = next;
    }
}

int main(int argc, char **argv) {
    spdk_env_opts_init(&opts);
    if(spdk_env_init(&opts)) {
        fprintf(stderr, "unable to initialize SPDK env");
        return 1;
    }

    /* Probe for active namespaces on NVME controllers */
    void *cb_ctx = malloc(100);
    if (spdk_nvme_probe(NULL, cb_ctx, probe_cb, attach_cb, NULL)) {
        fprintf(stderr, "probe failed");
        return 1;
    }

    /* For each active namespace in the singly list, print hello world */
    head = n_list;
    while (n_list != NULL) {
        struct task iotask;
        iotask.is_completed = false;
        iotask.ns = n_list->current;

        /* Print namespace size, namespace capacity and formatted lba size */
        const struct spdk_nvme_ns_data *ns_data = spdk_nvme_ns_get_data(n_list->current);
        printf("Namespace size is %ld\n", ns_data->nsze);
        printf("Namespace capacity is %ld\n", ns_data->ncap);
        printf("Namespace sector size is %u\n", spdk_nvme_ns_get_sector_size(n_list->current));

        /* Allocate queue pair for this thread */
        struct spdk_nvme_qpair *qpair = spdk_nvme_ctrlr_alloc_io_qpair(n_list->ctrlr, NULL, 0);
        iotask.qpair = qpair;

        /* Initialize the task for this namespace */
        iotask.ctrlr = n_list->ctrlr;
        if ((iotask.buffer = spdk_nvme_ctrlr_alloc_cmb_io_buffer(iotask.ctrlr, sizeof(str))) == NULL) {
            /* cannot allocate memory on cmb */
            iotask.is_on_cmb = false;
            /* Allocate on host memory */
            iotask.buffer = spdk_zmalloc(sizeof(str), 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
        } else {
            iotask.is_on_cmb = true;
        }
        memcpy(iotask.buffer, str, sizeof(str));

        /* Submit write command to queue */
        if (spdk_nvme_ns_cmd_write(n_list->current, qpair, iotask.buffer, 0, 1, write_complete, &iotask, 0)) {
            fprintf(stderr, "Write command cannot be submitted");
            return 1;
        }

        /* Poll for completitions on this qpair until the io routine is completed */
        while (iotask.is_completed == false) {
            spdk_nvme_qpair_process_completions(qpair, 0);
        }

        /* Proceed to process the next namespace */
        n_list = n_list->next;
    }

    /* Free memory allocated to n_list */
    cleanup();

    return 0;
}
