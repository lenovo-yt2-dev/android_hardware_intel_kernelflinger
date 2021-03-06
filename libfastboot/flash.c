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
#include <fastboot.h>
#include <android.h>

#include "uefi_utils.h"
#include "gpt.h"
#include "gpt_bin.h"
#include "flash.h"
#include "storage.h"
#include "sparse.h"
#include "oemvars.h"
#include "vars.h"
#include "bootloader.h"
#include "authenticated_action.h"

static struct gpt_partition_interface gparti;
static UINT64 cur_offset;

#define part_start (gparti.part.starting_lba * gparti.bio->Media->BlockSize)
#define part_end ((gparti.part.ending_lba + 1) * gparti.bio->Media->BlockSize)

#define is_inside_partition(off, sz) \
		(off >= part_start && off + sz <= part_end)

EFI_STATUS flash_skip(UINT64 size)
{
	if (!is_inside_partition(cur_offset, size)) {
		error(L"Attempt to skip outside of partition [%ld %ld] [%ld %ld]",
				part_start, part_end, cur_offset, cur_offset + size);
		return EFI_INVALID_PARAMETER;
	}
	cur_offset += size;
	return EFI_SUCCESS;
}

EFI_STATUS flash_write(VOID *data, UINTN size)
{
	EFI_STATUS ret;

	if (!gparti.bio)
		return EFI_INVALID_PARAMETER;

	if (!is_inside_partition(cur_offset, size)) {
		error(L"Attempt to write outside of partition [%ld %ld] [%ld %ld]",
				part_start, part_end, cur_offset, cur_offset + size);
		return EFI_INVALID_PARAMETER;
	}
	ret = uefi_call_wrapper(gparti.dio->WriteDisk, 5, gparti.dio, gparti.bio->Media->MediaId, cur_offset, size, data);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Failed to write bytes");

	cur_offset += size;
	return ret;
}

EFI_STATUS flash_fill(UINT32 pattern, UINTN size)
{
	UINT32 *buf;
	UINTN i;
	EFI_STATUS ret;

	buf = AllocatePool(size);
	if (!buf)
		return EFI_OUT_OF_RESOURCES;

	for (i = 0; i < size / sizeof(*buf); i++)
		buf[i] = pattern;

	ret = flash_write(buf, size);
	FreePool(buf);
	return ret;
}

static EFI_STATUS flash_into_esp(VOID *data, UINTN size, CHAR16 *label)
{
	EFI_STATUS ret;
	EFI_FILE_IO_INTERFACE *io;

	ret = get_esp_fs(&io);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get partition ESP");
		return ret;
	}
	return uefi_write_file_with_dir(io, label, data, size);
}

static EFI_STATUS _flash_gpt(VOID *data, UINTN size, logical_unit_t log_unit)
{
	struct gpt_bin_header *gb_hdr;
	struct gpt_bin_part *gb_part;
	EFI_STATUS ret;

	gb_hdr = data;
	gb_part = (struct gpt_bin_part *)&gb_hdr[1];

	if (size < sizeof(*gb_hdr) ||
	    gb_hdr->magic != GPT_BIN_MAGIC ||
	    size != sizeof(*gb_hdr) + (gb_hdr->npart * sizeof(*gb_part))) {
		error(L"Invalid gpt binary");
		return EFI_INVALID_PARAMETER;
	}

	ret = gpt_create(gb_hdr->start_lba, gb_hdr->npart, gb_part, log_unit);
	if (EFI_ERROR(ret))
		return ret;

	return EFI_SUCCESS;
}

static EFI_STATUS flash_gpt(VOID *data, UINTN size)
{
	EFI_STATUS ret;

	ret = _flash_gpt(data, size, LOGICAL_UNIT_USER);
	return EFI_ERROR(ret) ? ret : EFI_SUCCESS | REFRESH_PARTITION_VAR;
}

static EFI_STATUS flash_gpt_gpp1(VOID *data, UINTN size)
{
	return _flash_gpt(data, size, LOGICAL_UNIT_FACTORY);
}

#ifndef USER
static EFI_STATUS flash_efirun(VOID *data, UINTN size)
{
	return fastboot_stop(NULL, data, size, UNKNOWN_TARGET);
}

static EFI_STATUS flash_mbr(VOID *data, UINTN size)
{
	struct gpt_partition_interface gparti;
	EFI_STATUS ret;

	if (size > MBR_CODE_SIZE)
		return EFI_INVALID_PARAMETER;

	ret = gpt_get_root_disk(&gparti, LOGICAL_UNIT_USER);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get disk information");
		return ret;
	}

	ret = uefi_call_wrapper(gparti.dio->WriteDisk, 5, gparti.dio,
				gparti.bio->Media->MediaId, 0, size, data);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Failed to flash MBR");

	return ret;
}
#endif

static EFI_STATUS flash_sfu(VOID *data, UINTN size)
{
	return flash_into_esp(data, size, L"BIOSUPDATE.fv");
}

static EFI_STATUS flash_ifwi(VOID *data, UINTN size)
{
	return flash_into_esp(data, size, L"ifwi.bin");
}

static EFI_STATUS flash_zimage(VOID *data, UINTN size)
{
	struct boot_img_hdr *bootimage, *new_bootimage;
	VOID *new_cur, *cur;
	UINTN new_size, partlen;
	EFI_STATUS ret;

	ret = gpt_get_partition_by_label(L"boot", &gparti, LOGICAL_UNIT_USER);
	if (EFI_ERROR(ret)) {
		error(L"Unable to get information on the boot partition");
		return ret;
	}

	partlen = (gparti.part.ending_lba + 1 - gparti.part.starting_lba)
		* gparti.bio->Media->BlockSize;
	bootimage = AllocatePool(partlen);
	if (!bootimage) {
		error(L"Unable to allocate bootimage buffer");
		return EFI_OUT_OF_RESOURCES;
	}

	ret = uefi_call_wrapper(gparti.dio->ReadDisk, 5, gparti.dio,
				gparti.bio->Media->MediaId,
				gparti.part.starting_lba * gparti.bio->Media->BlockSize,
				partlen, bootimage);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to load the current bootimage");
		goto out;
	}

	if (strncmpa((CHAR8 *)BOOT_MAGIC, bootimage->magic, BOOT_MAGIC_SIZE)) {
		error(L"boot partition does not contain a valid bootimage");
		ret = EFI_UNSUPPORTED;
		goto out;
	}

	new_size = bootimage_size(bootimage) - bootimage->kernel_size
		+ pagealign(bootimage, size);
	if (new_size > partlen) {
		error(L"Kernel image is too large to fit in the boot partition");
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	new_bootimage = AllocateZeroPool(new_size);
	if (!new_bootimage) {
		ret = EFI_OUT_OF_RESOURCES;
		goto out;
	}

	/* Create the new bootimage. */
	memcpy((VOID *)new_bootimage, bootimage, bootimage->page_size);

	new_bootimage->kernel_size = size;
	new_bootimage->kernel_addr = bootimage->kernel_addr;
	new_cur = (VOID *)new_bootimage + bootimage->page_size;
	memcpy(new_cur, data, size);

	new_cur += pagealign(new_bootimage, size);
	cur = (VOID *)bootimage + bootimage->page_size
		+ pagealign(bootimage, bootimage->kernel_size);
	memcpy(new_cur, cur, bootimage->ramdisk_size);

	new_cur += pagealign(new_bootimage, new_bootimage->ramdisk_size);
	cur += pagealign(bootimage, bootimage->ramdisk_size);
	memcpy(new_cur, cur, bootimage->second_size);

	/* Flash new the bootimage. */
	cur_offset = gparti.part.starting_lba * gparti.bio->Media->BlockSize;
	ret = flash_write(new_bootimage, new_size);

	FreePool(new_bootimage);

 out:
	FreePool(bootimage);
	return ret;
}

EFI_STATUS flash_partition(VOID *data, UINTN size, CHAR16 *label)
{
	EFI_STATUS ret;

	ret = gpt_get_partition_by_label(label, &gparti, LOGICAL_UNIT_USER);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get partition %s", label);
		return ret;
	}

	cur_offset = gparti.part.starting_lba * gparti.bio->Media->BlockSize;

	if (is_sparse_image(data, size))
		ret = flash_sparse(data, size);
	else
		ret = flash_write(data, size);

	if (EFI_ERROR(ret))
		return ret;

	if (!CompareGuid(&gparti.part.type, &EfiPartTypeSystemPartitionGuid))
		return gpt_refresh();

	return EFI_SUCCESS;
}

static struct label_exception {
	CHAR16 *name;
	EFI_STATUS (*flash_func)(VOID *data, UINTN size);
} LABEL_EXCEPTIONS[] = {
	{ L"gpt", flash_gpt },
	{ L"gpt-gpp1", flash_gpt_gpp1 },
#ifndef USER
	{ L"efirun", flash_efirun },
	{ L"mbr", flash_mbr },
#endif
	{ L"sfu", flash_sfu },
	{ L"ifwi", flash_ifwi },
	{ L"oemvars", flash_oemvars },
	{ L"zimage", flash_zimage },
	{ BOOTLOADER_PART, flash_bootloader },
#ifdef BOOTLOADER_POLICY
	{ CONVERT_TO_WIDE(ACTION_AUTHORIZATION), authenticated_action }
#endif
};

EFI_STATUS flash(VOID *data, UINTN size, CHAR16 *label)
{
	UINTN i;

#ifndef USER
	/* special case for writing inside esp partition */
	CHAR16 esp[] = L"/ESP/";
	if (!StrnCmp(esp, label, StrLen(esp)))
		return flash_into_esp(data, size, &label[ARRAY_SIZE(esp) - 1]);
#endif
	/* special cases */
	for (i = 0; i < ARRAY_SIZE(LABEL_EXCEPTIONS); i++)
		if (!StrCmp(LABEL_EXCEPTIONS[i].name, label))
			return LABEL_EXCEPTIONS[i].flash_func(data, size);

	return flash_partition(data, size, label);
}

EFI_STATUS flash_file(EFI_HANDLE image, CHAR16 *filename, CHAR16 *label)
{
	EFI_STATUS ret;
	EFI_FILE_IO_INTERFACE *io = NULL;
	VOID *buffer = NULL;
	UINTN size = 0;

	ret = uefi_call_wrapper(BS->HandleProtocol, 3, image, &FileSystemProtocol, (void *)&io);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get FileSystemProtocol");
		goto out;
	}

	ret = uefi_read_file(io, filename, &buffer, &size);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to read file %s", filename);
		goto out;
	}

	ret = flash(buffer, size, label);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to flash file %s on partition %s", filename, label);
		goto free_buffer;
	}

free_buffer:
	FreePool(buffer);
out:
	return ret;

}

#define FS_MGR_SIZE 4096
static EFI_STATUS erase_blocks(EFI_HANDLE handle, EFI_BLOCK_IO *bio, UINT64 start, UINT64 end)
{
	EFI_STATUS ret;
	UINTN min_end;

	ret = storage_erase_blocks(handle, bio, start, end);
	if (ret == EFI_SUCCESS) {
		/* If the Android fs_mgr fails mounting a partition,
		   it tries to detect if the partition has been wiped
		   out to determine if it has to format it.  fs_mgr
		   considers that the partition has been wiped out if
		   the first 4096 bytes are filled up with all 0 or
		   all 1.  storage_erase_blocks() uses hardware
		   support to erase the blocks which does not garantee
		   that content will be all 0 or all 1.  It also can
		   be indeterminate data. */
		min_end = start + (FS_MGR_SIZE / bio->Media->BlockSize) + 1;
		return fill_zero(bio, start, min(min_end, end));
	}

	debug(L"Fallbacking to filling with zeros");
	return fill_zero(bio, start, end);
}

EFI_STATUS erase_by_label(CHAR16 *label)
{
	EFI_STATUS ret;

	ret = gpt_get_partition_by_label(label, &gparti, LOGICAL_UNIT_USER);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get partition %s", label);
		return ret;
	}
	ret = erase_blocks(gparti.handle, gparti.bio, gparti.part.starting_lba, gparti.part.ending_lba);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to erase partition %s", label);
		return ret;
	}
	if (!CompareGuid(&gparti.part.type, &EfiPartTypeSystemPartitionGuid))
		return gpt_refresh();

	return EFI_SUCCESS;
}

EFI_STATUS garbage_disk(void)
{
	struct gpt_partition_interface gparti;
	EFI_STATUS ret;
	VOID *chunk;
	VOID *aligned_chunk;
	UINTN size;

	ret = gpt_get_root_disk(&gparti, LOGICAL_UNIT_USER);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get disk information");
		return ret;
	}

	size = gparti.bio->Media->BlockSize * N_BLOCK;
	ret = alloc_aligned(&chunk, &aligned_chunk, size, gparti.bio->Media->IoAlign);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Unable to allocate the garbage chunk");
		return ret;
	}

	ret = generate_random_numbers(aligned_chunk, size);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to generate random numbers");
		FreePool(chunk);
		return ret;
	}

	ret = fill_with(gparti.bio, gparti.part.starting_lba,
			gparti.part.ending_lba, aligned_chunk, N_BLOCK);

	FreePool(chunk);
	return gpt_refresh();
}
