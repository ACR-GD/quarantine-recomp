// System layer: DOS/DPMI/BIOS emulation, VGA, ports, host frontend.
#pragma once
#include <stdio.h>
#include <stdint.h>

#define MAGIC_BASE    0xFFF00000u
#define MAGIC_FARRET  0xFFF00004u
#define MAGIC_IRQRET  0xFFF00008u
#define MAGIC_VEC(n)  (0xFFF10000u + ((n) << 4))   // default PM interrupt handlers

#define SEL_CODE 0x0168
#define SEL_DATA 0x0170
#define SEL_PSP  0x0024
#define SEL_ENV  0x002c

void rt_dump_state(FILE *f);
void rt_trace(uint32_t a);
void rt_trace_dump(FILE *f);
void rt_dispatch_tail_noret(uint32_t off);

void sys_init(int argc, char **argv);
void sys_shutdown(void);
void sys_magic(uint32_t a);
void sys_obj2_call(uint32_t off);
void sys_host_tick(void);
int sys_irq_waiting(void);
int sys_next_irq(void);
uint32_t sys_pm_vector(int vec);
uint32_t sel_base(uint16_t sel);
void cd_init(const char *audio_dir);
void cd_audio_start(int headless);
int cd_int2f(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx, uint16_t es);
void rt_deliver_int(uint32_t handler);
void snd_init(void);
void snd_start(int headless);
void snd_shutdown(void);
void cd_stats(void);
void pad_init(void);
void pad_poll(void);
