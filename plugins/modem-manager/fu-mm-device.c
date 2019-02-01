/*
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <string.h>

#include "fu-io-channel.h"
#include "fu-archive.h"
#include "fu-mm-device.h"
#include "fu-mm-utils.h"
#include "fu-qmi-pdc-updater.h"

/* amount of time for the modem to be re-probed and exposed in MM after being uninhibited */
#define FU_MM_DEVICE_REMOVE_DELAY_REPROBE	45000	/* ms */

struct _FuMmDevice {
	FuDevice			 parent_instance;
	MMManager			*manager;

	/* ModemManager-based devices will have MMObject and inhibition_uid set,
	 * udev-based ones won't (as device is already inhibited) */
	MMObject			*omodem;
	gchar				*inhibition_uid;

	/* Properties read from the ModemManager-exposed modem, and to be
	 * propagated to plain udev-exposed modem objects. We assume that
	 * the firmware upgrade operation doesn't change the USB layout, and
	 * therefore the USB interface of the modem device that was an
	 * AT-capable TTY is assumed to be the same one after the upgrade.
	 */
	MMModemFirmwareUpdateMethod	 update_methods;
	gchar				*detach_fastboot_at;
	gint				 port_at_ifnum;

	/* fastboot detach handling */
	gchar				*port_at;
	FuIOChannel			*io_channel;

	/* qmi-pdc update logic */
	gchar				*port_qmi;
	FuQmiPdcUpdater			*qmi_pdc_updater;
};

G_DEFINE_TYPE (FuMmDevice, fu_mm_device, FU_TYPE_DEVICE)

static void
fu_mm_device_to_string (FuDevice *device, GString *str)
{
	FuMmDevice *self = FU_MM_DEVICE (device);
	g_string_append (str, "	 FuMmDevice:\n");
	if (self->port_at != NULL) {
		g_string_append_printf (str, "	at-port:\t\t\t%s\n",
					self->port_at);
	}
	if (self->port_qmi != NULL) {
		g_string_append_printf (str, "	qmi-port:\t\t\t%s\n",
					self->port_qmi);
	}
}

const gchar *
fu_mm_device_get_inhibition_uid (FuMmDevice *device)
{
	g_return_val_if_fail (FU_IS_MM_DEVICE (device), NULL);
	return device->inhibition_uid;
}

MMModemFirmwareUpdateMethod
fu_mm_device_get_update_methods (FuMmDevice *device)
{
	g_return_val_if_fail (FU_IS_MM_DEVICE (device), MM_MODEM_FIRMWARE_UPDATE_METHOD_NONE);
	return device->update_methods;
}

const gchar *
fu_mm_device_get_detach_fastboot_at (FuMmDevice *device)
{
	g_return_val_if_fail (FU_IS_MM_DEVICE (device), NULL);
	return device->detach_fastboot_at;
}

gint
fu_mm_device_get_port_at_ifnum (FuMmDevice *device)
{
	g_return_val_if_fail (FU_IS_MM_DEVICE (device), -1);
	return device->port_at_ifnum;
}

static gboolean
fu_mm_device_probe_default (FuDevice *device, GError **error)
{
	FuMmDevice *self = FU_MM_DEVICE (device);
	MMModemFirmware *modem_fw;
	MMModem *modem = mm_object_peek_modem (self->omodem);
	MMModemPortInfo *ports = NULL;
	const gchar **device_ids;
	const gchar *version;
	guint n_ports = 0;
	g_autoptr(MMFirmwareUpdateSettings) update_settings = NULL;
	g_autofree gchar *device_sysfs_path = NULL;

	/* inhibition uid is the modem interface 'Device' property, which may
	 * be the device sysfs path or a different user-provided id */
	self->inhibition_uid = mm_modem_dup_device (modem);

	/* find out what update methods we should use */
	modem_fw = mm_object_peek_modem_firmware (self->omodem);
	update_settings = mm_modem_firmware_get_update_settings (modem_fw);
	self->update_methods = mm_firmware_update_settings_get_method (update_settings);
	if (self->update_methods == MM_MODEM_FIRMWARE_UPDATE_METHOD_NONE) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "modem cannot be put in programming mode");
		return FALSE;
	}

	/* various fastboot commands */
	if (self->update_methods & MM_MODEM_FIRMWARE_UPDATE_METHOD_FASTBOOT) {
		const gchar *tmp;
		tmp = mm_firmware_update_settings_get_fastboot_at (update_settings);
		if (tmp == NULL) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOT_SUPPORTED,
					     "modem does not set fastboot command");
			return FALSE;
		}
		self->detach_fastboot_at = g_strdup (tmp);
	}

	/* get GUIDs */
	device_ids = mm_firmware_update_settings_get_device_ids (update_settings);
	if (device_ids == NULL || device_ids[0] == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "modem did not specify any device IDs");
		return FALSE;
	}

	/* get version string, which is fw_ver+config_ver */
	version = mm_firmware_update_settings_get_version (update_settings);
	if (version == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "modem did not specify a firmware version");
		return FALSE;
	}

	/* look for the AT and QMI/MBIM ports */
	if (!mm_modem_get_ports (modem, &ports, &n_ports)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "failed to get port information");
		return FALSE;
	}
	if (self->update_methods & MM_MODEM_FIRMWARE_UPDATE_METHOD_FASTBOOT) {
		for (guint i = 0; i < n_ports; i++) {
			if (ports[i].type == MM_MODEM_PORT_TYPE_AT) {
				self->port_at = g_strdup_printf ("/dev/%s", ports[i].name);
				break;
			}
		}
	}
	if (self->update_methods & MM_MODEM_FIRMWARE_UPDATE_METHOD_QMI_PDC) {
		for (guint i = 0; i < n_ports; i++) {
			if ((ports[i].type == MM_MODEM_PORT_TYPE_QMI) ||
			    (ports[i].type == MM_MODEM_PORT_TYPE_MBIM)) {
				self->port_qmi = g_strdup_printf ("/dev/%s", ports[i].name);
				break;
			}
		}
	}
	mm_modem_port_info_array_free (ports, n_ports);

	/* an at port is required for fastboot */
	if ((self->update_methods & MM_MODEM_FIRMWARE_UPDATE_METHOD_FASTBOOT) &&
	    (self->port_at == NULL)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "failed to find AT port");
		return FALSE;
	}

	/* a qmi port is required for qmi-pdc */
	if ((self->update_methods & MM_MODEM_FIRMWARE_UPDATE_METHOD_QMI_PDC) &&
	    (self->port_qmi == NULL)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "failed to find QMI port");
		return FALSE;
	}

	/* if we have the at port reported, get sysfs path and interface number */
	if (self->port_at != NULL) {
		fu_mm_utils_get_port_info (self->port_at, &device_sysfs_path, &self->port_at_ifnum, NULL);
	} else if (self->port_qmi != NULL) {
		fu_mm_utils_get_port_info (self->port_qmi, &device_sysfs_path, NULL, NULL);
	} else {
		g_assert_not_reached ();
	}

	/* if no device sysfs file, error out */
	if (device_sysfs_path == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "failed to find device sysfs path");
		return FALSE;
	}

	/* add properties to fwupd device */
	fu_device_set_physical_id (device, device_sysfs_path);
	fu_device_set_vendor (device, mm_modem_get_manufacturer (modem));
	fu_device_set_name (device, mm_modem_get_model (modem));
	fu_device_set_version (device, version);
	for (guint i = 0; device_ids[i] != NULL; i++)
		fu_device_add_guid (device, device_ids[i]);

	return TRUE;
}

static gboolean
fu_mm_device_probe_udev (FuDevice *device, GError **error)
{
	FuMmDevice *self = FU_MM_DEVICE (device);

	/* an at port is required for fastboot */
	if ((self->update_methods & MM_MODEM_FIRMWARE_UPDATE_METHOD_FASTBOOT) &&
	    (self->port_at == NULL)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "failed to find AT port");
		return FALSE;
	}

	/* a qmi port is required for qmi-pdc */
	if ((self->update_methods & MM_MODEM_FIRMWARE_UPDATE_METHOD_QMI_PDC) &&
	    (self->port_qmi == NULL)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "failed to find QMI port");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_mm_device_probe (FuDevice *device, GError **error)
{
	FuMmDevice *self = FU_MM_DEVICE (device);

	if (self->omodem) {
		return fu_mm_device_probe_default (device, error);
	} else {
		return fu_mm_device_probe_udev (device, error);
	}
}

static gboolean
fu_mm_device_at_cmd (FuMmDevice *self, const gchar *cmd, GError **error)
{
	const gchar *buf;
	gsize bufsz = 0;
	g_autoptr(GBytes) at_req  = NULL;
	g_autoptr(GBytes) at_res  = NULL;
	g_autofree gchar *cmd_cr = g_strdup_printf ("%s\r\n", cmd);

	/* command */
	at_req = g_bytes_new (cmd_cr, strlen (cmd_cr));
	if (g_getenv ("FWUPD_MODEM_MANAGER_VERBOSE") != NULL)
		fu_common_dump_bytes (G_LOG_DOMAIN, "writing", at_req);
	if (!fu_io_channel_write_bytes (self->io_channel, at_req, 1500,
					FU_IO_CHANNEL_FLAG_FLUSH_INPUT, error)) {
		g_prefix_error (error, "failed to write %s: ", cmd);
		return FALSE;
	}

	/* response */
	at_res = fu_io_channel_read_bytes (self->io_channel, -1, 1500,
					   FU_IO_CHANNEL_FLAG_SINGLE_SHOT, error);
	if (at_res == NULL) {
		g_prefix_error (error, "failed to read response for %s: ", cmd);
		return FALSE;
	}
	if (g_getenv ("FWUPD_MODEM_MANAGER_VERBOSE") != NULL)
		fu_common_dump_bytes (G_LOG_DOMAIN, "read", at_res);
	buf = g_bytes_get_data (at_res, &bufsz);
	if (bufsz < 6) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to read valid response for %s", cmd);
		return FALSE;
	}
	if (memcmp (buf, "\r\nOK\r\n", 6) != 0) {
		g_autofree gchar *tmp = g_strndup (buf + 2, bufsz - 4);
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to read valid response for %s: %s",
			     cmd, tmp);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_mm_device_io_open (FuMmDevice *self, GError **error)
{
	/* open device */
	self->io_channel = fu_io_channel_new_file (self->port_at, error);
	if (self->io_channel == NULL)
		return FALSE;

	/* success */
	return TRUE;
}

static gboolean
fu_mm_device_io_close (FuMmDevice *self, GError **error)
{
	if (!fu_io_channel_shutdown (self->io_channel, error))
		return FALSE;
	g_clear_object (&self->io_channel);
	return TRUE;
}

static gboolean
fu_mm_device_detach_fastboot (FuDevice *device, GError **error)
{
	FuMmDevice *self = FU_MM_DEVICE (device);
	g_autoptr(FuDeviceLocker) locker  = NULL;

	/* boot to fastboot mode */
	locker = fu_device_locker_new_full (device,
					    (FuDeviceLockerFunc) fu_mm_device_io_open,
					    (FuDeviceLockerFunc) fu_mm_device_io_close,
					    error);
	if (locker == NULL)
		return FALSE;
	if (!fu_mm_device_at_cmd (self, "AT", error))
		return FALSE;
	if (!fu_mm_device_at_cmd (self, self->detach_fastboot_at, error)) {
		g_prefix_error (error, "rebooting into fastboot not supported: ");
		return FALSE;
	}

	/* success */
	fu_device_set_remove_delay (device, FU_DEVICE_REMOVE_DELAY_RE_ENUMERATE);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	return TRUE;
}

static gboolean
fu_mm_device_detach (FuDevice *device, GError **error)
{
	FuMmDevice *self = FU_MM_DEVICE (device);
	g_autoptr(FuDeviceLocker) locker  = NULL;

	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;

	/* fastboot */
	if (self->update_methods & MM_MODEM_FIRMWARE_UPDATE_METHOD_FASTBOOT)
		return fu_mm_device_detach_fastboot (device, error);

	/* otherwise, assume we don't need any detach */
	return TRUE;
}

typedef struct {
	gchar  *filename;
	GBytes *bytes;
} FuMmFileInfo;

static void
fu_mm_file_info_free (FuMmFileInfo *file_info)
{
	if (file_info != NULL) {
		g_free (file_info->filename);
		g_bytes_unref (file_info->bytes);
		g_free (file_info);
	}
}

typedef struct {
	FuMmDevice	*device;
	GError		*error;
	GPtrArray	*file_infos;
	gsize		 total_written;
	gsize		 total_bytes;
} FuMmArchiveIterateCtx;

static void
fu_mm_qmi_pdc_archive_iterate_mcfg (FuArchive *archive, const gchar *filename, GBytes *bytes, gpointer user_data)
{
	FuMmArchiveIterateCtx *ctx = user_data;
	FuMmFileInfo *file_info;

	/* filenames should be named as 'mcfg.*.mbn', e.g.: mcfg.A2.018.mbn */
	if (!g_str_has_prefix (filename, "mcfg.") || !g_str_has_suffix (filename, ".mbn"))
		return;

	file_info = g_new0 (FuMmFileInfo, 1);
	file_info->filename = g_strdup (filename);
	file_info->bytes = g_bytes_ref (bytes);
	g_ptr_array_add (ctx->file_infos, file_info);
	ctx->total_bytes += g_bytes_get_size (file_info->bytes);
}

static gboolean
fu_mm_device_qmi_open (FuMmDevice *self, GError **error)
{
	self->qmi_pdc_updater = fu_qmi_pdc_updater_new (self->port_qmi);
	return fu_qmi_pdc_updater_open (self->qmi_pdc_updater, error);
}

static gboolean
fu_mm_device_qmi_close (FuMmDevice *self, GError **error)
{
	g_autoptr(FuQmiPdcUpdater) updater = NULL;

	updater = g_steal_pointer (&self->qmi_pdc_updater);
	return fu_qmi_pdc_updater_close (updater, error);
}

static gboolean
fu_mm_device_write_firmware_qmi_pdc (FuDevice *device, GBytes *fw, GError **error)
{
	g_autoptr(FuArchive) archive = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(GPtrArray) file_infos = g_ptr_array_new_with_free_func ((GDestroyNotify)fu_mm_file_info_free);
	FuMmArchiveIterateCtx iterate_context = {
		.device = FU_MM_DEVICE (device),
		.file_infos = file_infos,
	};

	/* decompress entire archive ahead of time */
	archive = fu_archive_new (fw, FU_ARCHIVE_FLAG_IGNORE_PATH, error);
	if (archive == NULL)
		return FALSE;

	/* boot to fastboot mode */
	locker = fu_device_locker_new_full (device,
					    (FuDeviceLockerFunc) fu_mm_device_qmi_open,
					    (FuDeviceLockerFunc) fu_mm_device_qmi_close,
					    error);
	if (locker == NULL)
		return FALSE;

	/* Process the list of MCFG files to write */
	fu_archive_iterate (archive, fu_mm_qmi_pdc_archive_iterate_mcfg, &iterate_context);

	for (guint i = 0; i < file_infos->len; i++) {
		FuMmFileInfo *file_info = g_ptr_array_index (file_infos, i);
		if (!fu_qmi_pdc_updater_write (iterate_context.device->qmi_pdc_updater,
					       file_info->filename,
					       file_info->bytes,
					       &iterate_context.error)) {
			g_warning ("Failed to write file '%s': %s",
				   file_info->filename, iterate_context.error->message);
			break;
		}
	}

	if (iterate_context.error != NULL) {
		g_propagate_error (error, iterate_context.error);
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_mm_device_write_firmware (FuDevice *device, GBytes *fw, GError **error)
{
	FuMmDevice *self = FU_MM_DEVICE (device);
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(FuArchive) archive = NULL;
	g_autoptr(GPtrArray) array = NULL;

	/* lock device */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;

	/* qmi pdc write operation */
	if (self->update_methods & MM_MODEM_FIRMWARE_UPDATE_METHOD_QMI_PDC)
		return fu_mm_device_write_firmware_qmi_pdc (device, fw, error);

	/* otherwise, nothing else to do (e.g. maybe only fastboot was required
	 * for this modem */
	return TRUE;
}

static gboolean
fu_mm_device_attach (FuDevice *device, GError **error)
{
	g_autoptr(FuDeviceLocker) locker  = NULL;

	/* lock device */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;

	/* wait for re-probing after uninhibiting */
	fu_device_set_remove_delay (device, FU_MM_DEVICE_REMOVE_DELAY_REPROBE);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	return TRUE;
}

static void
fu_mm_device_init (FuMmDevice *self)
{
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_USE_RUNTIME_VERSION);
	fu_device_set_summary (FU_DEVICE (self), "Mobile broadband device");
	fu_device_add_icon (FU_DEVICE (self), "network-modem");
}

static void
fu_mm_device_finalize (GObject *object)
{
	FuMmDevice *self = FU_MM_DEVICE (object);
	g_object_unref (self->manager);
	if (self->omodem != NULL)
		g_object_unref (self->omodem);
	g_free (self->detach_fastboot_at);
	g_free (self->port_at);
	g_free (self->port_qmi);
	g_free (self->inhibition_uid);
	G_OBJECT_CLASS (fu_mm_device_parent_class)->finalize (object);
}

static void
fu_mm_device_class_init (FuMmDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	object_class->finalize = fu_mm_device_finalize;
	klass_device->to_string = fu_mm_device_to_string;
	klass_device->probe = fu_mm_device_probe;
	klass_device->detach = fu_mm_device_detach;
	klass_device->write_firmware = fu_mm_device_write_firmware;
	klass_device->attach = fu_mm_device_attach;
}

FuMmDevice *
fu_mm_device_new (MMManager *manager, MMObject *omodem)
{
	FuMmDevice *self = g_object_new (FU_TYPE_MM_DEVICE, NULL);
	self->manager = g_object_ref (manager);
	self->omodem = g_object_ref (omodem);
	self->port_at_ifnum = -1;
	return self;
}

FuMmDevice *
fu_mm_device_udev_new (MMManager	*manager,
		       const gchar	*physical_id,
		       const gchar	*vendor,
		       const gchar	*name,
		       const gchar	*version,
		       const gchar     **device_ids,
		       MMModemFirmwareUpdateMethod update_methods,
		       const gchar	*detach_fastboot_at,
		       gint		 port_at_ifnum)
{
	FuMmDevice *self = g_object_new (FU_TYPE_MM_DEVICE, NULL);
	g_debug ("creating udev-based mm device at %s", physical_id);
	self->manager = g_object_ref (manager);
	fu_device_set_physical_id (FU_DEVICE (self), physical_id);
	fu_device_set_vendor (FU_DEVICE (self), vendor);
	fu_device_set_name (FU_DEVICE (self), name);
	fu_device_set_version (FU_DEVICE (self), version);
	for (guint i = 0; device_ids[i] != NULL; i++)
		fu_device_add_guid (FU_DEVICE (self), device_ids[i]);
	self->update_methods = update_methods;
	self->detach_fastboot_at = g_strdup (detach_fastboot_at);
	self->port_at_ifnum = port_at_ifnum;
	return self;
}

void
fu_mm_device_udev_add_port (FuMmDevice	*self,
			    const gchar	*subsystem,
			    const gchar	*path,
			    gint	 ifnum)
{
	g_return_if_fail (FU_IS_MM_DEVICE (self));

	/* cdc-wdm ports always added unless one already set */
	if (g_str_equal (subsystem, "usbmisc") &&
	    (self->port_qmi == NULL)) {
		g_debug ("added QMI port %s (%s)", path, subsystem);
		self->port_qmi = g_strdup (path);
		return;
	}

	if (g_str_equal (subsystem, "tty") &&
	    (self->port_at == NULL) &&
	    (ifnum >= 0) && (ifnum == self->port_at_ifnum)) {
		g_debug ("added AT port %s (%s)", path, subsystem);
		self->port_at = g_strdup (path);
		return;
	}

	/* otherwise, ignore all other ports */
	g_debug ("ignoring port %s (%s)", path, subsystem);
}
