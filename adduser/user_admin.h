/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@lists.sourceforge.net
 */

#ifndef __KSMBD_USER_ADMIN_H__
#define __KSMBD_USER_ADMIN_H__

#define MAX_NT_PWD_LEN 129

typedef int user_command_fn(char *pwddb, char *name, char *password);

int command_add_user(char *pwddb, char *name, char *password);
int command_update_user(char *pwddb, char *name, char *password);
int command_delete_user(char *pwddb, char *name, char *password);

#endif /* __KSMBD_USER_ADMIN_H__ */
