#include "smp.h"
#include "task.h"

static struct smp_cpu_info g_smp_cpus[ORTHOX_MAX_CPUS];
static uint32_t g_smp_cpu_count = 1;
static int g_bsp_cpu_index = 0;

void smp_init(struct limine_smp_response* response) {
    g_smp_cpu_count = 1;
    g_bsp_cpu_index = 0;

    if (!response || response->cpu_count == 0 || !response->cpus) {
        task_set_cpu_count(1);
        g_smp_cpus[0].cpu_index = 0;
        g_smp_cpus[0].processor_id = 0;
        g_smp_cpus[0].lapic_id = 0;
        g_smp_cpus[0].is_bsp = 1;
        return;
    }

    uint32_t cpu_count = (uint32_t)response->cpu_count;
    if (cpu_count > ORTHOX_MAX_CPUS) cpu_count = ORTHOX_MAX_CPUS;

    g_smp_cpu_count = cpu_count;
    task_set_cpu_count(cpu_count);

    for (uint32_t i = 0; i < cpu_count; i++) {
        struct limine_smp_info* cpu = response->cpus[i];
        g_smp_cpus[i].cpu_index = i;
        g_smp_cpus[i].processor_id = cpu ? cpu->processor_id : 0;
        g_smp_cpus[i].lapic_id = cpu ? cpu->lapic_id : 0;
        g_smp_cpus[i].is_bsp = (cpu && cpu->lapic_id == response->bsp_lapic_id) ? 1 : 0;
        if (g_smp_cpus[i].is_bsp) g_bsp_cpu_index = (int)i;
    }
}

uint32_t smp_get_cpu_count(void) {
    return g_smp_cpu_count;
}

int smp_get_bsp_cpu_index(void) {
    return g_bsp_cpu_index;
}

const struct smp_cpu_info* smp_get_cpu_info(uint32_t cpu_index) {
    if (cpu_index >= g_smp_cpu_count) return 0;
    return &g_smp_cpus[cpu_index];
}
