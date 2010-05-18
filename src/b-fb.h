/* b-fb.h --- basic file operations

   Copyright (C) 2010 Thien-Thi Nguyen
   Copyright (C) 1990, 1991, 1992, 1993, 1994, 1995 Paul Eggert
   Copyright (C) 1982, 1988, 1989 Walter Tichy

   This file is part of GNU RCS.

   GNU RCS is free software: you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 2 of the License, or
   (at your option) any later version.

   GNU RCS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   See the GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

extern void Ierror (void) exiting;
extern void testIerror (FILE *f);
extern void Oerror (void) exiting;
extern void testOerror (FILE *);
extern FILE *fopen_safer (char const *filename, char const *type);
extern void Ozclose (FILE **p);
extern void Orewind (FILE *f);
extern void aflush (FILE *f);
extern void oflush (void);
extern void afputc (int c, FILE *f);
extern void aputs (char const *s, FILE *iop);
extern void aprintf (FILE *iop, char const *fmt, ...)
  printf_string (2, 3);

/* b-fb.h ends here */
