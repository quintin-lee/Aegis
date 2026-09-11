#define _POSIX_C_SOURCE 200809L
#include "aegis/common/error.h"
#include "aegis/common/result.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    /* Create simple error */
    aegis_error_t* e1 = NULL;
    assert(aegis_error_new(&e1, AEGIS_ERROR_NOMEM, "out of memory") == AEGIS_ERROR_NONE);
    assert(aegis_error_code(e1) == AEGIS_ERROR_NOMEM);
    assert(strstr(aegis_error_message(e1), "out of memory") != NULL);
    assert(aegis_error_cause(e1) == NULL);
    aegis_error_destroy(e1);

    /* Create error with cause chain */
    aegis_error_t* cause = NULL;
    aegis_error_t* err   = NULL;
    assert(aegis_error_new(&cause, AEGIS_ERROR_IO, "read failed") == AEGIS_ERROR_NONE);
    assert(aegis_error_new_cause(&err, AEGIS_ERROR_PROVIDER, cause, "upstream: %s",
                                 aegis_error_message(cause)) == AEGIS_ERROR_NONE);
    assert(aegis_error_cause(err) == cause);
    char buf[512];
    int  n = aegis_error_chain_snprintf(buf, sizeof(buf), err);
    assert(n > 0);
    assert(strstr(buf, "caused by:") != NULL);
    aegis_error_destroy(err);
    aegis_error_destroy(cause);

    /* Clone */
    aegis_error_t* clone = NULL;
    assert(aegis_error_new(&e1, AEGIS_ERROR_TIMEOUT, "timed out") == AEGIS_ERROR_NONE);
    assert(aegis_error_clone(e1, &clone) == AEGIS_ERROR_NONE);
    assert(aegis_error_code(clone) == aegis_error_code(e1));
    aegis_error_destroy(e1);
    aegis_error_destroy(clone);

    /* Result Ok */
    int             payload = 42;
    aegis_result_t* r       = aegis_result_create_ok(&payload);
    assert(r != NULL);
    assert(aegis_result_is_ok(r));
    assert(!aegis_result_is_err(r));
    assert(aegis_result_get(r) == &payload);
    aegis_result_destroy(r);

    /* Result Err */
    aegis_error_t* err2 = NULL;
    aegis_error_new(&err2, AEGIS_ERROR_INVALID, "bad arg");
    aegis_result_t* r2 = aegis_result_create_err(err2);
    assert(r2 != NULL);
    assert(aegis_result_is_err(r2));
    assert(aegis_result_err_get(r2) == err2);
    aegis_error_t* e3 = aegis_result_take_err(r2);
    assert(e3 == err2);
    aegis_error_destroy(e3);
    aegis_result_destroy(r2);

    /* Result from format */
    aegis_result_t* r3 = aegis_result_create_errf(AEGIS_ERROR_BUSY, "retry later");
    assert(r3 != NULL);
    assert(aegis_result_is_err(r3));
    aegis_result_destroy(r3);

    return 0;
}
