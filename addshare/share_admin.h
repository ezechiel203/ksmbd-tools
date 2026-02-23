/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   Copyright (C) 2019 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@lists.sourceforge.net
 */

#ifndef __KSMBD_SHARE_ADMIN_H__
#define __KSMBD_SHARE_ADMIN_H__

typedef int share_command_fn(char *smbconf, char *name, char **options);

int command_add_share(char *smbconf, char *name, char **options);
int command_update_share(char *smbconf, char *name, char **options);
int command_delete_share(char *smbconf, char *name, char **options);

#endif /* __KSMBD_SHARE_ADMIN_H__ */
