/*
 * Copyright (C) 2020 Quectel Wireless Solutions Co., Ltd.
 * Copyright (C) 2020 Aleksander Morgado <aleksander@aleksander.es>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <string.h>

#ifdef HAVE_IOCTL_H
#include <sys/ioctl.h>
#endif

#include "fu-common.h"
#include "fu-io-channel.h"
#include "fu-mhi-bhi-updater.h"

struct _FuMhiBhiUpdater {
	GObject		 parent_instance;
	gchar		*port;
	FuIOChannel	*io_channel;
};

G_DEFINE_TYPE (FuMhiBhiUpdater, fu_mhi_bhi_updater, G_TYPE_OBJECT)

/* Quectel MHI BHI port ioctl()s */
#define IOCTL_BHI_GETDEVINFO 0x8BE0 + 1
#define IOCTL_BHI_WRITEIMAGE 0x8BE0 + 2

typedef struct {
   guint bhi_ver_minor;
   guint bhi_ver_major;
   guint bhi_image_address_low;
   guint bhi_image_address_high;
   guint bhi_image_size;
   guint bhi_rsvd1;
   guint bhi_imgtxdb;
   guint bhi_rsvd2;
   guint bhi_msivec;
   guint bhi_rsvd3;
   guint bhi_ee;
   guint bhi_status;
   guint bhi_errorcode;
   guint bhi_errdbg1;
   guint bhi_errdbg2;
   guint bhi_errdbg3;
   guint bhi_sernum;
   guint bhi_sblantirollbackver;
   guint bhi_numsegs;
   guint bhi_msmhwid[6];
   guint bhi_oempkhash[48];
   guint bhi_rsvd5;
} BhiInfo;

/* The module is put in Emergency Download execution environment with the
 * DIAG/QCDM command. In this state, the mhi_BHI port allows uploading the
 * firehose programmer file. */
#define MHI_EE_EDL (guint) 0x6

gboolean
fu_mhi_bhi_updater_open (FuMhiBhiUpdater *self, GError **error)
{
#ifdef HAVE_IOCTL_H
	BhiInfo bhi_info = { 0 };

	g_debug ("opening boot host interface port...");
	self->io_channel = fu_io_channel_new_file (self->port, error);
	if (self->io_channel == NULL)
		return FALSE;

	g_debug ("checking boot host interface port state...");
	if (ioctl (fu_io_channel_unix_get_fd (self->io_channel), IOCTL_BHI_GETDEVINFO, &bhi_info) != 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     g_io_error_from_errno (errno),
			     "Couldn't get MHI BHI device info");
		return FALSE;
	}

	if (bhi_info.bhi_ee != MHI_EE_EDL) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_INITIALIZED,
			     "Device is not in emergency download mode: 0x%x (expected 0x%x)",
			     bhi_info.bhi_ee, MHI_EE_EDL);
		return FALSE;
	}
	g_debug ("boot host interface port is in emergency download mode");

	return TRUE;
#else
	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_NOT_SUPPORTED,
		     "Not supported as ioctl() is unavailable");
	return FALSE;
#endif
}

gboolean
fu_mhi_bhi_updater_close (FuMhiBhiUpdater *self, GError **error)
{
	if (!fu_io_channel_shutdown (self->io_channel, error))
		return FALSE;
	g_clear_object (&self->io_channel);
	return TRUE;
}

gboolean
fu_mhi_bhi_updater_write (FuMhiBhiUpdater *self, GBytes *blob, GError **error)
{
	GByteArray *byte_array;
	gsize blob_size = g_bytes_get_size (blob);
	g_autoptr(GBytes) blob_buffer = NULL;

	g_debug ("writing firehose prog...");

	byte_array = g_bytes_unref_to_array (g_bytes_ref (blob));
	byte_array = g_byte_array_prepend (byte_array, (const guint8 *)&blob_size, sizeof (blob_size));
	blob_buffer = g_byte_array_free_to_bytes (byte_array);
	g_warn_if_fail (g_bytes_get_size (blob_buffer) == blob_size + sizeof (blob_size));

	if (g_getenv ("FWUPD_MODEM_MANAGER_VERBOSE") != NULL)
		fu_common_dump_bytes (G_LOG_DOMAIN, "writing", blob_buffer);
	if (ioctl (fu_io_channel_unix_get_fd (self->io_channel), IOCTL_BHI_WRITEIMAGE, g_bytes_get_data (blob_buffer, NULL)) != 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     g_io_error_from_errno (errno),
			     "Couldn't write to MHI BHI device");
		return FALSE;
	}

	return TRUE;
}

static void
fu_mhi_bhi_updater_init (FuMhiBhiUpdater *self)
{
}

static void
fu_mhi_bhi_updater_finalize (GObject *object)
{
	FuMhiBhiUpdater *self = FU_MHI_BHI_UPDATER (object);
	g_warn_if_fail (self->io_channel == NULL);
	g_free (self->port);
	G_OBJECT_CLASS (fu_mhi_bhi_updater_parent_class)->finalize (object);
}

static void
fu_mhi_bhi_updater_class_init (FuMhiBhiUpdaterClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = fu_mhi_bhi_updater_finalize;
}

FuMhiBhiUpdater *
fu_mhi_bhi_updater_new (const gchar *port)
{
	FuMhiBhiUpdater *self = g_object_new (FU_TYPE_MHI_BHI_UPDATER, NULL);
	self->port = g_strdup (port);
	return self;
}
