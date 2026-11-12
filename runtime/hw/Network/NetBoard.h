// Net board is not needed by Sega Rally 2 (single-cabinet play): inert stub.
#ifndef INCLUDED_NETBOARD_H
#define INCLUDED_NETBOARD_H
#include "INetBoard.h"
#include "Util/NewConfig.h"
class CNetBoard : public INetBoard
{
public:
	CNetBoard(const Util::Config::Node &) {}
	void SaveState(CBlockFile *) {}
	void LoadState(CBlockFile *) {}
	void RunFrame(void) {}
	void Reset(void) {}
	bool IsAttached(void) { return false; }
	bool IsRunning(void) { return false; }
	Result Init(UINT8 *, UINT8 *) { return Result::OKAY; }
	void GetGame(const Game &) {}
	UINT8 ReadCommRAM8(unsigned) { return 0xFF; }
	UINT16 ReadCommRAM16(unsigned) { return 0xFFFF; }
	UINT32 ReadCommRAM32(unsigned) { return 0xFFFFFFFF; }
	void WriteCommRAM8(unsigned, UINT8) {}
	void WriteCommRAM16(unsigned, UINT16) {}
	void WriteCommRAM32(unsigned, UINT32) {}
	UINT16 ReadIORegister(unsigned) { return 0; }
	void WriteIORegister(unsigned, UINT16) {}
};
#endif
