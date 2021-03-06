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
#include <vars.h>
#include <ui.h>
#include <em.h>
#include <transport.h>

#include "uefi_utils.h"
#include "gpt.h"
#include "fastboot.h"
#include "flash.h"
#include "fastboot_oem.h"
#include "fastboot_flashing.h"
#include "fastboot_ui.h"
#include "smbios.h"
#include "info.h"
#include "authenticated_action.h"
#include "fastboot_transport.h"

/* size of "INFO" "OKAY" or "FAIL" */
#define CODE_LENGTH 4
#define INFO_PAYLOAD (MAGIC_LENGTH - CODE_LENGTH)
#define MAX_VARIABLE_LENGTH 64

struct fastboot_var {
	struct fastboot_var *next;
	char name[MAX_VARIABLE_LENGTH];
	char value[MAX_VARIABLE_LENGTH];
	char *(*get_value)(void);
};

struct fastboot_tx_buffer {
	struct fastboot_tx_buffer *next;
	char msg[MAGIC_LENGTH];
};

struct cmdlist {
	struct cmdlist *next;
	struct fastboot_cmd *cmd;
};

enum fastboot_states {
	STATE_OFFLINE,
	STATE_COMMAND,
	STATE_COMPLETE,
	STATE_START_DOWNLOAD,
	STATE_DOWNLOAD,
	STATE_TX,
	STATE_STOPPING,
	STATE_STOPPED,
	STATE_ERROR,
};

EFI_GUID guid_linux_data = {0x0fc63daf, 0x8483, 0x4772, {0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4}};

static cmdlist_t cmdlist;
static char *command_buffer;
static UINTN command_buffer_size;
static struct fastboot_var *varlist;
static struct fastboot_tx_buffer *txbuf_head;
static enum fastboot_states fastboot_state;
static enum fastboot_states next_state;

/* Download buffer and size, for download and flash commands */
static void *dlbuffer;
static unsigned dlsize, bufsize;

static const char *flash_locked_whitelist[] = {
#ifdef BOOTLOADER_POLICY
	ACTION_AUTHORIZATION,
#endif
	NULL
};

void fastboot_set_dlbuffer(void *buffer, unsigned size)
{
	dlbuffer = buffer;
	dlsize = size;
}

EFI_STATUS fastboot_set_command_buffer(char *buffer, UINTN size)
{
	if (!buffer)
		return EFI_INVALID_PARAMETER;

	command_buffer = buffer;
	command_buffer_size = size;

	return EFI_SUCCESS;
}

EFI_STATUS fastboot_register_into(cmdlist_t *list, struct fastboot_cmd *cmd)
{
	cmdlist_t node;

	if (!list || !cmd)
		return EFI_INVALID_PARAMETER;

	node = AllocatePool(sizeof(*node));
	if (!node) {
		error(L"Failed to allocate fastboot command %a", cmd->name);
		return EFI_OUT_OF_RESOURCES;
	}
	node->cmd = cmd;
	node->next = *list;
	*list = node;

	return EFI_SUCCESS;
}

EFI_STATUS fastboot_register(struct fastboot_cmd *cmd)
{
	return fastboot_register_into(&cmdlist, cmd);
}

void fastboot_cmdlist_unregister(cmdlist_t *list)
{
	cmdlist_t next, node;

	if (!list)
		return;

	for (node = *list; node; node = next) {
		next = node->next;
		FreePool(node);
	}
	*list = NULL;
}

struct fastboot_var *fastboot_getvar(const char *name)
{
	struct fastboot_var *var;

	for (var = varlist; var; var = var->next)
		if (!strcmp((CHAR8 *)name, (const CHAR8 *)var->name))
			return var;

	return NULL;
}

static struct fastboot_var *fastboot_getvar_or_create(const char *name)
{
	struct fastboot_var *var;
	UINTN size;

	size = strlena((CHAR8 *) name) + 1;
	if (size > sizeof(var->name)) {
		error(L"Name too long for variable '%a'", name);
		return NULL;
	}

	var = fastboot_getvar(name);
	if (!var) {
		var = AllocateZeroPool(sizeof(*var));
		if (!var) {
			error(L"Failed to allocate variable '%a'", name);
			return NULL;
		}
		var->next = varlist;
		varlist = var;
		CopyMem(var->name, name, size);
	}

	return var;
}

/*
 * remove all fastboot variable which starts with partition-
 */
#define MATCH_PART "partition-"
static void clean_partition_var(void)
{
	struct fastboot_var *var;
	struct fastboot_var *old_varlist;
	struct fastboot_var *next;

	old_varlist = varlist;
	varlist = NULL;

	for (var = old_varlist; var; var = next) {
		next = var->next;
		if (!memcmp(MATCH_PART, var->name, strlena((CHAR8 *) MATCH_PART))) {
			FreePool(var);
		} else {
			var->next = varlist;
			varlist = var;
		}
	}
}

static void fastboot_unpublish_all()
{
	struct fastboot_var *next, *var;

	for (var = varlist; var; var = next) {
		next = var->next;
		FreePool(var);
	}

	varlist = NULL;
}

EFI_STATUS fastboot_publish_dynamic(const char *name, char *(get_value)(void))
{
	struct fastboot_var *var;

	if (!name || !get_value)
		return EFI_INVALID_PARAMETER;

	var = fastboot_getvar_or_create(name);
	if (!var)
		return EFI_INVALID_PARAMETER;

	var->get_value = get_value;

	return EFI_SUCCESS;
}

EFI_STATUS fastboot_publish(const char *name, const char *value)
{
	struct fastboot_var *var;
	UINTN valuelen;

	if (!name || !value)
		return EFI_INVALID_PARAMETER;

	valuelen = strlena((CHAR8 *) value) + 1;
	if (valuelen > sizeof(var->value)) {
		error(L"name or value too long for variable '%a'", name);
		return EFI_BUFFER_TOO_SMALL;
	}
	var = fastboot_getvar_or_create(name);
	if (!var)
		return EFI_INVALID_PARAMETER;

	CopyMem(var->value, value, valuelen);

	return EFI_SUCCESS;
}

static char *get_ptype_str(EFI_GUID *guid)
{
	if (!CompareGuid(guid, &guid_linux_data))
		return "ext4";

	if (!CompareGuid(guid, &EfiPartTypeSystemPartitionGuid))
		return "vfat";

	return "none";
}

static char *get_psize_str(UINT64 size)
{
	static char part_size[MAX_VARIABLE_LENGTH];
	int len;

	len = snprintf((CHAR8 *)part_size, sizeof(part_size),
		       (CHAR8 *)"0x%lX", size);
	if (len < 0 || len >= (int)sizeof(part_size))
		return NULL;

	return part_size;
}

static EFI_STATUS publish_part(CHAR16 *part_name, UINT64 size, EFI_GUID *guid)
{
	struct descriptor {
		char *name;
		char *value;
	} descriptors[] = {
		{ "partition-size",	get_psize_str(size) },
		{ "partition-type",	get_ptype_str(guid) },
		{ "has-slot",		"no" }
	};
	char var[MAX_VARIABLE_LENGTH];
	int len;
	UINTN i;
	struct descriptor *desc;

	for (i = 0; i < ARRAY_SIZE(descriptors); i++) {
		desc = &descriptors[i];
		if (!desc->value)
			return EFI_INVALID_PARAMETER;

		len = snprintf((CHAR8 *)var, sizeof(var), (CHAR8 *)"%a:%s",
			       desc->name, part_name);
		if (len < 0 || len >= (int)sizeof(var))
			return EFI_INVALID_PARAMETER;

		fastboot_publish(var, desc->value);
	}

	return EFI_SUCCESS;
}

static EFI_STATUS publish_partsize(void)
{
	EFI_STATUS ret;
	struct gpt_partition_interface *gparti;
	UINTN part_count;
	UINTN i;

	ret = gpt_list_partition(&gparti, &part_count, LOGICAL_UNIT_USER);
	if (EFI_ERROR(ret) || part_count == 0)
		return EFI_SUCCESS;

	for (i = 0; i < part_count; i++) {
		UINT64 size;

		size = gparti[i].bio->Media->BlockSize
			* (gparti[i].part.ending_lba + 1 - gparti[i].part.starting_lba);

		ret = publish_part(gparti[i].part.name, size, &gparti[i].part.type);
		if (EFI_ERROR(ret))
			return ret;

		/* stay compatible with userdata/data naming */
		if (!StrCmp(gparti[i].part.name, L"data")) {
			ret = publish_part(L"userdata", size, &gparti[i].part.type);
			if (EFI_ERROR(ret))
				return ret;
		} else if (!StrCmp(gparti[i].part.name, L"userdata")) {
			ret = publish_part(L"data", size, &gparti[i].part.type);
			if (EFI_ERROR(ret))
				return ret;
		}
	}

	FreePool(gparti);

	return EFI_SUCCESS;
}

static char *get_battery_voltage_var()
{
	EFI_STATUS ret;
	int len;
	static char battery_voltage[30]; /* Enough space for %dmV format */
	UINTN voltage;

	ret = get_battery_voltage(&voltage);
	if (EFI_ERROR(ret))
		return NULL;

	len = snprintf((CHAR8 *)battery_voltage, sizeof(battery_voltage),
		       (CHAR8 *)"%dmV", voltage);
	if (len < 0) {
		error(L"Failed to format voltage string");
		return NULL;
	}

	return battery_voltage;
}

static EFI_STATUS fastboot_build_ack_msg(char *msg, const char *code, const char *fmt, va_list ap)
{
	char *response;
	int len;

	CopyMem(msg, code, CODE_LENGTH);
	response = &msg[CODE_LENGTH];

	len = vsnprintf((CHAR8 *)response, INFO_PAYLOAD, (CHAR8 *)fmt, ap);
	if (len < 0) {
		error(L"Failed to build reason string");
		return EFI_INVALID_PARAMETER;
	}

	return EFI_SUCCESS;

}

void fastboot_ack(const char *code, const char *fmt, va_list ap)
{
	static CHAR8 msg[MAGIC_LENGTH];
	EFI_STATUS ret;

	ret = fastboot_build_ack_msg((char *)msg, code, fmt, ap);
	if (EFI_ERROR(ret))
		return;

	debug(L"SENT %a", msg);
	fastboot_state = next_state;
	ret = transport_write(msg, MAGIC_LENGTH);
	if (EFI_ERROR(ret))
		fastboot_state = STATE_ERROR;
}

void fastboot_ack_buffered(const char *code, const char *fmt, va_list ap)
{
	struct fastboot_tx_buffer *new_txbuf;
	struct fastboot_tx_buffer *txbuf;
	EFI_STATUS ret;

	new_txbuf = AllocateZeroPool(sizeof(*new_txbuf));
	if (!new_txbuf) {
		error(L"Failed to allocate memory");
		return;
	}

	ret = fastboot_build_ack_msg(new_txbuf->msg, code, fmt, ap);
	if (EFI_ERROR(ret)) {
		FreePool(new_txbuf);
		return;
	}
	if (!txbuf_head)
		txbuf_head = new_txbuf;
	else {
		txbuf = txbuf_head;
		while (txbuf->next)
			txbuf = txbuf->next;
		txbuf->next = new_txbuf;
	}
	fastboot_state = STATE_TX;
}

EFI_STATUS fastboot_info_long_string(char *str, VOID *context _unused)
{
	char linebuf[INFO_PAYLOAD];
	const UINTN max_len = sizeof(linebuf) - 1;

	linebuf[max_len] = '\0';

	while (strlen((CHAR8 *)str) > max_len) {
		memcpy(linebuf, str, max_len);
		fastboot_info(linebuf);
		str += max_len;
	}
	fastboot_info(str);

	return EFI_SUCCESS;
}

void fastboot_info(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fastboot_ack_buffered("INFO", fmt, ap);
	va_end(ap);
}

void fastboot_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (fastboot_state == STATE_TX)
		fastboot_ack_buffered("FAIL", fmt, ap);
	else
		fastboot_ack("FAIL", fmt, ap);
	va_end(ap);
}

void fastboot_okay(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (fastboot_state == STATE_TX)
		fastboot_ack_buffered("OKAY", fmt, ap);
	else
		fastboot_ack("OKAY", fmt, ap);
	va_end(ap);
}

static void flush_tx_buffer(void)
{
	EFI_STATUS ret;
	struct fastboot_tx_buffer *msg;
	static CHAR8 buf[sizeof(msg->msg)];

	msg = txbuf_head;
	txbuf_head = txbuf_head->next;
	if (!txbuf_head)
		fastboot_state = next_state;

	memcpy(buf, msg->msg, sizeof(buf));
	FreePool(msg);
	ret = transport_write(buf, sizeof(buf));
	if (EFI_ERROR(ret))
		fastboot_state = STATE_ERROR;
}

static BOOLEAN is_in_white_list(const CHAR8 *key, const char **white_list)
{
	for (; *white_list; white_list++)
		if (!strcmp(key, (CHAR8 *)*white_list))
			return TRUE;

	return FALSE;
}

EFI_STATUS refresh_partition_var(void)
{
	clean_partition_var();
	return publish_partsize();
}

static void cmd_flash(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;
	CHAR16 *label;

	if (argc != 2) {
		fastboot_fail("Invalid parameter");
		return;
	}

	if (get_current_state() == LOCKED &&
	    !is_in_white_list(argv[1], flash_locked_whitelist)) {
		error(L"Flash %a is prohibited in %a state.", argv[1],
		      get_current_state_string());
		fastboot_fail("Prohibited command in %a state.", get_current_state_string());
		return;
	}

	label = stra_to_str((CHAR8*)argv[1]);
	if (!label) {
		error(L"Failed to get label %a", argv[1]);
		fastboot_fail("Allocation error");
		return;
	}
	ui_print(L"Flashing %s ...", label);

	ret = flash(dlbuffer, dlsize, label);
	FreePool(label);
	if (EFI_ERROR(ret)) {
		fastboot_fail("Flash failure: %r", ret);
		return;
	}

	gpt_sync();

	/* update partition variable in case it has changed */
	if (ret & REFRESH_PARTITION_VAR) {
		ret = refresh_partition_var();
		if (EFI_ERROR(ret)) {
			fastboot_fail("Failed to publish partition variables, %r", ret);
			return;
		}
	}

	ui_print(L"Flash done.");
	fastboot_okay("");
}

static void cmd_erase(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;
	CHAR16 *label;

	if (argc != 2) {
		fastboot_fail("Invalid parameter");
		return;
	}

	label = stra_to_str((CHAR8*)argv[1]);
	if (!label) {
		error(L"Failed to get label %a", argv[1]);
		fastboot_fail("Allocation error");
		return;
	}
	ui_print(L"Erasing %s ...", label);
	ret = erase_by_label(label);
	FreePool(label);
	if (EFI_ERROR(ret)) {
		fastboot_fail("Erase failure: %r", ret);
		return;
	}

	ui_print(L"Erase done.");
	fastboot_okay("");
}

static void cmd_boot(__attribute__((__unused__)) INTN argc,
		     __attribute__((__unused__)) CHAR8 **argv)
{
	EFI_STATUS ret;

	ret = fastboot_stop(dlbuffer, NULL, dlsize, UNKNOWN_TARGET);
	if (EFI_ERROR(ret)) {
		fastboot_fail("Failed to stop transport");
		return;
	}
	ui_print(L"Booting received image ...");
	fastboot_okay("");
}

static char *fastboot_var_value(struct fastboot_var *var)
{
	char *value;

	if (!var->get_value)
		return var->value;

	value = var->get_value();
	if (!value)
		return "";

	if (strlena((CHAR8 *)value) + 1 > sizeof(var->value)) {
		error(L"value too long for '%a' variable");
		return "";
	}

	return value;
}

static void cmd_getvar(INTN argc, CHAR8 **argv)
{
	struct fastboot_var *var;
	if (argc != 2) {
		fastboot_fail("Invalid parameter");
		return;
	}

	if (!strcmp(argv[1], (CHAR8 *)"all")) {
		for (var = varlist; var; var = var->next)
			fastboot_info("%a: %a", var->name, fastboot_var_value(var));
		fastboot_okay("");
		return;
	}

	var = fastboot_getvar((char *)argv[1]);
	fastboot_okay("%a", var ? fastboot_var_value(var) : "");
}

void fastboot_reboot(enum boot_target target, CHAR16 *msg)
{
	EFI_STATUS ret = fastboot_stop(NULL, NULL, 0, target);
	if (EFI_ERROR(ret)) {
		fastboot_fail("Failed to stop transport");
		return;
	}
	ui_print(msg);
	fastboot_okay("");
}

static void cmd_continue(__attribute__((__unused__)) INTN argc,
			 __attribute__((__unused__)) CHAR8 **argv)
{
	fastboot_reboot(NORMAL_BOOT, L"Continuing ...");
}

static void cmd_reboot(__attribute__((__unused__)) INTN argc,
		       __attribute__((__unused__)) CHAR8 **argv)
{
	fastboot_reboot(NORMAL_BOOT, L"Rebooting ...");
}

static void cmd_reboot_bootloader(__attribute__((__unused__)) INTN argc,
				  __attribute__((__unused__)) CHAR8 **argv)
{
	fastboot_reboot(FASTBOOT, L"Rebooting to bootloader ...");
}

static struct fastboot_cmd *get_cmd(cmdlist_t list, const char *name)
{
	cmdlist_t node;

	if (!name || !list)
		return NULL;

	for (node = list; node; node = node->next)
		if (!strcmp((CHAR8 *)name, (CHAR8 *)node->cmd->name))
			return node->cmd;

	return NULL;
}

struct fastboot_cmd *fastboot_get_root_cmd(const char *name)
{
	return get_cmd(cmdlist, name);
}

void fastboot_run_cmd(cmdlist_t list, const char *name, INTN argc, CHAR8 **argv)
{
	struct fastboot_cmd *cmd;

	cmd = get_cmd(list, name);
	if (!cmd) {
		error(L"unknown command '%a'", name);
		fastboot_fail("unknown command");
		return;
	}

	if (cmd->min_state > get_current_state()) {
		fastboot_fail("command not allowed in %a state",
			      get_current_state_string());
		return;
	}
	cmd->handle(argc, argv);
}

void fastboot_run_root_cmd(const char *name, INTN argc, CHAR8 **argv)
{
	fastboot_run_cmd(cmdlist, name, argc, argv);
}

static void fastboot_read_command(void)
{
	transport_read(command_buffer, command_buffer_size);
}

static void cmd_download(INTN argc, CHAR8 **argv)
{
	static CHAR8 response[MAGIC_LENGTH];
	EFI_STATUS ret;
	int len;
	UINTN newdlsize;

	if (argc != 2) {
		fastboot_fail("Invalid parameter");
		return;
	}

	newdlsize = strtoul((const char *)argv[1], NULL, 16);

	ui_print(L"Receiving %d bytes ...", newdlsize);
	if (newdlsize == 0) {
		fastboot_fail("no data to download");
		return;
	} else if (newdlsize > MAX_DOWNLOAD_SIZE) {
		fastboot_fail("data too large");
		return;
	}

	if (newdlsize > bufsize) {
		if (dlbuffer)
			FreePool(dlbuffer);
		dlbuffer = AllocatePool(newdlsize);
		if (!dlbuffer) {
			error(L"Failed to allocate download buffer (0x%x bytes)",
			      newdlsize);
			fastboot_fail("Memory allocation failure");
			dlsize = bufsize = 0;
			return;
		}
		bufsize = newdlsize;
	}
	dlsize = newdlsize;

	len = snprintf(response, sizeof(response), (CHAR8 *)"DATA%08x", dlsize);
	if (len < 0) {
		error(L"Failed to format DATA response");
		fastboot_fail("Failed to format DATA response");
		return;
	}

	fastboot_state = STATE_START_DOWNLOAD;
	ret = transport_write(response, strlen((CHAR8 *)response));
	if (EFI_ERROR(ret)) {
		fastboot_state = STATE_ERROR;
		return;
	}
}

static void worker_download(void)
{
	EFI_STATUS ret;

	ret = transport_read(dlbuffer, dlsize);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to receive %d bytes", dlsize);
		fastboot_fail("Transport receive failed");
		return;
	}
	fastboot_state = STATE_DOWNLOAD;
}

static void fastboot_process_tx(__attribute__((__unused__)) void *buf,
				__attribute__((__unused__)) unsigned len)
{
	switch (fastboot_state) {
	case STATE_STOPPING:
		fastboot_state = STATE_STOPPED;
		break;
	case STATE_TX:
		flush_tx_buffer();
		break;
	case STATE_COMPLETE:
		fastboot_read_command();
		break;
	case STATE_START_DOWNLOAD:
		worker_download();
		break;
	default:
		error(L"Unexpected tx event while in state %d", fastboot_state);
		break;
	}
}

static EFI_STATUS get_command_buffer_argv(INTN *argc, CHAR8 *argv[], UINTN max_argc)
{
	char *saveptr, *token = NULL;

	argv[0] = (CHAR8 *)strtok_r((char *)command_buffer, ": ", &saveptr);
	if (!argv[0])
		return EFI_INVALID_PARAMETER;

	for (*argc = 1; (UINTN)*argc < max_argc; (*argc)++) {
		token = strtok_r(NULL, " ", &saveptr);
		if (!token)
			break;
		argv[*argc] = (CHAR8 *)token;
	}

	if (token && strtok_r(NULL, " ", &saveptr))
		return EFI_INVALID_PARAMETER;

	return EFI_SUCCESS;
}

static unsigned received_len;
static unsigned last_received_len;
#define DATA_PROGRESS_THRESHOLD (5 * 1024 * 1024)
static void fastboot_run_command()
{
#define MAX_ARGS 16
	EFI_STATUS ret;
	CHAR8 *argv[MAX_ARGS];
	INTN argc;

	if (fastboot_state != STATE_COMMAND)
		return;

	ret = get_command_buffer_argv(&argc, argv, MAX_ARGS);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to split fastboot command line");
		return;
	}

	fastboot_run_root_cmd((char *)argv[0], argc, argv);
	received_len = 0;
	last_received_len = 0;

	if (fastboot_state == STATE_TX)
		flush_tx_buffer();
}

static void fastboot_process_rx(void *buf, unsigned len)
{
	CHAR8 *s;

	switch (fastboot_state) {
	case STATE_DOWNLOAD:
		received_len += len;
		if (received_len / DATA_PROGRESS_THRESHOLD >
		    last_received_len / DATA_PROGRESS_THRESHOLD) {
			if (dlsize > MiB)
				debug(L"\rRX %d MiB / %d MiB", received_len/MiB, dlsize / MiB);
			else
				debug(L"\rRX %d KiB / %d KiB", received_len/1024, dlsize / 1024);
		}
		last_received_len = received_len;
		if (received_len < dlsize) {
			s = buf;
			transport_read(&s[len], dlsize - received_len);
		} else {
			fastboot_state = STATE_COMMAND;
			fastboot_okay("");
		}
		break;
	case STATE_COMPLETE:
		if (buf != command_buffer || len >= command_buffer_size) {
			fastboot_fail("Inappropriate command buffer or length");
			return;
		}

		((CHAR8 *)buf)[len] = '\0';
		debug(L"GOT %a", (CHAR8 *)buf);

		fastboot_state = STATE_COMMAND;
		break;
	default:
		error(L"Inconsistent fastboot state: 0x%x", fastboot_state);
	}
}

static void fastboot_start_callback(void)
{
	fastboot_state = next_state;
	fastboot_read_command();
}

static struct fastboot_cmd COMMANDS[] = {
	{ "download",		LOCKED,		cmd_download },
	{ "flash",		LOCKED,		cmd_flash },
	{ "erase",		UNLOCKED,	cmd_erase },
	{ "getvar",		LOCKED,		cmd_getvar },
	{ "boot",		UNLOCKED,	cmd_boot },
	{ "continue",		LOCKED,		cmd_continue },
	{ "reboot",		LOCKED,		cmd_reboot },
	{ "reboot-bootloader",	LOCKED,		cmd_reboot_bootloader }
};

static EFI_STATUS fastboot_init()
{
	EFI_STATUS ret;
	UINTN i;
	char download_max_str[30];
	static char default_command_buffer[MAGIC_LENGTH];

	ret = fastboot_set_command_buffer(default_command_buffer,
					  sizeof(default_command_buffer));
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to set fastboot command buffer");
		goto error;
	}

	ret = uefi_call_wrapper(BS->SetWatchdogTimer, 4, 0, 0, 0, NULL);
	if (EFI_ERROR(ret) && ret != EFI_UNSUPPORTED) {
		efi_perror(ret, L"Couldn't disable watchdog timer");
		/* Might as well continue even though this failed ... */
	}

	ret = fastboot_publish("product", info_product());
	if (EFI_ERROR(ret))
		goto error;

#ifdef HAL_AUTODETECT
	ret = fastboot_publish("variant", info_variant());
	if (EFI_ERROR(ret))
		goto error;
#endif

	ret = fastboot_publish("version-bootloader", info_bootloader_version());
	if (EFI_ERROR(ret))
		goto error;

	ret = fastboot_publish_dynamic("battery-voltage", get_battery_voltage_var);
	if (EFI_ERROR(ret))
		goto error;

	if (snprintf((CHAR8 *)download_max_str, sizeof(download_max_str),
		     (CHAR8 *)"0x%lX", MAX_DOWNLOAD_SIZE) < 0) {
		error(L"Failed to set download_max_str string");
		ret = EFI_INVALID_PARAMETER;
		goto error;
	} else {
		ret = fastboot_publish("max-download-size", download_max_str);
		if (EFI_ERROR(ret))
			goto error;
	}

	ret = publish_partsize();
	if (EFI_ERROR(ret))
		goto error;

	/* Register commands */
	for (i = 0; i < ARRAY_SIZE(COMMANDS); i++) {
		ret = fastboot_register(&COMMANDS[i]);
		if (EFI_ERROR(ret))
			goto error;
	}
	ret = fastboot_oem_init();
	if (EFI_ERROR(ret))
		goto error;

	ret = fastboot_flashing_init();
	if (EFI_ERROR(ret))
		goto error;

	ret = fastboot_ui_init();
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Fastboot UI initialization failed, continue anyway.");

	fastboot_state = STATE_OFFLINE;
	next_state = STATE_COMPLETE;

	return EFI_SUCCESS;

error:
	fastboot_free();
	error(L"Fastboot library initialization failed");
	return ret;
}

static void *fastboot_bootimage;
static void *fastboot_efiimage;
static UINTN fastboot_imagesize;
static enum boot_target fastboot_target;

EFI_STATUS fastboot_start(void **bootimage, void **efiimage, UINTN *imagesize,
			  enum boot_target *target)
{
	EFI_STATUS ret;

	if (!bootimage || !efiimage || !imagesize || !target)
		return EFI_INVALID_PARAMETER;

	fastboot_bootimage = NULL;
	fastboot_efiimage = NULL;
	fastboot_target = UNKNOWN_TARGET;
	*target = UNKNOWN_TARGET;

	fastboot_init();

	/* In case user still holding it from answering a UX prompt
	 * or magic key */
	ui_wait_for_key_release();

	ret = fastboot_transport_register();
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"fastboot failed to register supported transport");
		goto exit;
	}

	ret = transport_start(fastboot_start_callback,
			      fastboot_process_rx,
			      fastboot_process_tx);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to initialize transport layer");
		goto exit;
	}

	for (;;) {
		*target = fastboot_ui_event_handler();
		if (*target != UNKNOWN_TARGET)
			break;

		/* Keeping this for:
		 * - retro-compatibility with previous USB device mode
		 *   protocol implementation;
		 * - the installer needs to be scheduled; */
		ret = transport_run();
		if (EFI_ERROR(ret) && ret != EFI_TIMEOUT) {
			efi_perror(ret, L"Error occurred during transport run");
			goto exit;
		}

		fastboot_run_command();

		if (fastboot_state == STATE_STOPPED)
			break;
	}

	ret = transport_stop();
	if (EFI_ERROR(ret))
		goto exit;

	if (fastboot_target != UNKNOWN_TARGET)
		*target = fastboot_target;

	*bootimage = fastboot_bootimage;
	*efiimage = fastboot_efiimage;
	*imagesize = fastboot_imagesize;

exit:
	fastboot_free();
	return ret;
}

EFI_STATUS fastboot_stop(void *bootimage, void *efiimage, UINTN imagesize,
			 enum boot_target target)
{
	VOID *imgbuffer = NULL;

	fastboot_imagesize = imagesize;
	fastboot_target = target;

	if (imagesize && (bootimage || efiimage)) {
		imgbuffer = AllocatePool(imagesize);
		if (!imgbuffer) {
			error(L"Failed to allocate image buffer");
			return EFI_OUT_OF_RESOURCES;
		}
		memcpy(imgbuffer, bootimage ? bootimage : efiimage, imagesize);
	}

	fastboot_bootimage = bootimage ? imgbuffer : NULL;
	fastboot_efiimage = efiimage ? imgbuffer : NULL;

	if (fastboot_state == STATE_COMPLETE)
		fastboot_state = STATE_STOPPED;
	else
		next_state = STATE_STOPPING;

	return EFI_SUCCESS;
}

void fastboot_free()
{
	if (dlbuffer) {
		FreePool(dlbuffer);
		dlbuffer = NULL;
		bufsize = dlsize = 0;
	}

	fastboot_unpublish_all();
	fastboot_cmdlist_unregister(&cmdlist);
	fastboot_oem_free();
	fastboot_flashing_free();
	fastboot_ui_destroy();
	gpt_free_cache();
}
