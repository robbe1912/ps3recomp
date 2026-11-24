/* spu_job_dump.h - opt-in dump of the jobs of one SPU image.
 *
 * For each job that enters at one address, the dump holds the local store at
 * entry, each exit from the image span with the local store at the last exit,
 * and the bytes of each DMA transfer in time order. The dump is off by
 * default. The switch and the record format are in spu_job_dump.inc. */
#ifndef SPU_JOB_DUMP_H
#define SPU_JOB_DUMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

/* 0: the switch is not read yet. 1: the dump is on. -1: the dump is off. The
 * two hooks below read it on their hot paths. */
extern int g_spu_job_dump;

/* Admission probe of the owned-job path. A probe at the entry address starts
 * a job. A probe that leaves the image span stores an exit point. The next
 * entry, the select-workload address and the exit of the process close the
 * job. `pc` is the address that the admission test examines. */
void spu_job_dump_probe(const struct spu_context* ctx, uint32_t pc);

/* One DMA transfer that will move `size` bytes. `is_get` is the result of
 * mfc_is_get(cmd). `bytes` is the source of the copy: guest memory for a
 * get, local store for a put. */
void spu_job_dump_dma(const struct spu_context* ctx, uint32_t lsa, uint32_t ea,
                      uint32_t size, uint32_t cmd, int is_get, const uint8_t* bytes);

#ifdef __cplusplus
}
#endif

#endif /* SPU_JOB_DUMP_H */
