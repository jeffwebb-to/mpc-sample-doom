/*
 * Sound effects for DOOM on the Akai MPC Sample: a small mixer feeding the I2S DAC (AK4558)
 * through the raw ALSA PCM device, S32_LE stereo 44.1 kHz, with the stock app stopped.
 *
 * - DMX sound lumps (8-bit unsigned mono, rate in the header) are converted to 16-bit on
 *   first use and kept in sfxinfo_t.driver_data.
 * - The audio thread (SCHED_FIFO) mixes up to 16 voices with linear-interpolation
 *   resampling into one period and writes it with SNDRV_PCM_IOCTL_WRITEI_FRAMES; on an
 *   underrun (EPIPE) it prepares the device again. It never touches the screen.
 * - The stream stays open for the whole run (the DAC has a ~1 s unmute delay), writing
 *   silence when nothing plays.
 * - The speaker/headphone switches are left as the stock app set them, except that a
 *   speaker switched off while no headphones are plugged is switched on (and restored at
 *   exit), because the app may mute it when it shuts down.
 * - No music (DG_music_module is a stub).
 *
 * Copyright (C) 2026 Jeff Webb (jeffwebb-to). GPL-2.0, like the engine.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

#include "doomtype.h"
#include "i_sound.h"
#include "deh_str.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#define RATE 44100
#define PERIOD 256                       /* frames per write: 5.8 ms */
#define NPERIODS 4
#define NVOICES 16

static void logs(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fputs("mpcdoom sound: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

typedef struct { uint32_t len, rate; int16_t data[]; } sample_t;

static struct voice {
    const sample_t *s;
    uint32_t pos;                        /* 16.16 */
    uint32_t step;                       /* 16.16: s->rate / RATE */
    int lvol, rvol;                      /* 0..255 */
    sfxinfo_t *sfx;
} voices[NVOICES];
static pthread_mutex_t vlock = PTHREAD_MUTEX_INITIALIZER;

/* config variables i_sound.c binds; they belong to the SDL module we do not build */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static int pcm_fd = -1, ctl_fd = -1;
static int pcm_channels = 2;             /* 2 on the device per the lab notes; the emulator's loopback takes 4 */
static int snd_card = -1;
static pthread_t audio_thread;
static volatile int audio_quit;
static boolean use_sfx_prefix;
static int speaker_was_off_and_we_turned_it_on;

/* ------------------------------------------------------------ ALSA card ---- */
static int find_card(void)
{
    /* the device and the lab both link /dev/snd/by-path/platform-sound to the output card */
    char target[128] = {0};
    ssize_t l = readlink("/dev/snd/by-path/platform-sound", target, sizeof target - 1);
    int c = -1;
    if (l > 0) { const char *q = strrchr(target, 'C'); if (q && sscanf(q, "C%d", &c) == 1) return c; }
    for (c = 0; c < 8; c++) {
        char p[64], id[64] = {0};
        snprintf(p, sizeof p, "/proc/asound/card%d/id", c);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (!fgets(id, sizeof id, f)) id[0] = 0;
        fclose(f);
        if (strcasestr(id, "i2s") || strcasestr(id, "rockchip") || strcasestr(id, "loopback")) return c;
    }
    return -1;
}

static int ctl_read(int iface, const char *name, long *val)
{
    struct snd_ctl_elem_value v; memset(&v, 0, sizeof v);
    v.id.iface = iface; strncpy((char *)v.id.name, name, sizeof v.id.name - 1);
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0) return -1;
    *val = v.value.integer.value[0];
    return 0;
}

static int ctl_write(int iface, const char *name, long val)
{
    struct snd_ctl_elem_value v; memset(&v, 0, sizeof v);
    v.id.iface = iface; strncpy((char *)v.id.name, name, sizeof v.id.name - 1);
    v.value.integer.value[0] = val;
    return ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0 ? -1 : 0;
}

static void switches_check(void)
{
    char p[32]; snprintf(p, sizeof p, "/dev/snd/controlC%d", snd_card);
    ctl_fd = open(p, O_RDWR | O_CLOEXEC);
    if (ctl_fd < 0) { logs("%s: %s", p, strerror(errno)); return; }
    long spk = -1, hp = -1, line = -1;
    ctl_read(SNDRV_CTL_ELEM_IFACE_MIXER, "Builtin Speaker Switch", &spk);
    ctl_read(SNDRV_CTL_ELEM_IFACE_CARD, "Headphones Jack", &hp);
    ctl_read(SNDRV_CTL_ELEM_IFACE_CARD, "Line Out Jack", &line);
    logs("speaker %ld, headphones jack %ld, line out jack %ld", spk, hp, line);
    if (spk == 0 && hp == 0) {
        if (ctl_write(SNDRV_CTL_ELEM_IFACE_MIXER, "Builtin Speaker Switch", 1) == 0) { speaker_was_off_and_we_turned_it_on = 1; logs("speaker switched on"); }
        else logs("speaker switch write: %s", strerror(errno));
    }
}

static void switches_restore(void)
{
    if (ctl_fd < 0) return;
    if (speaker_was_off_and_we_turned_it_on) ctl_write(SNDRV_CTL_ELEM_IFACE_MIXER, "Builtin Speaker Switch", 0);
    close(ctl_fd); ctl_fd = -1;
}

static void hw_set_mask(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
    struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m->bits, 0, sizeof m->bits);
    m->bits[bit >> 5] = 1u << (bit & 31);
}

static void hw_set_int(struct snd_pcm_hw_params *p, int n, unsigned v)
{
    struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = v; i->integer = 1;
}

static int pcm_try(int channels)
{
    struct snd_pcm_hw_params hw; memset(&hw, 0, sizeof hw);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
        memset(hw.masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits, 0xff, sizeof hw.masks[0].bits);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++)
        hw.intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = ~0u;
    hw.rmask = ~0u; hw.info = ~0u;
    hw_set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S32_LE);
    hw_set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    hw_set_int(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, channels);
    hw_set_int(&hw, SNDRV_PCM_HW_PARAM_RATE, RATE);
    hw_set_int(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
    hw_set_int(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, NPERIODS * PERIOD);
    return ioctl(pcm_fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw);
}

static int pcm_open(void)
{
    char p[32]; snprintf(p, sizeof p, "/dev/snd/pcmC%dD0p", snd_card);
    pcm_fd = open(p, O_RDWR | O_CLOEXEC);
    if (pcm_fd < 0) { logs("%s: %s%s", p, strerror(errno), errno == EBUSY ? " (is ac50 still running?)" : ""); return -1; }
    static const int tries[] = { 2, 4 };
    int ok = -1;
    for (size_t i = 0; i < sizeof tries / sizeof tries[0] && ok < 0; i++) {
        if (pcm_try(tries[i]) == 0) { pcm_channels = tries[i]; ok = 0; }
        else logs("hw_params with %d channels: %s", tries[i], strerror(errno));
    }
    if (ok < 0 || ioctl(pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { logs("pcm setup failed: %s", strerror(errno)); close(pcm_fd); pcm_fd = -1; return -1; }
    logs("pcm %s: S32_LE %dch %d Hz, period %d, buffer %d", p, pcm_channels, RATE, PERIOD, NPERIODS * PERIOD);
    return 0;
}

/* ---------------------------------------------------------------- mixer ---- */
static void mix(int32_t *out, int frames)
{
    static float acc[2 * PERIOD];
    memset(acc, 0, sizeof(float) * 2 * frames);
    pthread_mutex_lock(&vlock);
    for (int v = 0; v < NVOICES; v++) {
        struct voice *vc = &voices[v];
        if (!vc->s) continue;
        const sample_t *s = vc->s;
        float lg = vc->lvol / 255.0f, rg = vc->rvol / 255.0f;
        for (int i = 0; i < frames; i++) {
            uint32_t idx = vc->pos >> 16;
            if (idx + 1 >= s->len) { vc->s = NULL; break; }
            float frac = (vc->pos & 0xFFFF) / 65536.0f;
            float x = (s->data[idx] * (1.0f - frac) + s->data[idx + 1] * frac) / 32768.0f;
            acc[2 * i] += x * lg; acc[2 * i + 1] += x * rg;
            vc->pos += vc->step;
        }
    }
    pthread_mutex_unlock(&vlock);
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < pcm_channels; c++) {
            float x = c < 2 ? acc[2 * i + c] * 0.7f : 0.0f;
            if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
            out[i * pcm_channels + c] = (int32_t)(x * 2147483392.0f);
        }
    }
}

static void *audio_main(void *arg)
{
    (void)arg;
    struct sched_param sp = { .sched_priority = 40 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) logs("SCHED_FIFO not available, running normal priority");
    static int32_t buf[4 * PERIOD];
    unsigned underruns = 0, periods = 0;
    int32_t peak = 0;
    const char *dbg = getenv("MPCDOOM_DEBUG");
    int debug = dbg && *dbg;
    while (!audio_quit) {
        mix(buf, PERIOD);
        if (debug) {
            for (int i = 0; i < pcm_channels * PERIOD; i++) { int32_t a = buf[i] < 0 ? -buf[i] : buf[i]; if (a > peak) peak = a; }
            if (++periods >= RATE / PERIOD * 5) { logs("peak %.1f%% over 5 s", peak / 21474836.47); peak = 0; periods = 0; }
        }
        struct snd_xferi x = { .buf = buf, .frames = PERIOD };
        while (ioctl(pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
            if (errno == EINTR) { if (audio_quit) break; continue; }
            if (errno != EPIPE || ioctl(pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { logs("write: %s", strerror(errno)); audio_quit = 1; break; }
            if (++underruns <= 5 || underruns % 100 == 0) logs("underrun #%u", underruns);
        }
    }
    return NULL;
}

/* -------------------------------------------------------- sound module ---- */
static void lump_name(sfxinfo_t *sfx, char *buf, size_t n)
{
    if (sfx->link) sfx = sfx->link;
    if (use_sfx_prefix) M_snprintf(buf, n, "ds%s", DEH_String(sfx->name));
    else M_StringCopy(buf, DEH_String(sfx->name), n);
}

static const sample_t *cache(sfxinfo_t *sfx)
{
    if (sfx->driver_data) return sfx->driver_data;
    int lumpnum = sfx->lumpnum;
    if (lumpnum < 0) return NULL;
    byte *data = W_CacheLumpNum(lumpnum, PU_STATIC);
    unsigned lumplen = W_LumpLength(lumpnum);
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00) { W_ReleaseLumpNum(lumpnum); return NULL; }
    unsigned rate = (data[3] << 8) | data[2];
    unsigned length = (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];
    if (length > lumplen - 8 || length <= 48 || rate < 4000 || rate > 48000) { W_ReleaseLumpNum(lumpnum); return NULL; }
    /* DMX skips 16 bytes of padding at each end of the sample data */
    const byte *pcm = data + 8 + 16; length -= 32;
    sample_t *s = malloc(sizeof *s + sizeof(int16_t) * (length + 1));
    s->len = length; s->rate = rate;
    for (unsigned i = 0; i < length; i++) s->data[i] = (int16_t)((pcm[i] - 128) << 8);
    s->data[length] = 0;
    W_ReleaseLumpNum(lumpnum);
    sfx->driver_data = s;
    return s;
}

static boolean I_MPC_InitSound(boolean _use_sfx_prefix)
{
    use_sfx_prefix = _use_sfx_prefix;
    snd_card = find_card();
    if (snd_card < 0) { logs("no I2S sound card found"); return false; }
    switches_check();
    if (pcm_open() < 0) { switches_restore(); return false; }
    audio_quit = 0;
    if (pthread_create(&audio_thread, NULL, audio_main, NULL) != 0) { logs("audio thread: %s", strerror(errno)); close(pcm_fd); pcm_fd = -1; switches_restore(); return false; }
    return true;
}

static void I_MPC_ShutdownSound(void)
{
    if (pcm_fd < 0) return;
    audio_quit = 1;
    pthread_join(audio_thread, NULL);
    ioctl(pcm_fd, SNDRV_PCM_IOCTL_DROP);
    close(pcm_fd); pcm_fd = -1;
    switches_restore();
}

static int I_MPC_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];
    lump_name(sfx, namebuf, sizeof namebuf);
    return W_GetNumForName(namebuf);
}

static void I_MPC_UpdateSound(void) { }

static void I_MPC_UpdateSoundParams(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NVOICES) return;
    int left = ((254 - sep) * vol) / 127, right = (sep * vol) / 127;
    if (left < 0) left = 0; else if (left > 255) left = 255;
    if (right < 0) right = 0; else if (right > 255) right = 255;
    pthread_mutex_lock(&vlock);
    voices[channel].lvol = left; voices[channel].rvol = right;
    pthread_mutex_unlock(&vlock);
}

static int I_MPC_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    if (pcm_fd < 0 || channel < 0 || channel >= NVOICES) return -1;
    const sample_t *s = cache(sfx);
    if (!s) return -1;
    pthread_mutex_lock(&vlock);
    voices[channel].s = s; voices[channel].pos = 0;
    voices[channel].step = (uint32_t)(((uint64_t)s->rate << 16) / RATE);
    voices[channel].sfx = sfx;
    pthread_mutex_unlock(&vlock);
    I_MPC_UpdateSoundParams(channel, vol, sep);
    return channel;
}

static void I_MPC_StopSound(int channel)
{
    if (channel < 0 || channel >= NVOICES) return;
    pthread_mutex_lock(&vlock);
    voices[channel].s = NULL;
    pthread_mutex_unlock(&vlock);
}

static boolean I_MPC_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= NVOICES) return false;
    return voices[channel].s != NULL;
}

static void I_MPC_PrecacheSounds(sfxinfo_t *sounds, int num_sounds) { (void)sounds; (void)num_sounds; }

static snddevice_t sound_devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };

sound_module_t DG_sound_module = {
    sound_devices, arrlen(sound_devices),
    I_MPC_InitSound, I_MPC_ShutdownSound, I_MPC_GetSfxLumpNum, I_MPC_UpdateSound,
    I_MPC_UpdateSoundParams, I_MPC_StartSound, I_MPC_StopSound, I_MPC_SoundIsPlaying,
    I_MPC_PrecacheSounds,
};

/* ------------------------------------------------- music: none for now ---- */
static boolean M_Init(void) { return true; }
static void M_Shutdown(void) { }
static void M_SetVolume(int v) { (void)v; }
static void M_Pause(void) { }
static void M_Resume(void) { }
static void *M_Register(void *d, int n) { (void)d; (void)n; return NULL; }
static void M_Unregister(void *h) { (void)h; }
static void M_Play(void *h, boolean loop) { (void)h; (void)loop; }
static void M_Stop(void) { }
static boolean M_Playing(void) { return false; }
static void M_Poll(void) { }
static snddevice_t music_devices[] = { SNDDEVICE_ADLIB, SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_GENMIDI, SNDDEVICE_AWE32 };
music_module_t DG_music_module = {
    music_devices, arrlen(music_devices),
    M_Init, M_Shutdown, M_SetVolume, M_Pause, M_Resume, M_Register, M_Unregister, M_Play, M_Stop, M_Playing, M_Poll,
};
