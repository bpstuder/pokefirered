#include "global.h"
#include "task.h"
#include "trig.h"
#include "scanline_effect.h"
#ifdef PLATFORM_NATIVE
#include "gfx.h"
#endif

extern u16 gBattle_BG0_X;
extern u16 gBattle_BG0_Y;
extern u16 gBattle_BG1_X;
extern u16 gBattle_BG1_Y;
extern u16 gBattle_BG2_X;
extern u16 gBattle_BG2_Y;
extern u16 gBattle_BG3_X;
extern u16 gBattle_BG3_Y;

static void CopyValue16Bit(void);
static void CopyValue32Bit(void);

#ifdef PLATFORM_NATIVE
// [Phase 2] gScanlineEffect.dmaDest is always REG_ADDR_BG0HOFS + one of
// the SCANLINE_EFFECT_REG_* deltas (see scanline_effect.h) in this
// file's own use of the effect (ScanlineEffect_InitWave, below). It can
// resolve to HALGFX_REG_COUNT (invalid) here because
// battle_transition.c drives DMA0 directly with its own DmaSet calls
// instead of going through this file's abstraction — that's a
// separate, still-deferred Stage B slice (see ROADMAP.md Phase 2 Stage
// B), not a bug in this mapping.
static HalGfxReg DmaDestToHalGfxReg(const volatile void *dmaDest)
{
    u32 addr = (u32)dmaDest;

    if (addr == REG_ADDR_BG0HOFS) return HALGFX_REG_BG0HOFS;
    if (addr == REG_ADDR_BG0VOFS) return HALGFX_REG_BG0VOFS;
    if (addr == REG_ADDR_BG1HOFS) return HALGFX_REG_BG1HOFS;
    if (addr == REG_ADDR_BG1VOFS) return HALGFX_REG_BG1VOFS;
    if (addr == REG_ADDR_BG2HOFS) return HALGFX_REG_BG2HOFS;
    if (addr == REG_ADDR_BG2VOFS) return HALGFX_REG_BG2VOFS;
    if (addr == REG_ADDR_BG3HOFS) return HALGFX_REG_BG3HOFS;
    if (addr == REG_ADDR_BG3VOFS) return HALGFX_REG_BG3VOFS;
    return HALGFX_REG_COUNT;
}
#endif

// EWRAM vars

// Per-scanline register values.
// This is double buffered so that it can be safely written to at any time
// without overwriting the buffer that the DMA is currently reading
EWRAM_DATA u16 gScanlineEffectRegBuffers[2][0x3C0] = {0};

EWRAM_DATA struct ScanlineEffect gScanlineEffect = {0};
EWRAM_DATA static bool8 sShouldStopWaveTask = FALSE;

void ScanlineEffect_Stop(void)
{
    gScanlineEffect.state = 0;
    // [Phase 2] See docs/wiki/Hardware-Touchpoints.md §1 / ARCHITECTURE.md
#ifdef PLATFORM_NATIVE
    {
        HalGfxReg reg = DmaDestToHalGfxReg(gScanlineEffect.dmaDest);
        if (reg != HALGFX_REG_COUNT)
            HalGfx_ClearScanlineEffect(reg);
    }
#else
    DmaStop(0);
#endif
    if (gScanlineEffect.waveTaskId != 0xFF)
    {
        DestroyTask(gScanlineEffect.waveTaskId);
        gScanlineEffect.waveTaskId = 0xFF;
    }
}

void ScanlineEffect_Clear(void)
{
    CpuFill16(0, gScanlineEffectRegBuffers, sizeof(gScanlineEffectRegBuffers));
    gScanlineEffect.dmaSrcBuffers[0] = NULL;
    gScanlineEffect.dmaSrcBuffers[1] = NULL;
    gScanlineEffect.dmaDest = NULL;
    gScanlineEffect.dmaControl = 0;
    gScanlineEffect.srcBuffer = 0;
    gScanlineEffect.state = 0;
    gScanlineEffect.unused16 = 0;
    gScanlineEffect.unused17 = 0;
    gScanlineEffect.waveTaskId = 0xFF;
}

void ScanlineEffect_SetParams(struct ScanlineEffectParams params)
{
    if (params.dmaControl == SCANLINE_EFFECT_DMACNT_16BIT)  // 16-bit
    {
        // Set the DMA src to the value for the second scanline because the
        // first DMA transfer occurs in HBlank *after* the first scanline is drawn
        gScanlineEffect.dmaSrcBuffers[0] = (u16 *)gScanlineEffectRegBuffers[0] + 1;
        gScanlineEffect.dmaSrcBuffers[1] = (u16 *)gScanlineEffectRegBuffers[1] + 1;
        gScanlineEffect.setFirstScanlineReg = CopyValue16Bit;
    }
    else  // assume 32-bit
    {
        // Set the DMA src to the value for the second scanline because the
        // first DMA transfer occurs in HBlank *after* the first scanline is drawn
        gScanlineEffect.dmaSrcBuffers[0] = (u32 *)gScanlineEffectRegBuffers[0] + 1;
        gScanlineEffect.dmaSrcBuffers[1] = (u32 *)gScanlineEffectRegBuffers[1] + 1;
        gScanlineEffect.setFirstScanlineReg = CopyValue32Bit;
    }

    gScanlineEffect.dmaControl = params.dmaControl;
    gScanlineEffect.dmaDest    = params.dmaDest;
    gScanlineEffect.state      = params.initState;
    gScanlineEffect.unused16   = params.unused9;
    gScanlineEffect.unused17   = params.unused9;
}

void ScanlineEffect_InitHBlankDmaTransfer(void)
{
    if (gScanlineEffect.state == 0)
    {
        return;
    }
    else if (gScanlineEffect.state == 3)
    {
        gScanlineEffect.state = 0;
        // [Phase 2] See docs/wiki/Hardware-Touchpoints.md §1 / ARCHITECTURE.md
#ifdef PLATFORM_NATIVE
        {
            HalGfxReg reg = DmaDestToHalGfxReg(gScanlineEffect.dmaDest);
            if (reg != HALGFX_REG_COUNT)
                HalGfx_ClearScanlineEffect(reg);
        }
#else
        DmaStop(0);
#endif
        sShouldStopWaveTask = TRUE;
    }
    else
    {
        // [Phase 2] See docs/wiki/Hardware-Touchpoints.md §1 / ARCHITECTURE.md.
        // Native reads the per-scanline table starting at scanline 0
        // directly (HalGfx_SetScanlineEffect below), so it doesn't need
        // the GBA-hardware-timing workaround of manually poking the
        // first scanline's value separately — that trick exists only
        // because real HBlank DMA can't fire before scanline 0 is
        // already drawn.
#ifdef PLATFORM_NATIVE
        {
            HalGfxReg reg = DmaDestToHalGfxReg(gScanlineEffect.dmaDest);
            if (reg != HALGFX_REG_COUNT)
            {
                // 160 visible scanlines — see gfx.h's HalGfx_SetScanlineEffect
                // doc comment; Phase 3 should double check this count/timing
                // against real hardware behavior when the renderer is built.
                HalGfx_SetScanlineEffect(reg, gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer], 160);
            }
        }
#else
        DmaStop(0);
        // Set DMA to copy to dest register on each HBlank for the next frame.
        // The HBlank DMA transfers do not occurr during VBlank, so the transfer
        // will begin on the HBlank after the first scanline
        DmaSet(0, gScanlineEffect.dmaSrcBuffers[gScanlineEffect.srcBuffer], gScanlineEffect.dmaDest, gScanlineEffect.dmaControl);
        // Manually set the reg for the first scanline
        gScanlineEffect.setFirstScanlineReg();
#endif
        // Swap current buffer
        gScanlineEffect.srcBuffer ^= 1;
    }
}

// These two functions are used to copy the register for the first scanline,
// depending whether it is a 16-bit register or a 32-bit register.

static void CopyValue16Bit(void)
{
    vu16 *dest = (vu16 *)gScanlineEffect.dmaDest;
    vu16 *src = (vu16 *)&gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer];

    *dest = *src;
}

static void CopyValue32Bit(void)
{
    vu32 *dest = (vu32 *)gScanlineEffect.dmaDest;
    vu32 *src = (vu32 *)&gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer];

    *dest = *src;
}

#define tStartLine            data[0]
#define tEndLine              data[1]
#define tWaveLength           data[2]
#define tSrcBufferOffset      data[3]
#define tFramesUntilMove      data[4]
#define tDelayInterval        data[5]
#define tRegOffset            data[6]
#define tApplyBattleBgOffsets data[7]

static void TaskFunc_UpdateWavePerFrame(u8 taskId)
{
    int value = 0;
    int i;
    int offset;

    if (sShouldStopWaveTask)
    {
        DestroyTask(taskId);
        gScanlineEffect.waveTaskId = 0xFF;
    }
    else
    {
        if (gTasks[taskId].tApplyBattleBgOffsets)
        {
            switch (gTasks[taskId].tRegOffset)
            {
            case SCANLINE_EFFECT_REG_BG0HOFS:
                value = gBattle_BG0_X;
                break;
            case SCANLINE_EFFECT_REG_BG0VOFS:
                value = gBattle_BG0_Y;
                break;
            case SCANLINE_EFFECT_REG_BG1HOFS:
                value = gBattle_BG1_X;
                break;
            case SCANLINE_EFFECT_REG_BG1VOFS:
                value = gBattle_BG1_Y;
                break;
            case SCANLINE_EFFECT_REG_BG2HOFS:
                value = gBattle_BG2_X;
                break;
            case SCANLINE_EFFECT_REG_BG2VOFS:
                value = gBattle_BG2_Y;
                break;
            case SCANLINE_EFFECT_REG_BG3HOFS:
                value = gBattle_BG3_X;
                break;
            case SCANLINE_EFFECT_REG_BG3VOFS:
                value = gBattle_BG3_Y;
                break;
            }
        }
        if (gTasks[taskId].tFramesUntilMove != 0)
        {
            gTasks[taskId].tFramesUntilMove--;
            offset = gTasks[taskId].tSrcBufferOffset + 320;
            for (i = gTasks[taskId].tStartLine; i < gTasks[taskId].tEndLine; i++)
            {
                gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i] = gScanlineEffectRegBuffers[0][offset] + value;
                offset++;
            }
        }
        else
        {
            gTasks[taskId].tFramesUntilMove = gTasks[taskId].tDelayInterval;
            offset = gTasks[taskId].tSrcBufferOffset + 320;
            for (i = gTasks[taskId].tStartLine; i < gTasks[taskId].tEndLine; i++)
            {
                gScanlineEffectRegBuffers[gScanlineEffect.srcBuffer][i] = gScanlineEffectRegBuffers[0][offset] + value;
                offset++;
            }

            // increment src buffer offset
            gTasks[taskId].tSrcBufferOffset++;
            if (gTasks[taskId].tSrcBufferOffset == gTasks[taskId].tWaveLength)
                gTasks[taskId].tSrcBufferOffset = 0;
        }
    }
}

static void GenerateWave(u16 *buffer, u8 frequency, u8 amplitude, u8 unused)
{
    u16 i = 0;
    u8 theta = 0;

    while (i < 256)
    {
        buffer[i] = (gSineTable[theta] * amplitude) / 256;
        theta += frequency;
        i++;
    }
}

// Initializes a background "wave" effect that affects scanlines startLine (inclusive) to endLine (exclusive).
// 'frequency' and 'amplitude' control the frequency and amplitude of the wave.
// 'delayInterval' controls how fast the wave travels up the screen. The wave will shift upwards one scanline every 'delayInterval'+1 frames.
// 'regOffset' is the offset of the video register to modify.
u8 ScanlineEffect_InitWave(u8 startLine, u8 endLine, u8 frequency, u8 amplitude, u8 delayInterval, u8 regOffset, bool8 applyBattleBgOffsets)
{
    int i;
    int offset;
    struct ScanlineEffectParams params;
    u8 taskId;

    ScanlineEffect_Clear();

    params.dmaDest = (void *)(REG_ADDR_BG0HOFS + regOffset);
    params.dmaControl = SCANLINE_EFFECT_DMACNT_16BIT;
    params.initState = 1;
    params.unused9 = 0;
    ScanlineEffect_SetParams(params);

    taskId = CreateTask(TaskFunc_UpdateWavePerFrame, 0);

    gTasks[taskId].tStartLine            = startLine;
    gTasks[taskId].tEndLine              = endLine;
    gTasks[taskId].tWaveLength           = 256 / frequency;
    gTasks[taskId].tSrcBufferOffset      = 0;
    gTasks[taskId].tFramesUntilMove      = delayInterval;
    gTasks[taskId].tDelayInterval        = delayInterval;
    gTasks[taskId].tRegOffset            = regOffset;
    gTasks[taskId].tApplyBattleBgOffsets = applyBattleBgOffsets;

    gScanlineEffect.waveTaskId = taskId;
    sShouldStopWaveTask = FALSE;

    GenerateWave(&gScanlineEffectRegBuffers[0][320], frequency, amplitude, endLine - startLine);

    offset = 320;
    for (i = startLine; i < endLine; i++)
    {
        gScanlineEffectRegBuffers[0][i] = gScanlineEffectRegBuffers[0][offset];
        gScanlineEffectRegBuffers[1][i] = gScanlineEffectRegBuffers[0][offset];
        offset++;
    }

    return taskId;
}
