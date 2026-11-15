#include "hal_clock.h"
#include "rom/ets_sys.h"

void hal_clock_set_cpu_mhz(uint32_t freq_mhz)
{
    hal_clock_set_cpu_mhz_raw(freq_mhz);
    ets_update_cpu_frequency(freq_mhz);
}
