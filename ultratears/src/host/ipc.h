/* From MacNativeGames games/totk/runtime/ipc.h @b75be8c (MIT). Added: CMIF domains (objects in one session). */
/* Host-side Horizon IPC: HIPC message framing with CMIF/TIPC payloads, served in-process.
   The SDK client libraries run unchanged; services are host implementations registered by name. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define IPC_MAX_BUF 8
typedef struct { uint64_t addr, size; unsigned index; } IpcBuffer;

typedef struct {
    unsigned type, cmd;               // HIPC type (4/5/6/7 CMIF, >=16 TIPC) and command id
    int has_pid; uint64_t pid;
    uint32_t copy[8], move[8]; unsigned ncopy, nmove;
    IpcBuffer x[IPC_MAX_BUF], a[IPC_MAX_BUF], b[IPC_MAX_BUF], w[IPC_MAX_BUF], c[IPC_MAX_BUF];
    unsigned nx, na, nb, nw, nc;
    const uint8_t *data; uint32_t data_size;    // command arguments after the CMIF header
    uint32_t in_objects[8]; unsigned nin_objects; // domain requests: object ids passed in
} IpcRequest;

typedef struct {
    uint32_t result;
    uint8_t data[0x100]; uint32_t data_size;
    uint32_t copy[8], move[8]; unsigned ncopy, nmove;
    IpcBuffer xout[IPC_MAX_BUF]; unsigned nxout;  // out-pointer data placed by the service into client C buffers
    uint32_t out_objects[8]; unsigned nout_objects;  // domain responses: new object ids
} IpcResponse;

struct IpcSession;
typedef void (*IpcHandler)(struct IpcSession *s, const IpcRequest *rq, IpcResponse *rs);
#define IPC_MAX_OBJECTS 64
typedef struct IpcSession {
    const char *name; IpcHandler handler; void *state; void (*close)(void *state);
    struct IpcSession *domain;                     // the domain this object lives in (NULL: a plain session)
    struct IpcSession *objects[IPC_MAX_OBJECTS];   // a domain session: its objects by id (1 = its own interface)
} IpcSession;

/* Service registry: open(state out) creates per-session state for a named service. */
typedef void *(*IpcOpen)(void);
void runtime_ipc_register(const char *name, IpcHandler handler, IpcOpen open, void (*close)(void *));
uint32_t runtime_ipc_new_session(const char *name, IpcHandler handler, void *state, void (*close)(void *));
int runtime_ipc_svc(unsigned num, uint64_t *regs);   /* returns 1 when handled */

/* Response helpers */
void ipc_out(IpcResponse *rs, const void *p, uint32_t n);
#define IPC_OUT(rs, v) ipc_out((rs), &(v), sizeof(v))
#define IPC_ARG(rq, T, off) (*(const T *)ipc_arg((rq), (off), sizeof(T)))
const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t n);
void *ipc_guest(uint64_t addr, uint64_t size);   /* validated guest pointer */
_Noreturn void ipc_unimplemented(const IpcSession *s, const IpcRequest *rq);
/* A command that returns an interface: a new object id in a domain, a new session handle otherwise. */
void ipc_out_object(IpcSession *s, IpcResponse *rs, const char *name, IpcHandler handler, void *state, void (*close)(void *));

/* threads.c */
uint32_t runtime_handle_new(int session, void *obj);
void *runtime_handle_get(uint32_t h, int session);
uint32_t runtime_event_new(void);
uint32_t runtime_event_readable(uint32_t writable);
void runtime_event_signal(uint32_t writable);
void runtime_ipc_session_closed(void *session);
void runtime_ipc_tmem_closed(void *tmem);

/* Transfer memory created by svcCreateTransferMemory */
typedef struct { uint64_t addr, size; uint32_t perm; } IpcTransferMemory;

#define IPC_RESULT_UNKNOWN_COMMAND 0xf601u   /* sf: unknown command (module 10, description 123) */
