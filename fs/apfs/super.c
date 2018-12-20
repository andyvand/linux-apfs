// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/apfs/super.c
 *
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/parser.h>
#include <linux/buffer_head.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/iversion.h>
#include "apfs.h"
#include "btree.h"
#include "inode.h"
#include "key.h"
#include "message.h"
#include "super.h"
#include "table.h"
#include "xattr.h"

/*
 * Note that this is not a generic implementation of fletcher64, as it assumes
 * a message length that doesn't overflow sum1 and sum2.  This constraint is ok
 * for apfs, though, since the block size is limited to 2^16.  For a more
 * generic optimized implementation, see Nakassis (1988).
 */
static u64 apfs_fletcher64(void *addr, size_t len)
{
	__le32 *buff = addr;
	u64 sum1 = 0;
	u64 sum2 = 0;
	u64 c1, c2;
	int i;

	for (i = 0; i < len/sizeof(u32); i++) {
		sum1 += le32_to_cpu(buff[i]);
		sum2 += sum1;
	}

	c1 = sum1 + sum2;
	c1 = 0xFFFFFFFF - do_div(c1, 0xFFFFFFFF);
	c2 = sum1 + c1;
	c2 = 0xFFFFFFFF - do_div(c2, 0xFFFFFFFF);

	return (c2 << 32) | c1;
}

static int apfs_obj_verify_csum(struct super_block *sb,
				struct apfs_obj_phys *obj)
{
	return  (le64_to_cpu(obj->o_cksum) ==
		 apfs_fletcher64((char *) obj + APFS_MAX_CKSUM_SIZE,
				 sb->s_blocksize - APFS_MAX_CKSUM_SIZE));
}

/**
 * apfs_map_main_super - Verify the container superblock and map it into memory
 * @sb:	superblock structure
 *
 * Returns a negative error code in case of failure.  On success, returns 0
 * and sets APFS_SB(@sb)->s_msb_raw and APFS_SB(@sb)->s_mnode.
 */
static int apfs_map_main_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *msb_raw;
	struct buffer_head *bh;
	int blocksize;
	int err = -EINVAL;

	/*
	 * For now assume a small blocksize, we only need it so that we can
	 * read the actual blocksize from disk.
	 */
	if (!sb_set_blocksize(sb, APFS_NX_DEFAULT_BLOCK_SIZE)) {
		apfs_err(sb, "unable to set blocksize");
		return err;
	}
	bh = sb_bread(sb, APFS_NX_BLOCK_NUM);
	if (!bh) {
		apfs_err(sb, "unable to read superblock");
		return err;
	}
	msb_raw = (struct apfs_nx_superblock *)bh->b_data;
	blocksize = le32_to_cpu(msb_raw->nx_block_size);

	if (sb->s_blocksize != blocksize) {
		brelse(bh);

		if (!sb_set_blocksize(sb, blocksize)) {
			apfs_err(sb, "bad blocksize %d", blocksize);
			return err;
		}
		bh = sb_bread(sb, APFS_NX_BLOCK_NUM);
		if (!bh) {
			apfs_err(sb, "unable to read superblock 2nd time");
			return err;
		}
		msb_raw = (struct apfs_nx_superblock *)bh->b_data;
	}

	sb->s_magic = le32_to_cpu(msb_raw->nx_magic);
	if (sb->s_magic != APFS_NX_MAGIC) {
		apfs_err(sb, "not an apfs filesystem");
		goto fail;
	}
	if (!apfs_obj_verify_csum(sb, &msb_raw->nx_o)) {
		apfs_err(sb, "inconsistent container superblock");
		goto fail;
	}

	sbi->s_msb_raw = msb_raw;
	sbi->s_mnode.sb = sb;
	sbi->s_mnode.block_nr = APFS_NX_BLOCK_NUM;
	sbi->s_mnode.node_id = le64_to_cpu(msb_raw->nx_o.o_oid);
	sbi->s_mnode.bh = bh;
	return 0;

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_unmap_main_super - Clean up apfs_map_main_super()
 * @sb:	filesystem superblock
 */
static inline void apfs_unmap_main_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);

	brelse(sbi->s_mnode.bh);
}

/**
 * apfs_map_volume_super - Find the volume superblock and map it into memory
 * @sb:	superblock structure
 *
 * Returns a negative error code in case of failure.  On success, returns 0
 * and sets APFS_SB(@sb)->s_vsb_raw and APFS_SB(@sb)->s_vnode.
 */
static int apfs_map_volume_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *msb_raw = sbi->s_msb_raw;
	struct apfs_superblock *vsb_raw;
	struct apfs_omap_phys *msb_omap_raw;
	struct apfs_table *vtable;
	struct buffer_head *bh;
	u64 vol_id;
	u64 msb_omap, vb, vsb;
	int err;

	/* Get the id for the requested volume number */
	if (sizeof(*msb_raw) + 8 * (sbi->s_vol_nr + 1) >= sb->s_blocksize) {
		/* For now we assume that nodesize <= PAGE_SIZE */
		apfs_err(sb, "volume number out of range");
		return -EINVAL;
	}
	vol_id = le64_to_cpu(msb_raw->nx_fs_oid[sbi->s_vol_nr]);
	if (vol_id == 0) {
		apfs_err(sb, "requested volume does not exist");
		return -EINVAL;
	}

	/* Get the container's object map */
	msb_omap = le64_to_cpu(msb_raw->nx_omap_oid);
	bh = sb_bread(sb, msb_omap);
	if (!bh) {
		apfs_err(sb, "unable to read container object map");
		return -EINVAL;
	}
	msb_omap_raw = (struct apfs_omap_phys *)bh->b_data;

	/* Get the Volume Block */
	vb = le64_to_cpu(msb_omap_raw->om_tree_oid);
	msb_omap_raw = NULL;
	brelse(bh);

	vtable = apfs_read_table(sb, vb);
	if (IS_ERR(vtable)) {
		apfs_err(sb, "unable to read volume block");
		return PTR_ERR(vtable);
	}

	err = apfs_omap_lookup_block(sb, vtable, vol_id, &vsb);
	apfs_table_put(vtable);
	if (err) {
		apfs_err(sb, "volume not found, likely corruption");
		return err;
	}

	bh = sb_bread(sb, vsb);
	if (!bh) {
		apfs_err(sb, "unable to read volume superblock");
		return -EINVAL;
	}

	vsb_raw = (struct apfs_superblock *)bh->b_data;
	if (le32_to_cpu(vsb_raw->apfs_magic) != APFS_MAGIC) {
		apfs_err(sb, "wrong magic in volume superblock");
		brelse(bh);
		return -EINVAL;
	}

	sbi->s_vsb_raw = vsb_raw;
	sbi->s_vnode.sb = sb;
	sbi->s_vnode.block_nr = vsb;
	sbi->s_vnode.node_id = le64_to_cpu(vsb_raw->apfs_o.o_oid);
	sbi->s_vnode.bh = bh;
	return 0;
}

/**
 * apfs_unmap_volume_super - Clean up apfs_map_volume_super()
 * @sb:	filesystem superblock
 */
static inline void apfs_unmap_volume_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);

	brelse(sbi->s_vnode.bh);
}

/**
 * apfs_read_omap - Find and read the omap root node
 * @sb:	superblock structure
 *
 * On success, returns 0 and sets APFS_SB(@sb)->s_omap_root; on failure returns
 * a negative error code.
 */
static int apfs_read_omap(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	struct apfs_omap_phys *omap_raw;
	struct apfs_table *omap_root;
	struct buffer_head *bh;
	u64 omap_blk, omap_root_blk;

	/* Get the block holding the volume omap information */
	omap_blk = le64_to_cpu(vsb_raw->apfs_omap_oid);
	bh = sb_bread(sb, omap_blk);
	if (!bh) {
		apfs_err(sb, "unable to read the volume object map");
		return -EINVAL;
	}
	omap_raw = (struct apfs_omap_phys *)bh->b_data;

	/* Get the volume's object map */
	omap_root_blk = le64_to_cpu(omap_raw->om_tree_oid);
	brelse(bh);
	omap_root = apfs_read_table(sb, omap_root_blk);
	if (IS_ERR(omap_root)) {
		apfs_err(sb, "unable to read the omap root node");
		return PTR_ERR(omap_root);
	}

	sbi->s_omap_root = omap_root;
	return 0;
}

/**
 * apfs_read_catalog - Find and read the catalog root node
 * @sb:	superblock structure
 *
 * On success, returns 0 and sets APFS_SB(@sb)->s_cat_root; on failure returns
 * a negative error code.
 */
static int apfs_read_catalog(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	struct apfs_table *root_table;
	u64 root_id;

	root_id = le64_to_cpu(vsb_raw->apfs_root_tree_oid);
	root_table = apfs_omap_read_table(sb, root_id);
	if (IS_ERR(root_table)) {
		apfs_err(sb, "unable to read catalog root node");
		return PTR_ERR(root_table);
	}
	sbi->s_cat_root = root_table;
	return 0;
}

static void apfs_put_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);

	apfs_table_put(sbi->s_cat_root);
	apfs_table_put(sbi->s_omap_root);

	apfs_unmap_main_super(sb);
	apfs_unmap_volume_super(sb);

	sb->s_fs_info = NULL;
	kfree(sbi);
}

static struct kmem_cache *apfs_inode_cachep;

static struct inode *apfs_alloc_inode(struct super_block *sb)
{
	struct apfs_inode_info *ai;

	ai = kmem_cache_alloc(apfs_inode_cachep, GFP_KERNEL);
	if (!ai)
		return NULL;
	inode_set_iversion(&ai->vfs_inode, 1);
	return &ai->vfs_inode;
}

static void apfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	kmem_cache_free(apfs_inode_cachep, APFS_I(inode));
}

static void apfs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, apfs_i_callback);
}

static void init_once(void *p)
{
	struct apfs_inode_info *ai = (struct apfs_inode_info *)p;

	spin_lock_init(&ai->i_extent_lock);
	ai->i_cached_extent.len = 0;
	inode_init_once(&ai->vfs_inode);
}

static int __init init_inodecache(void)
{
	apfs_inode_cachep = kmem_cache_create("apfs_inode_cache",
					     sizeof(struct apfs_inode_info),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD|SLAB_ACCOUNT),
					     init_once);
	if (apfs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(apfs_inode_cachep);
}

/**
 * apfs_count_used_blocks - Count the blocks in use across all volumes
 * @sb:		filesystem superblock
 * @count:	on return it will store the block count
 *
 * This function probably belongs in a separate file, but for now it is
 * only called by statfs.
 */
static int apfs_count_used_blocks(struct super_block *sb, u64 *count)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *msb_raw = sbi->s_msb_raw;
	struct apfs_table *vtable;
	struct apfs_omap_phys *msb_omap_raw;
	struct apfs_superblock *vsb_raw;
	struct buffer_head *bh;
	u64 msb_omap, vb, vsb;
	int i;
	int err = 0;

	/* Get the container's object map */
	msb_omap = le64_to_cpu(msb_raw->nx_omap_oid);
	bh = sb_bread(sb, msb_omap);
	if (!bh) {
		apfs_err(sb, "unable to read container object map");
		return -EIO;
	}
	msb_omap_raw = (struct apfs_omap_phys *)bh->b_data;

	/* Get the Volume Block */
	vb = le64_to_cpu(msb_omap_raw->om_tree_oid);
	msb_omap_raw = NULL;
	brelse(bh);
	bh = NULL;
	vtable = apfs_read_table(sb, vb);
	if (IS_ERR(vtable)) {
		apfs_err(sb, "unable to read volume block");
		return PTR_ERR(vtable);
	}

	/* Iterate through the checkpoint superblocks and add the used blocks */
	*count = 0;
	for (i = 0; i < vtable->t_records; i++) {
		int len, off;
		__le64 *block;

		len = apfs_table_locate_data(vtable, i, &off);
		if (len != 16) {
			err = -EIO;
			apfs_err(sb, "bad index in volume block");
			goto cleanup;
		}

		/* The block number is in the second 64 bits of data */
		block = (__le64 *)(vtable->t_node.bh->b_data + off + 8);
		vsb = le64_to_cpu(*block);

		bh = sb_bread(sb, vsb);
		if (!bh) {
			err = -EIO;
			apfs_err(sb, "unable to read volume superblock");
			goto cleanup;
		}

		vsb_raw = (struct apfs_superblock *)bh->b_data;
		*count += le64_to_cpu(vsb_raw->apfs_fs_alloc_count);
		brelse(bh);
	}

cleanup:
	apfs_table_put(vtable);
	return err;
}

static int apfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *msb_raw = sbi->s_msb_raw;
	struct apfs_superblock *vol = sbi->s_vsb_raw;
	u64 fsid, used_blocks = 0;
	int err;

	buf->f_type = APFS_SUPER_MAGIC;
	/* Nodes are assumed to fit in a page, for now */
	buf->f_bsize = sb->s_blocksize;

	/* Volumes share the whole disk space */
	buf->f_blocks = le64_to_cpu(msb_raw->nx_block_count);
	err = apfs_count_used_blocks(sb, &used_blocks);
	if (err)
		return err;
	buf->f_bfree = buf->f_blocks - used_blocks;
	buf->f_bavail = buf->f_bfree; /* I don't know any better */

	/* The file count is only for the mounted volume */
	buf->f_files = le64_to_cpu(vol->apfs_num_files) +
		       le64_to_cpu(vol->apfs_num_directories) +
		       le64_to_cpu(vol->apfs_num_symlinks) +
		       le64_to_cpu(vol->apfs_num_other_fsobjects);

	/*
	 * buf->f_ffree is left undefined for now. Maybe it should report the
	 * number of available cnids, like hfsplus attempts to do.
	 */

	buf->f_namelen = 255; /* Again, I don't know any better */

	/* There are no clear rules for the fsid, so we follow ext2 here */
	fsid = le64_to_cpup((void *)vol->apfs_vol_uuid) ^
	       le64_to_cpup((void *)vol->apfs_vol_uuid + sizeof(u64));
	buf->f_fsid.val[0] = fsid & 0xFFFFFFFFUL;
	buf->f_fsid.val[1] = (fsid >> 32) & 0xFFFFFFFFUL;

	return 0;
}

static int apfs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct apfs_sb_info *sbi = APFS_SB(root->d_sb);

	if (sbi->s_vol_nr != 0)
		seq_printf(seq, ",vol=%u", sbi->s_vol_nr);
	if (sbi->s_flags & APFS_UID_OVERRIDE)
		seq_printf(seq, ",uid=%u", from_kuid(&init_user_ns,
						     sbi->s_uid));
	if (sbi->s_flags & APFS_GID_OVERRIDE)
		seq_printf(seq, ",gid=%u", from_kgid(&init_user_ns,
						     sbi->s_gid));

	return 0;
}

static const struct super_operations apfs_sops = {
	.alloc_inode	= apfs_alloc_inode,
	.destroy_inode	= apfs_destroy_inode,
	.put_super	= apfs_put_super,
	.statfs		= apfs_statfs,
	.show_options	= apfs_show_options,
};

enum {
	Opt_uid, Opt_gid, Opt_vol, Opt_err,
};

static const match_table_t tokens = {
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_vol, "vol=%u"},
	{Opt_err, NULL}
};

/*
 * Many of the parse_options() functions in other file systems return 0
 * on error. This one returns an error code, and 0 on success.
 */
static int parse_options(struct super_block *sb, char *options)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int option;
	int err = 0;

	/* Set default values before parsing */
	sbi->s_vol_nr = 0;
	sbi->s_flags = 0;

	if (!options)
		return 0;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;

		if (!*p)
			continue;
		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_uid:
			err = match_int(&args[0], &option);
			if (err)
				return err;
			sbi->s_uid = make_kuid(current_user_ns(), option);
			if (!uid_valid(sbi->s_uid)) {
				apfs_err(sb, "invalid uid");
				return -EINVAL;
			}
			sbi->s_flags |= APFS_UID_OVERRIDE;
			break;
		case Opt_gid:
			err = match_int(&args[0], &option);
			if (err)
				return err;
			sbi->s_gid = make_kgid(current_user_ns(), option);
			if (!gid_valid(sbi->s_gid)) {
				apfs_err(sb, "invalid gid");
				return -EINVAL;
			}
			sbi->s_flags |= APFS_GID_OVERRIDE;
			break;
		case Opt_vol:
			err = match_int(&args[0], &sbi->s_vol_nr);
			if (err)
				return err;
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

static int apfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct apfs_sb_info *sbi;
	struct inode *root;
	int err;

	apfs_notice(sb, "this module is read-only");
	sb->s_flags |= SB_RDONLY;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;

	err = apfs_map_main_super(sb);
	if (err)
		goto failed_main_super;

	/* For now we only support nodesize < PAGE_SIZE */
	sbi->s_nodesize = sb->s_blocksize;
	sbi->s_nodesize_bits = sb->s_blocksize_bits;

	err = parse_options(sb, data);
	if (err)
		goto failed_volume_super;

	err = apfs_map_volume_super(sb);
	if (err)
		goto failed_volume_super;

	/* The omap needs to be set before the call to apfs_read_catalog() */
	err = apfs_read_omap(sb);
	if (err)
		goto failed_omap;

	err = apfs_read_catalog(sb);
	if (err)
		goto failed_cat;

	sb->s_op = &apfs_sops;
	sb->s_d_op = &apfs_dentry_operations;
	sb->s_xattr = apfs_xattr_handlers;
	sb->s_maxbytes = MAX_LFS_FILESIZE;

	root = apfs_iget(sb, APFS_ROOT_DIR_INO_NUM);
	if (IS_ERR(root)) {
		apfs_err(sb, "unable to get root inode");
		err = PTR_ERR(root);
		goto failed_mount;
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		apfs_err(sb, "unable to get root dentry");
		err = -ENOMEM;
		goto failed_mount;
	}
	return 0;

failed_mount:
	apfs_table_put(sbi->s_cat_root);
failed_cat:
	apfs_table_put(sbi->s_omap_root);
failed_omap:
	apfs_unmap_volume_super(sb);
failed_volume_super:
	apfs_unmap_main_super(sb);
failed_main_super:
	sb->s_fs_info = NULL;
	kfree(sbi);
	return err;
}

static struct dentry *apfs_mount(struct file_system_type *fs_type,
		int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, apfs_fill_super);
}

static struct file_system_type apfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "apfs",
	.mount		= apfs_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS("apfs");

static int __init init_apfs_fs(void)
{
	int err = 0;

	err = init_inodecache();
	if (err)
		return err;
	err = register_filesystem(&apfs_fs_type);
	if (err)
		destroy_inodecache();
	return err;
}

static void __exit exit_apfs_fs(void)
{
	unregister_filesystem(&apfs_fs_type);
	destroy_inodecache();
}

MODULE_AUTHOR("Ernesto A. Fernández");
MODULE_DESCRIPTION("Apple File System");
MODULE_LICENSE("GPL");
module_init(init_apfs_fs)
module_exit(exit_apfs_fs)
