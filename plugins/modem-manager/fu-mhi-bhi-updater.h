/*
 * Copyright (C) 2020 Quectel Wireless Solutions Co., Ltd.
 * Copyright (C) 2020 Aleksander Morgado <aleksander@aleksander.es>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#ifndef __FU_MHI_BHI_UPDATER_H
#define __FU_MHI_BHI_UPDATER_H

#define FU_TYPE_MHI_BHI_UPDATER (fu_mhi_bhi_updater_get_type ())
G_DECLARE_FINAL_TYPE (FuMhiBhiUpdater, fu_mhi_bhi_updater, FU, MHI_BHI_UPDATER, GObject)

FuMhiBhiUpdater	*fu_mhi_bhi_updater_new		(const gchar		*port);
gboolean	fu_mhi_bhi_updater_open		(FuMhiBhiUpdater	*self,
						 GError			**error);
gboolean	fu_mhi_bhi_updater_write	(FuMhiBhiUpdater	*self,
						 GBytes			*blob,
						 GError			**error);
gboolean	fu_mhi_bhi_updater_close	(FuMhiBhiUpdater	*self,
						 GError			**error);

#endif /* __FU_MHI_BHI_UPDATER_H */
