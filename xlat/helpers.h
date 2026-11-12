// Runtime services reached from translated code through a table (pinned register). GPLv3.
#pragma once
namespace xl {
enum Helper {
	H_LD8, H_LD16, H_LD32, H_LD64, H_ST8, H_ST16, H_ST32, H_ST64,
	H_SAFEPOINT, H_SC, H_RFI, H_TBREAD, H_MTSPR, H_CALL, H_MFCR, H_MTCRF, H_UNIMPL, H_MTFSF,
	H_FMA, H_DIVW, H_DIVWU,      // 32-bit hosts without VFPv4 / integer divide
	H_COUNT
};
}
