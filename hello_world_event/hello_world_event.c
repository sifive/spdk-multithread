#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/event.h"

/* This program demonstrates the multithreaded hello world 
 * using the event framework provided in the SPDK library */

struct io_ctx {
    struct spdk_nvme_qpair *qpair;
    bool is_on_cmb;
    void *buffer;
    size_t load;
    uint64_t starting_lba;
    bool is_complete;
};

static char msg1[] = "Event framework does the I/O One!";
static size_t load1 = sizeof(msg1);

static char msg2[] = "Event framework does the I/O Two!";
static size_t load2 = sizeof(msg2);

static struct spdk_app_opts app_opts;

/* To simplify, only use one namespace for the following operation */
static struct spdk_nvme_ctrlr *nvme_ctrlr = NULL;
static struct spdk_nvme_ns *nvme_ns = NULL;

/* Callback on probing NVME controller */
static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
        struct spdk_nvme_ctrlr_opts *opts) {
    /* Only attach a controller if a valid namespace
     * has not been defined */
    return (nvme_ns == NULL);
}

/* Callback function on detaching an NVME controller */
static void remove_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr) {
    printf("The device with transfer address %s has been detached\n", 
            spdk_nvme_ctrlr_get_transport_id(ctrlr)->traddr);
}

/* Callback on attaching an NVME controller */
static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
        struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts) {
    printf("The controller %s has been attached\n", trid->traddr);
    int first_active_ns_id;
    if ((first_active_ns_id = spdk_nvme_ctrlr_get_first_active_ns(ctrlr)) == 0) {
        printf("There is no active namespace on controller %s\n", trid->traddr);
        /* Detach this nmve contoller because it does not have a valid namespaace */
        if (spdk_nvme_detach(ctrlr)) {
            printf("Failed to detach controller\n");
        }
        return;
    }
    /* register the chosen controller and namespace in the global pointer */
    nvme_ns = spdk_nvme_ctrlr_get_ns(ctrlr, first_active_ns_id);
    nvme_ctrlr = ctrlr;
    printf("Namespace %d of NVME controller %s is used, "
            "future controllers will be detached immediately\n", 
            first_active_ns_id, trid->traddr);
}

/* Intialize ONE nvme controller and the associated ONE activae namespace */
static void ctrlr_ns_init(void) {
    if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, remove_cb)) {
        fprintf(stderr, "Failed to probe for nvme controllers\n");
        exit(1);
    }
}

/* Callback on successful read */
static void read_complete(void *arg, const struct spdk_nvme_cpl *cpl) {
    struct io_ctx *ctx = arg;

    /* Print information to standard out */
    printf("%s is read from sector %ld\n", (char *) ctx->buffer, ctx->starting_lba);

    /* Deallocate read buffer */
    if (ctx->is_on_cmb) {
        spdk_nvme_ctrlr_free_cmb_io_buffer(nvme_ctrlr, ctx->buffer, load1);
        printf("Read buffer on cmb is deallocated\n");
    } else {
        spdk_free(ctx->buffer);
        printf("Read buffer on host memory is deallocated\n");
    }

    /* Mark task as complete */
    ctx->is_complete = true;
}

/* Callback on successful write */
static void write_complete(void *arg, const struct spdk_nvme_cpl *cpl) {
    struct io_ctx *ctx = arg;

    /* Deallocate write buffer */
    if (ctx->is_on_cmb) {
        spdk_nvme_ctrlr_free_cmb_io_buffer(nvme_ctrlr, ctx->buffer, load1);
        printf("Write buffer on cmb is deallocated\n");
    } else {
        spdk_free(ctx->buffer);
        printf("Write buffer on host memory is deallocated\n");
    }

    /* After write complete, do read */
    /* Allocate buffer on CMB or host for read */
    void *read_buffer = NULL;
    if ((read_buffer = spdk_nvme_ctrlr_alloc_cmb_io_buffer(nvme_ctrlr, ctx->load))) {
        ctx->is_on_cmb = true;
        printf("Read buffer is now on cmb\n");
    } else {
        read_buffer = spdk_malloc(ctx->load, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
        printf("Read buffer is on host memory\n");
    }
    ctx->buffer = read_buffer;

    /* Submit read command */
    if (spdk_nvme_ns_cmd_read(nvme_ns, ctx->qpair, read_buffer, ctx->starting_lba, 1,
                read_complete, ctx, 0)) {
        fprintf(stderr, "Submit read command failed\n");
    }
}

/* Event function that prints a message */
static void print_msg(void *arg1, void *arg2) {

    /* Allocate queue pair */
    struct spdk_nvme_qpair *qpair = 
        spdk_nvme_ctrlr_alloc_io_qpair(nvme_ctrlr, NULL, 0);

    /* Allcoate buffer on CMB or host for write */
    bool is_on_cmb1 = false;
    void *write_buffer1 = NULL;
    if ((write_buffer1 = spdk_nvme_ctrlr_alloc_cmb_io_buffer(nvme_ctrlr, load1))) {
        is_on_cmb1 = true;
        printf("Write buffer 1 is on cmb\n");
    } else {
        write_buffer1 = spdk_malloc(load1, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
        printf("Write buffer 1 is on host memory\n");
    }
    bool is_on_cmb2 = false;
    void *write_buffer2 = NULL;
    if ((write_buffer2 = spdk_nvme_ctrlr_alloc_cmb_io_buffer(nvme_ctrlr, load2))) {
        is_on_cmb2 = true;
        printf("Write buffer 2 is on cmb\n");
    } else {
        write_buffer2 = spdk_malloc(load2, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
        printf("Write buffer 2 is on host memory\n");
    }

    /* Write message to the allocated memory buffer */
    memcpy(write_buffer1, msg1, load1);
    memcpy(write_buffer2, msg2, load2);

    /* Submit write command */
    struct io_ctx ctx1 = {
        .qpair = qpair,
        .is_on_cmb = is_on_cmb1,
        .buffer = write_buffer1,
        .load = load1,
        .starting_lba = 0,
        .is_complete = false
    };
    if (spdk_nvme_ns_cmd_write(nvme_ns, qpair, write_buffer1, 0, 1, write_complete,
                &ctx1, 0)) {
        fprintf(stderr, "Failure in submitting write command 1\n");
    }
    printf("Submitted write command 1 to sector 0\n");
    struct io_ctx ctx2 = {
        .qpair = qpair,
        .is_on_cmb = is_on_cmb2,
        .buffer = write_buffer2,
        .load = load2,
        .starting_lba = 1,
        .is_complete = false
    };
    if (spdk_nvme_ns_cmd_write(nvme_ns, qpair, write_buffer2, 1, 1, write_complete,
                &ctx2, 0)) {
        fprintf(stderr, "Failure in submitting write command 2\n");
    }
    printf("Submitted write command 2 to sector 1\n");

    /* Continous process qpair until the two jobs are complete */
    while (!ctx1.is_complete || !ctx2.is_complete) {
        if (spdk_nvme_qpair_process_completions(qpair, 0) < 0) {
            fprintf(stderr, "Error occurred in processing queue pairs");
        }
    }
}

/* Main program after the hello is started */
static void hello_start(void *ctx) {
    ctrlr_ns_init();

    /* On virtual mahcine dpdk can only detect one logical core,
     * will send threads to work on this one lcore */
    uint32_t first_core_id = spdk_env_get_first_core();
    struct spdk_event *io_event = 
        spdk_event_allocate(first_core_id, print_msg, NULL, NULL);
    spdk_event_call(io_event);

    /* Stop the app framework to unblock the app_start call. Let the 
     * main function to continue executing */
    spdk_app_stop(0);
}

/* cleanup to do after termination of app */
static void cleanup(void) {
    /* Detach the controller after succesful termination */
    if (spdk_nvme_detach(nvme_ctrlr)) {
        fprintf(stderr, "Failed to detach controller %s\n", 
                spdk_nvme_ctrlr_get_transport_id(nvme_ctrlr)->traddr);
    } else {
        printf("Detached NVME driver %s\n",
                spdk_nvme_ctrlr_get_transport_id(nvme_ctrlr)->traddr);
    }
}

int main(int argc, char **argv) {
    /* Initialize the app event framework */
    spdk_app_opts_init(&app_opts);
    app_opts.reactor_mask = "0x4";
    /* Start the event app framework */
    if (spdk_app_start(&app_opts, hello_start, NULL)) {
        fprintf(stderr, "Failed to start app framework\n");
        return 1;
    }
    printf("app program successfully terminated\n");
    cleanup();
    return 0;
}
