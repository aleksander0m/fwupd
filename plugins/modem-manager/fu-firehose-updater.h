/*
 * Copyright (C) 2020 Quectel Wireless Solutions Co., Ltd.
 * Copyright (C) 2020 Aleksander Morgado <aleksander@aleksander.es>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#ifndef __FU_FIREHOSE_UPDATER_H
#define __FU_FIREHOSE_UPDATER_H

#include <xmlb.h>
#include "fu-archive.h"

#define FU_TYPE_FIREHOSE_UPDATER (fu_firehose_updater_get_type ())
G_DECLARE_FINAL_TYPE (FuFirehoseUpdater, fu_firehose_updater, FU, FIREHOSE_UPDATER, GObject)

FuFirehoseUpdater	*fu_firehose_updater_new	(const gchar		*port);
gboolean	 	fu_firehose_updater_open	(FuFirehoseUpdater	*self,
							 GError			**error);
gboolean	 	fu_firehose_updater_write	(FuFirehoseUpdater	*self,
							 XbSilo			*silo,
							 GPtrArray		*action_nodes,
							 GError			**error);
gboolean	 	fu_firehose_updater_close	(FuFirehoseUpdater	*self,
							 GError			**error);

/* helpers */

gboolean		fu_firehose_validate_rawprogram	(GBytes		*rawprogram,
							 FuArchive	*archive,
							 XbSilo		**out_silo,
							 GPtrArray	**out_action_nodes,
							 GError		**error);


#endif /* __FU_FIREHOSE_UPDATER_H */