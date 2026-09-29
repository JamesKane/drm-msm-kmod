/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 James Kane
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * The rest of the Linux interfaces msm uses: Command DB lookups on top of
 * qcom_cmd_db(4), the MDT firmware loader on top of qcom_scm(4), GEM pages
 * from shmem, and the debugfs and KMS entry points of the parts of msm not
 * built here.
 */

#include <sys/param.h>
#include <sys/elf32.h>

#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/soc/qcom/mdt_loader.h>

#include <soc/qcom/cmd-db.h>

#include <dev/qcom_cmd_db/qcom_cmd_db.h>
#include <dev/qcom_scm/qcom_scm.h>

#include <drm/drm_gem.h>

#include "msm_drv.h"
#include "msm_debugfs.h"
#include "msm_kms.h"

/* Command DB */

u32
cmd_db_read_addr(const char *id)
{
	return (qcom_cmd_db_read_addr(id));
}

const void *
cmd_db_read_aux_data(const char *id, size_t *len)
{
	const void *data;

	data = qcom_cmd_db_read_aux_data(id, len);
	return (data != NULL ? data : ERR_PTR(-ENODEV));
}

bool
cmd_db_match_resource_addr(u32 addr1, u32 addr2)
{
	/* Regulators (VRM) match on the whole address, others on 20 bits. */
	if (((addr1 >> 16) & 0xf) == CMD_DB_HW_VRM)
		return ((addr1 & 0xfffffff0) == (addr2 & 0xfffffff0));
	return (addr1 == addr2);
}

enum cmd_db_hw_type
cmd_db_read_slave_id(const char *id)
{
	int t;

	t = qcom_cmd_db_read_slave_id(id);
	return (t < 0 ? CMD_DB_HW_INVALID : t);
}

int
cmd_db_ready(void)
{
	return (-qcom_cmd_db_ready());
}

/*
 * MDT firmware: an ELF32 image, possibly split into a header file and one
 * file per segment ("<name>.bNN").  The program headers' flags mark the hash
 * segment, which authenticates the others, and relocatable images.
 */
#define	MDT_TYPE_MASK		(7u << 24)
#define	MDT_TYPE_HASH		(2u << 24)
#define	MDT_RELOCATABLE		(1u << 27)

static bool
mdt_seg_loadable(const Elf32_Phdr *ph)
{
	return (ph->p_type == PT_LOAD &&
	    (ph->p_flags & MDT_TYPE_MASK) != MDT_TYPE_HASH &&
	    ph->p_memsz != 0);
}

static int
mdt_headers(const struct firmware *fw, const Elf32_Ehdr **ehp,
    const Elf32_Phdr **php)
{
	const Elf32_Ehdr *eh = (const void *)fw->data;

	if (fw->size < sizeof(*eh) || !IS_ELF(*eh) ||
	    eh->e_ident[EI_CLASS] != ELFCLASS32 ||
	    eh->e_phentsize != sizeof(Elf32_Phdr) ||
	    eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf32_Phdr) > fw->size)
		return (-EINVAL);
	*ehp = eh;
	*php = (const void *)(fw->data + eh->e_phoff);
	return (0);
}

static void
mdt_span(const Elf32_Ehdr *eh, const Elf32_Phdr *ph, u64 *min, u64 *max,
    bool *reloc)
{
	int i;

	*min = UINT64_MAX;
	*max = 0;
	*reloc = false;
	for (i = 0; i < eh->e_phnum; i++) {
		if (!mdt_seg_loadable(&ph[i]))
			continue;
		if ((ph[i].p_flags & MDT_RELOCATABLE) != 0)
			*reloc = true;
		*min = MIN(*min, ph[i].p_paddr);
		*max = MAX(*max, roundup2((u64)ph[i].p_paddr + ph[i].p_memsz,
		    PAGE_SIZE));
	}
}

ssize_t
qcom_mdt_get_size(const struct firmware *fw)
{
	const Elf32_Ehdr *eh;
	const Elf32_Phdr *ph;
	u64 min, max;
	bool reloc;

	if (mdt_headers(fw, &eh, &ph) != 0)
		return (-EINVAL);
	mdt_span(eh, ph, &min, &max, &reloc);
	return (min < max ? (ssize_t)(max - min) : -EINVAL);
}

/* Read segment n from its own file, "<name without extension>.bNN". */
static int
mdt_load_split(void *dst, const Elf32_Phdr *ph, int n, const char *fw_name,
    struct device *dev)
{
	const struct firmware *seg;
	char *name;
	size_t len;
	int error;

	len = strlen(fw_name);
	if (len < 4)
		return (-EINVAL);
	name = kstrdup(fw_name, GFP_KERNEL);
	snprintf(name + len - 3, 4, "b%02d", n);
	error = request_firmware(&seg, name, dev);
	if (error == 0) {
		if (seg->size != ph->p_filesz)
			error = -EINVAL;
		else
			memcpy(dst, seg->data, seg->size);
		release_firmware(seg);
	}
	kfree(name);
	return (error);
}

int
qcom_mdt_load(struct device *dev, const struct firmware *fw,
    const char *fw_name, int pas_id, void *mem_region, phys_addr_t mem_phys,
    size_t mem_size, phys_addr_t *reloc_base)
{
	const Elf32_Ehdr *eh;
	const Elf32_Phdr *ph;
	u8 *meta;
	size_t hdr_size, hash_size;
	u64 min, max, base;
	s64 off;
	bool reloc;
	int error, hash, i;

	if ((error = mdt_headers(fw, &eh, &ph)) != 0)
		return (error);

	/* Metadata: the headers (the first segment) and the hash segment. */
	for (hash = 0; hash < eh->e_phnum; hash++)
		if ((ph[hash].p_flags & MDT_TYPE_MASK) == MDT_TYPE_HASH)
			break;
	if (hash == eh->e_phnum || ph[0].p_filesz > fw->size)
		return (-EINVAL);
	hdr_size = ph[0].p_filesz;
	hash_size = ph[hash].p_filesz;
	meta = kmalloc(hdr_size + hash_size, GFP_KERNEL);
	memcpy(meta, fw->data, hdr_size);
	if (hdr_size + hash_size == fw->size)		/* packed after them */
		memcpy(meta + hdr_size, fw->data + hdr_size, hash_size);
	else if ((size_t)ph[hash].p_offset + hash_size <= fw->size)
		memcpy(meta + hdr_size, fw->data + ph[hash].p_offset,
		    hash_size);
	else
		error = mdt_load_split(meta + hdr_size, &ph[hash], hash,
		    fw_name, dev);
	if (error == 0)
		error = -qcom_scm_pas_init_image(pas_id, meta,
		    hdr_size + hash_size);
	kfree(meta);
	if (error != 0)
		return (error);

	mdt_span(eh, ph, &min, &max, &reloc);
	if (reloc) {
		error = -qcom_scm_pas_mem_setup(pas_id, mem_phys, max - min);
		if (error != 0)
			return (error);
		base = min;
	} else
		base = mem_phys;

	for (i = 0; i < eh->e_phnum; i++) {
		if (!mdt_seg_loadable(&ph[i]))
			continue;
		off = (s64)ph[i].p_paddr - (s64)base;
		if (off < 0 || off + ph[i].p_memsz > mem_size)
			return (-EINVAL);
		if (ph[i].p_filesz > ph[i].p_memsz)
			return (-EINVAL);
		if (ph[i].p_filesz != 0 && ph[i].p_offset < fw->size &&
		    (size_t)ph[i].p_offset + ph[i].p_filesz <= fw->size)
			memcpy((u8 *)mem_region + off, fw->data +
			    ph[i].p_offset, ph[i].p_filesz);
		else if (ph[i].p_filesz != 0 &&
		    (error = mdt_load_split((u8 *)mem_region + off, &ph[i], i,
		    fw_name, dev)) != 0)
			return (error);
		memset((u8 *)mem_region + off + ph[i].p_filesz, 0,
		    ph[i].p_memsz - ph[i].p_filesz);
	}
	wmb();
	if (reloc_base != NULL)
		*reloc_base = base;
	return (0);
}

/* GEM pages, from the object's shmem backing (drm-kmod leaves these out). */

struct page **
drm_gem_get_pages(struct drm_gem_object *obj)
{
	struct page **pages, *p;
	long i, n;

	if (obj->filp == NULL)
		return (ERR_PTR(-EINVAL));
	n = obj->size >> PAGE_SHIFT;
	pages = kvmalloc_array(n, sizeof(*pages), GFP_KERNEL);
	if (pages == NULL)
		return (ERR_PTR(-ENOMEM));
	for (i = 0; i < n; i++) {
		p = shmem_read_mapping_page_gfp(obj->filp->f_shmem, i,
		    GFP_KERNEL);
		if (IS_ERR(p)) {
			while (i-- > 0)
				put_page(pages[i]);
			kvfree(pages);
			return (ERR_CAST(p));
		}
		pages[i] = p;
	}
	return (pages);
}

void
drm_gem_put_pages(struct drm_gem_object *obj, struct page **pages, bool dirty,
    bool accessed)
{
	long i, n;

	n = obj->size >> PAGE_SHIFT;
	for (i = 0; i < n; i++) {
		if (dirty)
			set_page_dirty(pages[i]);
		if (accessed)
			mark_page_accessed(pages[i]);
		put_page(pages[i]);
	}
	kvfree(pages);
}

/* Parts of msm not built here: debugfs, submit dumps, and KMS. */

void
msm_debugfs_init(struct drm_minor *minor __unused)
{
}

int
msm_debugfs_late_init(struct drm_device *dev __unused)
{
	return (0);
}

void
msm_rd_debugfs_cleanup(struct msm_drm_private *priv __unused)
{
}

void
msm_rd_dump_submit(struct msm_rd_state *rd __unused,
    struct msm_gem_submit *submit __unused, const char *fmt __unused, ...)
{
}

void
msm_perf_debugfs_cleanup(struct msm_drm_private *priv __unused)
{
}

int
msm_drm_kms_init(struct device *dev __unused,
    const struct drm_driver *drv __unused)
{
	return (-ENODEV);
}

void
msm_drm_kms_uninit(struct device *dev __unused)
{
}

int
msm_fbdev_driver_fbdev_probe(struct drm_fb_helper *helper __unused,
    struct drm_fb_helper_surface_size *sizes __unused)
{
	return (-ENODEV);
}
