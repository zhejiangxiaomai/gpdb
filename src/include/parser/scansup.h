/*-------------------------------------------------------------------------
 *
 * scansup.h
 *	  scanner support routines.  used by both the bootstrap lexer
 * as well as the normal lexer
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/scansup.h,v 1.19 2006/03/05 15:58:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef SCANSUP_H
#define SCANSUP_H

extern char *scanstr(const char *s);

extern char *downcase_truncate_identifier(const char *ident, int len,
							 bool warn);

extern void truncate_identifier(char *ident, int len, bool warn);

#endif   /* SCANSUP_H */
