/* merge - three-way file merge

   Copyright (C) 2010 Thien-Thi Nguyen
   Copyright (C) 1991, 1992, 1993, 1994, 1995 Paul Eggert

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

#include "rcsbase.h"

static void badoption (char const *);

static char const usage[] =
  "\nmerge: usage: merge [-AeEpqxX3] [-L lab [-L lab [-L lab]]] file1 file2 file3";

static void
badoption (char const *a)
{
  error ("unknown option: %s%s", a, usage);
}

char const cmdid[] = "merge";

int
main (int argc, char **argv)
{
  register char const *a;
  char const *arg[3], *label[3], *edarg = 0;
  int labels, tostdout;

  labels = 0;
  tostdout = false;

  for (; (a = *++argv) && *a++ == '-'; --argc)
    {
      switch (*a++)
        {
        case 'A':
        case 'E':
        case 'e':
          if (edarg && edarg[1] != (*argv)[1])
            error ("%s and %s are incompatible", edarg, *argv);
          edarg = *argv;
          break;

        case 'p':
          tostdout = true;
          break;
        case 'q':
          quietflag = true;
          break;

        case 'L':
          if (3 <= labels)
            faterror ("too many -L options");
          if (!(label[labels++] = *++argv))
            faterror ("-L needs following argument");
          --argc;
          break;

        case 'V':
          printf ("%s%s", cmdid, COMMAND_VERSION);
          return 0;

        default:
          badoption (a - 2);
          continue;
        }
      if (*a)
        badoption (a - 2);
    }

  if (argc != 4)
    faterror ("%s arguments%s", argc < 4 ? "not enough" : "too many", usage);

  /* This copy keeps us `const'-clean.  */
  arg[0] = argv[0];
  arg[1] = argv[1];
  arg[2] = argv[2];

  for (; labels < 3; labels++)
    label[labels] = arg[labels];

  if (nerror)
    exiterr ();
  return merge (tostdout, edarg, label, arg);
}

void
exiterr (void)
{
  tempunlink ();
  _exit (diff_trouble);
}
