// CPU interface used by the Model 3 hardware code. Implemented by the
// recompiled-guest runtime (runtime/cpu_glue.cpp), NOT by an interpreter.
#ifndef INCLUDED_PPC_H
#define INCLUDED_PPC_H
#include "Types.h"
#include "BlockFile.h"

typedef enum {
	BUS_FREQUENCY_16MHZ = 0, BUS_FREQUENCY_20MHZ, BUS_FREQUENCY_25MHZ, BUS_FREQUENCY_33MHZ,
	BUS_FREQUENCY_40MHZ, BUS_FREQUENCY_50MHZ, BUS_FREQUENCY_60MHZ, BUS_FREQUENCY_66MHZ, BUS_FREQUENCY_75MHZ,
} PPC_BUS_FREQUENCY;
typedef struct { int pvr; int bus_frequency_multiplier; PPC_BUS_FREQUENCY bus_frequency; } PPC_CONFIG;
typedef struct { UINT32 start; UINT32 end; UINT32 *ptr; } PPC_FETCH_REGION;

#define PPC_MODEL_603E 0x00060103
#define PPC_MODEL_603EV 0x00070000
#define PPC_MODEL_603R 0x00071202

extern UINT32 ppc_get_pc(void);
extern UINT32 ppc_get_lr(void);
extern void ppc_set_irq_line(int irqline);
extern void ppc_reset(void);
extern int ppc_execute(int cycles);
extern void ppc_shutdown(void);
extern void ppc_init(const PPC_CONFIG *config);
extern void ppc_set_fetch(PPC_FETCH_REGION *fetch);
extern UINT64 ppc_total_cycles(void);
extern int ppc_get_cycles_per_sec(void);
extern int ppc_get_bus_freq_multipler(void);
extern int ppc_get_timer_ratio(void);
extern void ppc_set_timer_ratio(int ratio);
extern void ppc_attach_bus(class IBus *BusPtr);
extern void ppc_save_state(CBlockFile *SaveState);
extern void ppc_load_state(CBlockFile *SaveState);
#endif
