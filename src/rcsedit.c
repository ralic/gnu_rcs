/* RCS stream editor

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

/******************************************************************************
 *                       edits the input file according to a
 *                       script from stdin, generated by diff -n
 ******************************************************************************
 */

#include "base.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#ifdef HAVE_UTIME_H
#include <utime.h>
#endif
#include <fcntl.h>
#include "same-inode.h"
#include "unistd-safer.h"
#include "b-complain.h"
#include "b-divvy.h"
#include "b-esds.h"
#include "b-fb.h"
#include "b-feph.h"
#include "b-fro.h"
#include "b-isr.h"
#include "b-kwxout.h"

struct editstuff
{
  struct fro *fedit;
  char const *filename;
  /* Edit file stream and filename.  */

  long lcount;
  /* Edit line counter; #lines before cursor.  */
  long corr;
  /* #adds - #deletes in each edit run, used to correct ‘es->lcount’
     in case file is not rewound after applying one delta.  */

  char **line;
  size_t gap, gapsize, lim;
  /* ‘line’ contains pointers to the lines in the currently "edited" file.
     It is a 0-origin array that represents ‘lim - gapsize’ lines.
     ‘line[0 .. gap-1]’ and ‘line[gap+gapsize .. lim-1]’ hold pointers to lines.
     ‘line[gap .. gap+gapsize-1]’ contains garbage.

     Any '@'s in lines are duplicated.  Lines are terminated by '\n',
     or (for a last partial line only) by single '@'.  */

  struct sff *sff;
};

#define lockname    (BE (sff)[SFFI_LOCKDIR].filename)
#define newRCSname  (BE (sff)[SFFI_NEWDIR].filename)

struct editstuff *
make_editstuff (void)
{
  return ZLLOC (1, struct editstuff);
}

void
unmake_editstuff (struct editstuff *es)
{
  free (es->line);
  memset (es, 0, sizeof (struct editstuff));
}

static void *
okalloc (void * p)
{
  if (!p)
    PFATAL ("out of memory");
  return p;
}

static void *
testalloc (size_t size)
/* Allocate a block, testing that the allocation succeeded.  */
{
  return okalloc (malloc (size));
}

static void *
testrealloc (void *ptr, size_t size)
/* Reallocate a block, testing that the allocation succeeded.  */
{
  return okalloc (realloc (ptr, size));
}

int
un_link (char const *s)
/* Remove ‘s’, even if it is unwritable.
   Ignore ‘unlink’ ‘ENOENT’ failures; NFS generates bogus ones.  */
{
  int rv = unlink (s);

  if (0 == rv)
    /* Good news, don't do anything.  */
    ;
  else
    {
      if (BAD_UNLINK)
        {
          int e = errno;

          /* Forge ahead even if ‘errno == ENOENT’;
             some completely brain-damaged hosts (e.g. PCTCP 2.2)
             return ‘ENOENT’ even for existing unwritable files.  */
          if (chmod (s, S_IWUSR) != 0)
            {
              errno = e;
              rv = -1;
            }
        }
      if (has_NFS && ENOENT == errno)
        rv = 0;
    }
  return rv;
}

#define EDIT_SCRIPT_SHORT()  \
  SYNTAX_ERROR ("edit script ends prematurely")

#define EDIT_SCRIPT_OVERFLOW()  \
  SYNTAX_ERROR ("edit script refers to line past end of file")

#define movelines(s1, s2, n)  memmove (s1, s2, (n) * sizeof (char *))

static void
insertline (struct editstuff *es, unsigned long n, char *l)
/* Before line ‘n’, insert line ‘l’.  */
{
  if (es->lim - es->gapsize < n)
    EDIT_SCRIPT_OVERFLOW ();
  if (!es->gapsize)
    es->line = !es->lim
      ? testalloc (sizeof (char *) * (es->lim = es->gapsize = 1024))
      : (es->gap = es->gapsize = es->lim,
         testrealloc (es->line, sizeof (char *) * (es->lim <<= 1)));
  if (n < es->gap)
    movelines (es->line + n + es->gapsize,
               es->line + n,
               es->gap - n);
  else if (es->gap < n)
    movelines (es->line + es->gap,
               es->line + es->gap + es->gapsize,
               n - es->gap);

  es->line[n] = l;
  es->gap = n + 1;
  es->gapsize--;
}

static void
deletelines (struct editstuff *es, unsigned long n, unsigned long nlines)
/* Delete lines ‘n’ through ‘n + nlines - 1’.  */
{
  unsigned long l = n + nlines;

  if (es->lim - es->gapsize < l || l < n)
    EDIT_SCRIPT_OVERFLOW ();
  if (l < es->gap)
    movelines (es->line + l + es->gapsize,
               es->line + l,
               es->gap - l);
  else if (es->gap < n)
    movelines (es->line + es->gap,
               es->line + es->gap + es->gapsize,
               n - es->gap);

  es->gap = n;
  es->gapsize += nlines;
}

static void
snapshotline (register FILE *f, register char *l)
{
  register int c;
  do
    {
      if ((c = *l++) == SDELIM && *l++ != SDELIM)
        return;
      aputc (c, f);
    }
  while (c != '\n');
}

static void
snapshotedit_fast (struct editstuff *es, FILE *f)
/* Copy the current state of the edits to ‘f’.  */
{
  register char **p, **lim, **l = es->line;

  for (p = l, lim = l + es->gap; p < lim;)
    snapshotline (f, *p++);
  for (p += es->gapsize, lim = l + es->lim; p < lim;)
    snapshotline (f, *p++);
}

static void
finisheditline (struct expctx *ctx, char *l)
{
  ctx->from->ptr = l;
  if (expandline (ctx) < 0)
    PFATAL ("finisheditline internal error");
}

static void
finishedit_fast (struct editstuff *es, struct hshentry const *delta,
                 FILE *outfile, bool done)
/* Do expansion if ‘delta’ is set, output the state of the edits to
   ‘outfile’.  But do nothing unless ‘done’ is set (which means we are
   on the last pass).  */
{
  if (done)
    {
      openfcopy (outfile);
      outfile = FLOW (res);
      if (!delta)
        snapshotedit_fast (es, outfile);
      else
        {
          register char **p, **lim, **l = es->line;
          register struct fro *fin = FLOW (from);
          char *here = fin->ptr;
          struct expctx ctx = EXPCTX_1OUT (outfile, fin, true, true);

          for (p = l, lim = l + es->gap; p < lim;)
            finisheditline (&ctx, *p++);
          for (p += es->gapsize, lim = l + es->lim; p < lim;)
            finisheditline (&ctx, *p++);
          fin->ptr = here;
        }
    }
}

static FILE *
fopen_update_truncate (char const *name)
/* Open a temporary NAME for output, truncating any previous contents.  */
{
  if (!STDIO_P (FLOW (from)))
    return fopen_safer (name, FOPEN_W_WORK);
  else
    {
      if (BAD_FOPEN_WPLUS && un_link (name) != 0)
        fatal_sys (name);
      return fopen_safer (name, FOPEN_WPLUS_WORK);
    }
}

void
openfcopy (FILE *f)
{
  if (!(FLOW (res) = f))
    {
      if (!FLOW (result))
        FLOW (result) = maketemp (2);
      if (!(FLOW (res) = fopen_update_truncate (FLOW (result))))
        fatal_sys (FLOW (result));
    }
}

static void
swapeditfiles (struct editstuff *es, FILE *outfile)
/* Swap ‘FLOW (result)’ and ‘es->filename’,
   assign ‘es->fedit = FLOW (res)’,
   and rewind ‘es->fedit’ for reading.
   Set ‘FLOW (res)’ to ‘outfile’ if non-NULL;
   otherwise, set ‘FLOW (res)’ to be ‘FLOW (result)’
   opened for reading and writing.  */
{
  char const *tmpptr;
  struct fro *newrile = ZLLOC (1, struct fro);

  es->lcount = 0;
  es->corr = 0;
  Orewind (FLOW (res));
  newrile->rm = RM_STDIO;
  newrile->stream = FLOW (res);
  es->fedit = newrile;
  tmpptr = es->filename;
  es->filename = FLOW (result);
  FLOW (result) = tmpptr;
  openfcopy (outfile);
}

static void
finishedit_slow (struct editstuff *es, struct hshentry const *delta,
                 FILE *outfile, bool done)
/* Copy the rest of the edit file and close it (if it exists).
   If ‘delta’, perform keyword substitution at the same time.
   If ‘done’ is set, we are finishing the last pass.  */
{
  register struct fro *fe;
  register FILE *fc;

  fe = es->fedit;
  if (fe)
    {
      fc = FLOW (res);
      if (delta)
        {
          struct expctx ctx = EXPCTX_1OUT (fc, fe, false, true);

          while (1 < expandline (&ctx))
            ;
        }
      else
        {
          fro_spew (fe, fc);
        }
      fro_close (fe);
    }
  if (!done)
    swapeditfiles (es, outfile);
}

static void
snapshotedit_slow (struct editstuff *es, FILE *f)
/* Copy the current state of the edits to ‘f’.  */
{
  finishedit_slow (es, NULL, NULL, false);
  fro_spew (es->fedit, f);
  fro_bob (es->fedit);
}

void
finishedit (struct editstuff *es, struct hshentry const *delta,
            FILE *outfile, bool done)
{
  (!STDIO_P (FLOW (from))
   ? finishedit_fast
   : finishedit_slow) (es, delta, outfile, done);
}

void
snapshotedit (struct editstuff *es, FILE *f)
{
  (!STDIO_P (FLOW (from))
   ? snapshotedit_fast
   : snapshotedit_slow) (es, f);
}

static void
copylines (struct editstuff *es, register long upto, struct hshentry const *delta)
/* Copy input lines ‘es->lcount+1..upto’ from ‘es->fedit’ to ‘FLOW (res)’.
   If ‘delta’, keyword expansion is done simultaneously.
   ‘es->lcount’ is updated.  Rewinds a file only if necessary.  */
{
  if (!STDIO_P (FLOW (from)))
    es->lcount = upto;
  else
    {
      int c;
      register FILE *fc;
      register struct fro *fe;

      if (upto < es->lcount)
        {
          /* Swap files.  */
          finishedit_slow (es, NULL, NULL, false);
          /* Assumes edit only during last pass, from the beginning.  */
        }
      fe = es->fedit;
      fc = FLOW (res);
      if (es->lcount < upto)
        {
          struct expctx ctx = EXPCTX_1OUT (fc, fe, false, true);

          if (delta)
            do
              {
                if (expandline (&ctx) <= 1)
                  EDIT_SCRIPT_OVERFLOW ();
              }
            while (++es->lcount < upto);
          else
            {
              do
                {
                  do
                    {
                      GETCHAR_OR (c, fe, EDIT_SCRIPT_OVERFLOW ());
                      aputc (c, fc);
                    }
                  while (c != '\n');
                }
              while (++es->lcount < upto);
            }
        }
    }
}

void
copystring (struct editstuff *es)
/* Copy a string terminated with a single ‘SDELIM’ from ‘FLOW (from)’ to
   ‘FLOW (res)’, replacing all double ‘SDELIM’ with a single ‘SDELIM’.  If
   ‘FLOW (to)’ is non-NULL, the string also copied unchanged to ‘FLOW (to)’.
   ‘es->lcount’ is incremented by the number of lines copied.  Assumption:
   next character read is first string character.  */
{
  int c;
  register FILE *frew, *fcop;
  register bool amidline;
  register struct fro *fin;

  fin = FLOW (from);
  frew = FLOW (to);
  fcop = FLOW (res);
  amidline = false;
  for (;;)
    {
      TEECHAR ();
      switch (c)
        {
        case '\n':
          ++es->lcount;
          ++LEX (lno);
          amidline = false;
          break;
        case SDELIM:
          TEECHAR ();
          if (c != SDELIM)
            {
              /* End of string.  */
              NEXT (c) = c;
              es->lcount += amidline;
              return;
            }
          /* fall into */
        default:
          amidline = true;
          break;
        }
      aputc (c, fcop);
    }
}

void
enterstring (struct editstuff *es)
/* Like ‘copystring’, except the string is
   put into the ‘edit’ data structure.  */
{
  if (STDIO_P (FLOW (from)))
    {
      es->filename = NULL;
      es->fedit = NULL;
      es->lcount = es->corr = 0;
      FLOW (result) = maketemp (1);
      if (!(FLOW (res) = fopen_update_truncate (FLOW (result))))
        fatal_sys (FLOW (result));
      copystring (es);
    }
  else
    {
      int c;
      register FILE *frew;
      register long e, oe;
      register bool amidline, oamidline;
      register char *optr;
      register struct fro *fin;

      e = 0;
      es->gap = 0;
      es->gapsize = es->lim;
      fin = FLOW (from);
      fro_trundling (false, fin);
      frew = FLOW (to);
      amidline = false;
      for (;;)
        {
          optr = fin->ptr;
          TEECHAR ();
          oamidline = amidline;
          oe = e;
          switch (c)
            {
            case '\n':
              ++e;
              ++LEX (lno);
              amidline = false;
              break;
            case SDELIM:
              TEECHAR ();
              if (c != SDELIM)
                {
                  /* End of string.  */
                  NEXT (c) = c;
                  es->lcount = e + amidline;
                  es->corr = 0;
                  return;
                }
              /* fall into */
            default:
              amidline = true;
              break;
            }
          if (!oamidline)
            insertline (es, oe, optr);
        }
    }
}

void
editstring (struct editstuff *es, struct hshentry const *delta)
/* Read an edit script from ‘FLOW (from)’ and applies it to the edit file.
   | -- ‘(STDIO_P (FLOW (from)))’ --
   | The result is written to ‘FLOW (res)’.
   | If ‘delta’, keyword expansion is performed simultaneously.
   | If running out of lines in ‘es->fedit’,
   | ‘es->fedit’ and ‘FLOW (res)’ are swapped.
   | ‘es->filename’ is the name of the file that goes with ‘es->fedit’.
   If ‘FLOW (to)’ is set, the edit script is also copied verbatim
   to ‘FLOW (to)’.  Assumes that all these files are open.
   ‘FLOW (result)’ is the name of the file that goes with ‘FLOW (res)’.
   Assumes the next input character from ‘FLOW (from)’ is the first
   character of the edit script.  Resets ‘NEXT (c)’ on exit.  */
{
  int ed;                               /* editor command */
  int c;
  register FILE *frew;
  register FILE *f = NULL;
  long line_lim = LONG_MAX;
  register struct fro *fe;
  register long i;
  register struct fro *fin;
  register long j = 0;
  struct diffcmd dc;

  es->lcount += es->corr;
  es->corr = 0;                         /* correct line number */
  frew = FLOW (to);
  fin = FLOW (from);
  initdiffcmd (&dc);
  while (0 <= (ed = getdiffcmd (fin, true, frew, &dc)))
    if (STDIO_P (fin)
        && line_lim <= dc.line1)
      EDIT_SCRIPT_OVERFLOW ();
    else if (!ed)
      {
        copylines (es, dc.line1 - 1, delta);
        /* Skip over unwanted lines.  */
        i = dc.nlines;
        es->corr -= i;
        es->lcount += i;
        if (!STDIO_P (fin))
          deletelines (es, es->lcount + es->corr, i);
        else
          {
            fe = es->fedit;
            do
              {
                /* Skip next line.  */
                do GETCHAR_OR (c, fe,
                               {
                                 if (i != 1)
                                   EDIT_SCRIPT_OVERFLOW ();
                                 line_lim = dc.dafter;
                                 goto done;
                               });
                while (c != '\n');
              done:
                ;
              }
            while (--i);
          }
      }
    else
      {
        /* Copy lines without deleting any.  */
        copylines (es, dc.line1, delta);
        i = dc.nlines;
        if (!STDIO_P (fin))
          j = es->lcount + es->corr;
        es->corr += i;
        if (STDIO_P (fin)
            && (f = FLOW (res),
                delta))
          {
            struct expctx ctx = EXPCTX (f, frew, fin, true, true);

            do
              {
                switch (expandline (&ctx))
                  {
                  case 0:
                  case 1:
                    if (i == 1)
                      return;
                    /* fall into */
                  case -1:
                    EDIT_SCRIPT_SHORT ();
                  }
              }
            while (--i);
          }
        else
          {
            do
              {
                if (!STDIO_P (fin))
                  insertline (es, j++, fin->ptr);
                for (;;)
                  {
                    TEECHAR ();
                    if (c == SDELIM)
                      {
                        TEECHAR ();
                        if (c != SDELIM)
                          {
                            if (--i)
                              EDIT_SCRIPT_SHORT ();
                            NEXT (c) = c;
                            return;
                          }
                      }
                    if (STDIO_P (fin))
                      aputc (c, f);
                    if (c == '\n')
                      break;
                  }
                ++LEX (lno);
              }
            while (--i);
          }
      }
}

static int
naturalize (struct maybe *m, bool *symbolicp)
/* If ‘fn’ is a symbolic link, resolve it to the name that it points to.
   If unsuccessful, set errno and return -1.
   If it points to an existing file, return 1.
   Otherwise, set ‘errno’ to ‘ENOENT’ and return 0.
   On return ‘*symbolicp’ set means the filename was a symlink, and
   ‘fn’ points to the resolved filename (no change if not a symlink).  */
{
  int e;
  ssize_t r;
  int linkcount = _POSIX_SYMLOOP_MAX;
  struct cbuf *fn = &m->tentative;
  struct divvy *space = m->space;
  size_t len = SIZEABLE_FILENAME_LEN;
  char *chased = alloc (space, "chased", len);
  char const *orig = fn->string;

  while ((r = readlink (fn->string, chased, len)) != -1)
    if (len == (size_t) r)
      {
        len <<= 1;
        chased = alloc (space, "chased", len);
      }
    else if (!linkcount--)
      {
        errno = ELOOP;
        return -1;
      }
    else
      {
        /* Blech, ‘readlink’ does not NUL-terminate.  */
        chased[r] = '\0';
        if (ABSFNAME (chased))
          {
            fn->string = chased;
            fn->size = r;
          }
        else
          {
            char const *s = fn->string;

            accumulate_range (space, s, basefilename (s));
            accs (space, chased);
            fn->string = finish_string (space, &fn->size);
          }
      }
  e = errno;
  *symbolicp = fn->string != orig;
  errno = e;
  switch (e)
    {
    case EINVAL:
      return 1;
    case ENOENT:
      return 0;
    default:
      return -1;
    }
}

struct fro *
rcswriteopen (struct maybe *m)
/* Create the lock file corresponding to ‘m->tentative’.  Then try to
   open ‘m->tentative’ for reading and return its ‘fro*’ descriptor.
   Put its status into ‘*(m->status)’ too.
   ‘m->mustread’ is true if the file must already exist, too.
   If all goes well, discard any previously acquired locks,
   and set ‘REPO (fd_lock)’ to the file descriptor of the RCS lockfile.  */
{
  struct cbuf *RCSbuf = &m->tentative;
  register char *tp;
  register char const *sp, *repofn, *x;
  struct fro *f;
  size_t len;
  int e, exists, fdesc, fdescSafer, r;
  bool waslocked;
  struct stat statbuf;
  bool symbolicp = false;
  char *lfn;                            /* lock filename */

  waslocked = 0 <= REPO (fd_lock);
  exists = naturalize (m, &symbolicp);
  if (exists < (m->mustread | waslocked))
    /* There's an unusual problem with the RCS file; or the RCS file
       doesn't exist, and we must read or we already have a lock
       elsewhere.  */
    return NULL;

  repofn = RCSbuf->string;
  accs (m->space, RCSbuf->string);
  lfn = finish_string (m->space, &len);
  sp = basefilename (lfn);
  x = rcssuffix (repofn);
  if (!x && symbolicp)
    {
      PERR ("symbolic link to non RCS file `%s'", repofn);
      errno = EINVAL;
      return NULL;
    }
  if (*sp == *x)
    {
      PERR ("RCS filename `%s' incompatible with suffix `%s'", sp, x);
      errno = EINVAL;
      return NULL;
    }
  /* Create a lock filename that is a function of the RCS filename.  */
  if (*x)
    {
      /* The suffix is nonempty.  The lock filename is the first char
         of of the suffix, followed by the RCS filename with last char
         removed.  E.g.:
         | foo,v  -- RCS filename with suffix ,v
         | ,foo,  -- lock filename
      */
      tp = lfn + len;
      *tp-- = '\0';
      while (sp < tp)
        {
          *tp = tp[-1];
          tp--;
        }
      *tp = *x;
    }
  else
    {
      /* The suffix is empty.  The lock filename is the RCS filename
         with last char replaced by '_'.  */
      tp = lfn + len - 1;
      if ('_' == *tp)
        {
          PERR ("RCS filename `%s' ends with `%c'", repofn, *tp);
          errno = EINVAL;
          return NULL;
        }
      *tp = '_';
    }

  BE (sff)[waslocked].filename = intern (SINGLE, lfn, len);

  f = NULL;

  /* good news:
     ‘open (f, O_CREAT|O_EXCL|O_TRUNC|..., OPEN_CREAT_READONLY)’
     is atomic according to POSIX 1003.1-1990.

     bad news:
     NFS ignores O_EXCL and doesn't comply with POSIX 1003.1-1990.

     good news:
     ‘(O_TRUNC,OPEN_CREAT_READONLY)’ normally guarantees atomicity
     even with NFS.

     bad news:
     If you're root, ‘(O_TRUNC,OPEN_CREAT_READONLY)’ doesn't guarantee atomicity.

     good news:
     Root-over-the-wire NFS access is rare for security reasons.
     This bug has never been reported in practice with RCS.
     So we don't worry about this bug.

     An even rarer NFS bug can occur when clients retry requests.
     This can happen in the usual case of NFS over UDP.
     Suppose client A releases a lock by renaming ",f," to "f,v" at
     about the same time that client B obtains a lock by creating ",f,",
     and suppose A's first rename request is delayed, so A reissues it.
     The sequence of events might be:
     - A sends rename (",f,", "f,v").
     - B sends create (",f,").
     - A sends retry of rename (",f,", "f,v").
     - server receives, does, and acknowledges A's first ‘rename’.
     - A receives acknowledgment, and its RCS program exits.
     - server receives, does, and acknowledges B's ‘create’.
     - server receives, does, and acknowledges A's retry of ‘rename’.
     This not only wrongly deletes B's lock, it removes the RCS file!
     Most NFS implementations have idempotency caches that usually prevent
     this scenario, but such caches are finite and can be overrun.

     This problem afflicts not only RCS, which uses ‘open’ and ‘rename’
     to get and release locks; it also afflicts the traditional
     Unix method of using ‘link’ and ‘unlink’ to get and release locks,
     and the less traditional method of using ‘mkdir’ and ‘rmdir’.
     There is no easy workaround.

     Any new method based on ‘lockf’ seemingly would be incompatible with
     the old methods; besides, ‘lockf’ is notoriously buggy under NFS.
     Since this problem afflicts scads of Unix programs, but is so rare
     that nobody seems to be worried about it, we won't worry either.  */

#define create(f) open (f, OPEN_O_BINARY | OPEN_O_LOCK                \
                        | OPEN_O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, \
                        OPEN_CREAT_READONLY)

  ISR_DO (CATCHINTS);
  IGNOREINTS ();

  /* Create a lock file for an RCS file.  This should be atomic,
     i.e.  if two processes try it simultaneously, at most one
     should succeed.  */
  seteid ();
  fdesc = create (lfn);
  /* Do it now; ‘setrid’ might use stderr.  */
  fdescSafer = fd_safer (fdesc);
  e = errno;
  setrid ();

  if (0 <= fdesc)
    BE (sff)[SFFI_LOCKDIR].disposition = effective;

  if (fdescSafer < 0)
    {
      if (e == EACCES && stat (lfn, &statbuf) == 0)
        /* The RCS file is busy.  */
        e = EEXIST;
    }
  else
    {
      e = ENOENT;
      if (exists)
        {
          f = fro_open (repofn, FOPEN_RB, m->status);
          e = errno;
          if (f && waslocked)
            {
              /* Discard the previous lock in favor of this one.  */
              ORCSclose ();
              seteid ();
              r = un_link (lockname);
              e = errno;
              setrid ();
              errno = e;
              if (r != 0)
                fatal_sys (lockname);
              BE (sff)[SFFI_LOCKDIR].filename = lfn;
            }
        }
      REPO (fd_lock) = fdescSafer;
    }

  RESTOREINTS ();

  errno = e;
  return f;
}

int
chnamemod (FILE ** fromp, char const *from, char const *to,
           int set_mode, mode_t mode, time_t mtime)
/* Rename a file (with stream pointer ‘*fromp’) from ‘from’ to ‘to’.
   ‘from’ already exists.
   If ‘0 < set_mode’, change the mode to ‘mode’, before renaming if possible.
   If ‘mtime’ is not -1, change its mtime to ‘mtime’ before renaming.
   Close and clear ‘*fromp’ before renaming it.
   Unlink ‘to’ if it already exists.
   Return -1 on error (setting ‘errno’), 0 otherwise.   */
{
  mode_t mode_while_renaming = mode;
  int fchmod_set_mode = 0;

#if BAD_A_RENAME || bad_NFS_rename
  struct stat st;
  if (bad_NFS_rename || (BAD_A_RENAME && set_mode <= 0))
    {
      if (fstat (fileno (*fromp), &st) != 0)
        return -1;
      if (BAD_A_RENAME && set_mode <= 0)
        mode = st.st_mode;
    }
#endif  /* BAD_A_RENAME || bad_NFS_rename */

#if BAD_A_RENAME
  /* There's a short window of inconsistency
     during which the lock file is writable.  */
  mode_while_renaming = mode | S_IWUSR;
  if (mode != mode_while_renaming)
    set_mode = 1;
#endif  /* BAD_A_RENAME */

#ifdef HAVE_FCHMOD
  if (0 < set_mode && fchmod (fileno (*fromp), mode_while_renaming) == 0)
    fchmod_set_mode = set_mode;
#endif
  /* On some systems, we must close before chmod.  */
  Ozclose (fromp);
  if (fchmod_set_mode < set_mode && chmod (from, mode_while_renaming) != 0)
    return -1;

  if (setmtime (from, mtime) != 0)
    return -1;

#if BAD_B_RENAME
  /* There's a short window of inconsistency
     during which ‘to’ does not exist.  */
  if (un_link (to) != 0 && errno != ENOENT)
    return -1;
#endif  /* BAD_B_RENAME */

  if (rename (from, to) != 0 && !(has_NFS && errno == ENOENT))
    return -1;

#if bad_NFS_rename
  {
    /* Check whether the rename falsely reported success.
       A race condition can occur between the rename and the stat.  */
    struct stat tostat;

    if (stat (to, &tostat) != 0)
      return -1;
    if (!SAME_INODE (st, tostat))
      {
        errno = EIO;
        return -1;
      }
  }
#endif  /* bad_NFS_rename */

#if BAD_A_RENAME
  if (0 < set_mode && chmod (to, mode) != 0)
    return -1;
#endif

  return 0;
}

int
setmtime (char const *file, time_t mtime)
/* Set ‘file’ last modified time to ‘mtime’,
   but do nothing if ‘mtime’ is -1.  */
{
  struct utimbuf amtime;

  if (mtime == -1)
    return 0;
  amtime.actime = now ();
  amtime.modtime = mtime;
  return utime (file, &amtime);
}

int
findlock (bool delete, struct hshentry **target)
/* Find the first lock held by caller and return a pointer
   to the locked delta; also, remove the lock if ‘delete’.
   If one lock, put it into ‘*target’.
   Return 0 for no locks, 1 for one, 2 for two or more.  */
{
  struct rcslock *rl;
  struct wlink wfake, *wtp, *found = NULL;

  for (wfake.next = ADMIN (locks), wtp = &wfake; wtp->next; wtp = wtp->next)
    {
      rl = wtp->next->entry;
      if (STR_SAME (getcaller (), rl->login))
        {
          if (found)
            {
              RERR ("multiple revisions locked by %s; please specify one",
                    getcaller ());
              return 2;
            }
          found = wtp;
        }
    }
  if (!found)
    return 0;
  rl = found->next->entry;
  *target = rl->delta;
  if (delete)
    {
      found->next = found->next->next;
      ADMIN (locks) = wfake.next;
      rl->delta->lockedby = NULL;
    }
  return 1;
}

int
addlock (struct hshentry *delta, bool verbose)
/* Add a lock held by caller to ‘delta’ and return 1 if successful.
   Print an error message if ‘verbose’ and return -1 if no lock is
   added because ‘delta’ is locked by somebody other than caller.
   Return 0 if the caller already holds the lock.   */
{
  register struct rcslock *rl;
  struct wlink *ls;

  for (ls = ADMIN (locks); ls; ls = ls->next)
    {
      rl = ls->entry;
      if (cmpnum (delta->num, rl->delta->num) == 0)
        {
          if (STR_SAME (getcaller (), rl->login))
            return 0;
          else
            {
              if (verbose)
                RERR ("Revision %s is already locked by %s.",
                      delta->num, rl->login);
              return -1;
            }
        }
    }
  rl = FALLOC (struct rcslock);
  delta->lockedby = rl->login = getcaller ();
  rl->delta = delta;
  ADMIN (locks) = wprepend (rl, ADMIN (locks), SINGLE);
  return 1;
}

int
addsymbol (char const *num, char const *name, bool rebind)
/* Associate with revision ‘num’ the new symbolic ‘name’.
   If ‘name’ already exists and ‘rebind’ is set, associate ‘name’
   with ‘num’; otherwise, print an error message and return false;
   Return -1 if unsuccessful, 0 if no change, 1 if change.  */
{
  struct wlink fake, *tp;
  struct symdef *d;

  fake.next = ADMIN (assocs);
  for (tp = &fake; tp->next; tp = tp->next)
    {
      d = tp->next->entry;
      if (STR_SAME (name, d->meaningful))
        {
          if (STR_SAME (d->underlying, num))
            return 0;
          else if (rebind)
            {
              d->underlying = num;
              return 1;
            }
          else
            {
              RERR ("symbolic name %s already bound to %s",
                    name, d->underlying);
              return -1;
            }
        }
    }
  d = FALLOC (struct symdef);
  d->meaningful = name;
  d->underlying = num;
  wextend (tp, d, SINGLE);
  ADMIN (assocs) = fake.next;
  return 1;
}

char const *
getcaller (void)
/* Get the caller's login name.  */
{
#if defined HAVE_SETUID
  return getusername (euid () != ruid ());
#else
  return getusername (false);
#endif
}

bool
checkaccesslist (void)
/* Return true if caller is the superuser, the owner of the
   file, the access list is empty, or caller is on the access list.
   Otherwise, print an error message and return false.  */
{
  if (!ADMIN (allowed) || myself (REPO (stat).st_uid)
      || STR_SAME (getcaller (), "root"))
    return true;

  for (struct link *ls = ADMIN (allowed); ls; ls = ls->next)
    if (STR_SAME (getcaller (), ls->entry))
      return true;

  RERR ("user %s not on the access list", getcaller ());
  return false;
}

int
dorewrite (bool lockflag, int changed)
/* Do nothing if not ‘lockflag’.
   Prepare to rewrite an RCS file if ‘changed’ is positive.
   Stop rewriting if ‘changed’ is zero, because there won't be any changes.
   Fail if ‘changed’ is negative.
   Return 0 on success, -1 on failure.  */
{
  int r = 0, e;

  if (lockflag)
    {
      if (changed)
        {
          if (changed < 0)
            return -1;
          putadmin ();
          puttree (ADMIN (head), FLOW (rewr));
          aprintf (FLOW (rewr), "\n\n%s%c", TINYKS (desc), NEXT (c));
          FLOW (to) = FLOW (rewr);
        }
      else
        {
#if BAD_CREAT0
          int nr = !!FLOW (rewr), ne = 0;
#endif
          ORCSclose ();
          seteid ();
          IGNOREINTS ();
#if BAD_CREAT0
          if (nr)
            {
              nr = un_link (newRCSname);
              ne = errno;
              keepdirtemp (newRCSname);
            }
#endif
          r = un_link (lockname);
          e = errno;
          keepdirtemp (lockname);
          RESTOREINTS ();
          setrid ();
          if (r != 0)
            syserror (e, lockname);
#if BAD_CREAT0
          if (nr != 0)
            {
              syserror (ne, newRCSname);
              r = -1;
            }
#endif
        }
      }
  return r;
}

int
donerewrite (int changed, time_t newRCStime)
/* Finish rewriting an RCS file if ‘changed’ is nonzero.
   Set its mode if ‘changed’ is positive.
   Set its modification time to ‘newRCStime’ unless it is -1.
   Return 0 on success, -1 on failure.  */
{
  int r = 0, e = 0;
#if BAD_CREAT0
  int lr, le;
#endif

  if (changed && !FLOW (erroneousp))
    {
      if (FLOW (from))
        {
          fro_spew (FLOW (from), FLOW (rewr));
          fro_zclose (&FLOW (from));
        }
      if (1 < REPO (stat).st_nlink)
        RWARN ("breaking hard link");
      aflush (FLOW (rewr));
      seteid ();
      IGNOREINTS ();
      r = chnamemod (&FLOW (rewr), newRCSname, REPO (filename), changed,
                     REPO (stat).st_mode & (mode_t) ~(S_IWUSR | S_IWGRP | S_IWOTH),
                     newRCStime);
      e = errno;
      keepdirtemp (newRCSname);
#if BAD_CREAT0
      lr = un_link (lockname);
      le = errno;
      keepdirtemp (lockname);
#endif
      RESTOREINTS ();
      setrid ();
      if (r != 0)
        {
          syserror (e, REPO (filename));
          PERR ("saved in %s", newRCSname);
        }
#if BAD_CREAT0
      if (lr != 0)
        {
          syserror (le, lockname);
          r = -1;
        }
#endif
    }
  return r;
}

void
ORCSclose (void)
{
  if (0 <= REPO (fd_lock))
    {
      if (close (REPO (fd_lock)) != 0)
        fatal_sys (lockname);
      REPO (fd_lock) = -1;
    }
  Ozclose (&FLOW (rewr));
}

void
ORCSerror (void)
/* Like ‘ORCSclose’, except we are cleaning up after an interrupt or
   fatal error.  Do not report errors, since this may loop.  This is
   needed only because some brain-damaged hosts (e.g. OS/2) cannot
   unlink files that are open, and some nearly-POSIX hosts (e.g. NFS)
   work better if the files are closed first.  This isn't a completely
   reliable away to work around brain-damaged hosts, because of the gap
   between actual file opening and setting ‘FLOW (rewr)’ etc., but it's
   better than nothing.  */
{
  if (0 <= REPO (fd_lock))
    close (REPO (fd_lock));
  if (FLOW (rewr))
    /* Avoid ‘fclose’, since stdio may not be reentrant.  */
    close (fileno (FLOW (rewr)));
}

/* rcsedit.c ends here */
