# autogen.sh
#
# Usage: sh autogen.sh
# Run this in the top directory to regenerate all the files.
# NB: You must have gnulib-tool on the PATH.  If you want to
# force gnulib-tool to "--add-import" (rather than "--update"),
# remove either lib/ or m4/ before invocation.

set -ex

# Update the gnulib modules.
#
# The sed script "tee"s the list of (requested and supporting)
# modules to the file .gnulib-utility, slightly formatted for
# inclusion in HACKING, q.v.
gnulib-tool --conditional-dependencies --update \
    | sed '
/^Module list/,/^[A-Z]/!b
/^ /!b
h
s/^/;/
w .gnulib-utility
x
'

autoreconf --install --symlink --force

# These override what ‘autoreconf --install’ creates.
# Another way is to use gnulib's config/srclist-update.
actually ()
{
    gnulib-tool --copy-file $1 $2
}
actually doc/INSTALL.UTF-8 INSTALL
actually build-aux/config.guess
actually build-aux/config.sub
actually build-aux/install-sh
actually build-aux/mdate-sh
actually build-aux/texinfo.tex
actually build-aux/depcomp
actually doc/fdl.texi

# We aren't really interested in the backup files.
rm -f INSTALL~ build-aux/*~

# autogen.sh ends here
