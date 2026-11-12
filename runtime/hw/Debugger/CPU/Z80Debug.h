#ifndef INCLUDED_Z80DEBUG_SHIM_H
#define INCLUDED_Z80DEBUG_SHIM_H
#include "Types.h"
class IBus;
namespace Debugger { class CZ80Debug {
public:
	void CPUActive() {} void CPUInactive() {}
	bool CPUExecute(UINT16, UINT8, int) { return false; }
	void CPUException(int) {}
	IBus *AttachBus(IBus *b) { return b; } IBus *DetachBus() { return nullptr; }
}; }
#endif
