#include "global.h"
#ifdef PLATFORM_NATIVE
#include "gfx.h"
#endif

#define GPU_REG_BUF_SIZE 0x60

#define GPU_REG_BUF(offset) (*(u16 *)(&sGpuRegBuffer[offset]))
#define GPU_REG(offset) (*(vu16 *)(REG_BASE + offset))

#define EMPTY_SLOT 0xFF

static u8 sGpuRegBuffer[GPU_REG_BUF_SIZE];
static u8 sGpuRegWaitingList[GPU_REG_BUF_SIZE];
static volatile bool8 sGpuRegBufferLocked;
static volatile bool8 sShouldSyncRegIE;
static vu16 sRegIE;

static void CopyBufferedValueToGpuReg(u8 regOffset);
static void SyncRegIE(void);
static void UpdateRegDispstatIntrBits(u16 regIE);

#ifdef PLATFORM_NATIVE
// [Phase 2] Maps a raw register byte offset (as used throughout this
// file) to the HAL's logical register ID — only regOffset values this
// file is ever called with need a case here; see gfx.h and
// docs/wiki/Hardware-Touchpoints.md §1 for the full register list.
static HalGfxReg GpuRegOffsetToHalGfxReg(u8 regOffset)
{
    switch (regOffset)
    {
        case REG_OFFSET_DISPCNT:   return HALGFX_REG_DISPCNT;
        case REG_OFFSET_DISPSTAT:  return HALGFX_REG_DISPSTAT;
        case REG_OFFSET_BG0CNT:    return HALGFX_REG_BG0CNT;
        case REG_OFFSET_BG1CNT:    return HALGFX_REG_BG1CNT;
        case REG_OFFSET_BG2CNT:    return HALGFX_REG_BG2CNT;
        case REG_OFFSET_BG3CNT:    return HALGFX_REG_BG3CNT;
        case REG_OFFSET_BG0HOFS:   return HALGFX_REG_BG0HOFS;
        case REG_OFFSET_BG0VOFS:   return HALGFX_REG_BG0VOFS;
        case REG_OFFSET_BG1HOFS:   return HALGFX_REG_BG1HOFS;
        case REG_OFFSET_BG1VOFS:   return HALGFX_REG_BG1VOFS;
        case REG_OFFSET_BG2HOFS:   return HALGFX_REG_BG2HOFS;
        case REG_OFFSET_BG2VOFS:   return HALGFX_REG_BG2VOFS;
        case REG_OFFSET_BG3HOFS:   return HALGFX_REG_BG3HOFS;
        case REG_OFFSET_BG3VOFS:   return HALGFX_REG_BG3VOFS;
        case REG_OFFSET_BG2PA:     return HALGFX_REG_BG2PA;
        case REG_OFFSET_BG2PB:     return HALGFX_REG_BG2PB;
        case REG_OFFSET_BG2PC:     return HALGFX_REG_BG2PC;
        case REG_OFFSET_BG2PD:     return HALGFX_REG_BG2PD;
        case REG_OFFSET_BG2X_L:    return HALGFX_REG_BG2X_L;
        case REG_OFFSET_BG2X_H:    return HALGFX_REG_BG2X_H;
        case REG_OFFSET_BG2Y_L:    return HALGFX_REG_BG2Y_L;
        case REG_OFFSET_BG2Y_H:    return HALGFX_REG_BG2Y_H;
        case REG_OFFSET_BG3PA:     return HALGFX_REG_BG3PA;
        case REG_OFFSET_BG3PB:     return HALGFX_REG_BG3PB;
        case REG_OFFSET_BG3PC:     return HALGFX_REG_BG3PC;
        case REG_OFFSET_BG3PD:     return HALGFX_REG_BG3PD;
        case REG_OFFSET_BG3X_L:    return HALGFX_REG_BG3X_L;
        case REG_OFFSET_BG3X_H:    return HALGFX_REG_BG3X_H;
        case REG_OFFSET_BG3Y_L:    return HALGFX_REG_BG3Y_L;
        case REG_OFFSET_BG3Y_H:    return HALGFX_REG_BG3Y_H;
        case REG_OFFSET_WIN0H:     return HALGFX_REG_WIN0H;
        case REG_OFFSET_WIN1H:     return HALGFX_REG_WIN1H;
        case REG_OFFSET_WIN0V:     return HALGFX_REG_WIN0V;
        case REG_OFFSET_WIN1V:     return HALGFX_REG_WIN1V;
        case REG_OFFSET_WININ:     return HALGFX_REG_WININ;
        case REG_OFFSET_WINOUT:    return HALGFX_REG_WINOUT;
        case REG_OFFSET_MOSAIC:    return HALGFX_REG_MOSAIC;
        case REG_OFFSET_BLDCNT:    return HALGFX_REG_BLDCNT;
        case REG_OFFSET_BLDALPHA:  return HALGFX_REG_BLDALPHA;
        case REG_OFFSET_BLDY:      return HALGFX_REG_BLDY;
        default:                   return HALGFX_REG_COUNT; // invalid
    }
}
#endif

void InitGpuRegManager(void)
{
	s32 i;

	for (i = 0; i < GPU_REG_BUF_SIZE; i++)
    {
		sGpuRegBuffer[i] = 0;
		sGpuRegWaitingList[i] = EMPTY_SLOT;
	}

	sGpuRegBufferLocked = FALSE;
	sShouldSyncRegIE = FALSE;
	sRegIE = 0;
}

static void CopyBufferedValueToGpuReg(u8 regOffset)
{
#ifdef PLATFORM_NATIVE
	HalGfx_WriteReg(GpuRegOffsetToHalGfxReg(regOffset), GPU_REG_BUF(regOffset));
#else
	if (regOffset == REG_OFFSET_DISPSTAT)
    {
		REG_DISPSTAT &= ~(DISPSTAT_HBLANK_INTR | DISPSTAT_VBLANK_INTR);
		REG_DISPSTAT |= GPU_REG_BUF(REG_OFFSET_DISPSTAT);
	}
	else
    {
		GPU_REG(regOffset) = GPU_REG_BUF(regOffset);
	}
#endif
}

void CopyBufferedValuesToGpuRegs(void)
{
	if (!sGpuRegBufferLocked)
    {
		s32 i;

		for (i = 0; i < GPU_REG_BUF_SIZE; i++)
        {
			u8 regOffset = sGpuRegWaitingList[i];
			if (regOffset == EMPTY_SLOT)
				return;
			CopyBufferedValueToGpuReg(regOffset);
			sGpuRegWaitingList[i] = EMPTY_SLOT;
		}
	}
}

void SetGpuReg(u8 regOffset, u16 value)
{
	if (regOffset < GPU_REG_BUF_SIZE)
	{
		u16 vcount;

		GPU_REG_BUF(regOffset) = value;
		vcount = REG_VCOUNT & 0xFF;

		if ((vcount >= 161 && vcount <= 225)
		 || (REG_DISPCNT & DISPCNT_FORCED_BLANK)) {
			CopyBufferedValueToGpuReg(regOffset);
		} else {
			s32 i;

			sGpuRegBufferLocked = TRUE;

			for (i = 0; i < GPU_REG_BUF_SIZE && sGpuRegWaitingList[i] != EMPTY_SLOT; i++) {
				if (sGpuRegWaitingList[i] == regOffset) {
					sGpuRegBufferLocked = FALSE;
					return;
				}
			}

			sGpuRegWaitingList[i] = regOffset;
			sGpuRegBufferLocked = FALSE;
		}
	}
}

u16 GetGpuReg(u8 regOffset)
{
#ifdef PLATFORM_NATIVE
	if (regOffset == REG_OFFSET_DISPSTAT)
		return HalGfx_ReadReg(HALGFX_REG_DISPSTAT);

	if (regOffset == REG_OFFSET_VCOUNT)
		return HalGfx_ReadReg(HALGFX_REG_VCOUNT);
#else
	if (regOffset == REG_OFFSET_DISPSTAT)
		return REG_DISPSTAT;

	if (regOffset == REG_OFFSET_VCOUNT)
		return REG_VCOUNT;
#endif

	return GPU_REG_BUF(regOffset);
}

void SetGpuRegBits(u8 regOffset, u16 mask)
{
	u16 regValue = GPU_REG_BUF(regOffset);
	SetGpuReg(regOffset, regValue | mask);
}

void ClearGpuRegBits(u8 regOffset, u16 mask)
{
	u16 regValue = GPU_REG_BUF(regOffset);
	SetGpuReg(regOffset, regValue & ~mask);
}

static void SyncRegIE(void)
{
	if (sShouldSyncRegIE) {
		u16 temp = REG_IME;
		REG_IME = 0;
		REG_IE = sRegIE;
		REG_IME = temp;
		sShouldSyncRegIE = FALSE;
	}
}

void EnableInterrupts(u16 mask)
{
	sRegIE |= mask;
	sShouldSyncRegIE = TRUE;
	SyncRegIE();
	UpdateRegDispstatIntrBits(sRegIE);
}

void DisableInterrupts(u16 mask)
{
	sRegIE &= ~mask;
	sShouldSyncRegIE = TRUE;
	SyncRegIE();
	UpdateRegDispstatIntrBits(sRegIE);
}

static void UpdateRegDispstatIntrBits(u16 regIE)
{
	u16 oldValue = GetGpuReg(REG_OFFSET_DISPSTAT) & (DISPSTAT_HBLANK_INTR | DISPSTAT_VBLANK_INTR);
	u16 newValue = 0;

	if (regIE & INTR_FLAG_VBLANK)
		newValue |= DISPSTAT_VBLANK_INTR;

	if (regIE & INTR_FLAG_HBLANK)
		newValue |= DISPSTAT_HBLANK_INTR;

	if (oldValue != newValue)
		SetGpuReg(REG_OFFSET_DISPSTAT, newValue);
}
