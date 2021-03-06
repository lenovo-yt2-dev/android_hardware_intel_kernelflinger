/*
 * Copyright (c) 2014, Intel Corporation
 * All rights reserved.
 *
 * Authors: Sylvain Chouleur <sylvain.chouleur@intel.com>
 *          Jeremy Compostella <jeremy.compostella@intel.com>
 *          Jocelyn Falempe <jocelyn.falempe@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <efi.h>
#include <efilib.h>
#include <lib.h>
#include <openssl/evp.h>

#include "fastboot.h"
#include "uefi_utils.h"
#include "gpt.h"
#include "android.h"
#include "signature.h"
#include "security.h"

static struct algorithm {
	const CHAR8 *name;
	const EVP_MD *(*get_md)(void);
} const ALGORITHMS[] = {
	{ (CHAR8*)"sha1", EVP_sha1 }, /* default algorithm */
	{ (CHAR8*)"md5", EVP_md5 }
};

static const EVP_MD *selected_md;
static unsigned int hash_len;

EFI_STATUS set_hash_algorithm(const CHAR8 *algo)
{
	EFI_STATUS ret = EFI_SUCCESS;
	unsigned int i;

	/* Use default algorithm */
	if (!algo) {
		selected_md = ALGORITHMS[0].get_md();
		goto out;
	}

	selected_md = NULL;
	for (i = 0; i < ARRAY_SIZE(ALGORITHMS); i++)
		if (!strcmp(algo, ALGORITHMS[i].name))
			selected_md = ALGORITHMS[i].get_md();

	if (!selected_md)
		return EFI_UNSUPPORTED;

out:
	hash_len = EVP_MD_size(selected_md);
	return ret;
}

static void hash_buffer(CHAR8 *buffer, UINT64 len, CHAR8 *hash)
{
	EVP_MD_CTX mdctx;

	if (!selected_md)
		set_hash_algorithm(NULL);

	EVP_MD_CTX_init(&mdctx);
	EVP_DigestInit_ex(&mdctx, selected_md, NULL);
	EVP_DigestUpdate(&mdctx, buffer, len);
	EVP_DigestFinal_ex(&mdctx, hash, NULL);
	EVP_MD_CTX_cleanup(&mdctx);
}

static EFI_STATUS report_hash(const CHAR16 *base, const CHAR16 *name, CHAR8 *hash)
{
	EFI_STATUS ret;
	CHAR8 hashstr[hash_len * 2 + 1];

	ret = bytes_to_hex_stra(hash, hash_len, hashstr, sizeof(hashstr));
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to convert bytes to hexadecimal string");
		return ret;
	}

	fastboot_info("target: %s%s", base, name);
	fastboot_info("hash: %a", hashstr);

	return EFI_SUCCESS;
}

static UINTN get_bootimage_len(CHAR8 *buffer, UINTN buffer_len)
{
	struct boot_img_hdr *hdr;
	struct boot_signature *bs;
	UINTN len;

	if (buffer_len < sizeof(*hdr)) {
		error(L"boot image too small");
		return 0;
	}
	hdr = (struct boot_img_hdr *) buffer;

	if (strncmp((CHAR8 *) BOOT_MAGIC, hdr->magic, BOOT_MAGIC_SIZE)) {
		error(L"bad boot magic");
		return 0;
	}

	len = bootimage_size(hdr);
	debug(L"len %lld", len);

	if (len > buffer_len + BOOT_SIGNATURE_MAX_SIZE) {
		error(L"boot image too big");
		return 0;
	}

	bs = get_boot_signature(&buffer[len], BOOT_SIGNATURE_MAX_SIZE);

	if (bs) {
		len += bs->total_size;
		free_boot_signature(bs);
	} else {
		debug(L"boot image doesn't seem to have a signature");
	}

	debug(L"total boot image size %d", len);
	return len;
}

EFI_STATUS get_boot_image_hash(const CHAR16 *label)
{
	struct gpt_partition_interface gparti;
	CHAR8 *data;
	UINT64 len;
	UINT64 offset;
	CHAR8 hash[EVP_MAX_MD_SIZE];
	EFI_STATUS ret;

	ret = gpt_get_partition_by_label(label, &gparti, LOGICAL_UNIT_USER);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get partition %s", label);
		return ret;
	}

	len = (gparti.part.ending_lba + 1 - gparti.part.starting_lba) * gparti.bio->Media->BlockSize;
	offset = gparti.part.starting_lba * gparti.bio->Media->BlockSize;

	if (len > 100 * MiB) {
		error(L"partition too large to contain a boot image");
		return EFI_INVALID_PARAMETER;
	}
	data = AllocatePool(len);
	if (!data) {
		return EFI_OUT_OF_RESOURCES;
	}

	ret = uefi_call_wrapper(gparti.dio->ReadDisk, 5, gparti.dio, gparti.bio->Media->MediaId, offset, len, data);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read partition");
		FreePool(data);
		return ret;
	}

	len = get_bootimage_len(data, len);
	if (len) {
		hash_buffer(data, len, hash);
		ret = report_hash(L"/", label, hash);
	}
	FreePool(data);
	return ret;
}

#define MAX_DIR 10
#define MAX_FILENAME_LEN (256 * sizeof(CHAR16))
#define DIR_BUFFER_SIZE (MAX_DIR * MAX_FILENAME_LEN)
static CHAR16 *path;
static CHAR16 *subname[MAX_DIR];
static INTN subdir;

static EFI_STATUS hash_file(EFI_FILE *dir, EFI_FILE_INFO *fi)
{
	EFI_FILE *file;
	void *data;
	CHAR8 hash[EVP_MAX_MD_SIZE];
	EFI_STATUS ret;
	UINTN size;

	if (!fi->Size) {
		hash_buffer(NULL, 0, hash);
		return report_hash(path, fi->FileName, hash);
	}

	ret = uefi_call_wrapper(dir->Open, 5, dir, &file, fi->FileName, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(ret))
		return ret;

	size = fi->FileSize;

	data = AllocatePool(size);
	if (!data)
		goto close;

	ret = uefi_call_wrapper(file->Read, 3, file, &size, data);
	if (EFI_ERROR(ret))
		goto free;

	hash_buffer(data, size, hash);
	ret = report_hash(path, fi->FileName, hash);

free:
	FreePool(data);
close:
	uefi_call_wrapper(file->Close, 1, file);
	return ret;
}

/*
 * generate a string with the current directory
 * updated each time we open/close a directory
 */
 static void initpath(void)
 {
	path = AllocateZeroPool(DIR_BUFFER_SIZE);
	if (!path)
		return;
	StrCat(path, L"/bootloader/");
 }

static void freepath(void)
{
	if (!path)
		return;

	FreePool(path);
	path = NULL;
	debug(L"Free path");
}

static void pushdir(CHAR16 *dir)
{
	if (!path)
		return;

	if (StrSize(path) + StrSize(dir) > DIR_BUFFER_SIZE)
		return;

	subname[subdir] = path + StrLen(path);
	StrCat(path, dir);
	StrCat(path, L"/");
	debug(L"Opening %s", path);
}

static void popdir(void)
{
	if (!path)
		return;
	if (subdir > 0) {
		*subname[subdir - 1] = L'\0';
		debug(L"Return to %s", path);
		return;
	}
	freepath();
}

EFI_STATUS get_esp_hash(__attribute__((__unused__)) const CHAR16 *label)
{
	EFI_STATUS ret;
	EFI_FILE_IO_INTERFACE *io;
	EFI_FILE *dirs[MAX_DIR];
	CHAR8 buf[sizeof(EFI_FILE_INFO) + MAX_FILENAME_LEN];
	EFI_FILE_INFO *fi = (EFI_FILE_INFO *) buf;
	UINTN size = sizeof(buf);

	ret = get_esp_fs(&io);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get partition ESP");
		return ret;
	}

	subdir = 0;
	ret = uefi_call_wrapper(io->OpenVolume, 2, io, &dirs[subdir]);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to open root directory");
		return ret;
	}
	initpath();
	do {
		size = sizeof(buf);
		ret = uefi_call_wrapper(dirs[subdir]->Read, 3, dirs[subdir], &size, fi);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Cannot read directory entry");
			/* continue to walk the ESP partition */
			size = 0;
		}
		if (!size && subdir >= 0) {
			/* size is 0 means there are no more files/dir in current directory
			 * so if we are in a subdir, go back 1 level */
			uefi_call_wrapper(dirs[subdir]->Close, 1, dirs[subdir]);
			popdir();
			subdir--;
			continue;
		}
		if (fi->Attribute & EFI_FILE_DIRECTORY) {
			EFI_FILE *parent;

			if (!StrCmp(fi->FileName, L".") || !StrCmp(fi->FileName, L".."))
				continue;
			if (subdir == MAX_DIR - 1) {
				error(L"too much subdir, ignoring %s", fi->FileName);
				continue;
			}
			parent = dirs[subdir];
			pushdir(fi->FileName);
			subdir++;
			ret = uefi_call_wrapper(parent->Open, 5, parent, &dirs[subdir], fi->FileName, EFI_FILE_MODE_READ, 0);
			if (EFI_ERROR(ret)) {
				efi_perror(ret, L"Cannot open directory %s", fi->FileName);
				/* continue to walk the ESP partition */
				popdir();
				subdir--;
			}
		} else {
			ret = hash_file(dirs[subdir], fi);
			if (EFI_ERROR(ret)) {
				freepath();
				return ret;
			}
		}
	} while (size || subdir >= 0);
	return EFI_SUCCESS;
}

/*
 * minimum ext4 definition to get the total size of the filesystem
 */

#define EXT4_SB_OFFSET 1024
#define EXT4_SUPER_MAGIC 0xEF53
#define EXT4_VALID_FS 0x0001

struct ext4_super_block {
	INT32 unused;
	INT32 s_blocks_count_lo;
	INT32 unused2[4];
	INT32 s_log_block_size;
	INT32 unused3[7];
	UINT16 s_magic;
	UINT16 s_state;
	INT32 unused4[69];
	INT32 s_blocks_count_hi;
};

struct ext4_verity_header {
	UINT32 magic;
	UINT32 protocol_version;
};

/*
 * minimum squashfs definition to get the total size of the filesystem
 */

#define SQUASHFS_MAGIC 0x73717368
#define SQUASHFS_PADDING 4096

struct squashfs_super_block {
	UINT32 s_magic;
	UINT32 inodes;
	UINT32 mkfs_time;
	UINT32 block_size;
	UINT32 fragments;
	UINT16 compression;
	UINT16 block_log;
	UINT16 flags;
	UINT16 no_ids;
	UINT16 s_major;
	UINT16 s_minor;
	UINT64 root_inode;
	UINT64 bytes_used;
	UINT64 id_table_start;
	UINT64 xattr_id_table_start;
	UINT64 inode_table_start;
	UINT64 directory_table_start;
	UINT64 fragment_table_start;
	UINT64 lookup_table_start;
};

/* verity definition */

#define VERITY_METADATA_SIZE 32768
#define VERITY_METADATA_MAGIC_NUMBER 0xb001b001
#define VERITY_HASH_SIZE 32
#define VERITY_BLOCK_SIZE 4096
#define VERITY_HASHES_PER_BLOCK (VERITY_BLOCK_SIZE / VERITY_HASH_SIZE)

/* adapted from build_verity_tree.cpp */
static UINT64 verity_tree_blocks(UINT64 data_size, INT32 level)
{
	UINT64 level_blocks = DIV_ROUND_UP(data_size, VERITY_BLOCK_SIZE);

	do {
		level_blocks = DIV_ROUND_UP(level_blocks, VERITY_HASHES_PER_BLOCK);
	} while (level--);

	return level_blocks;
}

/* adapted from build_verity_tree.cpp */
static UINT64 verity_tree_size(UINT64 data_size)
{
	UINT64 verity_blocks = 0;
	UINT64 level_blocks;
	INT32 levels = 0;
	UINT64 tree_size;

	do {
		level_blocks = verity_tree_blocks(data_size, levels);
		levels++;
		verity_blocks += level_blocks;
	} while (level_blocks > 1);

	tree_size = verity_blocks * VERITY_BLOCK_SIZE;
	debug(L"verity tree size %lld", tree_size);
	return tree_size;
}

static EFI_STATUS read_partition(struct gpt_partition_interface *gparti, UINT64 offset, UINT64 len, void *data)
{
	UINT64 partlen;
	UINT64 partoffset;
	EFI_STATUS ret;

	partlen = (gparti->part.ending_lba + 1 - gparti->part.starting_lba) * gparti->bio->Media->BlockSize;
	partoffset = gparti->part.starting_lba * gparti->bio->Media->BlockSize;

	if (len + offset > partlen) {
		error(L"attempt to read outside of partition %s, (len %lld offset %lld partition len %lld)", gparti->part.name, len, offset, partlen);
		return EFI_INVALID_PARAMETER;
	}
	ret = uefi_call_wrapper(gparti->dio->ReadDisk, 5, gparti->dio, gparti->bio->Media->MediaId, partoffset + offset, len, data);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"read partition %s failed", gparti->part.name);
	return ret;
}

#define CHUNK 1024 * 1024
#define MIN(a, b) ((a < b) ? (a) : (b))
static EFI_STATUS hash_partition(struct gpt_partition_interface *gparti, UINT64 len, CHAR8 *hash)
{
	EVP_MD_CTX mdctx;
	CHAR8 *buffer;
	UINT64 offset;
	UINT64 chunklen;
	EFI_STATUS ret = EFI_INVALID_PARAMETER;

	buffer = AllocatePool(CHUNK);
	if (!buffer)
		return EFI_OUT_OF_RESOURCES;

	if (!selected_md)
		set_hash_algorithm(NULL);

	EVP_MD_CTX_init(&mdctx);
	EVP_DigestInit_ex(&mdctx, selected_md, NULL);

	for (offset = 0; offset < len; offset += CHUNK) {
		chunklen = MIN(len - offset, CHUNK);
		ret = read_partition(gparti, offset, chunklen, buffer);
		if (EFI_ERROR(ret))
			goto free;
		EVP_DigestUpdate(&mdctx, buffer, chunklen);
	}
	EVP_DigestFinal_ex(&mdctx, hash, NULL);

free:
	EVP_MD_CTX_cleanup(&mdctx);
	FreePool(buffer);
	return ret;
}

static EFI_STATUS get_ext4_len(struct gpt_partition_interface *gparti, UINT64 *len)
{
	UINT64 block_size;
	UINT64 len_blocks;
	struct ext4_super_block sb;
	EFI_STATUS ret;

	ret = read_partition(gparti, EXT4_SB_OFFSET, sizeof(sb), &sb);
	if (EFI_ERROR(ret))
		return ret;

	if (sb.s_magic != EXT4_SUPER_MAGIC)
		return EFI_INVALID_PARAMETER;

	if ((sb.s_state & EXT4_VALID_FS) != EXT4_VALID_FS) {
		debug(L"Ext4 invalid FS [%02x]", sb.s_state);
		return EFI_INVALID_PARAMETER;
	}
	block_size = 1024 << sb.s_log_block_size;
	len_blocks = ((UINT64) sb.s_blocks_count_hi << 32) + sb.s_blocks_count_lo;
	*len = block_size * len_blocks;

	return EFI_SUCCESS;
}

static EFI_STATUS get_squashfs_len(struct gpt_partition_interface *gparti, UINT64 *len)
{
	struct squashfs_super_block sb;
	UINT64 padding = SQUASHFS_PADDING;
	EFI_STATUS ret;

	ret = read_partition(gparti, 0, sizeof(sb), &sb);
	if (EFI_ERROR(ret))
		return ret;

	if (sb.s_magic != SQUASHFS_MAGIC)
		return EFI_INVALID_PARAMETER;

	if (sb.bytes_used % padding)
		*len = ((sb.bytes_used + padding) / padding) * padding;
	else
		*len = sb.bytes_used;

	return EFI_SUCCESS;
}

static EFI_STATUS check_verity_header(struct gpt_partition_interface *gparti, UINT64 fs_len)
{
	EFI_STATUS ret;
	struct ext4_verity_header vh;

	ret = read_partition(gparti, fs_len, sizeof(vh), &vh);
	if (EFI_ERROR(ret))
		return ret;

	if (vh.magic != VERITY_METADATA_MAGIC_NUMBER) {
		debug(L"verity magic not found");
		return EFI_INVALID_PARAMETER;
	}
	if (vh.protocol_version) {
		debug(L"verity protocol version unsupported %d", vh.protocol_version);
		return EFI_INVALID_PARAMETER;
	}
	return EFI_SUCCESS;
}

EFI_STATUS get_fs_hash(const CHAR16 *label)
{
	static struct supported_fs {
		const char *name;
		EFI_STATUS (*get_len)(struct gpt_partition_interface *gparti, UINT64 *len);
	} SUPPORTED_FS[] = {
		{ "Ext4", get_ext4_len },
		{ "SquashFS", get_squashfs_len }
	};
	struct gpt_partition_interface gparti;
	CHAR8 hash[EVP_MAX_MD_SIZE];
	EFI_STATUS ret;
	UINT64 fs_len;
	UINTN i;

	ret = gpt_get_partition_by_label(label, &gparti, LOGICAL_UNIT_USER);
	if (EFI_ERROR(ret)) {
		debug(L"partition %s not found", label);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(SUPPORTED_FS); i++) {
		ret = SUPPORTED_FS[i].get_len(&gparti, &fs_len);
		if (EFI_ERROR(ret))
			continue;
		debug(L"%a filesystem found", SUPPORTED_FS[i].name);
		break;
	}
	if (EFI_ERROR(ret)) {
		error(L"%s partition does not contain a supported filesystem", label);
		return ret;
	}

	ret = check_verity_header(&gparti, fs_len);
	if (EFI_ERROR(ret))
		return ret;

	fs_len += verity_tree_size(fs_len) + VERITY_METADATA_SIZE;

	debug(L"filesystem size %lld", fs_len);

	ret = hash_partition(&gparti, fs_len, hash);
	if (EFI_ERROR(ret))
		return ret;
	return report_hash(L"/", gparti.part.name, hash);
}
