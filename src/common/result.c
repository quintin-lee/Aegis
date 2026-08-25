#include "aegis/common/result.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct aegis_result {
    int            ok;      /* 1 = Ok, 0 = Err */
    void*          payload; /* valid when ok */
    aegis_error_t* error;   /* valid when !ok */
};

aegis_result_t* aegis_result_create_ok(void* payload)
{
    aegis_result_t* r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->ok      = 1;
    r->payload = payload;
    return r;
}

aegis_result_t* aegis_result_create_err(aegis_error_t* err)
{
    aegis_result_t* r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->ok    = 0;
    r->error = err;
    return r;
}

aegis_result_t* aegis_result_create_errf(aegis_err_t code, const char* fmt, ...)
{
    aegis_error_t* err = NULL;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        aegis_error_new(&err, code, fmt, ap);
        va_end(ap);
    } else {
        aegis_error_new(&err, code, NULL);
    }
    if (!err) {
        return NULL;
    }
    return aegis_result_create_err(err);
}

bool aegis_result_is_ok(const aegis_result_t* r)
{
    return r && r->ok;
}

bool aegis_result_is_err(const aegis_result_t* r)
{
    return r && !r->ok;
}

void* aegis_result_get(const aegis_result_t* r)
{
    return r && r->ok ? r->payload : NULL;
}

const aegis_error_t* aegis_result_err_get(const aegis_result_t* r)
{
    return r && !r->ok ? r->error : NULL;
}

aegis_error_t* aegis_result_take_err(aegis_result_t* r)
{
    if (!r || r->ok) {
        return NULL;
    }
    aegis_error_t* e = r->error;
    r->error         = NULL;
    r->ok            = 1;
    return e;
}

void aegis_result_destroy(aegis_result_t* r)
{
    if (!r) {
        return;
    }
    if (!r->ok && r->error) {
        aegis_error_destroy(r->error);
        r->error = NULL;
    }
    r->ok = -1;
    free(r);
}
