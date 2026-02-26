/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   Copyright (C) 2020 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@lists.sourceforge.net
 */

#ifndef __KSMBD_CONTROL_H__
#define __KSMBD_CONTROL_H__

int control_shutdown(void);
int control_reload(void);
int control_list(void);
int control_debug(char *comp);
int control_show_version(void);
int control_status(void);
int control_features(char *pwddb, char *smbconf);

#endif /* __KSMBD_CONTROL_H__ */
