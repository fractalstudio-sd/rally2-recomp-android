#ifndef INCLUDED_M68KDEBUG_SHIM_H
#define INCLUDED_M68KDEBUG_SHIM_H
#include "Types.h"
namespace Debugger { class CMusashi68KDebug {
public:
	void CPUActive() {} void CPUInactive() {}
	bool CPUExecute(UINT32, UINT32, int) { return false; }
	void CPUException(int) {} void CPUInterrupt(int) {}
}; }
#endif
