/*
 * Copyright (C) 2003 Christophe Saout <christophe@saout.de>
 * Copyright (C) 2004 Clemens Fruhwirth <clemens@endorphin.org>
 * Copyright (C) 2006 Red Hat, Inc. All rights reserved.
 *
 * This file is released under the GPL.
 */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/mempool.h>
#include <linux/slab.h>
#include <linux/crypto.h>
#include <linux/workqueue.h>
#include <linux/backing-dev.h>
#include <asm/atomic.h>
#include <linux/scatterlist.h>
#include <asm/page.h>
#include <asm/unaligned.h>

#include <linux/device-mapper.h>

#if defined(CONFIG_OCF_DM_CRYPT)
extern int cesaReqResources[];
extern void DUMP_OCF_POOL(void);

//#define DM_DEBUG
#undef DM_DEBUG
#ifdef DM_DEBUG
#define dmprintk printk
#else
#define dmprintk(fmt,args...)
#endif

#include <../crypto/ocf/cryptodev.h>
#endif

#define DM_MSG_PREFIX "crypt"
#define MESG_STR(x) x, sizeof(x)

extern int crypto_debug;
/*
 * per bio private data
 */
struct crypt_io {
	struct dm_target *target;
	struct bio *base_bio;
	struct work_struct work;
	atomic_t pending;
	int error;
	int post_process;
};

/*
 * context holding the current state of a multi-part conversion
 */
struct convert_context {
	struct bio *bio_in;
	struct bio *bio_out;
	unsigned int offset_in;
	unsigned int offset_out;
	unsigned int idx_in;
	unsigned int idx_out;
	sector_t sector;
	int write;
};

struct crypt_config;

struct crypt_iv_operations {
	int (*ctr)(struct crypt_config *cc, struct dm_target *ti,
	           const char *opts);
	void (*dtr)(struct crypt_config *cc);
	const char *(*status)(struct crypt_config *cc);
	int (*generator)(struct crypt_config *cc, u8 *iv, sector_t sector);
};

/*
 * Crypt: maps a linear range of a block device
 * and encrypts / decrypts at the same time.
 */
enum flags { DM_CRYPT_SUSPENDED, DM_CRYPT_KEY_VALID };
struct crypt_config {
	struct dm_dev *dev;
	sector_t start;

	/*
	 * pool for per bio private data and
	 * for encryption buffer pages
	 */
	mempool_t *io_pool;
	mempool_t *page_pool;
	struct bio_set *bs;

	/*
	 * crypto related data
	 */
	struct crypt_iv_operations *iv_gen_ops;
	char *iv_mode;
	union {
		struct crypto_cipher *essiv_tfm;
		int benbi_shift;
	} iv_gen_private;
	sector_t iv_offset;
	unsigned int iv_size;

	char cipher[CRYPTO_MAX_ALG_NAME];
	char chainmode[CRYPTO_MAX_ALG_NAME];
#if defined(CONFIG_OCF_DM_CRYPT)
	struct cryptoini 	cr_dm;    		/* OCF session */
	uint64_t 	 	ocf_cryptoid;		/* OCF sesssion ID */
#else
	struct crypto_blkcipher *tfm;
#endif
	unsigned long flags;
	unsigned int key_size;
	u8 key[0];
};

#define MIN_IOS        256
#define MIN_POOL_PAGES 32
#define MIN_BIO_PAGES  8

static unsigned int _crypt_requests;
static DEFINE_SPINLOCK(_crypt_lock);
static wait_queue_head_t _crypt_waitq;
static struct kmem_cache *_crypt_io_pool;

static void clone_init(struct crypt_io *, struct bio *);

/*
 * Different IV generation algorithms:
 *
 * plain: the initial vector is the 32-bit little-endian version of the sector
 *        number, padded with zeros if neccessary.
 *
 * essiv: "encrypted sector|salt initial vector", the sector number is
 *        encrypted with the bulk cipher using a salt as key. The salt
 *        should be derived from the bulk cipher's key via hashing.
 *
 * benbi: the 64-bit "big-endian 'narrow block'-count", starting at 1
 *        (needed for LRW-32-AES and possible other narrow block modes)
 *
 * null: the initial vector is always zero.  Provides compatibility with
 *       obsolete loop_fish2 devices.  Do not use for new devices.
 *
 * plumb: unimplemented, see:
 * http://article.gmane.org/gmane.linux.kernel.device-mapper.dm-crypt/454
 */

static int crypt_iv_plain_gen(struct crypt_config *cc, u8 *iv, sector_t sector)
{
	memset(iv, 0, cc->iv_size);
	*(u32 *)iv = cpu_to_le32(sector & 0xffffffff);

	return 0;
}

static int crypt_iv_essiv_ctr(struct crypt_config *cc, struct dm_target *ti,
	                      const char *opts)
{
	struct crypto_cipher *essiv_tfm;
	struct crypto_hash *hash_tfm;
	struct hash_desc desc;
	struct scatterlist sg;
	unsigned int saltsize;
	u8 *salt;
	int err;

	if (opts == NULL) {
		ti->error = "Digest algorithm missing for ESSIV mode";
		return -EINVAL;
	}

	/* Hash the cipher key with the given hash algorithm */
	hash_tfm = crypto_alloc_hash(opts, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(hash_tfm)) {
		ti->error = "Error initializing ESSIV hash";
		return PTR_ERR(hash_tfm);
	}

	saltsize = crypto_hash_digestsize(hash_tfm);
	salt = kmalloc(saltsize, GFP_KERNEL);
	if (salt == NULL) {
		ti->error = "Error kmallocing salt storage in ESSIV";
		crypto_free_hash(hash_tfm);
		return -ENOMEM;
	}

	sg_init_one(&sg, cc->key, cc->key_size);
	desc.tfm = hash_tfm;
	desc.flags = CRYPTO_TFM_REQ_MAY_SLEEP;
	err = crypto_hash_digest(&desc, &sg, cc->key_size, salt);
	crypto_free_hash(hash_tfm);

	if (err) {
		ti->error = "Error calculating hash in ESSIV";
		kfree(salt);
		return err;
	}

	/* Setup the essiv_tfm with the given salt */
	essiv_tfm = crypto_alloc_cipher(cc->cipher, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(essiv_tfm)) {
		ti->error = "Error allocating crypto tfm for ESSIV";
		kfree(salt);
		return PTR_ERR(essiv_tfm);
	}
#if  defined(CONFIG_OCF_DM_CRYPT)
	if (crypto_cipher_blocksize(essiv_tfm) != cc->iv_size) {
#else
	if (crypto_cipher_blocksize(essiv_tfm) !=
	    crypto_blkcipher_ivsize(cc->tfm)) {
#endif
		ti->error = "Block size of ESSIV cipher does "
			        "not match IV size of block cipher";
		crypto_free_cipher(essiv_tfm);
		kfree(salt);
		return -EINVAL;
	}
	err = crypto_cipher_setkey(essiv_tfm, salt, saltsize);
	if (err) {
		ti->error = "Failed to set key for ESSIV cipher";
		crypto_free_cipher(essiv_tfm);
		kfree(salt);
		return err;
	}
	kfree(salt);

	cc->iv_gen_private.essiv_tfm = essiv_tfm;
	return 0;
}

static void crypt_iv_essiv_dtr(struct crypt_config *cc)
{
	crypto_free_cipher(cc->iv_gen_private.essiv_tfm);
	cc->iv_gen_private.essiv_tfm = NULL;
}

static int crypt_iv_essiv_gen(struct crypt_config *cc, u8 *iv, sector_t sector)
{
	memset(iv, 0, cc->iv_size);
	*(u64 *)iv = cpu_to_le64(sector);
	crypto_cipher_encrypt_one(cc->iv_gen_private.essiv_tfm, iv, iv);
	return 0;
}

#if !defined(CONFIG_OCF_DM_CRYPT)
static int crypt_iv_benbi_ctr(struct crypt_config *cc, struct dm_target *ti,
			      const char *opts)
{
	unsigned int bs = crypto_blkcipher_blocksize(cc->tfm);
	int log = ilog2(bs);

	/* we need to calculate how far we must shift the sector count
	 * to get the cipher block count, we use this shift in _gen */

	if (1 << log != bs) {
		ti->error = "cypher blocksize is not a power of 2";
		return -EINVAL;
	}

	if (log > 9) {
		ti->error = "cypher blocksize is > 512";
		return -EINVAL;
	}

	cc->iv_gen_private.benbi_shift = 9 - log;

	return 0;
}

static void crypt_iv_benbi_dtr(struct crypt_config *cc)
{
}

static int crypt_iv_benbi_gen(struct crypt_config *cc, u8 *iv, sector_t sector)
{
	__be64 val;

	memset(iv, 0, cc->iv_size - sizeof(u64)); /* rest is cleared below */

	val = cpu_to_be64(((u64)sector << cc->iv_gen_private.benbi_shift) + 1);
	put_unaligned(val, (__be64 *)(iv + cc->iv_size - sizeof(u64)));

	return 0;
}
#endif

static int crypt_iv_null_gen(struct crypt_config *cc, u8 *iv, sector_t sector)
{
	memset(iv, 0, cc->iv_size);

	return 0;
}

static struct crypt_iv_operations crypt_iv_plain_ops = {
	.generator = crypt_iv_plain_gen
};

static struct crypt_iv_operations crypt_iv_essiv_ops = {
	.ctr       = crypt_iv_essiv_ctr,
	.dtr       = crypt_iv_essiv_dtr,
	.generator = crypt_iv_essiv_gen
};

#if !defined(CONFIG_OCF_DM_CRYPT)
static struct crypt_iv_operations crypt_iv_benbi_ops = {
	.ctr	   = crypt_iv_benbi_ctr,
	.dtr	   = crypt_iv_benbi_dtr,
	.generator = crypt_iv_benbi_gen
};
#endif

static struct crypt_iv_operations crypt_iv_null_ops = {
	.generator = crypt_iv_null_gen
};

#if defined(CONFIG_OCF_DM_CRYPT)
static void dec_pending(struct crypt_io *io, int error);

struct ocf_wr_priv {
	u32 		 	dm_ocf_wr_completed;	/* Num of wr completions */
	u32 		 	dm_ocf_wr_pending;	/* Num of wr pendings */
	wait_queue_head_t	dm_ocf_wr_queue;	/* waiting Q, for wr completion */
};

/* WARN: ordering between processes is not guaranteed due to 'wake' handling */
static int dm_ocf_wr_cb(struct cryptop *crp)
{
	struct ocf_wr_priv *ocf_wr_priv;
	unsigned long flags;

	if(crp == NULL) {
		printk("dm_ocf_wr_cb: crp is NULL!! \n");
		return 0;
	}

	ocf_wr_priv = (struct ocf_wr_priv*)crp->crp_opaque;

	ocf_wr_priv->dm_ocf_wr_completed++;

	/* if no more pending for read, wake up the read task. */
	if(ocf_wr_priv->dm_ocf_wr_completed == ocf_wr_priv->dm_ocf_wr_pending)
		wake_up(&ocf_wr_priv->dm_ocf_wr_queue);

	crypto_freereq(crp);

	spin_lock_irqsave(&_crypt_lock, flags);
	if (_crypt_requests > 0)
		_crypt_requests -= 1;
	spin_unlock_irqrestore(&_crypt_lock, flags);

	wake_up(&_crypt_waitq);
	return 0;
}

static int dm_ocf_rd_cb(struct cryptop *crp)
{
	struct crypt_io *io;
	unsigned long flags;

	if(crp == NULL) {
		printk("dm_ocf_rd_cb: crp is NULL!! \n");
		return 0;
	}

	io = (struct crypt_io *)crp->crp_opaque;

	crypto_freereq(crp);

	if(io != NULL)
		dec_pending(io, 0);

	spin_lock_irqsave(&_crypt_lock, flags);
	if (_crypt_requests > 0)
		_crypt_requests -= 1;
	spin_unlock_irqrestore(&_crypt_lock, flags);

	wake_up(&_crypt_waitq);
	return 0;
}

static inline int dm_ocf_process(struct crypt_config *cc, struct scatterlist *out,
		struct scatterlist *in, unsigned int len, u8 *iv, int iv_size, int write, void *priv)
{
	struct cryptop *crp;
	struct cryptodesc *crda = NULL;
	unsigned long flags;
	unsigned int cr;

	if(!iv) {
		printk("dm_ocf_process: only CBC mode is supported\n");
		return -EPERM;
	}

	crp = crypto_getreq(1);	 /* only encryption/decryption */
	if (!crp) {
		printk("dm_ocf_process: crypto_getreq failed!!\n");
		return -ENOMEM;
	}

	crda = crp->crp_desc;

	crda->crd_flags  = (write)? CRD_F_ENCRYPT: 0;
	crda->crd_alg    = cc->cr_dm.cri_alg;
	crda->crd_skip   = 0;
	crda->crd_len    = len;
	crda->crd_inject = 0; /* NA */
	crda->crd_klen   = cc->cr_dm.cri_klen;
	crda->crd_key    = cc->cr_dm.cri_key;

	if (iv) {
		crda->crd_flags |= (CRD_F_IV_EXPLICIT | CRD_F_IV_PRESENT);
		if( iv_size > EALG_MAX_BLOCK_LEN ) {
			printk("dm_ocf_process: iv is too big!!\n");
		}
		memcpy(&crda->crd_iv, iv, iv_size);
	}

	/* according to the current implementation the in and the out are the same buffer for read, and different for write*/
	if (sg_virt(out) != sg_virt(in)) {
		memcpy(sg_virt(out), sg_virt(in), len);
		dmprintk("dm_ocf_process: copy buffers!! \n");
	}

	dmprintk("len: %d\n",len);
	crp->crp_ilen = len; /* Total input length */
	crp->crp_flags = CRYPTO_F_CBIMM | CRYPTO_F_BATCH;
	crp->crp_buf = sg_virt(out);
	crp->crp_opaque = priv;
	if (write) {
       crp->crp_callback = dm_ocf_wr_cb;
	}
	else {
		crp->crp_callback = dm_ocf_rd_cb;
	}
	crp->crp_sid = cc->ocf_cryptoid;

	spin_lock_irqsave(&_crypt_lock, flags);
	while (crypto_dispatch(crp) != 0) {
		if (_crypt_requests == 0) {
			spin_unlock_irqrestore(&_crypt_lock, flags);
			schedule();
			spin_lock_irqsave(&_crypt_lock, flags);
		} else {
			cr = _crypt_requests;
			spin_unlock_irqrestore(&_crypt_lock, flags);
			wait_event(_crypt_waitq, _crypt_requests < cr);
			spin_lock_irqsave(&_crypt_lock, flags);
		}
	}
	_crypt_requests += 1;
	spin_unlock_irqrestore(&_crypt_lock, flags);

	return 0;
}

static inline int
ocf_crypt_convert_scatterlist(struct crypt_config *cc, struct scatterlist *out,
                          struct scatterlist *in, unsigned int length,
                          int write, sector_t sector, void *priv)
{
	u8 iv[cc->iv_size];
	int r;
	if (cc->iv_gen_ops) {
		r = cc->iv_gen_ops->generator(cc, iv, sector);
		if (r < 0)
			return r;
		r = dm_ocf_process(cc, out, in, length, iv, cc->iv_size, write, priv);
	} else {
		r = dm_ocf_process(cc, out, in, length, NULL, 0, write, priv);
	}

	return r;
}

/*
 * Encrypt / decrypt data from one bio to another one (can be the same one)
 */
static int ocf_crypt_convert(struct crypt_config *cc,
                         struct convert_context *ctx, struct crypt_io *io)
{
	int r = 0;
	long wr_timeout = 30000;
	long wr_tm;
	int num = 0;
	void *priv = NULL;
	struct ocf_wr_priv *ocf_wr_priv = NULL;
	if (ctx->write) {
		ocf_wr_priv = kmalloc(sizeof(struct ocf_wr_priv),GFP_KERNEL);
		if(!ocf_wr_priv) {
			printk("ocf_crypt_convert: out of memory \n");
			return -ENOMEM;
		}
		ocf_wr_priv->dm_ocf_wr_pending = 0;
		ocf_wr_priv->dm_ocf_wr_completed = 0;
		init_waitqueue_head(&ocf_wr_priv->dm_ocf_wr_queue);
		priv = ocf_wr_priv;
	}

	while(ctx->idx_in < ctx->bio_in->bi_vcnt &&
	      ctx->idx_out < ctx->bio_out->bi_vcnt) {
		struct bio_vec *bv_in = bio_iovec_idx(ctx->bio_in, ctx->idx_in);
		struct bio_vec *bv_out = bio_iovec_idx(ctx->bio_out, ctx->idx_out);
		struct scatterlist sg_in, sg_out;
		sg_init_table(&sg_in, 1);
		sg_set_page(&sg_in, bv_in->bv_page, 1 << SECTOR_SHIFT,
				bv_in->bv_offset + ctx->offset_in);

		sg_init_table(&sg_out, 1);
		sg_set_page(&sg_out, bv_out->bv_page, 1 << SECTOR_SHIFT,
				bv_out->bv_offset + ctx->offset_out);

		ctx->offset_in += sg_in.length;
		if (ctx->offset_in >= bv_in->bv_len) {
			ctx->offset_in = 0;
			ctx->idx_in++;
		}

		ctx->offset_out += sg_out.length;
		if (ctx->offset_out >= bv_out->bv_len) {
			ctx->offset_out = 0;
			ctx->idx_out++;
		}

		if(ctx->write) {
			num++;
		}
		/* if last read in the context - send the io, so the OCF read callback will release the IO. */
		else if(!(ctx->idx_in < ctx->bio_in->bi_vcnt && ctx->idx_out < ctx->bio_out->bi_vcnt)) {
			priv = io;
		}

		r = ocf_crypt_convert_scatterlist(cc, &sg_out, &sg_in, sg_in.length,
		                              ctx->write, ctx->sector, priv);
		if (r < 0){
			printk("ocf_crypt_convert: ocf_crypt_convert_scatterlist failed \n");
			break;
		}

		ctx->sector++;
	}

	if (ctx->write) {
		ocf_wr_priv->dm_ocf_wr_pending += num;
		wr_tm = wait_event_timeout(ocf_wr_priv->dm_ocf_wr_queue,
				(ocf_wr_priv->dm_ocf_wr_pending == ocf_wr_priv->dm_ocf_wr_completed)
									, msecs_to_jiffies(wr_timeout) );
		if (!wr_tm) {
			printk("ocf_crypt_convert: wr work was not finished in %ld msecs, %d pending %d completed.\n",
				wr_timeout, ocf_wr_priv->dm_ocf_wr_pending, ocf_wr_priv->dm_ocf_wr_completed);
		}
		kfree(ocf_wr_priv);
	}

	return r;
}

#else /* CONFIG_OCF_DM_CRYPT */

static int
crypt_convert_scatterlist(struct crypt_config *cc, struct scatterlist *out,
                          struct scatterlist *in, unsigned int length,
                          int write, sector_t sector)
{
	u8 iv[cc->iv_size] __attribute__ ((aligned(__alignof__(u64))));
	struct blkcipher_desc desc = {
		.tfm = cc->tfm,
		.info = iv,
		.flags = CRYPTO_TFM_REQ_MAY_SLEEP,
	};
	int r;
	if (cc->iv_gen_ops) {
		r = cc->iv_gen_ops->generator(cc, iv, sector);
		if (r < 0)
			return r;

		if (write)
			r = crypto_blkcipher_encrypt_iv(&desc, out, in, length);
		else
			r = crypto_blkcipher_decrypt_iv(&desc, out, in, length);
	} else {
		if (write)
			r = crypto_blkcipher_encrypt(&desc, out, in, length);
		else
			r = crypto_blkcipher_decrypt(&desc, out, in, length);
	}

	return r;
}

#endif

static void
crypt_convert_init(struct crypt_config *cc, struct convert_context *ctx,
                   struct bio *bio_out, struct bio *bio_in,
                   sector_t sector, int write)
{
	ctx->bio_in = bio_in;
	ctx->bio_out = bio_out;
	ctx->offset_in = 0;
	ctx->offset_out = 0;
	ctx->idx_in = bio_in ? bio_in->bi_idx : 0;
	ctx->idx_out = bio_out ? bio_out->bi_idx : 0;
	ctx->sector = sector + cc->iv_offset;
	ctx->write = write;
}

#if !defined(CONFIG_OCF_DM_CRYPT)
/*
 * Encrypt / decrypt data from one bio to another one (can be the same one)
 */
static int crypt_convert(struct crypt_config *cc,
                         struct convert_context *ctx)
{
	int r = 0;
	while(ctx->idx_in < ctx->bio_in->bi_vcnt &&
	      ctx->idx_out < ctx->bio_out->bi_vcnt) {
		struct bio_vec *bv_in = bio_iovec_idx(ctx->bio_in, ctx->idx_in);
		struct bio_vec *bv_out = bio_iovec_idx(ctx->bio_out, ctx->idx_out);
		struct scatterlist sg_in, sg_out;
		sg_init_table(&sg_in, 1);
		sg_set_page(&sg_in, bv_in->bv_page, 1 << SECTOR_SHIFT,
				bv_in->bv_offset + ctx->offset_in);
		sg_init_table(&sg_out, 1);
		sg_set_page(&sg_out, bv_out->bv_page, 1 << SECTOR_SHIFT,
				bv_out->bv_offset + ctx->offset_out);

		ctx->offset_in += sg_in.length;
		if (ctx->offset_in >= bv_in->bv_len) {
			ctx->offset_in = 0;
			ctx->idx_in++;
		}

		ctx->offset_out += sg_out.length;
		if (ctx->offset_out >= bv_out->bv_len) {
			ctx->offset_out = 0;
			ctx->idx_out++;
		}

		r = crypt_convert_scatterlist(cc, &sg_out, &sg_in, sg_in.length,
		                              ctx->write, ctx->sector);
		if (r < 0)
			break;

		ctx->sector++;
	}

	return r;
}
#endif

/*
 * Generate a new unfragmented bio with the given size
 * This should never violate the device limitations
 * May return a smaller bio when running out of pages
 */
static struct bio *crypt_alloc_buffer(struct crypt_io *io, unsigned int size)
{
	struct crypt_config *cc = io->target->private;
	struct bio *clone;
	unsigned int nr_iovecs = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	gfp_t gfp_mask = GFP_NOIO;
	unsigned int i;
	clone = bio_alloc_bioset(GFP_NOIO, nr_iovecs, cc->bs);
	if (!clone)
		return NULL;

	clone_init(io, clone);

	for (i = 0; i < nr_iovecs; i++) {
		struct bio_vec *bv = bio_iovec_idx(clone, i);

		bv->bv_page = mempool_alloc(cc->page_pool, gfp_mask);
		if (!bv->bv_page)
			break;

		/*
		 * if additional pages cannot be allocated without waiting,
		 * return a partially allocated bio, the caller will then try
		 * to allocate additional bios while submitting this partial bio
		 */
		if (i == (MIN_BIO_PAGES - 1))
			gfp_mask = (gfp_mask | __GFP_NOWARN) & ~__GFP_WAIT;

		bv->bv_offset = 0;
		if (size > PAGE_SIZE)
			bv->bv_len = PAGE_SIZE;
		else
			bv->bv_len = size;

		clone->bi_size += bv->bv_len;
		clone->bi_vcnt++;
		size -= bv->bv_len;
	}

	if (!clone->bi_size) {
		bio_put(clone);
		return NULL;
	}

	return clone;
}

static void crypt_free_buffer_pages(struct crypt_config *cc,
                                    struct bio *clone)
{
	unsigned int i;
	struct bio_vec *bv;
	for (i = 0; i < clone->bi_vcnt; i++) {
		bv = bio_iovec_idx(clone, i);
		BUG_ON(!bv->bv_page);
		mempool_free(bv->bv_page, cc->page_pool);
		bv->bv_page = NULL;
	}
}

/*
 * One of the bios was finished. Check for completion of
 * the whole request and correctly clean up the buffer.
 */
static void dec_pending(struct crypt_io *io, int error)
{
	struct crypt_config *cc = (struct crypt_config *) io->target->private;
	struct bio_vec *tovec, *fromvec;
	struct bio *bio = io->base_bio;
#ifdef CONFIG_HIGHMEM
	struct bio *origbio;
	unsigned long flags;
	char *vfrom, *vto;
	unsigned int i;
#endif /* CONFIG_HIGHMEM */

	if (error < 0)
		io->error = error;

	if (!atomic_dec_and_test(&io->pending))
		return;

#ifdef CONFIG_HIGHMEM
	if (bio_flagged(bio, BIO_BOUNCED)) {
		origbio = bio->bi_private;

		/* We have bounced bio, so copy data back if it is necessary */
		if (bio_data_dir(bio) == READ) {
			__bio_for_each_segment(tovec, origbio, i, 0) {
				fromvec = bio->bi_io_vec + i;

				/* Page not bounced */
				if (tovec->bv_page == fromvec->bv_page)
					continue;

				/*
				 * Page bounced - we have to copy data.
				 * We are using tovec->bv_offset and
				 * tovec->bv_len as originals might
				 * have been modified.
				 */
				vfrom = page_address(fromvec->bv_page) + tovec->bv_offset;
				local_irq_save(flags);

				vto = kmap_atomic(tovec->bv_page);
				memcpy(vto + tovec->bv_offset, vfrom, tovec->bv_len);
				kunmap_atomic(vto);

				local_irq_restore(flags);
			}
		}

		/* Free bounced pages */
		__bio_for_each_segment(fromvec, bio, i, 0) {
			tovec = origbio->bi_io_vec + i;

			/* Page not bounced */
			if (tovec->bv_page == fromvec->bv_page)
				continue;

			/* Page bounced: free it! */
			mempool_free(fromvec->bv_page, cc->page_pool);
		}

		/* Release our bounced bio */
		bio_put(bio);
		bio = origbio;
	}
#endif /* CONFIG_HIGHMEM */

	bio_endio(bio, io->error);
	mempool_free(io, cc->io_pool);
}

/*
 * kcryptd:
 *
 * Needed because it would be very unwise to do decryption in an
 * interrupt context.
 */
static struct workqueue_struct *_kcryptd_workqueue;
static void kcryptd_do_work(struct work_struct *work);

static void kcryptd_queue_io(struct crypt_io *io)
{
	INIT_WORK(&io->work, kcryptd_do_work);
	queue_work(_kcryptd_workqueue, &io->work);
}

static void crypt_endio(struct bio *clone, int error)
{
	struct crypt_io *io = clone->bi_private;
	struct crypt_config *cc = io->target->private;
	unsigned read_io = bio_data_dir(clone) == READ;
	/*
	 * free the processed pages, even if
	 * it's only a partially completed write
	 */
	if (!read_io)
		crypt_free_buffer_pages(cc, clone);

	if (!read_io) {
		goto out;
	}
	if (unlikely(!bio_flagged(clone, BIO_UPTODATE))) {
		error = -EIO;
		goto out;
	}
	bio_put(clone);
	io->post_process = 1;
	kcryptd_queue_io(io);
	return;

out:
	bio_put(clone);
	dec_pending(io, error);
}

static void clone_init(struct crypt_io *io, struct bio *clone)
{
	struct crypt_config *cc = io->target->private;

	clone->bi_private = io;
	clone->bi_end_io  = crypt_endio;
	clone->bi_bdev    = cc->dev->bdev;
	clone->bi_rw      = io->base_bio->bi_rw;
}

static void process_read(struct crypt_io *io)
{
	struct crypt_config *cc = io->target->private;
	struct bio *base_bio = io->base_bio;
	struct bio *clone;
	sector_t sector = base_bio->bi_sector - io->target->begin;
	atomic_inc(&io->pending);

	/*
	 * The block layer might modify the bvec array, so always
	 * copy the required bvecs because we need the original
	 * one in order to decrypt the whole bio data *afterwards*.
	 */
	clone = bio_alloc_bioset(GFP_NOIO, bio_segments(base_bio), cc->bs);
	if (unlikely(!clone)) {
		dec_pending(io, -ENOMEM);
		return;
	}

	clone_init(io, clone);
	clone->bi_idx = 0;
	clone->bi_vcnt = bio_segments(base_bio);
	clone->bi_size = base_bio->bi_size;
	clone->bi_sector = cc->start + sector;
	memcpy(clone->bi_io_vec, bio_iovec(base_bio),
	       sizeof(struct bio_vec) * clone->bi_vcnt);

	generic_make_request(clone);
}

static void process_write(struct crypt_io *io)
{
	struct crypt_config *cc = io->target->private;
	struct bio *base_bio = io->base_bio;
	struct bio *clone;
	struct convert_context ctx;
	unsigned remaining = base_bio->bi_size;
	sector_t sector = base_bio->bi_sector - io->target->begin;
	atomic_inc(&io->pending);

	crypt_convert_init(cc, &ctx, NULL, base_bio, sector, 1);

	/*
	 * The allocated buffers can be smaller than the whole bio,
	 * so repeat the whole process until all the data can be handled.
	 */
	while (remaining) {
		clone = crypt_alloc_buffer(io, remaining);
		if (unlikely(!clone)) {
			dec_pending(io, -ENOMEM);
			return;
		}

		ctx.bio_out = clone;
		ctx.idx_out = 0;
#if defined(CONFIG_OCF_DM_CRYPT)
	if (unlikely(ocf_crypt_convert(cc, &ctx, io)< 0)) {
#else
		if (unlikely(crypt_convert(cc, &ctx) < 0)) {
#endif
			crypt_free_buffer_pages(cc, clone);
			bio_put(clone);
			dec_pending(io, -EIO);
			return;
		}

		/* crypt_convert should have filled the clone bio */
		BUG_ON(ctx.idx_out < clone->bi_vcnt);

		clone->bi_sector = cc->start + sector;
		remaining -= clone->bi_size;
		sector += bio_sectors(clone);

		/* Grab another reference to the io struct
		 * before we kick off the request */
		if (remaining)
			atomic_inc(&io->pending);

		generic_make_request(clone);

		/* Do not reference clone after this - it
		 * may be gone already. */

		/* out of memory -> run queues */
		if (remaining)
			congestion_wait(WRITE, HZ/100);
	}
}

static void process_read_endio(struct crypt_io *io)
{
	struct crypt_config *cc = io->target->private;
	struct convert_context ctx;
#if defined(CONFIG_OCF_DM_CRYPT)
	u32 r;
#endif

	crypt_convert_init(cc, &ctx, io->base_bio, io->base_bio,
			   io->base_bio->bi_sector - io->target->begin, 0);

#if defined(CONFIG_OCF_DM_CRYPT)
	r = ocf_crypt_convert(cc, &ctx, io);
	if (r < 0) {
		u32 rd_failed_timeout = 500;
		wait_queue_head_t dm_ocf_rd_failed_queu;

		init_waitqueue_head(&dm_ocf_rd_failed_queu);

		/* wait a bit before freeing the io, maybe few requests are still in process */
		wait_event_timeout(dm_ocf_rd_failed_queu, 0, msecs_to_jiffies(rd_failed_timeout) );

		dec_pending(io, r);

	}
#else
	dec_pending(io, crypt_convert(cc, &ctx));
#endif
}

static void kcryptd_do_work(struct work_struct *work)
{
	struct crypt_io *io = container_of(work, struct crypt_io, work);
	if (io->post_process) {
		process_read_endio(io);
	}
	else if (bio_data_dir(io->base_bio) == READ) {
		process_read(io);
	}
	else
		process_write(io);
}

/*
 * Decode key from its hex representation
 */
static int crypt_decode_key(u8 *key, char *hex, unsigned int size)
{
	char buffer[3];
	char *endp;
	unsigned int i;
	buffer[2] = '\0';

	for (i = 0; i < size; i++) {
		buffer[0] = *hex++;
		buffer[1] = *hex++;

		key[i] = (u8)simple_strtoul(buffer, &endp, 16);

		if (endp != &buffer[2])
			return -EINVAL;
	}

	if (*hex != '\0')
		return -EINVAL;

	return 0;
}

static int crypt_set_key(struct crypt_config *cc, char *key)
{
	unsigned key_size = strlen(key) >> 1;
	if (cc->key_size && cc->key_size != key_size)
		return -EINVAL;

	cc->key_size = key_size; /* initial settings */

	if ((!key_size && strcmp(key, "-")) ||
	    (key_size && crypt_decode_key(cc->key, key, key_size) < 0))
		return -EINVAL;

	set_bit(DM_CRYPT_KEY_VALID, &cc->flags);

	return 0;
}

static int crypt_wipe_key(struct crypt_config *cc)
{
	clear_bit(DM_CRYPT_KEY_VALID, &cc->flags);
	memset(&cc->key, 0, cc->key_size * sizeof(u8));
	return 0;
}

/*
 * Construct an encryption mapping:
 * <cipher> <key> <iv_offset> <dev_path> <start>
 */
static int crypt_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct crypt_config *cc;
#ifndef CONFIG_OCF_DM_CRYPT
	struct crypto_blkcipher *tfm;
#endif
	char *tmp;
	char *cipher;
	char *chainmode;
	char *ivmode;
	char *ivopts;
	unsigned int key_size;
	unsigned long long tmpll;
	if (argc != 5) {
		ti->error = "Not enough arguments";
		return -EINVAL;
	}

	tmp = argv[0];
	cipher = strsep(&tmp, "-");
	chainmode = strsep(&tmp, "-");
	ivopts = strsep(&tmp, "-");
	ivmode = strsep(&ivopts, ":");

	if (tmp)
		DMWARN("Unexpected additional cipher options");

	key_size = strlen(argv[1]) >> 1;

	cc = kzalloc(sizeof(*cc) + key_size * sizeof(u8), GFP_KERNEL);
	if (cc == NULL) {
		ti->error =
			"Cannot allocate transparent encryption context";
		return -ENOMEM;
	}

	if (crypt_set_key(cc, argv[1])) {
		ti->error = "Error decoding key";
		goto bad1;
	}

	/* Compatiblity mode for old dm-crypt cipher strings */
	if (!chainmode || (strcmp(chainmode, "plain") == 0 && !ivmode)) {
		chainmode = "cbc";
		ivmode = "plain";
	}

	if (strcmp(chainmode, "ecb") && !ivmode) {
		ti->error = "This chaining mode requires an IV mechanism";
		goto bad1;
	}

	if (snprintf(cc->cipher, CRYPTO_MAX_ALG_NAME, "%s(%s)", chainmode,
		     cipher) >= CRYPTO_MAX_ALG_NAME) {
		ti->error = "Chain mode + cipher name is too long";
		goto bad1;
	}

#if defined(CONFIG_OCF_DM_CRYPT)
	/* prepare a new OCF session */
        memset(&cc->cr_dm, 0, sizeof(struct cryptoini));

	if((strcmp(cipher,"aes") == 0) && (strcmp(chainmode, "cbc") == 0))
		cc->cr_dm.cri_alg  = CRYPTO_AES_CBC;
	else if((strcmp(cipher,"des") == 0) && (strcmp(chainmode, "cbc") == 0))
		cc->cr_dm.cri_alg  = CRYPTO_DES_CBC;
	else if((strcmp(cipher,"des3_ede") == 0) && (strcmp(chainmode, "cbc") == 0))
		cc->cr_dm.cri_alg  = CRYPTO_3DES_CBC;
	else {
		ti->error = DM_MSG_PREFIX "using OCF: unknown cipher or bad chain mode";
		goto bad1;
	}

	/*strcpy(cc->cipher, cipher);*/
	dmprintk("key size is %d\n",cc->key_size);
        cc->cr_dm.cri_klen = cc->key_size*8;
        cc->cr_dm.cri_key  = cc->key;
        cc->cr_dm.cri_next = NULL;

        if(crypto_newsession(&cc->ocf_cryptoid, &cc->cr_dm, 0)){
		dmprintk("crypt_ctr: crypto_newsession failed\n");
                ti->error = DM_MSG_PREFIX "crypto_newsession failed";
                goto bad2;
        }

#else

	tfm = crypto_alloc_blkcipher(cc->cipher, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm)) {
		ti->error = "Error allocating crypto tfm";
		goto bad1;
	}
#endif
	strcpy(cc->cipher, cipher);
	strcpy(cc->chainmode, chainmode);
#if !defined(CONFIG_OCF_DM_CRYPT)
	cc->tfm = tfm;
#endif

	/*
	 * Choose ivmode. Valid modes: "plain", "essiv:<esshash>", "benbi".
	 * See comments at iv code
	 */

	if (ivmode == NULL)
		cc->iv_gen_ops = NULL;
	else if (strcmp(ivmode, "plain") == 0)
		cc->iv_gen_ops = &crypt_iv_plain_ops;
	else if (strcmp(ivmode, "essiv") == 0)
		cc->iv_gen_ops = &crypt_iv_essiv_ops;
#if !defined(CONFIG_OCF_DM_CRYPT)
	else if (strcmp(ivmode, "benbi") == 0)
		cc->iv_gen_ops = &crypt_iv_benbi_ops;
#endif
	else if (strcmp(ivmode, "null") == 0)
		cc->iv_gen_ops = &crypt_iv_null_ops;
	else {
		ti->error = "Invalid IV mode";
		goto bad2;
	}

#if defined(CONFIG_OCF_DM_CRYPT)
	switch (cc->cr_dm.cri_alg) {
		case CRYPTO_AES_CBC:
			cc->iv_size = 16;
			break;
		default:
			cc->iv_size = 8;
			break;
	}

	if (cc->iv_gen_ops && cc->iv_gen_ops->ctr &&
	    cc->iv_gen_ops->ctr(cc, ti, ivopts) < 0)
		goto bad2;
#else

	if (cc->iv_gen_ops && cc->iv_gen_ops->ctr &&
	    cc->iv_gen_ops->ctr(cc, ti, ivopts) < 0)
		goto bad2;

	cc->iv_size = crypto_blkcipher_ivsize(tfm);
	if (cc->iv_size)
		/* at least a 64 bit sector number should fit in our buffer */
		cc->iv_size = max(cc->iv_size,
		                  (unsigned int)(sizeof(u64) / sizeof(u8)));
	else {
		if (cc->iv_gen_ops) {
			DMWARN("Selected cipher does not support IVs");
			if (cc->iv_gen_ops->dtr)
				cc->iv_gen_ops->dtr(cc);
			cc->iv_gen_ops = NULL;
		}
	}
#endif
	cc->io_pool = mempool_create_slab_pool(MIN_IOS, _crypt_io_pool);
	if (!cc->io_pool) {
		ti->error = "Cannot allocate crypt io mempool";
		goto bad3;
	}

	cc->page_pool = mempool_create_page_pool(MIN_POOL_PAGES, 0);
	if (!cc->page_pool) {
		ti->error = "Cannot allocate page mempool";
		goto bad4;
	}

	cc->bs = bioset_create(MIN_IOS, MIN_IOS);
	if (!cc->bs) {
		ti->error = "Cannot allocate crypt bioset";
		goto bad_bs;
	}
#if !defined(CONFIG_OCF_DM_CRYPT)
	if (crypto_blkcipher_setkey(tfm, cc->key, key_size) < 0) {
		ti->error = "Error setting key";
		goto bad5;
	}
#endif

	if (sscanf(argv[2], "%llu", &tmpll) != 1) {
		ti->error = "Invalid iv_offset sector";
		goto bad5;
	}
	cc->iv_offset = tmpll;

	if (sscanf(argv[4], "%llu", &tmpll) != 1) {
		ti->error = "Invalid device sector";
		goto bad5;
	}
	cc->start = tmpll;

	if (dm_get_device(ti, argv[3], dm_table_get_mode(ti->table),
								&cc->dev)) {
		ti->error = "Device lookup failed";
		goto bad5;
	}

	if (ivmode && cc->iv_gen_ops) {
		if (ivopts)
			*(ivopts - 1) = ':';
		cc->iv_mode = kmalloc(strlen(ivmode) + 1, GFP_KERNEL);
		if (!cc->iv_mode) {
			ti->error = "Error kmallocing iv_mode string";
			goto bad5;
		}
		strcpy(cc->iv_mode, ivmode);
	} else
		cc->iv_mode = NULL;

	ti->private = cc;
	return 0;

bad5:
	bioset_free(cc->bs);
bad_bs:
	mempool_destroy(cc->page_pool);
bad4:
	mempool_destroy(cc->io_pool);
bad3:
	if (cc->iv_gen_ops && cc->iv_gen_ops->dtr)
		cc->iv_gen_ops->dtr(cc);
bad2:
#if defined(CONFIG_OCF_DM_CRYPT)
	crypto_freesession(cc->ocf_cryptoid);
#else
	crypto_free_blkcipher(tfm);
#endif
bad1:
	/* Must zero key material before freeing */
	memset(cc, 0, sizeof(*cc) + cc->key_size * sizeof(u8));
	kfree(cc);
	return -EINVAL;
}

static void crypt_dtr(struct dm_target *ti)
{
	struct crypt_config *cc = (struct crypt_config *) ti->private;
	flush_workqueue(_kcryptd_workqueue);

	bioset_free(cc->bs);
	mempool_destroy(cc->page_pool);
	mempool_destroy(cc->io_pool);

	kfree(cc->iv_mode);
	if (cc->iv_gen_ops && cc->iv_gen_ops->dtr)
		cc->iv_gen_ops->dtr(cc);
#if defined(CONFIG_OCF_DM_CRYPT)
	crypto_freesession(cc->ocf_cryptoid);
#else
	crypto_free_blkcipher(cc->tfm);
#endif
	dm_put_device(ti, cc->dev);

	/* Must zero key material before freeing */
	memset(cc, 0, sizeof(*cc) + cc->key_size * sizeof(u8));
	kfree(cc);
}

static int crypt_map(struct dm_target *ti, struct bio *bio)
{
	struct crypt_config *cc = ti->private;
	struct crypt_io *io;
#ifdef CONFIG_HIGHMEM
	struct bio *newbio = NULL;
	struct bio_vec *from, *to;
	struct page *page;
	char *vto, *vfrom;
	unsigned int i;
#endif /* CONFIG_HIGHMEM */

	io = mempool_alloc(cc->io_pool, GFP_NOIO);

	/*
	 * Because OCF and CESA do not support high memory
	 * we have to create bounce pages if request
	 * with data in high memory arrives.
	 */

#ifdef CONFIG_HIGHMEM
	/* Check if we have to bounce */
	bio_for_each_segment(from, bio, i) {
		page = from->bv_page;

		if (!PageHighMem(page))
			continue;

		/* We have to bounce */
		if (newbio == NULL) {
			newbio = bio_alloc(GFP_NOIO, bio->bi_vcnt);
			memset(newbio->bi_io_vec, 0, bio->bi_vcnt *
							sizeof(struct bio_vec));
		}

		/* Allocate new vector */
		to = newbio->bi_io_vec + i;
		to->bv_page = mempool_alloc(cc->page_pool, GFP_NOIO);
		to->bv_len = from->bv_len;
		to->bv_offset = from->bv_offset;

		/* Copy data if this is required */
		if (bio_data_dir(bio) == WRITE) {
			vto = page_address(to->bv_page) + to->bv_offset;
			vfrom = kmap(from->bv_page) + from->bv_offset;
			memcpy(vto, vfrom, to->bv_len);
			kunmap(from->bv_page);
		}
	}

	/* We have at least one page bounced */
	if (newbio != NULL) {
		__bio_for_each_segment(from, bio, i, 0) {
			to = bio_iovec_idx(newbio, i);
			if (!to->bv_page) {
				to->bv_page = from->bv_page;
				to->bv_len = from->bv_len;
				to->bv_offset = from->bv_offset;
			}
		}

		newbio->bi_bdev = bio->bi_bdev;
		newbio->bi_sector = bio->bi_sector;
		newbio->bi_rw = bio->bi_rw;
		newbio->bi_vcnt = bio->bi_vcnt;
		newbio->bi_idx = bio->bi_idx;
		newbio->bi_size = bio->bi_size;

		newbio->bi_flags |= (1 << BIO_BOUNCED);
		newbio->bi_private = bio;
		bio = newbio;
	}
#endif /* CONFIG_HIGHMEM */

	io->target = ti;
	io->base_bio = bio;
	io->error = io->post_process = 0;
	atomic_set(&io->pending, 0);
	kcryptd_queue_io(io);

	return DM_MAPIO_SUBMITTED;
}

static void crypt_status(struct dm_target *ti, status_type_t type,
			 unsigned status_flags, char *result, unsigned maxlen)
{
	struct crypt_config *cc = ti->private;
	unsigned i, sz = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		result[0] = '\0';
		break;

	case STATUSTYPE_TABLE:
		if (cc->iv_mode)
			DMEMIT("%s-%s-%s ", cc->cipher, cc->chainmode,
			       cc->iv_mode);
		else
			DMEMIT("%s-%s ", cc->cipher, cc->chainmode);

		if (cc->key_size > 0)
			for (i = 0; i < cc->key_size; i++)
				DMEMIT("%02x", cc->key[i]);
		else
			DMEMIT("-");

		DMEMIT(" %llu %s %llu", (unsigned long long)cc->iv_offset,
				cc->dev->name, (unsigned long long)cc->start);
		break;
	}
}

static void crypt_postsuspend(struct dm_target *ti)
{
	struct crypt_config *cc = ti->private;
	set_bit(DM_CRYPT_SUSPENDED, &cc->flags);
}

static int crypt_preresume(struct dm_target *ti)
{
	struct crypt_config *cc = ti->private;
	if (!test_bit(DM_CRYPT_KEY_VALID, &cc->flags)) {
		DMERR("aborting resume - crypt key is not set.");
		return -EAGAIN;
	}

	return 0;
}

static void crypt_resume(struct dm_target *ti)
{
	struct crypt_config *cc = ti->private;

	clear_bit(DM_CRYPT_SUSPENDED, &cc->flags);
}

/* Message interface
 *	key set <key>
 *	key wipe
 */
static int crypt_message(struct dm_target *ti, unsigned argc, char **argv)
{
	struct crypt_config *cc = ti->private;
	if (argc < 2)
		goto error;

	if (!strnicmp(argv[0], MESG_STR("key"))) {
		if (!test_bit(DM_CRYPT_SUSPENDED, &cc->flags)) {
			DMWARN("not suspended during key manipulation.");
			return -EINVAL;
		}
		if (argc == 3 && !strnicmp(argv[1], MESG_STR("set")))
			return crypt_set_key(cc, argv[2]);
		if (argc == 2 && !strnicmp(argv[1], MESG_STR("wipe")))
			return crypt_wipe_key(cc);
	}

error:
	DMWARN("unrecognised message received.");
	return -EINVAL;
}

static int crypt_merge(struct dm_target *ti, struct bvec_merge_data *bvm,
		       struct bio_vec *biovec, int max_size)
{
	struct crypt_config *cc = ti->private;
	struct request_queue *q = bdev_get_queue(cc->dev->bdev);
	if (!q->merge_bvec_fn)
		return max_size;

	bvm->bi_bdev = cc->dev->bdev;
	bvm->bi_sector = cc->start + bvm->bi_sector - ti->begin;

	return min(max_size, q->merge_bvec_fn(q, bvm, biovec));
}

static int crypt_iterate_devices(struct dm_target *ti,
				 iterate_devices_callout_fn fn, void *data)
{
	struct crypt_config *cc = ti->private;
	return fn(ti, cc->dev, cc->start, ti->len, data);
}


static struct target_type crypt_target = {
	.name   = "crypt",
	.version = {1, 5, 1},
	.module = THIS_MODULE,
	.ctr    = crypt_ctr,
	.dtr    = crypt_dtr,
	.map    = crypt_map,
	.status = crypt_status,
	.postsuspend = crypt_postsuspend,
	.preresume = crypt_preresume,
	.resume = crypt_resume,
	.message = crypt_message,
	.merge  = crypt_merge,
	.iterate_devices = crypt_iterate_devices,
};

static int __init dm_crypt_init(void)
{
	int r;
	_crypt_io_pool = KMEM_CACHE(crypt_io, 0);
	if (!_crypt_io_pool)
		return -ENOMEM;

	_kcryptd_workqueue = create_workqueue("kcryptd");
	if (!_kcryptd_workqueue) {
		r = -ENOMEM;
		DMERR("couldn't create kcryptd");
		goto bad1;
	}

	r = dm_register_target(&crypt_target);
	if (r < 0) {
		DMERR("register failed %d", r);
		goto bad2;
	}

	_crypt_requests = 0;
	init_waitqueue_head(&_crypt_waitq);

#ifdef CONFIG_OCF_DM_CRYPT
	printk("dm_crypt using the OCF package.\n");
#endif

	return 0;

bad2:
	destroy_workqueue(_kcryptd_workqueue);
bad1:
	kmem_cache_destroy(_crypt_io_pool);
	return r;
}

static void __exit dm_crypt_exit(void)
{
	dm_unregister_target(&crypt_target);
	destroy_workqueue(_kcryptd_workqueue);
	kmem_cache_destroy(_crypt_io_pool);
}

module_init(dm_crypt_init);
module_exit(dm_crypt_exit);

MODULE_AUTHOR("Christophe Saout <christophe@saout.de>");
MODULE_DESCRIPTION(DM_NAME " target for transparent encryption / decryption");
MODULE_LICENSE("GPL");
