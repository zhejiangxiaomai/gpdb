/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/copy.h,v 1.18 2006/03/05 15:58:51 momjian Exp $
 */
#ifndef COPY_H
#define COPY_H

#include "libpq-fe.h"


/* handler for \copy */
bool		do_copy(const char *args);

/* lower level processors for copy in/out streams */

bool		handleCopyOut(PGconn *conn, FILE *copystream);
bool		handleCopyIn(PGconn *conn, FILE *copystream);

#endif
