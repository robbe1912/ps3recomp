#include "rsx_capture_event.h"
#include <stdatomic.h>

static atomic_uint s_wanted;
static atomic_uint s_pending;

void rsx_capture_pad_configure(unsigned mask)
{
    atomic_store(&s_pending, 0);
    atomic_store(&s_wanted, mask);
}

void rsx_capture_pad_notify(unsigned mask)
{
    unsigned wanted = atomic_load(&s_wanted);
    if (wanted && (mask & wanted) == wanted) {
        unsigned empty = 0;
        atomic_compare_exchange_strong(&s_pending, &empty, mask);
    }
}

unsigned rsx_capture_pad_take(void)
{
    return atomic_exchange(&s_pending, 0);
}
