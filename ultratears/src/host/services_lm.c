/* lm: the system log manager. TOTK 1.4.3's SDK opens it unconditionally in nninitInitializeSdkModule
 * (nn::lm::Initialize -> CreateLogServiceByHipc). Command ids follow the public service description (libnx / Switchbrew:
 * lm OpenLogger = 0; ILogger Log = 0, SetDestination = 1). Log packets are accepted and dropped unless
 * TOTK_LM_LOG is set, which counts them. */
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "ipc.h"

static unsigned long g_packets;

static void logger(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0:   /* Log(buffer) */
        if (getenv("TOTK_LM_LOG") && (++g_packets % 100) == 1)
            fprintf(stderr, "[totk-lm] %lu log packets (%llu bytes last)\n", g_packets,
                    (unsigned long long)(rq->nx ? rq->x[0].size : rq->na ? rq->a[0].size : 0));
        return;
    case 1:   /* SetDestination(u32) */
        return;
    default: ipc_unimplemented(s, rq);
    }
}

static void lm(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0:   /* OpenLogger(pid) -> ILogger */
        ipc_out_object(s, rs, "lm:ILogger", logger, NULL, NULL);
        return;
    default: ipc_unimplemented(s, rq);
    }
}

void runtime_audio_register(void);  // audio_renderer.c
void host_services_register(void) {
    runtime_ipc_register("lm", lm, NULL, NULL);
    runtime_audio_register();
}
