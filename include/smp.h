#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "limine.h"

struct smp_cpu_info {
    uint32_t cpu_index;
    uint32_t processor_id;
    uint32_t lapic_id;
    uint8_t is_bsp;
};

void smp_init(struct limine_smp_response* response);
uint32_t smp_get_cpu_count(void);
int smp_get_bsp_cpu_index(void);
const struct smp_cpu_info* smp_get_cpu_info(uint32_t cpu_index);

#endif
