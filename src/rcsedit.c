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
#ifdef HAVE_UTIME_H
#include <utime.h>
#endif
#include <fcntl.h>
#include "same-inode.h"
#include "unistd-safer.h"
#include "b-complain.h"
#include "b-fb.h"
#include "b-isr.h"
#include "b-kwxout.h"
#include "b-divvy.h"

/* (Somewhat) fleeting files.  */
enum maker { notmade, real, effective };

struct sff
{
  struct buf filename;
  /* Unlink this when done.  */
  enum maker volatile disposition;
  /* (But only if it is in the right mood.)  */
};

/* Indexes into `sff'.  */
#define SFFI_LOCKDIR  0
#define SFFI_NEWDIR   BAD_CREAT0

#define SFF_COUNT  (SFFI_NEWDIR + 2)

struct editstuff
{
#if !large_memory
  RILE *fedit;
  char const *filename;
  /* Edit file stream and filename.  */
#endif

  long lcount;
  /* Edit line counter; #lines before cursor.  */
  long corr;
  /* #adds - #deletes in each edit run, used to correct `EDIT (lcount)'
     in case file is not rewound after applying one delta.  */

  struct sff sff[SFF_COUNT];
  /* (Somewhat) fleeting files.  */

#if large_memory
  Iptr_type *line;
  size_t gap, gapsize, lim;
  /* `line' contains pointers to the lines in the currently "edited" file.
     It is a 0-origin array that represents `lim - gapsize' lines.
     `line[0 .. gap-1]' and `line[gap+gapsize .. lim-1]' hold pointers to lines.
     `line[gap .. gap+gapsize-1]' contains garbage.

     Any '@'s in lines are duplicated.  Lines are terminated by '\n',
     or (for a last partial line only) by single '@'.  */
#endif
};

#define EDIT(x)  (((struct editstuff *) BE (editstuff))-> x)

#define lockname    (EDIT (sff)[SFFI_LOCKDIR].filename.string)
#define newRCSname  (EDIT (sff)[SFFI_NEWDIR].filename.string)

int
un_link (char const *s)
/* Remove `s', even if it is unwritable.
   Ignore `unlink' `ENOENT' failures; NFS generates bogus ones.  */
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

          /* Forge ahead even if `errno == ENOENT';
             some completely brain-damaged hosts (e.g. PCTCP 2.2)
             return `ENOENT' even for existing unwritable files.  */
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

static void
editEndsPrematurely (void)
{
  fatal_syntax ("edit script ends prematurely");
}

static void
editLineNumberOverflow (void)
{
  fatal_syntax ("edit script refers to line past end of file");
}

#if large_memory

#define movelines(s1, s2, n)  memmove (s1, s2, (n) * sizeof (Iptr_type))

static void
insertline (unsigned long n, Iptr_type l)
/* Before line `n', insert line `l'.  */
{
  if (EDIT (lim) - EDIT (gapsize) < n)
    editLineNumberOverflow ();
  if (!EDIT (gapsize))
    EDIT (line) = !EDIT (lim)
      ? testalloc (sizeof (Iptr_type) * (EDIT (lim) = EDIT (gapsize) = 1024))
      : (EDIT (gap) = EDIT (gapsize) = EDIT (lim),
         testrealloc (EDIT (line), sizeof (Iptr_type) * (EDIT (lim) <<= 1)));
  if (n < EDIT (gap))
    movelines (EDIT (line) + n + EDIT (gapsize),
               EDIT (line) + n,
               EDIT (gap) - n);
  else if (EDIT (gap) < n)
    movelines (EDIT (line) + EDIT (gap),
               EDIT (line) + EDIT (gap) + EDIT (gapsize),
               n - EDIT (gap));

  EDIT (line)[n] = l;
  EDIT (gap) = n + 1;
  EDIT (gapsize)--;
}

static void
deletelines (unsigned long n, unsigned long nlines)
/* Delete lines `n' through `n + nlines - 1'.  */
{
  unsigned long l = n + nlines;

  if (EDIT (lim) - EDIT (gapsize) < l || l < n)
    editLineNumberOverflow ();
  if (l < EDIT (gap))
    movelines (EDIT (line) + l + EDIT (gapsize),
               EDIT (line) + l,
               EDIT (gap) - l);
  else if (EDIT (gap) < n)
    movelines (EDIT (line) + EDIT (gap),
               EDIT (line) + EDIT (gap) + EDIT (gapsize),
               n - EDIT (gap));

  EDIT (gap) = n;
  EDIT (gapsize) += nlines;
}

static void
snapshotline (register FILE *f, register Iptr_type l)
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

void
snapshotedit (FILE *f)
/* Copy the current state of the edits to `f'.  */
{
  register Iptr_type *p, *lim, *l = EDIT (line);

  for (p = l, lim = l + EDIT (gap); p < lim;)
    snapshotline (f, *p++);
  for (p += EDIT (gapsize), lim = l + EDIT (lim); p < lim;)
    snapshotline (f, *p++);
}

static void
finisheditline (RILE *fin, FILE *fout, Iptr_type l,
                struct hshentry const *delta)
{
  struct expctx ctx = EXPCTX_1OUT (fout, fin, true, true);

  fin->ptr = l;
  if (expandline (&ctx) < 0)
    PFATAL ("finisheditline internal error");
}

void
finishedit (struct hshentry const *delta, FILE *outfile, bool done)
/* Doing expansion if `delta' is set, output the state of the edits to
   `outfile'.  But do nothing unless `done' is set (which means we are
   on the last pass).  */
{
  if (done)
    {
      openfcopy (outfile);
      outfile = FLOW (res);
      if (!delta)
        snapshotedit (outfile);
      else
        {
          register Iptr_type *p, *lim, *l = EDIT (line);
          register RILE *fin = FLOW (from);
          Iptr_type here = fin->ptr;

          for (p = l, lim = l + EDIT (gap); p < lim;)
            finisheditline (fin, outfile, *p++, delta);
          for (p += EDIT (gapsize), lim = l + EDIT (lim); p < lim;)
            finisheditline (fin, outfile, *p++, delta);
          fin->ptr = here;
        }
    }
}

/* Open a temporary NAME for output, truncating any previous contents.  */
#define fopen_update_truncate(name)  fopen_safer (name, FOPEN_W_WORK)

#else /* !large_memory */

static FILE *
fopen_update_truncate (char const *name)
{
  if (BAD_FOPEN_WPLUS && un_link (name) != 0)
    fatal_sys (name);
  return fopen_safer (name, FOPEN_WPLUS_WORK);
}

#endif  /* !large_memory */

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

#if !large_memory

static void
swapeditfiles (FILE *outfile)
/* Swap `FLOW (result)' and `EDIT (filename)',
   assign `EDIT (fedit) = FLOW (res)',
   and rewind `EDIT (fedit)' for reading.
   Set `FLOW (res)' to `outfile' if non-NULL;
   otherwise, set `FLOW (res)' to be `FLOW (result)'
   opened for reading and writing.  */
{
  char const *tmpptr;

  EDIT (lcount) = 0;
  EDIT (corr) = 0;
  Orewind (FLOW (res));
  EDIT (fedit) = FLOW (res);
  tmpptr = EDIT (filename);
  EDIT (filename) = FLOW (result);
  FLOW (result) = tmpptr;
  openfcopy (outfile);
}

void
snapshotedit (FILE *f)
/* Copy the current state of the edits to `f'.  */
{
  finishedit (NULL, NULL, false);
  fastcopy (EDIT (fedit), f);
  Irewind (EDIT (fedit));
}

void
finishedit (struct hshentry const *delta, FILE *outfile, bool done)
/* Copy the rest of the edit file and close it (if it exists).
   If `delta', perform keyword substitution at the same time.
   If `done' is set, we are finishing the last pass.  */
{
  register RILE *fe;
  register FILE *fc;

  fe = EDIT (fedit);
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
          fastcopy (fe, fc);
        }
      Ifclose (fe);
    }
  if (!done)
    swapeditfiles (outfile);
}
#endif  /* !large_memory */

#if large_memory
#define copylines(upto,delta)  (EDIT (lcount) = (upto))
#else  /* !large_memory */
static void
copylines (register long upto, struct hshentry const *delta)
/* Copy input lines `EDIT (lcount)+1..upto' from `EDIT (fedit)' to `FLOW (res)'.
   If `delta', keyword expansion is done simultaneously.
   `EDIT (lcount)' is updated.  Rewinds a file only if necessary.  */
{
  register int c;
  declarecache;
  register FILE *fc;
  register RILE *fe;

  if (upto < EDIT (lcount))
    {
      /* Swap files.  */
      finishedit (NULL, NULL, false);
      /* Assumes edit only during last pass, from the beginning.  */
    }
  fe = EDIT (fedit);
  fc = FLOW (res);
  if (EDIT (lcount) < upto)
    {
      struct expctx ctx = EXPCTX_1OUT (fc, fe, false, true);

      if (delta)
        do
          {
            if (expandline (&ctx) <= 1)
              editLineNumberOverflow ();
          }
        while (++EDIT (lcount) < upto);
      else
        {
          setupcache (fe);
          cache (fe);
          do
            {
              do
                {
                  cachegeteof (c, editLineNumberOverflow ());
                  aputc (c, fc);
                }
              while (c != '\n');
            }
          while (++EDIT (lcount) < upto);
          uncache (fe);
        }
    }
}
#endif  /* !large_memory */

void
xpandstring (struct hshentry const *delta)
/* Read a string terminated by `SDELIM' from `FLOW (from)' and write it to
   `FLOW (res)'.  Double `SDELIM' is replaced with single `SDELIM'.  Keyword
   expansion is performed with data from `delta'.  If `FLOW (to)' is
   non-NULL, the string is also copied unchanged to `FLOW (to)'.  */
{
  struct expctx ctx = EXPCTX (FLOW (res), FLOW (to),
                              FLOW (from), true, true);

  while (1 < expandline (&ctx))
    continue;
}

static void
ensure_editstuff (void)
{
  if (! BE (editstuff))
    BE (editstuff) = ZLLOC (1, struct editstuff);
}

void
copystring (void)
/* Copy a string terminated with a single `SDELIM' from `FLOW (from)' to
   `FLOW (res)', replacing all double `SDELIM' with a single `SDELIM'.  If
   `FLOW (to)' is non-NULL, the string also copied unchanged to `FLOW (to)'.
   `EDIT (lcount)' is incremented by the number of lines copied.  Assumption:
   next character read is first string character.  */
{
  register int c;
  declarecache;
  register FILE *frew, *fcop;
  register bool amidline;
  register RILE *fin;

  ensure_editstuff ();

  fin = FLOW (from);
  setupcache (fin);
  cache (fin);
  frew = FLOW (to);
  fcop = FLOW (res);
  amidline = false;
  for (;;)
    {
      GETC (frew, c);
      switch (c)
        {
        case '\n':
          ++EDIT (lcount);
          ++LEX (lno);
          amidline = false;
          break;
        case SDELIM:
          GETC (frew, c);
          if (c != SDELIM)
            {
              /* End of string.  */
              NEXT (c) = c;
              EDIT (lcount) += amidline;
              uncache (fin);
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
enterstring (void)
/* Like `copystring', except the string is
   put into the `edit' data structure.  */
{
  ensure_editstuff ();

#if !large_memory
  EDIT (filename) = NULL;
  EDIT (fedit) = NULL;
  EDIT (lcount) = EDIT (corr) = 0;
  FLOW (result) = maketemp (1);
  if (!(FLOW (res) = fopen_update_truncate (FLOW (result))))
    fatal_sys (FLOW (result));
  copystring ();
#else  /* large_memory */
  register int c;
  declarecache;
  register FILE *frew;
  register long e, oe;
  register bool amidline, oamidline;
  register Iptr_type optr;
  register RILE *fin;

  e = 0;
  EDIT (gap) = 0;
  EDIT (gapsize) = EDIT (lim);
  fin = FLOW (from);
  setupcache (fin);
  cache (fin);
  hey_trundling (false, fin);
  frew = FLOW (to);
  amidline = false;
  for (;;)
    {
      optr = cacheptr ();
      GETC (frew, c);
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
          GETC (frew, c);
          if (c != SDELIM)
            {
              /* End of string.  */
              NEXT (c) = c;
              EDIT (lcount) = e + amidline;
              EDIT (corr) = 0;
              uncache (fin);
              return;
            }
          /* fall into */
        default:
          amidline = true;
          break;
        }
      if (!oamidline)
        insertline (oe, optr);
    }
#endif  /* large_memory */
}

#if large_memory
#define UNUSED_IF_LARGE_MEMORY  RCS_UNUSED
#else
#define UNUSED_IF_LARGE_MEMORY
#endif

void
editstring (struct hshentry const *delta UNUSED_IF_LARGE_MEMORY)
/* Read an edit script from `FLOW (from)' and applies it to the edit file.
#if !large_memory
   The result is written to `FLOW (res)'.
   If `delta', keyword expansion is performed simultaneously.
   If running out of lines in `EDIT (fedit)',
   `EDIT (fedit)' and `FLOW (res)' are swapped.
   `EDIT (filename)' is the name of the file that goes with `EDIT (fedit)'.
#endif
   If `FLOW (to)' is set, the edit script is also copied verbatim
   to `FLOW (to)'.  Assumes that all these files are open.
   `FLOW (result)' is the name of the file that goes with `FLOW (res)'.
   Assumes the next input character from `FLOW (from)' is the first
   character of the edit script.  Resets `NEXT (c)' on exit.  */
{
  int ed;                               /* editor command */
  register int c;
  declarecache;
  register FILE *frew;
#if !large_memory
  register FILE *f;
  long line_lim = LONG_MAX;
  register RILE *fe;
#endif
  register long i;
  register RILE *fin;
#if large_memory
  register long j;
#endif
  struct diffcmd dc;

  EDIT (lcount) += EDIT (corr);
  EDIT (corr) = 0;                         /* correct line number */
  frew = FLOW (to);
  fin = FLOW (from);
  setupcache (fin);
  initdiffcmd (&dc);
  while (0 <= (ed = getdiffcmd (fin, true, frew, &dc)))
#if !large_memory
    if (line_lim <= dc.line1)
      editLineNumberOverflow ();
    else
#endif
    if (!ed)
      {
        copylines (dc.line1 - 1, delta);
        /* Skip over unwanted lines.  */
        i = dc.nlines;
        EDIT (corr) -= i;
        EDIT (lcount) += i;
#if large_memory
        deletelines (EDIT (lcount) + EDIT (corr), i);
#else  /* !large_memory */
        fe = EDIT (fedit);
        do
          {
            /* Skip next line.  */
            do Igeteof (fe, c,
                        {
                          if (i != 1)
                            editLineNumberOverflow ();
                          line_lim = dc.dafter;
                          goto done;
                        });
            while (c != '\n');
          done:
            ;
          }
        while (--i);
#endif  /* !large_memory */
      }
    else
      {
        /* Copy lines without deleting any.  */
        copylines (dc.line1, delta);
        i = dc.nlines;
#if large_memory
        j = EDIT (lcount) + EDIT (corr);
#endif
        EDIT (corr) += i;
#if !large_memory
        f = FLOW (res);
        if (delta)
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
                    editEndsPrematurely ();
                  }
              }
            while (--i);
          }
        else
#endif  /* !large_memory */
          {
            cache (fin);
            do
              {
#if large_memory
                insertline (j++, cacheptr ());
#endif
                for (;;)
                  {
                    GETC (frew, c);
                    if (c == SDELIM)
                      {
                        GETC (frew, c);
                        if (c != SDELIM)
                          {
                            if (--i)
                              editEndsPrematurely ();
                            NEXT (c) = c;
                            uncache (fin);
                            return;
                          }
                      }
#if !large_memory
                    aputc (c, f);
#endif
                    if (c == '\n')
                      break;
                  }
                ++LEX (lno);
              }
            while (--i);
            uncache (fin);
          }
      }
}

static int
naturalize (struct buf *fn, bool *symbolicp)
/* If `fn' is a symbolic link, resolve it to the name that it points to.
   If unsuccessful, set errno and return -1.
   If it points to an existing file, return 1.
   Otherwise, set `errno' to `ENOENT' and return 0.
   On return `*symbolicp' set means the filename was a symlink, and
   `fn' points to the resolved filename (no change if not a symlink).  */
{
  int e;
  ssize_t r;
  int linkcount = _POSIX_SYMLOOP_MAX;
  struct divvy *space = make_space ("nature");
  size_t len = SIZEABLE_PATH;
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
        /* Blech, `readlink' does not NUL-terminate.  */
        chased[r] = '\0';
        if (ROOTPATH (chased))
          {
            fn->string = chased;
            fn->size = r;
          }
        else
          {
            for (char const *p = fn->string; p < basefilename (fn->string); p++)
              accumulate_byte (space, *p);
            accumulate_nonzero_bytes (space, chased);
            fn->string = finish_string (space, &fn->size);
          }
      }
  e = errno;
  if ((*symbolicp = fn->string != orig))
    fn->string = intern (SINGLE, fn->string, fn->size);
  close_space (space);
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

RILE *
rcswriteopen (struct buf *RCSbuf, struct stat *status, bool mustread)
/* Create the lock file corresponding to `RCSbuf'.
   Then try to open `RCSbuf' for reading and return its `RILE*' descriptor.
   Put its status into `*status' too.
   `mustread' is true if the file must already exist, too.
   If all goes well, discard any previously acquired locks,
   and set `REPO (fd_lock)' to the file descriptor of the RCS lockfile.  */
{
  register char *tp;
  register char const *sp, *RCSpath, *x;
  RILE *f;
  size_t l;
  int e, exists, fdesc, fdescSafer, r;
  bool waslocked;
  struct buf *dirt;
  struct stat statbuf;
  bool symbolicp = false;

  ensure_editstuff ();                  /* ugh */
  waslocked = 0 <= REPO (fd_lock);
  exists = naturalize (RCSbuf, &symbolicp);
  if (exists < (mustread | waslocked))
    /* There's an unusual problem with the RCS file; or the RCS file
       doesn't exist, and we must read or we already have a lock
       elsewhere.  */
    return NULL;

  RCSpath = RCSbuf->string;
  sp = basefilename (RCSpath);
  l = sp - RCSpath;
  dirt = &EDIT (sff)[waslocked].filename;
  bufscpy (dirt, RCSpath);
  tp = dirt->string + l;
  x = rcssuffix (RCSpath);
  if (!x && symbolicp)
    {
      PERR ("symbolic link to non RCS file `%s'", RCSpath);
      errno = EINVAL;
      return NULL;
    }
  if (*sp == *x)
    {
      PERR ("RCS pathname `%s' incompatible with suffix `%s'", sp, x);
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
      *tp++ = *x;
      while (*sp)
        *tp++ = *sp++;
      *--tp = '\0';
    }
  else
    {
      /* The suffix is empty.  The lock filename is the RCS filename
         with last char replaced by '_'.  */
      while ((*tp++ = *sp++))
        continue;
      tp -= 2;
      if (*tp == '_')
        {
          PERR ("RCS pathname `%s' ends with `%c'", RCSpath, *tp);
          errno = EINVAL;
          return NULL;
        }
      *tp = '_';
    }

  sp = dirt->string;

  f = NULL;

  /* good news:
     `open (f, O_CREAT|O_EXCL|O_TRUNC|..., OPEN_CREAT_READONLY)'
     is atomic according to POSIX 1003.1-1990.

     bad news:
     NFS ignores O_EXCL and doesn't comply with POSIX 1003.1-1990.

     good news:
     `(O_TRUNC,OPEN_CREAT_READONLY)' normally guarantees atomicity
     even with NFS.

     bad news:
     If you're root, `(O_TRUNC,OPEN_CREAT_READONLY)' doesn't guarantee atomicity.

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
     - server receives, does, and acknowledges A's first `rename'.
     - A receives acknowledgment, and its RCS program exits.
     - server receives, does, and acknowledges B's `create'.
     - server receives, does, and acknowledges A's retry of `rename'.
     This not only wrongly deletes B's lock, it removes the RCS file!
     Most NFS implementations have idempotency caches that usually prevent
     this scenario, but such caches are finite and can be overrun.

     This problem afflicts not only RCS, which uses `open' and `rename'
     to get and release locks; it also afflicts the traditional
     Unix method of using `link' and `unlink' to get and release locks,
     and the less traditional method of using `mkdir' and `rmdir'.
     There is no easy workaround.

     Any new method based on `lockf' seemingly would be incompatible with
     the old methods; besides, `lockf' is notoriously buggy under NFS.
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
  fdesc = create (sp);
  /* Do it now; `setrid' might use stderr.  */
  fdescSafer = fd_safer (fdesc);
  e = errno;
  setrid ();

  if (0 <= fdesc)
    EDIT (sff)[SFFI_LOCKDIR].disposition = effective;

  if (fdescSafer < 0)
    {
      if (e == EACCES && stat (sp, &statbuf) == 0)
        /* The RCS file is busy.  */
        e = EEXIST;
    }
  else
    {
      e = ENOENT;
      if (exists)
        {
          f = Iopen (RCSpath, FOPEN_RB, status);
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
              bufscpy (&EDIT (sff)[SFFI_LOCKDIR].filename, sp);
            }
        }
      REPO (fd_lock) = fdescSafer;
    }

  RESTOREINTS ();

  errno = e;
  return f;
}

void
keepdirtemp (char const *name)
/* Do not unlink `name', either because it's not there any more,
   or because it has already been unlinked.  */
{
  register int i;

  for (i = SFF_COUNT; 0 <= --i;)
    if (EDIT (sff)[i].filename.string == name)
      {
        EDIT (sff)[i].disposition = notmade;
        return;
      }
  PFATAL ("keepdirtemp");
}

char const *
makedirtemp (bool isworkfile)
/* Create a unique pathname and store it into `EDIT (sff)'.  Because of
   storage in `EDIT (sff)', `dirtempunlink' can unlink the file later.
   Return a pointer to the pathname created.
   If `isworkfile', put it into the working file's directory;
   otherwise, put the unique file in RCSfile's directory.  */
{
  int slot = SFFI_NEWDIR + isworkfile;

  ensure_editstuff ();
  set_temporary_file_name (&EDIT (sff)[slot].filename, isworkfile
                           ? MANI (filename)
                           : REPO (filename));
  EDIT (sff)[slot].disposition = real;
  return EDIT (sff)[slot].filename.string;
}

void
dirtempunlink (void)
/* Clean up `makedirtemp' files.
   May be invoked by signal handler.  */
{
  register int i;
  enum maker m;

  if (! BE (editstuff))
    return;

  for (i = SFF_COUNT; 0 <= --i;)
    if ((m = EDIT (sff)[i].disposition) != notmade)
      {
        if (m == effective)
          seteid ();
        un_link (EDIT (sff)[i].filename.string);
        if (m == effective)
          setrid ();
        EDIT (sff)[i].disposition = notmade;
      }
}

int
chnamemod (FILE ** fromp, char const *from, char const *to,
           int set_mode, mode_t mode, time_t mtime)
/* Rename a file (with stream pointer `*fromp') from `from' to `to'.
   `from' already exists.
   If `0 < set_mode', change the mode to `mode', before renaming if possible.
   If `mtime' is not -1, change its mtime to `mtime' before renaming.
   Close and clear `*fromp' before renaming it.
   Unlink `to' if it already exists.
   Return -1 on error (setting `errno'), 0 otherwise.   */
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
     during which `to' does not exist.  */
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
/* Set `file' last modified time to `mtime',
   but do nothing if `mtime' is -1.  */
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
   to the locked delta; also removes the lock if `delete'.
   If one lock, put it into `*target'.
   Return 0 for no locks, 1 for one, 2 for two or more.  */
{
  register struct rcslock *next, **trail, **found;

  found = 0;
  for (trail = &ADMIN (locks); (next = *trail); trail = &next->nextlock)
    if (strcmp (getcaller (), next->login) == 0)
      {
        if (found)
          {
            RERR ("multiple revisions locked by %s; please specify one",
                  getcaller ());
            return 2;
          }
        found = trail;
      }
  if (!found)
    return 0;
  next = *found;
  *target = next->delta;
  if (delete)
    {
      next->delta->lockedby = NULL;
      *found = next->nextlock;
    }
  return 1;
}

int
addlock (struct hshentry *delta, bool verbose)
/* Add a lock held by caller to `delta' and return 1 if successful.
   Print an error message if `verbose' and return -1 if no lock is
   added because `delta' is locked by somebody other than caller.
   Return 0 if the caller already holds the lock.   */
{
  register struct rcslock *next;

  for (next = ADMIN (locks); next; next = next->nextlock)
    if (cmpnum (delta->num, next->delta->num) == 0)
      {
        if (strcmp (getcaller (), next->login) == 0)
          return 0;
        else
          {
            if (verbose)
              RERR ("Revision %s is already locked by %s.",
                    delta->num, next->login);
            return -1;
          }
      }
  next = FALLOC (struct rcslock);
  delta->lockedby = next->login = getcaller ();
  next->delta = delta;
  next->nextlock = ADMIN (locks);
  ADMIN (locks) = next;
  return 1;
}

int
addsymbol (char const *num, char const *name, bool rebind)
/* Associate with revision `num' the new symbolic `name'.
   If `name' already exists and `rebind' is set, associate `name'
   with `num'; otherwise, print an error message and return false;
   Return -1 if unsuccessful, 0 if no change, 1 if change.  */
{
  register struct assoc *next;

  for (next = ADMIN (assocs); next; next = next->nextassoc)
    if (strcmp (name, next->symbol) == 0)
      {
        if (strcmp (next->num, num) == 0)
          return 0;
        else if (rebind)
          {
            next->num = num;
            return 1;
          }
        else
          {
            RERR ("symbolic name %s already bound to %s", name, next->num);
            return -1;
          }
      }
  next = FALLOC (struct assoc);
  next->symbol = name;
  next->num = num;
  next->nextassoc = ADMIN (assocs);
  ADMIN (assocs) = next;
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
  register struct access const *next;

  if (!ADMIN (allowed) || myself (REPO (stat).st_uid)
      || strcmp (getcaller (), "root") == 0)
    return true;

  next = ADMIN (allowed);
  do
    {
      if (strcmp (getcaller (), next->login) == 0)
        return true;
    }
  while ((next = next->nextaccess));

  RERR ("user %s not on the access list", getcaller ());
  return false;
}

int
dorewrite (bool lockflag, int changed)
/* Do nothing if not `lockflag'.
   Prepare to rewrite an RCS file if `changed' is positive.
   Stop rewriting if `changed' is zero, because there won't be any changes.
   Fail if `changed' is negative.
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
          aprintf (FLOW (rewr), "\n\n%s%c", Kdesc, NEXT (c));
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
/* Finish rewriting an RCS file if `changed' is nonzero.
   Set its mode if `changed' is positive.
   Set its modification time to `newRCStime' unless it is -1.
   Return 0 on success, -1 on failure.  */
{
  int r = 0, e = 0;
#if BAD_CREAT0
  int lr, le;
#endif

  if (changed && !LEX (erroneousp))
    {
      if (FLOW (from))
        {
          fastcopy (FLOW (from), FLOW (rewr));
          Izclose (&FLOW (from));
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
/* Like `ORCSclose', except we are cleaning up after an interrupt or
   fatal error.  Do not report errors, since this may loop.  This is
   needed only because some brain-damaged hosts (e.g. OS/2) cannot
   unlink files that are open, and some nearly-POSIX hosts (e.g. NFS)
   work better if the files are closed first.  This isn't a completely
   reliable away to work around brain-damaged hosts, because of the gap
   between actual file opening and setting `FLOW (rewr)' etc., but it's
   better than nothing.  */
{
  if (0 <= REPO (fd_lock))
    close (REPO (fd_lock));
  if (FLOW (rewr))
    /* Avoid `fclose', since stdio may not be reentrant.  */
    close (fileno (FLOW (rewr)));
}

/* rcsedit.c ends here */
