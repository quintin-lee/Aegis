#include "aegis/common/time.h"
#include <assert.h>

int main(void)
{
    aegis_mono_ns_t t1 = aegis_mono_now();
    aegis_sleep_ms(50);
    aegis_mono_ns_t t2      = aegis_mono_now();
    int64_t         elapsed = aegis_mono_elapsed(t1, t2);
    assert(elapsed >= 40000000LL);  /* at least 40ms */
    assert(elapsed < 2000000000LL); /* less than 2s */

    aegis_wall_ns_t w1 = aegis_wall_now();
    aegis_sleep_ns(10000000ULL); /* 10ms */
    aegis_wall_ns_t w2 = aegis_wall_now();
    assert(w2 >= w1);

    /* Conversions */
    assert(aegis_ns_to_us(1000) == 1);
    assert(aegis_ns_to_ms(1000000) == 1);
    assert(aegis_ns_to_sec(1000000000.0) == 1.0);

    return 0;
}
