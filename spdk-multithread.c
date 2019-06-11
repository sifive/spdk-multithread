#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"

static struct spdk_env_opts opts;
static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, 
        struct spdk_nvme_ctrlr_opts *opts) {
	printf("Attaching to %s\n", trid->traddr);
	return true;
}

static bool attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
        struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts) {
    const struct spdk_nvme_ctrlr_data *data = spdk_nvme_ctrlr_get_data(ctrlr);
    printf("Number of namespaces is %d\n", data->nn);
    return true;
}

int main(int argc, char **argv) {
    spdk_env_opts_init(&opts);
    if(spdk_env_init(&opts)) {
        printf("unable to initialize SPDK env");
    }
    void *cb_ctx = malloc(100);
    if (spdk_nvme_probe(NULL, cb_ctx, probe_cb, attach_cb, NULL)) {
        printf("probe failed");
    }
    return 0;
}
