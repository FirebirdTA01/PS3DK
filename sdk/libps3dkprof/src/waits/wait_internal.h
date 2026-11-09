#ifndef PS3DK_PROF_WAIT_RUNTIME_H
#define PS3DK_PROF_WAIT_RUNTIME_H
#include "../core/prof_internal.h"
#include <ps3dk/prof_wait.h>
#include <ps3dk/prof_wait_internal.h>
#define W_REC 96u
#define W_CONTROL 256u
#define W_CAPTURE 4096u
typedef struct wait_actor {
    uint64_t head, tail, seq, dropped, refused, nested;
    uint64_t frontier, written, last, final_drop, final_refused, final_nested, final_attempted;
    uint32_t generation, valid, ack, declared, saturated, final_flags, active, reserved;
    uint8_t pending[W_REC];
} wait_actor;
typedef struct wait_capture {
    ps3dk_prof_wait_config cfg;
    uint32_t allocated, stride, configured, frozen, status;
    uint64_t refused, unregistered, internal_excluded;
    uint32_t saturated;
} wait_capture;
_Static_assert(sizeof(wait_actor)<=W_CONTROL,"wait actor exceeds accounted control");
_Static_assert(sizeof(wait_capture)<=W_CAPTURE,"wait capture exceeds accounted control");
_Static_assert(sizeof(ps3dk_prof_wait_cookie)==32,"private wait cookie ABI");
#define p_wait_init ps3dk_prof__wait_init
#define p_wait_unwind ps3dk_prof__wait_unwind
#define p_wait_enroll ps3dk_prof__wait_enroll
#define p_wait_ack ps3dk_prof__wait_ack
#define p_wait_declared ps3dk_prof__wait_declared
#define p_wait_emit_ready ps3dk_prof__wait_emit_ready
#define p_wait_freeze ps3dk_prof__wait_freeze
#define p_wait_emit_status ps3dk_prof__wait_emit_status
#define p_wait_emit_chunk ps3dk_prof__wait_emit_chunk
#define p_wait_actor ps3dk_prof__wait_actor
#define p_wait_record ps3dk_prof__wait_record
#define p_wait_layout ps3dk_prof__wait_layout
#define p_wait_options ps3dk_prof__wait_options
extern ps3dk_prof_wait_config p_wait_options;
int p_wait_layout(ps3dk_prof_wait_config cfg, uint32_t *bytes);
int p_wait_init(prof *p);
void p_wait_unwind(prof *p);
void p_wait_enroll(prof *p,uint32_t incarnation,uint32_t generation);
void p_wait_ack(prof *p);
void p_wait_declared(prof *p,const tdecl *decl,uint32_t count);
int p_wait_emit_ready(prof *p);
void p_wait_freeze(prof *p);
int p_wait_emit_status(prof *p);
int p_wait_emit_chunk(prof *p,uint32_t type,const uint8_t *payload,size_t len);
wait_actor *p_wait_actor(wait_capture *w,uint32_t incarnation);
uint8_t *p_wait_record(wait_capture *w,wait_actor *a,uint64_t position);
#endif
