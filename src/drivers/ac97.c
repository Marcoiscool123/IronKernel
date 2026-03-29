/* IronKernel — ac97.c
   Intel ICH AC97 audio controller driver.
   Uses PCI I/O BARs (NAM = mixer registers, NABM = bus-master DMA).
   No IRQs — playback is polled.  Supports PCM 16-bit stereo / mono WAV.

   QEMU: add  -device AC97,audiodev=snd0  to the QEMU command line.
*/

#include "ac97.h"
#include "pci.h"
#include "vga.h"
#include "pit.h"
#include "fat32.h"
#include "../kernel/types.h"
#include "../kernel/sched.h"

/* ── PCI identifiers ─────────────────────────────────────────────── */
#define AC97_VENDOR  0x8086u   /* Intel */

static const uint16_t g_known_devids[] = {
    0x2415u,  /* ICH  82801AA */
    0x2425u,  /* ICH1 82801AB */
    0x2445u,  /* ICH2 82801BA */
    0x2485u,  /* ICH3 82801CA */
    0x24C5u,  /* ICH4 82801DB */
    0x24D5u,  /* ICH5 82801EB */
    0x266Eu,  /* ICH6 82801FB */
    0x27DEu,  /* ICH7 82801GB */
    0x0000u   /* sentinel */
};

/* ── NAM (Native Audio Mixer) I/O register offsets ──────────────── */
#define NAM_RESET          0x00u
#define NAM_MASTER_VOL     0x02u
#define NAM_HEADPHONE_VOL  0x04u
#define NAM_PCM_VOL        0x18u
#define NAM_EXT_AUDIO_ID   0x28u
#define NAM_EXT_AUDIO_STA  0x2Au
#define NAM_PCM_RATE       0x2Cu   /* Front DAC sample rate */

/* ── NABM (Native Audio Bus Master) I/O register offsets ─────────── */
/* PCM Out channel base = 0x10 */
#define PO_BDBAR   0x10u   /* BDL Base Address (32-bit) */
#define PO_CIV     0x14u   /* Current Index Value (8-bit) */
#define PO_LVI     0x15u   /* Last Valid Index (8-bit) */
#define PO_SR      0x16u   /* Status Register (16-bit) */
#define PO_PICB    0x18u   /* Position in Current Buffer (16-bit) */
#define PO_CR      0x1Bu   /* Control Register (8-bit) */

#define GLOB_CNT   0x2Cu   /* Global Control (32-bit) */
#define GLOB_STA   0x30u   /* Global Status (32-bit, bit8=primary codec ready) */

/* PCM CR bits */
#define CR_RPBM   0x01u    /* Run/Pause Bus Master */
#define CR_RR     0x02u    /* Reset Registers */

/* PCM SR bits */
#define SR_DCH    0x0001u  /* DMA Controller Halted */
#define SR_CELV   0x0002u  /* Current Equals Last Valid */
#define SR_LVBCI  0x0004u  /* Last Valid Buffer Completion Interrupt */
#define SR_BCIS   0x0008u  /* Buffer Completion Interrupt Status */
#define SR_FIFOE  0x0010u  /* FIFO Error */

/* Extended Audio bits */
#define EA_VRA    0x0001u  /* Variable Rate Audio */

/* ── BDL ─────────────────────────────────────────────────────────── */
#define BDL_SIZE         32
#define BDL_FLAG_IOC  0x8000u   /* Interrupt on Completion */
#define BDL_FLAG_BUP  0x4000u   /* Buffer Underrun Policy (fill silence) */

typedef struct __attribute__((packed)) {
    uint32_t addr;    /* 32-bit physical address of audio data */
    uint16_t count;   /* number of 16-bit samples (not bytes) */
    uint16_t flags;
} bdl_entry_t;

static bdl_entry_t g_bdl[BDL_SIZE] __attribute__((aligned(8)));

/* ── Shared audio buffers (BSS — no cost in kernel binary size) ──── */
/* PCM synthesis / mono-to-stereo conversion buffer.
   130000 stereo frames ≈ 3 s at 44100 Hz. */
#define PCM_BUF_FRAMES  130000u
static int16_t g_pcm_buf[PCM_BUF_FRAMES * 2u];

/* WAV file load buffer: 16 MB supports 10MB+ WAV files.
   BSS allocation — no cost in kernel binary/ISO size. */
#define WAV_BUF_SIZE  (16u * 1024u * 1024u)
static uint8_t g_wav_buf[WAV_BUF_SIZE];

/* ── Driver state ─────────────────────────────────────────────────── */
static int      g_ac97_present = 0;
static uint16_t g_nam_port     = 0;
static uint16_t g_nabm_port    = 0;
static uint32_t g_sample_rate  = 48000u;
static int      g_vra_enabled  = 0;
static uint32_t g_phase_const  = 0;   /* 2^32 / sample_rate */

static uint8_t  g_pci_bus, g_pci_dev, g_pci_fn;

/* ── I/O port helpers ────────────────────────────────────────────── */
static inline void outb(uint16_t p, uint8_t v)
{ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v)
{ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void outl_(uint16_t p, uint32_t v)
{ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p)
{ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p)
{ uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }

static void     nam_w16(uint8_t r, uint16_t v){ outw((uint16_t)(g_nam_port  + r), v); }
static uint16_t nam_r16(uint8_t r)            { return inw((uint16_t)(g_nam_port + r)); }

static void     nabm_w8 (uint8_t r, uint8_t  v){ outb((uint16_t)(g_nabm_port + r), v); }
static void     nabm_w16(uint8_t r, uint16_t v){ outw((uint16_t)(g_nabm_port + r), v); }
static void     nabm_w32(uint8_t r, uint32_t v){ outl_((uint16_t)(g_nabm_port + r), v); }
static uint8_t  nabm_r8 (uint8_t r)            { return inb((uint16_t)(g_nabm_port + r)); }
static uint16_t nabm_r16(uint8_t r)            { return inw((uint16_t)(g_nabm_port + r)); }

/* ── Sine table — 256 entries, amplitude = INT16_MAX (32767) ─────── */
/* sin(2π·i/256)·32767 for i = 0..255                                */
static const int16_t g_sin256[256] = {
    /* Q1: 0 → peak */
       0,  804, 1608, 2410, 3212, 4011, 4808, 5602,
    6393, 7179, 7962, 8739, 9512,10278,11039,11793,
   12539,13279,14010,14732,15446,16151,16846,17530,
   18204,18868,19520,20159,20787,21403,22005,22594,
   23170,23731,24279,24811,25330,25832,26319,26790,
   27245,27683,28105,28510,28898,29268,29621,29956,
   30273,30571,30852,31113,31356,31580,31785,31971,
   32137,32285,32412,32521,32609,32678,32728,32757,
    /* Q2: peak → 0 */
   32767,32757,32728,32678,32609,32521,32412,32285,
   32137,31971,31785,31580,31356,31113,30852,30571,
   30273,29956,29621,29268,28898,28510,28105,27683,
   27245,26790,26319,25832,25330,24811,24279,23731,
   23170,22594,22005,21403,20787,20159,19520,18868,
   18204,17530,16846,16151,15446,14732,14010,13279,
   12539,11793,11039,10278, 9512, 8739, 7962, 7179,
    6393, 5602, 4808, 4011, 3212, 2410, 1608,  804,
    /* Q3: 0 → trough */
       0, -804,-1608,-2410,-3212,-4011,-4808,-5602,
   -6393,-7179,-7962,-8739,-9512,-10278,-11039,-11793,
   -12539,-13279,-14010,-14732,-15446,-16151,-16846,-17530,
   -18204,-18868,-19520,-20159,-20787,-21403,-22005,-22594,
   -23170,-23731,-24279,-24811,-25330,-25832,-26319,-26790,
   -27245,-27683,-28105,-28510,-28898,-29268,-29621,-29956,
   -30273,-30571,-30852,-31113,-31356,-31580,-31785,-31971,
   -32137,-32285,-32412,-32521,-32609,-32678,-32728,-32757,
    /* Q4: trough → 0 */
   -32767,-32757,-32728,-32678,-32609,-32521,-32412,-32285,
   -32137,-31971,-31785,-31580,-31356,-31113,-30852,-30571,
   -30273,-29956,-29621,-29268,-28898,-28510,-28105,-27683,
   -27245,-26790,-26319,-25832,-25330,-24811,-24279,-23731,
   -23170,-22594,-22005,-21403,-20787,-20159,-19520,-18868,
   -18204,-17530,-16846,-16151,-15446,-14732,-14010,-13279,
   -12539,-11793,-11039,-10278, -9512, -8739, -7962, -7179,
    -6393, -5602, -4808, -4011, -3212, -2410, -1608,  -804
};

/* ── Synthesis helpers ────────────────────────────────────────────── */

/* Mix a sine tone at 'hz' into stereo buffer buf[nframes×2].
   amplitude 0..32767; *phase: 32-bit accumulator (top 8 bits = table index). */
static void synth_add(int16_t *buf, uint32_t nframes, uint32_t hz,
                      int16_t amplitude, uint32_t *phase)
{
    uint32_t inc = hz * g_phase_const;
    uint32_t ph  = *phase;
    for (uint32_t i = 0; i < nframes; i++) {
        int32_t s = ((int32_t)g_sin256[ph >> 24] * (int32_t)amplitude) >> 15;
        int32_t l = (int32_t)buf[i*2]   + s;
        int32_t r = (int32_t)buf[i*2+1] + s;
        buf[i*2]   = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        buf[i*2+1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        ph += inc;
    }
    *phase = ph;
}

/* Zero-fill nframes stereo samples. */
static void synth_silence(int16_t *buf, uint32_t nframes)
{
    uint32_t n = nframes * 2u;
    for (uint32_t i = 0; i < n; i++) buf[i] = 0;
}

/* Linear fade-in over the first fade_frames of buf. */
static void envelope_in(int16_t *buf, uint32_t nframes, uint32_t fade)
{
    if (fade > nframes) fade = nframes;
    for (uint32_t i = 0; i < fade; i++) {
        int32_t sc = (int32_t)(i * 256u) / (int32_t)fade;
        buf[i*2]   = (int16_t)((int32_t)buf[i*2]   * sc >> 8);
        buf[i*2+1] = (int16_t)((int32_t)buf[i*2+1] * sc >> 8);
    }
}

/* Linear fade-out over the last fade_frames of buf. */
static void envelope_out(int16_t *buf, uint32_t nframes, uint32_t fade)
{
    if (fade > nframes) fade = nframes;
    uint32_t start = nframes - fade;
    for (uint32_t i = 0; i < fade; i++) {
        int32_t sc = (int32_t)((fade - 1u - i) * 256u) / (int32_t)fade;
        uint32_t j = start + i;
        buf[j*2]   = (int16_t)((int32_t)buf[j*2]   * sc >> 8);
        buf[j*2+1] = (int16_t)((int32_t)buf[j*2+1] * sc >> 8);
    }
}

/* ── DMA playback ─────────────────────────────────────────────────── */
/* Handles arbitrarily large buffers by looping through multiple BDL
   batches (32 entries × 0x7FFF frames ≈ 4 MB each).  After each batch
   completes the channel is reset and re-armed with the next batch. */
int ac97_play_pcm(const int16_t *samples, uint32_t num_stereo_frames,
                  uint32_t sample_rate)
{
    if (!g_ac97_present) return -1;
    if (!num_stereo_frames) return 0;

    /* Update sample rate if VRA is available and rate changed */
    if (g_vra_enabled && sample_rate && sample_rate != g_sample_rate) {
        nam_w16(NAM_PCM_RATE, (uint16_t)sample_rate);
        g_sample_rate = nam_r16(NAM_PCM_RATE);
        g_phase_const = (uint32_t)(4294967296ULL / g_sample_rate);
    }

    /* Each BDL entry: max 0xFFFE 16-bit samples = 0x7FFF stereo frames */
    const uint32_t CHUNK   = 0xFFFEu / 2u;
    const uint32_t sr_used = g_sample_rate ? g_sample_rate : 48000u;

    uint32_t       remaining = num_stereo_frames;
    const int16_t *ptr       = samples;

    while (remaining > 0) {
        /* Stop and reset PCM Out channel */
        nabm_w8(PO_CR, 0);
        nabm_w8(PO_CR, CR_RR);
        { int t = 200000; while ((nabm_r8(PO_CR) & CR_RR) && t-- > 0); }
        nabm_w16(PO_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);

        /* Fill up to BDL_SIZE entries for this batch */
        int      nentries    = 0;
        uint32_t batch_frames = 0;

        while (remaining > 0 && nentries < BDL_SIZE) {
            uint32_t chunk = (remaining > CHUNK) ? CHUNK : remaining;
            g_bdl[nentries].addr  = (uint32_t)(uintptr_t)ptr;
            g_bdl[nentries].count = (uint16_t)(chunk * 2u);
            g_bdl[nentries].flags = 0;
            remaining     -= chunk;
            ptr           += chunk * 2u;
            batch_frames  += chunk;
            nentries++;
        }
        if (!nentries) break;
        g_bdl[nentries - 1].flags = BDL_FLAG_IOC | BDL_FLAG_BUP;

        nabm_w32(PO_BDBAR, (uint32_t)(uintptr_t)g_bdl);
        nabm_w8 (PO_LVI,   (uint8_t)(nentries - 1));
        nabm_w8 (PO_CR,    CR_RPBM);

        /* Poll until this batch completes or DMA halts.
           sched_yield() gives other tasks (WM) CPU time so the GUI
           stays responsive during playback. */
        uint32_t secs = batch_frames / sr_used + 4u;
        uint32_t tmax = pit_get_ticks() + secs * 100u;
        while (pit_get_ticks() < tmax) {
            uint16_t sr = nabm_r16(PO_SR);
            if (sr & (SR_LVBCI | SR_DCH)) break;
            sched_yield();
        }
    }

    nabm_w8(PO_CR, 0);
    return 0;
}

/* ── ac97_stop ────────────────────────────────────────────────────── */
void ac97_stop(void)
{
    if (!g_ac97_present) return;
    nabm_w8(PO_CR, 0);
    nabm_w8(PO_CR, CR_RR);
    { int t = 200000; while ((nabm_r8(PO_CR) & CR_RR) && t-- > 0); }
}

/* ── WAV parser ───────────────────────────────────────────────────── */
int ac97_play_wav(const uint8_t *wav, uint32_t wav_len)
{
    if (!g_ac97_present) return -1;
    if (wav_len < 44) return -1;

    /* RIFF header sanity check */
    if (wav[0]!='R'||wav[1]!='I'||wav[2]!='F'||wav[3]!='F') return -1;
    if (wav[8]!='W'||wav[9]!='A'||wav[10]!='V'||wav[11]!='E') return -1;

    uint16_t audio_fmt  = 0;
    uint16_t channels   = 0;
    uint32_t sample_rate = 0;
    uint16_t bits       = 0;
    const uint8_t *pcm  = 0;
    uint32_t pcm_len    = 0;

    /* Scan chunks */
    uint32_t pos = 12u;
    while (pos + 8u <= wav_len) {
        uint32_t cid = *(const uint32_t *)(wav + pos);
        uint32_t csz = *(const uint32_t *)(wav + pos + 4u);
        pos += 8u;
        if (pos + csz > wav_len) break;

        if (cid == 0x20746D66u && csz >= 16u) {   /* 'fmt ' */
            audio_fmt   = *(const uint16_t *)(wav + pos);
            channels    = *(const uint16_t *)(wav + pos + 2u);
            sample_rate = *(const uint32_t *)(wav + pos + 4u);
            bits        = *(const uint16_t *)(wav + pos + 14u);
        } else if (cid == 0x61746164u) {           /* 'data' */
            pcm     = wav + pos;
            pcm_len = csz;
            break;
        }
        pos += (csz + 1u) & ~1u;   /* word-align */
    }

    if (audio_fmt != 1)  { vga_print("[AC97] not PCM WAV\n");  return -1; }
    if (bits != 16)      { vga_print("[AC97] not 16-bit WAV\n"); return -1; }
    if (!pcm || !pcm_len){ vga_print("[AC97] no data chunk\n"); return -1; }

    if (channels == 1) {
        /* Mono → stereo upmix streamed in PCM_BUF_FRAMES chunks so that
           files of any size are handled without a second large buffer. */
        const int16_t *mono        = (const int16_t *)pcm;
        uint32_t       total_frames = pcm_len / 2u;   /* bytes / 2 = mono samples */
        while (total_frames > 0) {
            uint32_t chunk = (total_frames > PCM_BUF_FRAMES) ? PCM_BUF_FRAMES
                                                              : total_frames;
            for (uint32_t i = 0; i < chunk; i++) {
                g_pcm_buf[i*2]   = mono[i];
                g_pcm_buf[i*2+1] = mono[i];
            }
            int rc = ac97_play_pcm(g_pcm_buf, chunk, sample_rate);
            if (rc) return rc;
            mono        += chunk;
            total_frames -= chunk;
        }
        return 0;
    }

    /* Stereo: play directly from the WAV buffer — no frame cap */
    return ac97_play_pcm((const int16_t *)pcm, pcm_len / 4u, sample_rate);
}

/* ── FAT32 WAV loader ─────────────────────────────────────────────── */
int ac97_play_file(const char *name8, const char *ext3)
{
    if (!g_ac97_present) return -1;
    uint32_t bytes = 0;
    if (fat32_read_file(name8, ext3, g_wav_buf, WAV_BUF_SIZE, &bytes) != 0) {
        vga_print("[AC97] file not found\n");
        return -1;
    }
    return ac97_play_wav(g_wav_buf, bytes);
}

/* ── Boot chime — C-major arpeggiated chord ───────────────────────── */
/*  Phase 1 (0–150 ms):     C5 alone
    Phase 2 (150–300 ms):   C5 + E5
    Phase 3 (300–450 ms):   C5 + E5 + G5
    Phase 4 (450–600 ms):   C5 + E5 + G5 + C6 (full chord entry)
    Phase 5 (600–950 ms):   full chord sustained
    Phase 6 (last 150 ms):  fade out
*/
void ac97_boot_chime(void)
{
    if (!g_ac97_present) return;

    uint32_t SR     = g_sample_rate;
    uint32_t ms     = SR / 1000u;        /* frames per millisecond */

    /* Segment lengths in frames */
    uint32_t arp    = 150u * ms;         /* each arpeggio step */
    uint32_t hold   = 350u * ms;         /* sustained chord after arpeggio */
    uint32_t fade   = 150u * ms;         /* fade-out duration */
    uint32_t total  = arp * 4u + hold + fade;   /* 1500 ms × ms frames */

    if (total > PCM_BUF_FRAMES) total = PCM_BUF_FRAMES;

    synth_silence(g_pcm_buf, total);

    /* Note frequencies */
    uint32_t C5 = 523u, E5 = 659u, G5 = 784u, C6 = 1047u;
    /* Per-voice amplitude: 4 voices → max sum ≈ 28000 (safe) */
    int16_t amp = 7000;

    uint32_t t0 = 0,       p0 = 0;      /* C5 phase */
    uint32_t t1 = arp,     p1 = 0;      /* E5 enters at t1 */
    uint32_t t2 = arp*2u,  p2 = 0;      /* G5 enters at t2 */
    uint32_t t3 = arp*3u,  p3 = 0;      /* C6 enters at t3 */
    uint32_t t4 = arp*4u;               /* full chord hold start */

    /* Phase 1: C5 alone */
    synth_add(g_pcm_buf + t0*2u, arp, C5, amp, &p0);

    /* Phase 2: C5 + E5 */
    synth_add(g_pcm_buf + t1*2u, arp, C5, amp, &p0);
    synth_add(g_pcm_buf + t1*2u, arp, E5, amp, &p1);

    /* Phase 3: C5 + E5 + G5 */
    synth_add(g_pcm_buf + t2*2u, arp, C5, amp, &p0);
    synth_add(g_pcm_buf + t2*2u, arp, E5, amp, &p1);
    synth_add(g_pcm_buf + t2*2u, arp, G5, amp, &p2);

    /* Phase 4: full chord entry */
    synth_add(g_pcm_buf + t3*2u, arp, C5, amp, &p0);
    synth_add(g_pcm_buf + t3*2u, arp, E5, amp, &p1);
    synth_add(g_pcm_buf + t3*2u, arp, G5, amp, &p2);
    synth_add(g_pcm_buf + t3*2u, arp, C6, amp, &p3);

    /* Phase 5+6: full chord hold + fade */
    uint32_t chord_len = total - t4;
    synth_add(g_pcm_buf + t4*2u, chord_len, C5, amp, &p0);
    synth_add(g_pcm_buf + t4*2u, chord_len, E5, amp, &p1);
    synth_add(g_pcm_buf + t4*2u, chord_len, G5, amp, &p2);
    synth_add(g_pcm_buf + t4*2u, chord_len, C6, amp, &p3);

    /* Envelopes */
    envelope_in (g_pcm_buf, total, 50u  * ms);  /* 50 ms fade-in  */
    envelope_out(g_pcm_buf, total, fade);        /* 150 ms fade-out */

    ac97_play_pcm(g_pcm_buf, total, SR);
}

/* ── Boot WAV with chime fallback ────────────────────────────────── */
/* Try to load BOOT.WAV from FAT32 and play it.
   If absent or unreadable, fall back to the synthesised chime.
   No error output — silent failure is intentional here. */
void ac97_play_boot_wav(void)
{
    if (!g_ac97_present) return;
    uint32_t bytes = 0;
    /* fat32_read_file with 8-char space-padded name "BOOT    " ext "WAV" */
    if (fat32_read_file("BOOT    ", "WAV", g_wav_buf, WAV_BUF_SIZE, &bytes) == 0
        && bytes > 44u) {
        /* File found — play it (ac97_play_wav may print format errors but
           those are still useful to see if someone drops a bad file). */
        ac97_play_wav(g_wav_buf, bytes);
    } else {
        /* No BOOT.WAV — use the synthesised chord */
        ac97_boot_chime();
    }
}

/* ── ac97_init ────────────────────────────────────────────────────── */
void ac97_init(void)
{
    /* Probe known device IDs */
    const pci_device_t *dev = 0;
    for (int i = 0; g_known_devids[i]; i++) {
        dev = pci_find_device(AC97_VENDOR, g_known_devids[i]);
        if (dev) break;
    }
    if (!dev) {
        vga_print("[AC97] not found\n");
        return;
    }

    g_pci_bus = dev->bus;
    g_pci_dev = dev->dev;
    g_pci_fn  = dev->fn;

    /* Enable I/O space + Bus Mastering */
    uint32_t cmd = pci_read32(g_pci_bus, g_pci_dev, g_pci_fn, 0x04u);
    cmd |= 0x05u;   /* bit0 = I/O enable, bit2 = bus master */
    pci_write32(g_pci_bus, g_pci_dev, g_pci_fn, 0x04u, cmd);

    /* BAR0 = NAM I/O base, BAR1 = NABM I/O base */
    uint32_t bar0 = pci_read32(g_pci_bus, g_pci_dev, g_pci_fn, 0x10u);
    uint32_t bar1 = pci_read32(g_pci_bus, g_pci_dev, g_pci_fn, 0x14u);
    g_nam_port  = (uint16_t)(bar0 & ~0x3u);
    g_nabm_port = (uint16_t)(bar1 & ~0x3u);

    /* Cold reset: clear bit1 of GLOB_CNT, wait, then set it */
    nabm_w32(GLOB_CNT, 0x00000000u);
    { uint32_t t = pit_get_ticks() + 2u; while (pit_get_ticks() < t); }
    nabm_w32(GLOB_CNT, 0x00000002u);   /* exit cold reset */

    /* Wait for primary codec ready (GLOB_STA bit 8) */
    {
        int t = 2000000;
        while (!(nabm_r16(GLOB_STA) & 0x0100u) && t-- > 0)
            __asm__ volatile("pause");
    }

    /* Reset AC97 mixer (NAM reset register) */
    nam_w16(NAM_RESET, 0xFFFFu);
    { uint32_t t = pit_get_ticks() + 2u; while (pit_get_ticks() < t); }

    /* Maximum volume, no mute:
       AC97 volume regs: 6-bit attenuation per channel + bit15 mute.
       0x0000 = both channels at max, unmuted. */
    nam_w16(NAM_MASTER_VOL,    0x0000u);
    nam_w16(NAM_HEADPHONE_VOL, 0x0000u);
    nam_w16(NAM_PCM_VOL,       0x0000u);

    /* Check VRA capability */
    uint16_t ext_id = nam_r16(NAM_EXT_AUDIO_ID);
    if (ext_id & EA_VRA) {
        uint16_t ext_sta = nam_r16(NAM_EXT_AUDIO_STA);
        nam_w16(NAM_EXT_AUDIO_STA, (uint16_t)(ext_sta | EA_VRA));
        g_vra_enabled = 1;
        /* Prefer 44100 Hz */
        nam_w16(NAM_PCM_RATE, 44100u);
        g_sample_rate = nam_r16(NAM_PCM_RATE);
    } else {
        g_sample_rate = 48000u;
    }

    g_phase_const  = (uint32_t)(4294967296ULL / g_sample_rate);
    g_ac97_present = 1;

    /* Print status */
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print("[AC97] Intel AC97 audio  ");
    /* print sample rate */
    {
        uint32_t sr = g_sample_rate;
        char buf[6]; int pos = 5; buf[5] = '\0';
        if (!sr) sr = 48000u;
        do { buf[--pos] = (char)('0' + sr % 10); sr /= 10; } while (sr && pos > 0);
        vga_print(buf + pos);
    }
    vga_print(" Hz");
    if (g_vra_enabled) vga_print(" VRA");
    vga_print("\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

int ac97_detected(void) { return g_ac97_present; }
