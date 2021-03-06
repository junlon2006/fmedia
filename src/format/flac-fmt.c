/** FLAC output.
Copyright (c) 2018 Simon Zolin */

#include <fmedia.h>

#include <FF/aformat/flac.h>
#include <FF/audio/flac.h>
#include <FF/mtags/mmtag.h>
#include <FF/pic/png.h>
#include <FF/pic/jpeg.h>


#undef dbglog
#undef warnlog
#undef errlog
#undef syserrlog
#define dbglog(trk, ...)  fmed_dbglog(core, trk, NULL, __VA_ARGS__)
#define warnlog(trk, ...)  fmed_warnlog(core, trk, NULL, __VA_ARGS__)
#define errlog(trk, ...)  fmed_errlog(core, trk, NULL, __VA_ARGS__)
#define syserrlog(trk, ...)  fmed_syserrlog(core, trk, NULL, __VA_ARGS__)


extern const fmed_core *core;
extern const fmed_queue *qu;


//IN
static void* flac_in_create(fmed_filt *d);
static void flac_in_free(void *ctx);
static int flac_in_read(void *ctx, fmed_filt *d);
const fmed_filter fmed_flac_input = {
	&flac_in_create, &flac_in_read, &flac_in_free
};

//OUT
static void* flac_out_create(fmed_filt *d);
static void flac_out_free(void *ctx);
static int flac_out_encode(void *ctx, fmed_filt *d);
const fmed_filter fmed_flac_output = {
	&flac_out_create, &flac_out_encode, &flac_out_free
};


struct flac {
	ffflac fl;
	int64 abs_seek;
	uint seek_ready :1;
};


typedef struct flac_out {
	ffflac_cook fl;
	uint state;
} flac_out;

static int flac_out_addmeta(flac_out *f, fmed_filt *d);

static struct flac_out_conf_t {
	uint sktab_int;
	uint min_meta_size;
} flac_out_conf;

static const ffpars_arg flac_out_conf_args[] = {
	{ "min_meta_size",  FFPARS_TINT,  FFPARS_DSTOFF(struct flac_out_conf_t, min_meta_size) },
	{ "seektable_interval",	FFPARS_TINT,  FFPARS_DSTOFF(struct flac_out_conf_t, sktab_int) },
};


static void* flac_in_create(fmed_filt *d)
{
	struct flac *f = ffmem_new(struct flac);
	if (f == NULL)
		return NULL;
	ffflac_init(&f->fl);

	if (0 != ffflac_open(&f->fl)) {
		errlog(d->trk, "ffflac_open(): %s", ffflac_errstr(&f->fl));
		flac_in_free(f);
		return NULL;
	}

	if ((int64)d->input.size != FMED_NULL)
		f->fl.total_size = d->input.size;
	return f;
}

static void flac_in_free(void *ctx)
{
	struct flac *f = ctx;
	ffflac_close(&f->fl);
	ffmem_free(f);
}

static void flac_meta(struct flac *f, fmed_filt *d)
{
	const ffvorbtag *vtag = &f->fl.vtag;
	dbglog(d->trk, "%S: %S", &vtag->name, &vtag->val);

	ffstr name = vtag->name;
	if (vtag->tag == FFMMTAG_PICTURE)
		return;
	if (vtag->tag != 0)
		ffstr_setz(&name, ffmmtag_str[vtag->tag]);
	d->track->meta_set(d->trk, &name, &vtag->val, FMED_QUE_TMETA);
}

static int flac_in_read(void *ctx, fmed_filt *d)
{
	struct flac *f = ctx;
	int r;

	if (d->flags & FMED_FSTOP) {
		d->outlen = 0;
		return FMED_RLASTOUT;
	}

	if (d->flags & FMED_FFWD) {
		ffflac_input(&f->fl, d->data, d->datalen);
		if (d->flags & FMED_FLAST)
			f->fl.fin = 1;
	}

	if (f->seek_ready) {
		if ((int64)d->audio.seek != FMED_NULL) {
			ffflac_seek(&f->fl, f->abs_seek + ffpcm_samples(d->audio.seek, f->fl.fmt.sample_rate));
			d->audio.seek = FMED_NULL;
		}
	}

	for (;;) {
		r = ffflac_read(&f->fl);
		switch (r) {
		case FFFLAC_RMORE:
			if (d->flags & FMED_FLAST) {
				warnlog(d->trk, "file is incomplete");
				d->outlen = 0;
				return FMED_RDONE;
			}
			return FMED_RMORE;

		case FFFLAC_RHDR:
			d->audio.decoder = "FLAC";
			ffpcm_fmtcopy(&d->audio.fmt, &f->fl.fmt);
			d->audio.fmt.ileaved = 0;
			d->datatype = "flac";

			if (d->audio.abs_seek != 0)
				f->abs_seek = fmed_apos_samples(d->audio.abs_seek, f->fl.fmt.sample_rate);

			d->audio.total = ffflac_totalsamples(&f->fl) - f->abs_seek;
			break;

		case FFFLAC_RTAG:
			flac_meta(f, d);
			break;

		case FFFLAC_RHDRFIN:
			dbglog(d->trk, "blocksize:%u..%u  framesize:%u..%u  MD5:%16xb  seek-table:%u  meta-length:%u"
				, (int)f->fl.info.minblock, (int)f->fl.info.maxblock, (int)f->fl.info.minframe, (int)f->fl.info.maxframe
				, f->fl.info.md5, (int)f->fl.sktab.len, (int)f->fl.framesoff);
			d->audio.bitrate = ffflac_bitrate(&f->fl);

			if (d->input_info)
				return FMED_RDONE;

			fmed_setval("flac.in.minblock", f->fl.info.minblock);
			fmed_setval("flac.in.maxblock", f->fl.info.maxblock);

			if (0 != d->track->cmd2(d->trk, FMED_TRACK_ADDFILT, "flac.decode"))
				return FMED_RERR;

			f->seek_ready = 1;
			if (f->abs_seek != 0)
				ffflac_seek(&f->fl, f->abs_seek);
			if ((int64)d->audio.seek != FMED_NULL) {
				ffflac_seek(&f->fl, f->abs_seek + ffpcm_samples(d->audio.seek, f->fl.fmt.sample_rate));
				d->audio.seek = FMED_NULL;
			}
			break;

		case FFFLAC_RDATA:
			goto data;

		case FFFLAC_RSEEK:
			d->input.seek = ffflac_seekoff(&f->fl);
			return FMED_RMORE;

		case FFFLAC_RDONE:
			d->outlen = 0;
			return FMED_RDONE;

		case FFFLAC_RWARN:
			warnlog(d->trk, "ffflac_decode(): at offset 0x%xU: %s"
				, f->fl.off, ffflac_errstr(&f->fl));
			break;

		case FFFLAC_RERR:
			errlog(d->trk, "ffflac_decode(): %s", ffflac_errstr(&f->fl));
			return FMED_RERR;
		}
	}

data:
	dbglog(d->trk, "frame samples:%u pos:%U"
		, f->fl.frame.samples, ffflac_cursample(&f->fl));
	d->audio.pos = ffflac_cursample(&f->fl) - f->abs_seek;

	fmed_setval("flac.in.frsamples", f->fl.frame.samples);
	fmed_setval("flac.in.frpos", f->fl.frame.pos);
	if (f->fl.seek_ok)
		fmed_setval("flac.in.seeksample", f->fl.seeksample);
	ffstr out = ffflac_output(&f->fl);
	d->out = out.ptr;
	d->outlen = out.len;
	return FMED_RDATA;
}


int flac_out_config(ffpars_ctx *conf)
{
	flac_out_conf.sktab_int = 1;
	flac_out_conf.min_meta_size = 1000;
	ffpars_setargs(conf, &flac_out_conf, flac_out_conf_args, FFCNT(flac_out_conf_args));
	return 0;
}

static int pic_meta_png(struct flac_picinfo *info, const ffstr *data)
{
	struct ffpngr png = {};
	int rc = -1, r;
	if (0 != ffpngr_open(&png))
		goto err;
	png.input = *data;
	r = ffpngr_read(&png);
	if (r != FFPNG_HDR)
		goto err;

	info->mime = FFPNG_MIME;
	info->width = png.info.width;
	info->height = png.info.height;
	info->bpp = png.info.bpp;
	rc = 0;

err:
	ffpngr_close(&png);
	return rc;
}

static int pic_meta_jpeg(struct flac_picinfo *info, const ffstr *data)
{
	struct ffjpegr jpeg = {};
	int rc = -1, r;
	if (0 != ffjpegr_open(&jpeg))
		goto err;
	jpeg.input = *data;
	r = ffjpegr_read(&jpeg);
	if (r != FFJPEG_HDR)
		goto err;

	info->mime = FFJPEG_MIME;
	info->width = jpeg.info.width;
	info->height = jpeg.info.height;
	info->bpp = jpeg.info.bpp;
	rc = 0;

err:
	ffjpegr_close(&jpeg);
	return rc;
}

static void pic_meta(struct flac_picinfo *info, const ffstr *data, void *trk)
{
	if (0 == pic_meta_png(info, data))
		return;
	if (0 == pic_meta_jpeg(info, data))
		return;
	warnlog(trk, "picture write: can't detect MIME; writing without MIME and image dimensions");
}

static int flac_out_addmeta(flac_out *f, fmed_filt *d)
{
	uint i;
	ffstr name, *val;
	void *qent;

	const char *vendor = flac_vendor();
	if (0 != ffflac_addtag(&f->fl, NULL, vendor, ffsz_len(vendor))) {
		syserrlog(d->trk, "can't add tag: %S", &name);
		return -1;
	}

	if (FMED_PNULL == (qent = (void*)fmed_getval("queue_item")))
		return 0;

	for (i = 0;  NULL != (val = qu->meta(qent, i, &name, FMED_QUE_UNIQ));  i++) {
		if (val == FMED_QUE_SKIP
			|| ffstr_eqcz(&name, "vendor"))
			continue;

		if (ffstr_eqcz(&name, "picture")) {
			struct flac_picinfo info = {};
			pic_meta(&info, val, d->trk);
			ffflac_setpic(&f->fl, &info, val);
			continue;
		}

		if (0 != ffflac_addtag(&f->fl, name.ptr, val->ptr, val->len)) {
			syserrlog(d->trk, "can't add tag: %S", &name);
			return -1;
		}
	}
	return 0;
}

static void* flac_out_create(fmed_filt *d)
{
	flac_out *f = ffmem_tcalloc1(flac_out);
	if (f == NULL)
		return NULL;

	ffflac_winit(&f->fl);
	if (!d->out_seekable) {
		f->fl.seekable = 0;
		f->fl.seektable_int = 0;
	}
	return f;
}

static void flac_out_free(void *ctx)
{
	flac_out *f = ctx;
	ffflac_wclose(&f->fl);
	ffmem_free(f);
}

static int flac_out_encode(void *ctx, fmed_filt *d)
{
	enum { I_FIRST, I_INIT, I_DATA0, I_DATA };
	flac_out *f = ctx;
	int r;

	switch (f->state) {
	case I_FIRST:
		if (0 != d->track->cmd2(d->trk, FMED_TRACK_ADDFILT_PREV, "flac.encode"))
			return FMED_RERR;
		f->state = I_INIT;
		return FMED_RMORE;

	case I_INIT:
		if (!ffsz_eq(d->datatype, "flac")) {
			errlog(d->trk, "unsupported input data format: %s", d->datatype);
			return FMED_RERR;
		}

		if ((int64)d->audio.total != FMED_NULL)
			f->fl.total_samples = (d->audio.total - d->audio.pos) * d->audio.convfmt.sample_rate / d->audio.fmt.sample_rate;

		f->fl.seektable_int = flac_out_conf.sktab_int * d->audio.convfmt.sample_rate;
		f->fl.min_meta = flac_out_conf.min_meta_size;

		if (d->datalen != sizeof(ffflac_info)) {
			errlog(d->trk, "invalid first input data block");
			return FMED_RERR;
		}

		if (0 != ffflac_wnew(&f->fl, (void*)d->data)) {
			errlog(d->trk, "ffflac_wnew(): %s", ffflac_out_errstr(&f->fl));
			return FMED_RERR;
		}
		d->datalen = 0;
		if (0 != flac_out_addmeta(f, d))
			return FMED_RERR;

		f->state = I_DATA0;
		break;

	case I_DATA0:
	case I_DATA:
		break;
	}

	if (d->flags & FMED_FFWD) {
		ffstr_set(&f->fl.in, (const void**)d->datani, d->datalen);
		if (d->flags & FMED_FLAST) {
			if (d->datalen != sizeof(ffflac_info)) {
				errlog(d->trk, "invalid last input data block");
				return FMED_RERR;
			}
			ffflac_wfin(&f->fl, (void*)d->data);
		}
	}

	for (;;) {
	r = ffflac_write(&f->fl, fmed_getval("flac_in_frsamples"));

	switch (r) {
	case FFFLAC_RMORE:
		return FMED_RMORE;

	case FFFLAC_RDATA:
		if (f->state == I_DATA0) {
			d->output.size = ffflac_wsize(&f->fl);
			f->state = I_DATA;
		}
		goto data;

	case FFFLAC_RDONE:
		goto data;

	case FFFLAC_RSEEK:
		d->output.seek = f->fl.seekoff;
		continue;

	case FFFLAC_RERR:
	default:
		errlog(d->trk, "ffflac_write(): %s", ffflac_out_errstr(&f->fl));
		return FMED_RERR;
	}
	}

data:
	dbglog(d->trk, "output: %L bytes", f->fl.out.len);
	d->out = f->fl.out.ptr;
	d->outlen = f->fl.out.len;
	if (r == FFFLAC_RDONE)
		return FMED_RDONE;
	return FMED_ROK;
}
