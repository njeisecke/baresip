/**
 * @file winwave/src.c Windows sound driver -- source
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 */
#include <re.h>
#include <rem.h>
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <baresip.h>
#include "winwave.h"


#define READ_BUFFERS   4
#define INC_RPOS(a) ((a) = (((a) + 1) % READ_BUFFERS))


struct ausrc_st {
	struct dspbuf bufs[READ_BUFFERS];
	int pos;
	HWAVEIN wavein;
	volatile bool rdy;
	size_t inuse;
	size_t sampsz;
	struct ausrc_prm prm;
	ausrc_read_h *rh;
	void *arg;
	HANDLE thread;
	volatile bool run;
	CRITICAL_SECTION crit;
};


static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;
	int i;
	MMRESULT res;

	debug("winwave src: destroying %p. stopping thread...\n", arg);

	if (st->run) {
		st->run = false;
		(void)WaitForSingleObject(st->thread, INFINITE);
	}

	debug("winwave src: thread stopped\n");

	st->rh = NULL;

	waveInStop(st->wavein);
	waveInReset(st->wavein);

	debug("winwave src: release buffers...\n");

	for (i = 0; i < READ_BUFFERS; i++) {
		waveInUnprepareHeader(st->wavein, &st->bufs[i].wh,
				      sizeof(WAVEHDR));
		mem_deref(st->bufs[i].mb);
	}

	info("winwave src: close device...\n");

	res = waveInClose(st->wavein);
	if (res != MMSYSERR_NOERROR)
		debug("winwave src: error closing device %p %p res=%d\n", st, st->wavein, res);
	else
		debug("winwave src: device closed %p %p\n", st, st->wavein);

	DeleteCriticalSection(&st->crit);
}


static DWORD WINAPI add_wave_in(LPVOID arg)
{
	struct ausrc_st *st = (struct ausrc_st *)arg;
	MMRESULT res;
	struct mbuf *mb;
	WAVEHDR *wh;
	size_t inuse;
	struct auframe af;

	while (st->run) {
		Sleep(1);

		EnterCriticalSection(&st->crit);
		inuse = st->inuse;
		LeaveCriticalSection(&st->crit);

		if (!st->rdy)
			continue;

		if (inuse == READ_BUFFERS)
			continue;

		wh = &st->bufs[st->pos].wh;
		if (wh->dwFlags & WHDR_PREPARED)
			waveInUnprepareHeader(st->wavein, wh, sizeof(*wh));

		mb = st->bufs[st->pos].mb;
		wh->lpData          = (LPSTR)mb->buf;

		if (st->rh) {
			auframe_init(&af, st->prm.fmt, (void *)wh->lpData,
				     wh->dwBytesRecorded / st->sampsz, st->prm.srate,
				     st->prm.ch);
			af.timestamp = tmr_jiffies_usec();

			st->rh(&af, st->arg);
		}

		wh->dwBufferLength  = mb->size;
		wh->dwBytesRecorded = 0;
		wh->dwFlags         = 0;
		wh->dwUser          = (DWORD_PTR)mb;

		waveInPrepareHeader(st->wavein, wh, sizeof(*wh));
		INC_RPOS(st->pos);

		res = waveInAddBuffer(st->wavein, wh, sizeof(*wh));
		if (res != MMSYSERR_NOERROR)
			warning("winwave src: add_wave_in: waveInAddBuffer failed: %d\n", res);
		else {
			EnterCriticalSection(&st->crit);
			st->inuse++;
			LeaveCriticalSection(&st->crit);
		}

	}

	return 0;
}


static void CALLBACK waveInCallback(HWAVEOUT hwo,
				    UINT uMsg,
				    DWORD_PTR dwInstance,
				    DWORD_PTR dwParam1,
				    DWORD_PTR dwParam2)
{
	struct ausrc_st *st = (struct ausrc_st *)dwInstance;

	(void)hwo;
	(void)dwParam1;
	(void)dwParam2;

	switch (uMsg) {

	case WIM_CLOSE:
		st->rdy = false;
		break;

	case WIM_OPEN:
		st->rdy = true;
		break;

	case WIM_DATA:
		EnterCriticalSection(&st->crit);
		st->inuse--;
		LeaveCriticalSection(&st->crit);
		break;

	default:
		break;
	}
}


static int read_stream_open(struct ausrc_st *st, const struct ausrc_prm *prm,
			    unsigned int dev)
{
	WAVEFORMATEX wfmt;
	MMRESULT res;
	uint32_t sampc;
	unsigned format;
	int i, err = 0;

	st->sampsz = aufmt_sample_size(prm->fmt);

	format = winwave_get_format(prm->fmt);
	if (format == WAVE_FORMAT_UNKNOWN) {
		warning("winwave src: source: unsupported sample format (%s)\n",
			aufmt_name(prm->fmt));
		return ENOTSUP;
	}

	/* Open an audio INPUT stream. */
	st->wavein = NULL;
	st->pos = 0;
	st->rdy = false;
	st->prm = *prm;

	sampc = prm->srate * prm->ch * prm->ptime / 1000;

	for (i = 0; i < READ_BUFFERS; i++) {
		memset(&st->bufs[i].wh, 0, sizeof(WAVEHDR));
		st->bufs[i].mb = mbuf_alloc(st->sampsz * sampc);
		if (!st->bufs[i].mb)
			return ENOMEM;
	}

	wfmt.wFormatTag      = format;
	wfmt.nChannels       = prm->ch;
	wfmt.nSamplesPerSec  = prm->srate;
	wfmt.wBitsPerSample  = (WORD)(st->sampsz * 8);
	wfmt.nBlockAlign     = (WORD)(prm->ch * st->sampsz);
	wfmt.nAvgBytesPerSec = wfmt.nSamplesPerSec * wfmt.nBlockAlign;
	wfmt.cbSize          = 0;

	res = waveInOpen(&st->wavein, dev, &wfmt,
			  (DWORD_PTR) waveInCallback,
			  (DWORD_PTR) st,
			  CALLBACK_FUNCTION | WAVE_FORMAT_DIRECT);
	if (res != MMSYSERR_NOERROR) {
		warning("winwave src: waveInOpen: failed %p res=%d\n", st, res);
		return EINVAL;
	}

	debug("winwave src: device opened %p %p\n", st, st->wavein);

	waveInStart(st->wavein);

	return err;
}


static int winwave_get_dev_name(unsigned int i, char name[32])
{
	WAVEINCAPS wic;
	int err = 0;

	if (waveInGetDevCaps(i, &wic,
			     sizeof(WAVEINCAPS)) == MMSYSERR_NOERROR) {
		str_ncpy(name, wic.szPname, 32);
	}
	else {
		err = ENODEV;
	}

	return err;
}


static unsigned int winwave_get_num_devs(void)
{
	return waveInGetNumDevs();
}


static int find_dev(const char *name, unsigned int *dev)
{
	return winwave_enum_devices(name, NULL, dev, winwave_get_num_devs,
				    winwave_get_dev_name);
}


int winwave_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		      struct ausrc_prm *prm, const char *device,
		      ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st = NULL;
	int err;
	unsigned int dev;

	(void)errh;

	if (!stp || !as || !prm)
		return EINVAL;

	err = find_dev(device, &dev);
	if (err)
		goto out;

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->rh  = rh;
	st->arg = arg;

	InitializeCriticalSection(&st->crit);

	info("winwave src: open device %s %d %p...\n", device, dev, st);

	err = read_stream_open(st, prm, dev);
	if (err)
		goto out;

	st->run = true;
	st->thread = CreateThread(NULL, 0, add_wave_in, st, 0, NULL);
	if (!st->thread) {
		st->run = false;
		err = ENOMEM;
	}

out:

	if (err) {
		warning("winwave src: failed opening device %p...\n", st);
		mem_deref(st);
	}
	else
		*stp = st;

	return err;
}


static int set_available_devices(struct list *dev_list)
{
	return winwave_enum_devices(NULL, dev_list, NULL,
				    winwave_get_num_devs,
				    winwave_get_dev_name);
}


int winwave_src_init(struct ausrc *as)
{
	if (!as)
		return EINVAL;

	list_init(&as->dev_list);

	return set_available_devices(&as->dev_list);
}
