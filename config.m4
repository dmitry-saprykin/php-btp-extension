PHP_ARG_ENABLE(btp, Extension for btp counters,
[  --enable-btp[=DIR]         Include Btp support.])

if test "$PHP_BTP" != "no"; then
  PHP_NEW_EXTENSION(btp, btp.c, $ext_shared,, -DNEWBTP)
fi
