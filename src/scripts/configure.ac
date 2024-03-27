# Numerical representation of Charm++ release line and API revision
# Essentially just the version number stripped of decimal points with minor
#   fields padded to 2 digits each. (e.g. 6.10.1 -> 61001)
# Ensure that this number is always numerically and lexicographically increasing.
#
# Increment the last 2 digits when making a bugfix release
# Increment the second and third digits when making a minor feature release
# Increment the first digit when making a major feature release

AC_INIT([Charm++],[70000],[charm@cs.illinois.edu],[charm],[https://charm.cs.illinois.edu/])

AC_CONFIG_SRCDIR(./Makefile)

AC_CONFIG_HEADERS([conv-autoconfig.h])

AC_CONFIG_COMMANDS([config-cleanup],
                   [sed -i -e 's:^#define\s\+PACKAGE://&:' conv-autoconfig.h])

get_full_command_name()
{
    if test "$(basename "$*")" != "$*"; then
        echo "$(cd "$(dirname "$*")" && pwd)/$(basename "$*")"
    else
        echo "$*"
    fi
}

ConvSh='../tmp/conv-mach-opt.sh'
ConvMak='../tmp/conv-mach-opt.mak'

#clean up conv-mach-opt.sh
sed -e '/Option added by configure/d' "$ConvSh" | sed -e '/^$/d' > conv-mach-opt.sh.clean
cp -f conv-mach-opt.sh.clean   "$ConvSh"
rm -f conv-mach-opt.sh.clean

#clean up conv-mach-opt.mak
sed -e '/Option added by configure/d' "$ConvMak" | sed -e '/^$/d' > conv-mach-opt.mak.clean
cp -f conv-mach-opt.mak.clean   "$ConvMak"
rm -f conv-mach-opt.mak.clean

CHARMINC="."
CHARMBIN="../bin"
test -r ./conv-config.sh && . ./conv-config.sh

CHARM_VERSION=AC_PACKAGE_VERSION
CHARM_VERSION_MAJOR=`echo $CHARM_VERSION | cut -c1`
CHARM_VERSION_MINOR=`echo $CHARM_VERSION | cut -c2-3`
CHARM_VERSION_PATCH=`echo $CHARM_VERSION | cut -c4-5`

if test "$CHARM_VERSION_PATCH" -lt 10 ; then
  CHARM_VERSION_PATCH=${CHARM_VERSION_PATCH#0} # remove leading '0'
fi

echo "#define CHARM_VERSION $CHARM_VERSION" > ../include/charm-version.h
echo "#define CHARM_VERSION_MAJOR $CHARM_VERSION_MAJOR" >> ../include/charm-version.h
echo "#define CHARM_VERSION_MINOR $CHARM_VERSION_MINOR" >> ../include/charm-version.h
echo "#define CHARM_VERSION_PATCH $CHARM_VERSION_PATCH" >> ../include/charm-version.h

charmout="charmconfig.out"
/bin/rm -rf $charmout
MAKEFILE_EXT=Make.extlib

Echo() {
	echo $* 
	echo $* >> $charmout
}

# add into conv-mach-opt.sh
# add_flag $1 $2
# $2 is description
add_flag() {
        key=$1
        key="${key%%=*}"
        last=`grep -w "$key" "$ConvSh" 2>/dev/null | tail -1 | sed -e 's/ *#.*$//'`
        skip=0
	if test -n "$last" -a  "$last" = "$1" 
	then
	  skip=1
        fi
	if test $skip = 0
        then
	  cat >> "$ConvSh" << EOT

$1   # Option added by configure script's $2 section
EOT
	fi
}

# remove_flag $1
remove_flag() {
     sed -e '/^'"$1"'=/d' "$ConvSh" > tmp.$$
     cp tmp.$$ "$ConvSh"
     /bin/rm -f tmp.$$
}

# add into conv-mach-opt.mak
# add_make_flag $1 $2
# $2 is description
add_make_flag() {
        key=$1
        key="${key%%=*}"
        key="${key%%:*}"
        last=`grep -w "$key" "$ConvMak" 2>/dev/null | tail -1 | sed -e 's/ *#.*$//'`
        skip=0
	if test -n "$last" -a  "$last" = "$1"
	then
	  skip=1
        fi
	if test $skip = 0
        then
	  # The lack of whitespace around $1 here is necessary.
	  cat >> "$ConvMak" << EOT

$1# Option added by configure script's $2 section
EOT
	fi
}

# remove_make_flag $1
remove_make_flag() {
     sed -e '/^'"$1"'=/d' "$ConvMak" > tmp.$$
     cp tmp.$$ "$ConvMak"
     /bin/rm -f tmp.$$
}

add_flag "OPTS_CC='$OPTS_CC'"   'Pass through user-provided C compiler options'
add_flag "OPTS_CXX='$OPTS_CXX'" 'Pass through user-provided C++ compiler options'
add_flag "CMK_MACOSX='$CMK_MACOSX'" 'basic setup'
add_flag "CMK_WINDOWS='$CMK_WINDOWS'" 'basic setup'
add_flag "CMK_POST_EXE='$CMK_POST_EXE'" 'basic setup'
add_flag "CMK_SHARED_SUF='$CMK_SHARED_SUF'" 'basic setup'
add_flag "CMK_USER_SUFFIX='$CMK_USER_SUFFIX'" 'basic setup'
add_make_flag "CMK_COMPILER:=$CMK_COMPILER" 'basic setup'
add_make_flag "CMK_SMP:=$CMK_SMP" 'basic setup'
add_make_flag "CMK_SHARED_SUF:=$CMK_SHARED_SUF" 'basic setup'
add_make_flag "CMK_USER_SUFFIX:=$CMK_USER_SUFFIX" 'basic setup'
add_make_flag "CMK_NO_PARTITIONS:=$CMK_NO_PARTITIONS" 'basic setup'
add_make_flag "CMK_MACOSX:=$CMK_MACOSX" 'basic setup'
add_make_flag "CMK_WINDOWS:=$CMK_WINDOWS" 'basic setup'
add_make_flag "CMK_MULTICORE:=$CMK_MULTICORE" 'basic setup'
add_make_flag "CMK_USE_LRTS:=$CMK_USE_LRTS" 'basic setup'
add_make_flag "BUILD_CUDA:=$BUILD_CUDA" 'basic setup'
AC_DEFINE_UNQUOTED(CMK_POST_EXE, $CMK_POST_EXE, [basic setup])
AC_DEFINE_UNQUOTED(CMK_SHARED_SUF, $CMK_SHARED_SUF, [basic setup])
AC_DEFINE_UNQUOTED(CMK_USER_SUFFIX, $CMK_USER_SUFFIX, [basic setup])

# enable/disable error checking
AC_ARG_ENABLE([error-checking],
            [AS_HELP_STRING([--enable-error-checking],
              [enable error checking])], ,
            [enable_error_checking=yes])

if test "$enable_error_checking" = "no"
then
  Echo "Charm++/LIBS error checking is disabled" 
  AC_DEFINE_UNQUOTED(CMK_ERROR_CHECKING, 0, [disable error checking])
  add_flag 'OPTS_CC="$OPTS_CC -U_FORTIFY_SOURCE"' 'error checking'
  add_flag 'OPTS_CXX="$OPTS_CXX -U_FORTIFY_SOURCE"' 'error checking'
else
  Echo "Charm++/LIBS error checking is enabled" 
  AC_DEFINE_UNQUOTED(CMK_ERROR_CHECKING, 1, [enable error checking])
fi

# enable/disable AMPI error checking
AC_ARG_ENABLE([ampi-error-checking],
            [AS_HELP_STRING([--enable-ampi-error-checking],
              [enable AMPI error checking])], ,
            [enable_ampi_error_checking=yes])

if test "$enable_ampi_error_checking" = "no"
then
  Echo "AMPI error checking is disabled"
  AC_DEFINE_UNQUOTED(AMPI_ERROR_CHECKING, 0, [disable ampi error checking])
else
  Echo "AMPI error checking is enabled"
  AC_DEFINE_UNQUOTED(AMPI_ERROR_CHECKING, 1, [enable ampi error checking])
fi

# enable/disable statistics collection
AC_ARG_ENABLE([stats],
            [AS_HELP_STRING([--enable-stats],
              [enable statistics collection])], ,
            [enable_stats=yes])

if test "$enable_stats" = "no"
then
  Echo "Statistics collection is disabled" 
  AC_DEFINE_UNQUOTED(CMK_WITH_STATS, 0, [disable statistics collection])
else
  Echo "Statistics collection is enabled" 
  AC_DEFINE_UNQUOTED(CMK_WITH_STATS, 1, [enable statistics collection])
fi

# check enable/disable
AC_ARG_ENABLE([tracing],
            [AS_HELP_STRING([--enable-tracing],
              [enable tracing modules])], ,
            [enable_tracing=yes])

if test "$enable_tracing" = "no"
then
  Echo "Charm tracing is disabled"
  AC_DEFINE_UNQUOTED(CMK_TRACE_ENABLED, 0, [disable tracing])
  add_flag CMK_TRACE_ENABLED=0
  add_make_flag 'CMK_TRACE_ENABLED:=0' 'tracing'
else
  Echo "Charm tracing is enabled"
  AC_DEFINE_UNQUOTED(CMK_TRACE_ENABLED, 1, [enable tracing])
  add_flag CMK_TRACE_ENABLED=1
  add_make_flag 'CMK_TRACE_ENABLED:=1' 'tracing'
fi

AC_ARG_ENABLE([tls],
            [AS_HELP_STRING([--enable-tls],
              [enable tls support])],
            [enable_tls=$enableval],
            [enable_tls=yes])

AC_ARG_ENABLE([tracing-commthread],
            [AS_HELP_STRING([--enable-tracing-commthread],
              [enable tracing communication thread])], ,
            [enable_tracing_commthread=no])

if test "$enable_tracing_commthread" = "yes"
then
  Echo "Charm tracing communication thread is enabled"
  AC_DEFINE_UNQUOTED(CMK_SMP_TRACE_COMMTHREAD, 1, [enable tracing comm thread])
else
  Echo "Charm tracing communication thread is disabled"
  AC_DEFINE_UNQUOTED(CMK_SMP_TRACE_COMMTHREAD, 0, [disable tracing comm thread])
fi


# enable task queue
AC_ARG_ENABLE([task_queue],
            [AS_HELP_STRING([--enable-task-queue],
              [enable task queue])],
            [enable_task_queue=$enableval],
            [enable_task_queue=no])

if test "$enable_task_queue" = "no"
then
  Echo "Task Queue is disabled"
  AC_DEFINE_UNQUOTED(CMK_TASKQUEUE, 0, [disable task queue])
else
  Echo "Task Queue is enabled"
  AC_DEFINE_UNQUOTED(CMK_TASKQUEUE, 1, [enable task queue])
fi

# enable drone mode
AC_ARG_ENABLE([drone_mode],
            [AS_HELP_STRING([--enable-drone-mode],
              [enable drone mode])],
            [enable_drone_mode=$enableval],
            [enable_drone_mode=no])

if test "$enable_drone_mode" = "no"
then
  Echo "Drone mode is disabled"
  AC_DEFINE_UNQUOTED(CMK_DRONE_MODE, 0, [disable drone mode])
else
  Echo "Drone mode is enabled"
  AC_DEFINE_UNQUOTED(CMK_DRONE_MODE, 1, [enable drone mode])
fi

AC_ARG_ENABLE([ccs],
            [AS_HELP_STRING([--enable-ccs],
              [enable CCS])], ,
            [enable_ccs=yes])

if test "$enable_ccs" = "no" -o "$CMK_CCS_AVAILABLE" = '0'
then
  Echo "CCS is disabled"
  AC_DEFINE_UNQUOTED(CMK_CCS_AVAILABLE, 0, [disable ccs])
  CMK_CCS_AVAILABLE='0'
else
  Echo "CCS is enabled"
  AC_DEFINE_UNQUOTED(CMK_CCS_AVAILABLE, 1, [enable ccs])
fi

AC_ARG_ENABLE([charmdebug],
            [AS_HELP_STRING([--enable-charmdebug],
              [enable charmDebug])], ,
            [enable_charmdebug=yes])

if test "$enable_charmdebug" = "no" -o "$CMK_CCS_AVAILABLE" = '0'
then
  Echo "CharmDebug is disabled" 
  AC_DEFINE_UNQUOTED(CMK_CHARMDEBUG, 0, [disable charmdebug])
  add_flag CMK_CHARMDEBUG=0
  add_make_flag 'CMK_CHARMDEBUG:=0'
else
  Echo "CharmDebug is enabled" 
  AC_DEFINE_UNQUOTED(CMK_CHARMDEBUG, 1, [enable charmdebug])
  add_flag CMK_CHARMDEBUG=1
  add_make_flag 'CMK_CHARMDEBUG:=1'
fi

AC_ARG_ENABLE([replay],
            [AS_HELP_STRING([--enable-replay],
              [enable record/replay])],
            [enable_replay=$enableval],
            [enable_replay=yes])

if test "$enable_replay" = "no"
then
  Echo "Charm record/replay is disabled"
  AC_DEFINE_UNQUOTED(CMK_REPLAYSYSTEM, 0, [disable replay])
else
  if test "$enable_tracing" = "no"
  then
    Echo "Charm record/replay is disabled because tracing is disabled"
    AC_DEFINE_UNQUOTED(CMK_REPLAYSYSTEM, 0, [disable replay])
  else
    Echo "Charm record/replay is enabled"
    AC_DEFINE_UNQUOTED(CMK_REPLAYSYSTEM, 1, [enable replay])
  fi
fi

AC_ARG_ENABLE([controlpoint],
            [AS_HELP_STRING([--enable-controlpoint],
              [enable control point])],
            [enable_controlpoint=$enableval],
            [enable_controlpoint=yes])

if test "$enable_controlpoint" = "yes"
then
  Echo "Charm control point is enabled"
  AC_DEFINE_UNQUOTED(CMK_WITH_CONTROLPOINT, 1, [enable controlpoint])
else
  Echo "Charm control point is disabled"
  AC_DEFINE_UNQUOTED(CMK_WITH_CONTROLPOINT, 0, [disable controlpoint])
fi

AC_ARG_ENABLE([lbuserdata],
            [AS_HELP_STRING([--enable-lbuserdata],
              [enable LB user data])],
            [enable_lbuserdata=$enableval],
            [enable_lbuserdata=no])

if test "$enable_lbuserdata" = "yes"
then
  Echo "Charm LB user data is enabled"
  AC_DEFINE_UNQUOTED(CMK_LB_USER_DATA, 1, [enable lb user data])
else
  Echo "Charm LB user data  is disabled"
  AC_DEFINE_UNQUOTED(CMK_LB_USER_DATA, 0, [disable lb user data])
fi

AC_ARG_ENABLE([lockless-queue],
            [AS_HELP_STRING([--enable-lockless-queue],
              [enable lockless queue for PE local and node queue])],
            [enable_lockless_queue=$enableval],
            [enable_lockless_queue=no])

if test "$enable_lockless_queue" = "no"
then
  Echo "Lockless queue for PE local and node queue is disabled"
  AC_DEFINE_UNQUOTED(CMK_LOCKLESS_QUEUE, 0, [disable lockless queue for pe/node queue])
else
  Echo "Lockless queue for PE local and node queue is enabled"
  AC_DEFINE_UNQUOTED(CMK_LOCKLESS_QUEUE, 1, [enable lockless queue for pe/node queue])
fi


AC_ARG_ENABLE([shrinkexpand],
            [AS_HELP_STRING([--enable-shrinkexpand],
              [enable malleable jobs / shrink expand])],
            [enable_shrinkexpand=$enableval],
            [enable_shrinkexpand=no])

if test "$enable_shrinkexpand" = "yes"
then
  if test "$enable_ccs" = "no"
  then
    Echo "CCS cannot be disabled when enabling shrink-expand"
    test_finish 1
  else
    Echo "Charm shrink expand is enabled - Controlpoint is disabled."
    AC_DEFINE_UNQUOTED(CMK_SHRINK_EXPAND, 1, [enable shrinkexpand])
    AC_DEFINE_UNQUOTED(CMK_WITH_CONTROLPOINT, 0, [disable controlpoint])
  fi
else
  Echo "Charm shrink expand is disabled"
  AC_DEFINE_UNQUOTED(CMK_SHRINK_EXPAND, 0, [disable shrinkexpand])
fi

AC_ARG_ENABLE([charm4py],
            [AS_HELP_STRING([--enable-charm4py],
              [enable charm4py support])],
            [enable_charm4py=$enableval],
            [enable_charm4py=no])

if test "$enable_charm4py" = "yes"
then
  Echo "charm4py support is enabled"
  AC_DEFINE_UNQUOTED(CMK_CHARM4PY, 1, [enable charm4py])
else
  Echo "charm4py support is disabled"
  AC_DEFINE_UNQUOTED(CMK_CHARM4PY, 0, [disable charm4py])
fi

AC_ARG_WITH([numa],
            [AS_HELP_STRING([--with-numa],
              [support memory affinity with NUMA])],
            [],
            [with_numa=no])

WITH_NUMA=no
if test "$with_numa" = "yes"
then
  Echo "Charm NUMA support is builtin"
  WITH_NUMA=yes
fi

AC_ARG_WITH([lbtime-type],
            [AS_HELP_STRING([--with-lbtime-type=type],
                            [load balancing timer type])],
            [], [with_lbtime_type=double])

if test "$with_lbtime_type" = "float" -o "$with_lbtime_type" = "double"
then
  Echo "Setting load balancing timer type as '$with_lbtime_type'"
  AC_DEFINE_UNQUOTED(CMK_LBTIME_TYPE, $with_lbtime_type, [Setting load balancing timer type])
else
  Echo "Invalid type specified for load balancing timer type"
  test_finish 1
fi

AC_DEFINE_UNQUOTED(CMK_LBID_64BIT, 1, [enable 64 bit LB ID])

AC_DEFINE_UNQUOTED(CMK_CKSECTIONINFO_STL, 1, [enable STL CkSectionInfo])

AC_ARG_WITH([qlogic],
            [AS_HELP_STRING([--with-qlogic],
              [QLogic based Infiniband])],[with_qlogic=yes],
            [with_qlogic=no])

if test "$with_qlogic" = "no"
then
  AC_DEFINE_UNQUOTED(QLOGIC, 0, [disable qlogic])
else
  Echo "QLogic based Infiniband"
  AC_DEFINE_UNQUOTED(QLOGIC, 1, [enable qlogic])
fi

#
AC_MSG_CHECKING(machine name)
version=`pwd | awk -F/ '{print $(NF-1)}'`
base_version="$CMK_VDIR"
AC_DEFINE_UNQUOTED(CMK_MACHINE_NAME, "$version",[machine name])
AC_MSG_RESULT($version)
AC_SUBST(base_version)

t="test.cpp"
tc="test.c"

# test result passed in $1
# If the test suceeded, print $3 and set "pass"/clear "fail"
# If the test failed, print $2 and clear "pass"/set "fail"
test_result() {
	if test $1 -eq 0
	then
                AC_MSG_RESULT("$3")
		pass="1"
		fail="0"
	else
                AC_MSG_RESULT("$4")
		pass="0"
		fail="1"
# For debugging the configure script, just "export autoconf_debug=1"
#  to get this verbose data on any failed tests:
		if test ! -z "$autoconf_debug"
		then
			echo "------- test script for $2 failed:"
			cat out
			echo "------- the test program was:"
			cat $t
			echo "-------"
		fi
	fi
}

# Test: tries to compile C file $t (described by $1).
#  If successful, prints $2 and sets $pass/clears $fail
#  If failure, prints $3 and sets $pass/clears $fail
#  additional arguments to c++ compiler are passed as $4
test_cc() {
        AC_MSG_CHECKING("$1")
	echo "### $1" >> $charmout
	cat $tc >> $charmout
	echo $CMK_CC $CMK_CC_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CC -c $tc -o test.o $4 >> $charmout
	$CMK_CC $CMK_CC_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CC -c $tc -o test.o $4 > out 2>&1
	test_result $? "$1" "$2" "$3"
 	strictpass=$pass
	strictfail=$fail
        if test $pass -eq 1
	then 
          if grep -i "warn" out > /dev/null 2>&1 || grep -i "(W)" out > /dev/null 2>&1
	  then 
	    strictpass="0" && strictfail="1"
          fi
        fi
	cat out >> $charmout
	/bin/rm -f out
}

# Test: tries to compile C++ file $t (described by $1).
#  If successful, prints $2 and sets $pass/clears $fail
#  If failure, prints $3 and sets $pass/clears $fail
#  additional arguments to c++ compiler are passed as $4
test_cxx() {
        AC_MSG_CHECKING("$1")
	echo "### $1" >> $charmout
	cat $t >> $charmout
	echo $CMK_CXX $CMK_CXX_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CXX -c $t -o test.o $4 >> $charmout
	$CMK_CXX $CMK_CXX_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CXX -c $t -o test.o $4 > out 2>&1
	test_result $? "$1" "$2" "$3"
 	strictpass=$pass
	strictfail=$fail
        if test $pass -eq 1
	then 
          if grep -i "warn" out > /dev/null 2>&1 || grep -i "(W)" out > /dev/null 2>&1
	  then 
	    strictpass="0" && strictfail="1"
          fi
        fi
	cat out >> $charmout
	/bin/rm -f out
}

# Test: tries to compile C++ file $t (described by $1) using native compiler.
#  If successful, prints $2 and sets $pass/clears $fail
#  If failure, prints $3 and sets $pass/clears $fail
#  additional arguments to c++ compiler are passed as $4
test_native_cxx() {
        AC_MSG_CHECKING("$1")
	echo "### $1" >> $charmout
	cat $t >> $charmout
	echo $CMK_NATIVE_CXX $CMK_NATIVE_CXX_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CXX -c $t -o test.o $4 >> $charmout
	$CMK_NATIVE_CXX $CMK_NATIVE_CXX_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CXX -c $t -o test.o $4 > out 2>&1
	test_result $? "$1" "$2" "$3"
	strictpass=$pass
	strictfail=$fail
        if test $pass -eq 1
	then
          if grep -i "warn" out > /dev/null 2>&1 || grep -i "(W)" out > /dev/null 2>&1
	  then
	    strictpass="0" && strictfail="1"
          fi
        fi
	cat out >> $charmout
	/bin/rm -f out
}

# Testlink: tries to compile and link a C++ file $t (described by $1).
#  If successful, prints $2 and sets $pass/clears $fail
#  If failure, prints $3 and sets $pass/clears $fail
#  additional arguments to c++ compiler are passed as $4
test_link() {
        AC_MSG_CHECKING("$1")
	echo "### $1" >> $charmout
	cat $t >> $charmout
	echo $CMK_CXX $CMK_CXX_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CXX -c $t -o test.o $4 >> $charmout
	$CMK_CXX $CMK_CXX_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CXX -c $t -o test.o $4 > out 2>&1
        if test $? -ne 0
        then
          test_result 1 "$1" "$2" "$3"
        else
          echo $CMK_LDXX $CMK_LDXX_FLAGS $CMK_LINK_BINARY -o testlink test.o $CMK_LIBDIR $OPTS_LD $CMK_SYSLIBS $4 $5 >> $charmout
          $CMK_LDXX $CMK_LDXX_FLAGS $CMK_LINK_BINARY -o testlink test.o $CMK_LIBDIR $OPTS_LD $CMK_SYSLIBS $4 $5 >> out 2>&1
          ret=$?
	  test ! -x testlink && ret=1
          test_result $ret "$1" "$2" "$3"
        fi
 	strictpass=$pass
	strictfail=$fail
        if test $pass -eq 1
	then 
 	  if cat out | grep -i "warn" > /dev/null 2>&1
	  then 
	    strictpass="0" && strictfail="1"
          fi
        fi
	cat out >> $charmout
	/bin/rm -f out
}

# Testlinkc: tries to compile and link a C file $t (described by $1).
#  If successful, prints $2 and sets $pass/clears $fail
#  If failure, prints $3 and sets $pass/clears $fail
#  additional arguments to c++ compiler are passed as $4
test_linkc() {
        AC_MSG_CHECKING("$1")
	echo "### $1" >> $charmout
	cat $tc >> $charmout
	echo $CMK_CC $CMK_CC_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CC -c $tc -o test.o $4 >> $charmout
	$CMK_CC $CMK_CC_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CC -c $tc -o test.o $4 > out 2>&1
        if test $? -ne 0
        then
          test_result 1 "$1" "$2" "$3"
        else
          echo $CMK_LD $CMK_LD_FLAGS $CMK_LINK_BINARY -o testlink test.o $CMK_LIBDIR $OPTS_LD $CMK_SYSLIBS $4 >> $charmout
          $CMK_LD $CMK_LD_FLAGS $CMK_LINK_BINARY -o testlink test.o $CMK_LIBDIR $OPTS_LD $CMK_SYSLIBS $4 >> out 2>&1
          test_result $? "$1" "$2" "$3"
        fi
 	strictpass=$pass
	strictfail=$fail
        if test $pass -eq 1
	then 
 	  if cat out | grep -i "warn" > /dev/null 2>&1
	  then 
	    strictpass="0" && strictfail="1"
          fi
        fi
	cat out >> $charmout
	/bin/rm -f out
}

# test_linkso $1 $2 $3 $4, where
# $1: debug msg
# $2: yes msg
# $3: no msg
# $4: extra link option
test_linkso() {
        AC_MSG_CHECKING("$1")
	echo $1 >> $charmout
	cat $t >> $charmout
	echo $CMK_CXX $CMK_CXX_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CXX $CMK_PIC -c $t -o test.o $4 >> $charmout
	$CMK_CXX $CMK_CXX_FLAGS -I../include -I. $CMK_INCDIR $CMK_SYSINC $OPTS_CXX -c $CMK_PIC $t -o test.o $4 > out 2>&1
        if test $? = 1
        then
          test_result 1 "$1" "$2" "$3"
        else
	  echo $CMK_LD $CMK_LD_FLAGS $CMK_LIBDIR $OPTS_LD $CMK_LD_SHARED $CMK_SYSLIBS test.o -o testlink.$CMK_SHARED_SUF $4 >> $charmout
	  $CMK_LD $CMK_LD_FLAGS $CMK_LIBDIR $OPTS_LD $CMK_LD_SHARED $CMK_SYSLIBS test.o -o testlink.$CMK_SHARED_SUF $4 > out 2>&1
	  test_result $? "$1" "$2" "$3"
	fi
	cat out >> $charmout
	/bin/rm -f out testlink.$CMK_SHARED_SUF
}

test_finish() {
	rm -f $t $tc testlink test.o	> /dev/null 2>&1
	/bin/rm -f out
	test $1 -eq 1 && echo "*** Please find detailed output in tmp/charmconfig.out ***"
	exit $1
}

# detect OS
OSNAME=`uname -s`
if test $OSNAME = "Linux"
then
  AC_DEFINE_UNQUOTED(CMK_OS_IS_LINUX, 1, [OS is Linux])
else
  AC_DEFINE_UNQUOTED(CMK_OS_IS_LINUX, 0, [OS is Linux])
fi

# test cp -p
AC_MSG_CHECKING("cp command as")
CP="cp -p"
touch test_file
if ! $CP test_file test_file.cp 2>err
then
  CP="cp"
fi
rm -f test_file test_file.cp
AC_MSG_RESULT($CP)
add_flag CP=\""$CP"\" "cp command"
 

cat > $t <<EOT
#include <stdio.h>
void foo(void) {
	printf("Hello, world!\n");
}
EOT

if test "$CMK_BUILD_CRAY" = "1"
then
#echo "Test for known incompatible compiler versions"

 if test "$CRAY_CC_VERSION" = "8.1.4"
 then
	echo "CCE 8.1.4 produces incorrect Charm++ code"
	echo "Please use a newer version of the CCE compiler"
	echo "e.g. module load cce/8.1.7"
	test_finish 1
 fi
fi

#echo "set C++ compiler as: $CMK_CXX $OPTS_CXX $OPTS"
AC_MSG_CHECKING("C++ compiler as")
AC_MSG_RESULT("$CMK_CXX $OPTS_CXX")
test_cxx "whether C++ compiler works" "ok" "no" ""
if test $fail -eq 1
then
	echo "Cannot compile C++ programs with $CMK_CXX"
	echo " (check your charm++ version)"
	test_finish 1
fi

cat > $t <<EOT
#include <stdio.h>
int main() {
	printf("Hello, world!\n");
	return 0;
}
EOT
#echo "set C++ linker as: $CMK_LDXX $OPTS_LD"
AC_MSG_CHECKING("C++ linker as")
AC_MSG_RESULT("$CMK_LDXX $OPTS_LD")
test_link "whether linker works" "ok" "no" ""
if test $fail -eq 1
then
	echo "Cannot link C++ programs with $CMK_LDXX"
	echo " (check your charm++ version)"
	test_finish 1
fi

AC_MSG_CHECKING("Native C++ compiler as")
AC_MSG_RESULT("$CMK_NATIVE_CXX")
AC_MSG_CHECKING("Sequential C++ compiler as")
AC_MSG_RESULT("$CMK_SEQ_CXX")

# Needed to avoid migratable threads failing the stack check
# See https://github.com/UIUC-PPL/charm/pull/3174 for details.
test_link "whether compiler accepts -fno-stack-protector" "ok" "no" "-fno-stack-protector"
if test $strictpass -eq 1
then
  add_flag OPTS_CC='"$OPTS_CC -fno-stack-protector"' "stack-protection disabling"
  add_flag OPTS_CXX='"$OPTS_CXX -fno-stack-protector"' "stack-protection disabling"
fi

#### check if C++ compiler will accept C++11 features without warning ####
cat > $t <<EOT
// Check for Intel compiler incompatibility with the active g++/libstdc++ by
// including an arbitrary standard library header (cf bug #1560)
#include <map>

// Check for an excessively old g++/libstdc++ that can't handle features we use
#include <memory>
#include <vector>
std::unique_ptr<int> i;

class CkMigrateMessage;

struct base {
  base(void) { }
  base(CkMigrateMessage *) { }
};

template <class Parent>
struct CBaseT1 : Parent {
  std::vector<int> v; // check for C++11's shrink_to_fit()

  CBaseT1(void) :Parent()  { v.shrink_to_fit(); }
  CBaseT1(CkMigrateMessage *m) :Parent(m) { }
  CBaseT1(CBaseT1&& rhs) :Parent() { }

  template <typename... Args>
    CBaseT1(Args... args) : Parent(args...) { }
};

template struct CBaseT1<base>;
EOT
got_cpp11=""
test_cxx "whether C++ compiler supports C++11 without flags" "yes" "no" ""
if test $strictpass -eq 1
then
  got_cpp11="true"
else
# Flags for g++/clang++/icpc/xlC++, pgCC, and CrayCC respectively
for i in "-std=c++11" "--c++11" "-h std=c++11" "-hstd=c++11"; do
    test_cxx "whether C++ compiler supports C++11 with '$i'" "yes" "no" "$i"
    if test $strictpass -eq 1
    then
      add_flag "$(echo CMK_CXX_FLAGS=\"\$CMK_CXX_FLAGS $i\")" "Enable C++11 support"
      CMK_CXX_FLAGS="$CMK_CXX_FLAGS $i"
      got_cpp11="true"
      break
    fi
done
fi

if test -z $got_cpp11
then
  echo "Charm++ requires C++11 support, but doesn't know the flag to enable it"
  echo
  echo "For Intel's compiler please see"
  echo "https://github.com/UIUC-PPL/charm/issues/1560"
  echo "about making a suitable version of gcc/g++/libstdc++ available"
  test_finish 1
fi

test_native_cxx "whether native C++ compiler supports C++11 without flags" "yes" "no" ""
if test $strictpass -ne 1
then
  # Flags for g++/clang++/icpc/xlC++, pgCC, and CrayCC respectively
  for i in "-std=c++11" "--c++11" "-h std=c++11" "-hstd=c++11"; do
    test_native_cxx "whether C++ compiler supports C++11 with '$i'" "yes" "no" "$i"
    if test $strictpass -eq 1
    then
      add_flag "$(echo CMK_NATIVE_CXX_FLAGS=\"\$CMK_NATIVE_CXX_FLAGS $i\")" "Enable C++11 support"
      break
    fi
done
fi

# Workaround for bug #1045 appearing in GCC >6.x
test_cxx "whether C++ compiler accepts -fno-lifetime-dse" "yes" "no" "-fno-lifetime-dse"
if test $strictpass -eq 1
then
    add_flag "$(echo OPTS_CXX=\"\$OPTS_CXX -fno-lifetime-dse\")" "Disable 'Lifetime DSE' optimization to work around bug #1045 in GCC >6.x"
    OPTS_CXX="$OPTS_CXX -fno-lifetime-dse"
fi

# Test for a flag tlsglobals sometimes depends on
test_cxx "whether C++ compiler accepts -mno-tls-direct-seg-refs" "yes" "no" "-mno-tls-direct-seg-refs"
CMK_COMPILER_KNOWS_TLSDIRECTSEGREFS="$strictpass"
if test $strictpass -eq 1
then
    add_flag 'CMK_COMPILER_KNOWS_TLSDIRECTSEGREFS="1"' "tlsglobals"
fi

# Needed so that tlsglobals works correctly with --build-shared
# See https://github.com/UIUC-PPL/charm/issues/3168 for details.
test_cxx "whether C++ compiler accepts -ftls-model=initial-exec" "yes" "no" "-ftls-model=initial-exec"
if test $strictpass -eq 1
then
    if test "$enable_tls" = "yes"
    then
      add_flag 'CMK_COMPILER_KNOWS_FTLS_MODEL="1"' "tlsglobals"
      OPTS_CC="$OPTS_CC -ftls-model=initial-exec"
      OPTS_CXX="$OPTS_CXX -ftls-model=initial-exec"
      OPTS_LD="$OPTS_LD -ftls-model=initial-exec"
    fi
fi

# Test for a flag important for shared linking
test_cxx "whether C++ compiler accepts -fvisibility=hidden" "yes" "no" "-fvisibility=hidden"
if test $strictpass -eq 1
then
    add_flag 'CMK_COMPILER_KNOWS_FVISIBILITY="1"' "-fvisibility=hidden"
fi

# Determine compiler/linker flags to build libcharm.so for charm4py
if test "$enable_charm4py" = "yes"
then

  cat > $t <<EOT
int main() { return 0; }
EOT

  test_cxx "whether C++ compiler accepts --no-as-needed" "yes" "no" "--no-as-needed"
  if test $strictpass -eq 1
  then
      add_flag "$(echo CXX_NO_AS_NEEDED=\"--no-as-needed\")" "--no-as-needed flag necessary for compilers that default to linking with --as-needed"
  fi

  got_opt_whole_archive=""
  # Flags for g++/clang++/icpc, Apple-LLVM respectively
  for i in "-Wl,--whole-archive -Wl,--no-whole-archive" "-Wl,-all_load"; do
      test_link "whether linker supports '$i'" "yes" "no" "" "$i"
      if test $strictpass -eq 1
      then
        got_opt_whole_archive="true"
        IFS=' ' read LDXX_WHOLE_ARCHIVE_PRE LDXX_WHOLE_ARCHIVE_POST <<EOF
        $i
EOF
        add_flag "$(echo LDXX_WHOLE_ARCHIVE_PRE=\"$LDXX_WHOLE_ARCHIVE_PRE\")" "Flags to link whole archives into libcharm.so"
        add_flag "$(echo LDXX_WHOLE_ARCHIVE_POST=\"$LDXX_WHOLE_ARCHIVE_POST\")" "Flags to link whole archives into libcharm.so"
        break
      fi
  done

  if test -z $got_opt_whole_archive
  then
    echo "Don't know how to build libcharm.so for Charm4py"
    test_finish 1
  fi
fi

# Figure out if the user has asked to enable the latest language standards
USER_ASKED_FOR_NEW_STD=`echo "$BUILDOPTS" | grep "\-use-new-std" | wc -l`

# If the user has asked for the newer standards, check if the compilers know about them
if test $USER_ASKED_FOR_NEW_STD -ge 1
then

#### check if c compiler supports c11 compilation flags ####
cat > $tc <<EOT
#include <stdio.h>

int foo()
{
  return 0;
}
EOT
test_cc "whether c compiler knows of the c11 standard" "ok" "no" "$CMK_ENABLE_C11"
AC_DEFINE_UNQUOTED(CMK_COMPILER_KNOWS_C11, $pass, [whether c compiler knows of the c11 standard] )
if test $pass -eq 1
then
        # Record results for charmc's future use
        add_flag 'CMK_COMPILER_KNOWS_C11="1"' "c11"
        # For the rest of configure, append this to compilation flags
        OPTS_CC="$CMK_ENABLE_C11 $OPTS_CC"
fi
fi # endif USER_ASKED_FOR_NEW_STD

# Perform the tests

AC_ARG_WITH([refnum-type],
            [AS_HELP_STRING([--with-refnum-type=type],
                            [size of the envelope refnum field])],
            [], [with_refnum_type=no])

if test "$with_refnum_type" = "no" -o "$with_refnum_type" = "short"
then
  Echo "Setting charm++ envelope refnum field to unsigned short"
  AC_DEFINE_UNQUOTED(CMK_REFNUM_TYPE, unsigned short, [envelope refnum field set to UShort])
elif test "$with_refnum_type" = "int"
then
  Echo "Setting charm++ envelope refnum field to unsigned int"
  AC_DEFINE_UNQUOTED(CMK_REFNUM_TYPE, unsigned int, [envelope refnum field set to UInt])
else
  Echo "Invalid size specified for refnum field"
  test_finish 1
fi


AC_ARG_WITH([prio-type],
            [AS_HELP_STRING([--with-prio-type=type],
                            [size of expected message priorities])],
            [], [with_prio_type=bitvec])

if test "$with_prio_type" = "char" -o "$with_prio_type" = "short" -o "$with_prio_type" = "int" -o "$with_prio_type" = "long" -o "$with_prio_type" = "float" -o "$with_prio_type" = "double"
then
  Echo "Configuring support for message priorities of sizeof type $with_prio_type"
  AC_DEFINE_UNQUOTED(CMK_USE_STL_MSGQ, 1, [whether charm scheduler should use an STL-based msg q])
  AC_DEFINE_UNQUOTED(CMK_MSG_PRIO_TYPE, $with_prio_type, [expected message priorities are sizeof $with_prio_type])
elif test "$with_prio_type" = "bitvec"
then
  Echo "Configuring support for message priorities of arbitrary size (bitvectors)"
  AC_DEFINE_UNQUOTED(CMK_USE_STL_MSGQ, 0, [whether charm scheduler should use an STL-based msg q])
  AC_DEFINE_UNQUOTED(CMK_MSG_PRIO_TYPE, $with_prio_type, [expected message priorities are arbitrarily sized])
else
  Echo "Invalid size ($with_prio_type) specified for message priorities. Can only accept char, short, int, long, float, double and bitvec"
  test_finish 1
fi

# enable/disable randomized scheduler queue
AC_ARG_ENABLE([randomized-msgq],
            [AS_HELP_STRING([--enable-randomized-msgq],
              [enable a randomized msg queue (for debugging etc)])], ,
            [enable_randomized_msgq=no])

if test "$enable_randomized_msgq" = "yes" -a "$with_prio_type" != "bitvec"
then
  Echo "The charm message queue will be randomized (and will not respect priorities)"
  AC_DEFINE_UNQUOTED(CMK_RANDOMIZED_MSGQ, 1, [enable the randomized msgq in the scheduler])
else
  AC_DEFINE_UNQUOTED(CMK_RANDOMIZED_MSGQ, 0, [disable the randomized msgq in the scheduler])
  if test "$enable_randomized_msgq" = "yes"
  then
    Echo 'A randomized message queue is only available when --with-prio-type != bitvec.'
    Echo "Specify prio-type to be a data type long enough to hold the message priorities in your application"
    Echo "for example: --with-prio-type=int (or short / long etc)."
    test_finish 1
  fi
fi

AC_ARG_WITH([mempool-cutoff],
            [AS_HELP_STRING([--with-mempool-cutoff=N],
                            [exponent of the maximum power of two to use for bin sizes in the mempool])],
            [], [with_mempool_cutoff=26])

if test "$((6 < $with_mempool_cutoff && $with_mempool_cutoff < 32))" = '1'
then
  Echo "Using mempool cutoff... 2^$with_mempool_cutoff"
  AC_DEFINE_UNQUOTED(CMK_MEMPOOL_CUTOFFNUM, $((with_mempool_cutoff - 6)), [mempool cutoff])
else
  Echo "Invalid number ($with_mempool_cutoff) specified for mempool cutoff. Valid range: 7 <= N <= 31"
  test_finish 1
fi

# enable mpich tests
AC_ARG_ENABLE([ampi-mpich-tests],
            [AS_HELP_STRING([--enable-ampi-mpich-tests],
            [enable mpich tests for ampi])],
            [enable_ampi_mpich_tests=yes],
            [enable_ampi_mpich_tests=no])

if test "$enable_ampi_mpich_tests" = "yes"
then
  AC_DEFINE([AMPI_ERRHANDLER_RETURN], [1], [enable ampi fatal error return])
  add_make_flag "BUILD_MPICH_TESTS:=true" 'mpich tests setup'
else
  AC_DEFINE([AMPI_ERRHANDLER_RETURN], [0], [disable ampi fatal error return])
fi

#### Check if compiler is 64 bit ####
cat > $t <<EOT
#include <stdio.h>

int foo()
{
int x[[(int)(sizeof(void *) - 7)]]={0};
return x[[0]];
}
EOT
test_cxx "whether compiler generates code for 64-bit" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_SIZET_64BIT, $strictpass, [whether size_t 64bit])
in64bit=$strictpass

### Check if compiler supports std::is_constructible<> ###
cat > $t <<EOT
#include <type_traits>

struct s {
       s(int a) { }
};

bool foo()
{
  return std::is_constructible<s, int>::value;
}
EOT
test_cxx "whether compiler supports std::is_constructible" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_IS_CONSTRUCTIBLE, $strictpass, [whether compiler supports std::is_constructible])

### Check if compiler supports std::alignment_of<> ###
cat > $t <<EOT
#include <type_traits>

int foo()
{
  return std::alignment_of<int>::value;
}
EOT
test_cxx "whether compiler supports std::alignment_of" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_ALIGNMENT_OF, $strictpass, [whether compiler supports std::alignment_of])

### Check if compiler implements regex ###
cat > $t <<EOT
#include <regex>
void foo()
{
#if __cplusplus >= 201103L &&                             \
    (!defined(__GLIBCXX__) || (__cplusplus >= 201402L) || \
        (defined(_GLIBCXX_REGEX_DFS_QUANTIFIERS_LIMIT) || \
         defined(_GLIBCXX_REGEX_STATE_LIMIT)           || \
             (defined(_GLIBCXX_RELEASE)                && \
             _GLIBCXX_RELEASE > 4)))
// compiler has regex support, continue
#else
// force compilation to fail
static_assert(false, "compiler has no regex implementation");
#endif
}
EOT
test_cxx "whether compiler implements regex" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_REGEX, $pass, [whether compiler implements regex])

#### test if has values.h ####
cat > $t <<EOT
#include <values.h>
int main() { 
  double d = MAXDOUBLE;
  return 0;
}
EOT
test_cxx "whether has values.h " "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_VALUES_H, $pass, [whether has values.h])

#### test if has stdint.h ####
cat > $t <<EOT
#include <stdint.h>
int main() { 
  return 0;
}
EOT
test_cxx "whether has stdint.h " "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_STDINT_H, $pass, [whether has stdint.h])

#### test if has malloc.h ####
cat > $t <<EOT
#include <malloc.h>
int main() { 
  return 0;
}
EOT
test_cxx "whether has malloc.h " "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_MALLOC_H, $pass, [whether has malloc.h])

#### test if has strings.h ####
cat > $t <<EOT
#include <strings.h>
int main() {
  return 0;
}
EOT
test_cxx "whether has strings.h " "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_STRINGS_H, $pass, [whether has strings.h])

#### test if has alloca.h ####
cat > $t <<EOT
#include <alloca.h>
int main() {
  double *s=(double *)alloca(sizeof(double));
  *s=1.0;
  return 0;
}
EOT
test_cxx "whether has alloca.h " "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_ALLOCA_H, $pass, [whether has alloca.h])

#### test if has regex.h ####
cat > $t <<EOT
#include <regex.h>
int main() {
  regex_t re;
  regcomp(&re, ".*MOD.*", REG_EXTENDED|REG_NOSUB);
  return 0;
}
EOT
test_cxx "whether has regex.h " "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_REGEX_H, $pass, [whether has regex.h])

#### Check long long ####
cat > $t <<EOT
#include <stdlib.h>
long long foo(void) { return 17; }
EOT
test_cxx "whether long long works" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_LONG_LONG_DEFINED, $pass, [whether long long works])

#### Check __int64 ####
cat > $t <<EOT
#include <stdlib.h>
__int64 foo(void) { return 17; }
EOT
test_cxx "whether __int64 works" "yes" "no" ""
AC_DEFINE_UNQUOTED([CMK___int64_DEFINED], $pass, [whether __int64 works])


CMK_HAS_INT16=0

#### Check __int128 ####
cat > $t <<EOT
#include <stdlib.h>
int foo(void) {
  __int128   a;
  int x[[(int)(sizeof(__int128) - 15)]]={0};
  return x[[0]];
}
EOT
test_cxx "whether __int128 (128-bit integer) works" "yes" "no" ""
AC_DEFINE_UNQUOTED([CMK___int128_DEFINED], $pass, [whether __int128 works])
if test $pass -eq 1
then
  CMK_HAS_INT16=1
  add_flag CMK_HAS_INT16=1
fi

#### Check __int128_t ####
cat > $t <<EOT
#include <stdlib.h>
int foo(void) {
  __int128_t   a;
  __uint128_t   b;
  a = a + a;
  int x[[(int)(sizeof(__int128_t) - 15)]]={0};
  return x[[0]];
}
EOT
test_cxx "whether __int128_t (128-bit integer) works" "yes" "no" ""
AC_DEFINE_UNQUOTED([CMK___int128_t_DEFINED], $pass, [whether __int128_t works])
if test $pass -eq 1
then
  CMK_HAS_INT16=1
  add_flag CMK_HAS_INT16=1
fi

#### Summarize *int128* ####

AC_DEFINE_UNQUOTED([CMK_HAS_INT16], $CMK_HAS_INT16, [whether any 128-bit integer works])


### Check if we have C++11 <atomic> ###
cat > $t <<EOT
#include <atomic>
int main(int argc, char** argv) { 
  return 0;
}
EOT
test_cxx "whether C++ library has <atomic> " "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_CXX11_ATOMIC, $pass, [whether C++ library has <atomic>])

if test $pass -ne 1
then
### Check if we have pre-C++11 <cstdatomic> ###
cat > $t <<EOT
#include <cstdatomic>
int main(int argc, char** argv) { 
  return 0;
}
EOT
test_cxx "whether C++ library has <cstdatomic> " "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_CXX0X_CSTDATOMIC, $pass, [whether C++ library has <cstdatomic>])
fi

if test $pass -ne 1
then
	echo "Charm++ requires C++11 atomic support"
	test_finish 1
fi

#### Check long double ####
cat > $t <<EOT
#include <stdlib.h>
long double foo(void) { return 17.0; }
EOT
test_cxx "whether long double works" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_LONG_DOUBLE_DEFINED, $strictpass, [whether long double works])

#### Check ucontext and FPU pointer ####
cat > $t <<EOT
#include <ucontext.h>
struct _libc_fpstate   fpstate;
fpregset_t *fp;
int main() {
  ucontext_t context;
  context.uc_mcontext.fpregs = 0;
}
EOT
test_cxx "whether ucontext has FPU pointer" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_CONTEXT_FPU_POINTER, $pass, [whether ucontext has pointer])

if test $pass -eq 1
then
cat > $t <<EOT
#include <ucontext.h>

int main()
{
  ucontext_t context;
  context.uc_mcontext.uc_regs = 0;
}
EOT
test_cxx "whether ucontext uses uc_regs" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_CONTEXT_FPU_POINTER_UCREGS, $pass, [whether ucontext uses uc_regs union])
fi

cat > $t <<EOT
#include <ucontext.h>
vrregset_t *v_regs;
ucontext_t  uc;

void foo()
{
  vrregset_t *ptr = uc.uc_mcontext.v_regs;
}
EOT
test_cxx "whether ucontext has pointer (v_regs) of vector type" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_CONTEXT_V_REGS, $pass, [whether ucontext has pointer (v_regs) of vector type])

#### Check ucontext and FPU pointer ####
cat > $t <<EOT
#include <infiniband/verbs.h>
void test()
{
    struct ibv_context    *context;
    int ibPort;
    struct ibv_port_attr attr;
    if (ibv_query_port(context, ibPort, &attr) != 0) return;
    if (attr.link_layer == IBV_LINK_LAYER_INFINIBAND)  return;
}
EOT
test_cxx "whether ibverbs ibv_port_attr has link_layer field" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_IBV_PORT_ATTR_HAS_LINK_LAYER, $pass, [whether ibv_port_attr has link_layer field])


###################### C++ Compiler Features #####################

#### check C inline ####
cat > $tc <<EOT
inline static int foo()
{
  return 1;
}
EOT
test_cc "whether inline works in C" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_C_INLINE, $strictpass, [whether C inline works in C])

#### check if signed char is same as char ####
cat > $t <<EOT
#include <stdlib.h>
class er {
 protected:
   void operator()(char &v,const char *desc=NULL) {};
   void operator()(signed char &v,const char *desc=NULL) {};
};
EOT
test_cxx "whether C++ signed char and char differ" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_SIGNEDCHAR_DIFF_CHAR, $pass, [whether C++ signed char and char differ])

#### check if typeinfo exists and works #####
cat > $t <<EOT
#include <typeinfo>
const char *foo(void) {
	int x;
	return typeid(x).name();
}
EOT
test_cxx "whether typeinfo/typeid works" "ok" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_TYPEINFO, $pass, [whether typeinfo/typeid works])

#### check if iterator_traits is defined #####
cat > $t <<EOT
#include <iterator>

template <typename T> // T models Input Iterator
typename std::iterator_traits<T>::value_type accumulate(T first, T last)
{
      typename std::iterator_traits<T>::value_type result = 0;
      while(first != last)
            result += *first++;
      return result;
}
EOT
test_cxx "whether std::iterator_traits is defined" "ok" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_ITERATOR_TRAITS, $pass, [whether iterator_traits works])

#### check if std::distance is defined #####
cat > $t <<EOT
#include <vector>
#include <iterator>

int foo()
{
  std::vector<int> tree;
  return std::distance(tree.begin(), tree.end());
}
EOT
test_cxx "whether std::distance is defined" "ok" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_STD_DISTANCE, $pass, [whether std::distance works])

#### check if std::inserter is defined #####
cat > $t <<EOT
#include <list>
#include <iterator>

void foo()
{
  using namespace std;
  list<int> L;
  inserter ( L, L.end ( ) ) = 500;
}
EOT
test_cxx "whether std::inserter is defined" "ok" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_STD_INSERTER, $pass, [whether std::inserter works])

#### check if C++17's std::void_t is defined #####
cat > $t <<EOT
#include <type_traits>

template<typename T, typename = std::void_t<>>
struct has_pup_member : std::false_type {};

EOT
test_cxx "whether std::void_t is defined" "ok" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_STD_VOID_T, $pass, [whether std::void_t works])

####### Check support for features added by the new C11 and CPP11 standards ########

#### test if we can have multiple delete like this ####
cat > $t <<EOT
class foo {
public:
  void operator delete(void*p){};
  void operator delete(void*p,int*){};
};
EOT
test_cxx "whether operator delete can be overloaded in same class" "ok" "no" ""
AC_DEFINE_UNQUOTED(CMK_MULTIPLE_DELETE, $strictpass, [whether operator delete can be overloaded in same class])

#### test if offsetof exists ####
cat > $t <<EOT
#include <stddef.h>

struct FooType {
        int f1;
        int f2;
        double f3;
};

void foo()
{
  int off = offsetof(FooType, f2);
}
EOT
test_cxx "whether offsetof is defined" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_OFFSETOF, $pass, [whether offsetof exists])

#### test if we can use gcc x86 assembly like this ####
cat > $t <<EOT
double foo(void)
{
  unsigned long long int v=0;
  int *lo=0+(int *)&v;
  int *hi=1+(int *)&v;
  __asm__ __volatile__(
      "rdtsc; movl %%edx,%0; movl %%eax,%1"
      : /* output  */ "=m" (*hi), "=m" (*lo)
      : /* input */
      : /* trashes */ "%edx", "%eax"
  );
  return v;
}
EOT
test_cxx "whether GCC x86 assembly works" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_GCC_X86_ASM, $strictpass, [Allows gcc x86 assembly.])

#### test if we can use gcc x86 assembly like this ####
if test $strictpass = 1
then
cat > $t <<EOT
int foo(void)
{
  int x;
  asm("lock incl %0" :: "m" (x));
  asm("lock decl %0" :: "m" (x));
  return x;
}
EOT
test_cxx "whether GCC x86 assembly for atomic increment works" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_GCC_X86_ASM_ATOMICINCREMENT, $strictpass, [Allows gcc x86 assembly for atomic increment.])
fi

#### test if we can use asm eieio assembly like this ####
cat > $t <<EOT
double foo(void)
{
        unsigned long long int x;
	asm volatile("eieio" ::: "memory");
        return x;
}
EOT
test_cxx "whether asm eieio assembly works" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_PPC_ASM, $strictpass, [Allows asm eieio assembly.])



#### test if we can use __thread ####
cat > $t <<EOT
__thread unsigned long long int x;
static __thread  int y;
void foo(void)
{
	x = 1;
	y = 1;
}
EOT
test_cxx "whether __thread (Thread Local Storage) is supported" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_TLS_VARIABLES, $strictpass, [Allows __thread.])

#### check __sync_add_and_fetch ####
cat > $tc <<EOT
#include <stdio.h>
int main()
{
  int t=1;
  __sync_add_and_fetch(&t, 1);
  return 1;
}
EOT
test_linkc "whether synchronization primitives (__sync_add_and_fetch) works in C" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_C_SYNC_ADD_AND_FETCH_PRIMITIVE, $pass, [whether sync_add_and_fetch primitive works in C])

#### check __sync_synchronize ####
cat > $tc <<EOT
#include <stdio.h>
int main()
{
  __sync_synchronize();
}
EOT

test_linkc "whether synchronization primitives (__sync_synchronize) works in C" "yes" "no" ""

AC_DEFINE_UNQUOTED(CMK_C_SYNC_SYNCHRONIZE_PRIMITIVE, $pass, [whether sync_synchronize primitives works in C])


### test for _IO_file_overflow ###

cat > $tc <<EOT
#include <stdio.h>
extern int _IO_file_overflow(FILE *, int);
int main()
{
  return _IO_file_overflow(stdout, -1);
}
EOT
test_linkc "whether has _IO_file_overflow" "yes" "no" ""
AC_DEFINE_UNQUOTED([CMK_HAS_IO_FILE_OVERFLOW], $pass, [whether has _IO_file_overflow])


### test for __executable_start ###

cat > $tc <<EOT
extern int __executable_start;
int main()
{
  return __executable_start;
}
EOT
test_linkc "whether has __executable_start" "yes" "no" ""
AC_DEFINE_UNQUOTED([CMK_HAS_EXECUTABLE_START], $pass, [whether has __executable_start])

### test for dl_iterate_phdr ###

cat > $tc <<EOT
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#ifndef __USE_GNU
# define __USE_GNU
#endif
#include <link.h>
#include <stddef.h>
static int callback(struct dl_phdr_info* info, size_t size, void* data)
{
  return 0;
}
int main()
{
  dl_iterate_phdr(callback, NULL);
  return 0;
}
EOT
test_linkc "whether has dl_iterate_phdr" "yes" "no" ""
CMK_HAS_DL_ITERATE_PHDR="$pass"
AC_DEFINE_UNQUOTED([CMK_HAS_DL_ITERATE_PHDR], $pass, [whether has dl_iterate_phdr])


### test for __malloc_hook ###

cat > $tc <<EOT
extern int __malloc_hook;
int main()
{
  return __malloc_hook;
}
EOT
test_linkc "whether has __malloc_hook" "yes" "no" ""
AC_DEFINE_UNQUOTED([CMK_HAS_MALLOC_HOOK], $pass, [whether has __malloc_hook])
add_make_flag "CMK_HAS_MALLOC_HOOK:=$pass" 'whether has __malloc_hook'


#### test if we can build OFI ####
if test "$CMK_BUILD_OFI" = 1
then
    CMK_LIBFABRIC_INC=`pkg-config --cflags libfabric`
    if test "$?" = 0
    then
        add_flag CMK_INCDIR='"$CMK_INCDIR $CMK_LIBFABRIC_INC"' "libfabric inc"
	CMK_INCDIR="$CMK_INCDIR $CMK_LIBFABRIC_INC"
    fi
    CMK_LIBFABRIC_LIBS=`pkg-config --libs libfabric`
    if test "$?" = 0
    then
	add_flag CMK_LIBCDIR='"$CMK_LIBDIR $CMK_LIBFABRIC_LIBONLY"' "libfabric lib"
	CMK_LIBDIR="$CMK_LIBDIR $CMK_LIBFABRIC_LIB"
    fi
cat > $tc <<EOT
#include <rdma/fabric.h>
int main(int argc, char **argv)
{
  struct fi_info *providers;
  int ret = fi_getinfo(FI_VERSION(1,0), NULL, NULL, 0ULL, NULL, &providers);
  return 0;
}
EOT
test_cc "whether build on OFI" "yes" "no"
AC_DEFINE_UNQUOTED(CMK_BUILD_ON_OFI, $strictpass, [build OFI.])
BUILD_OFI=$strictpass

if test $BUILD_OFI -eq 0
then
  echo "Error: Unable to compile OFI"
  test_finish 1
else
  test_linkc "whether -lfabric" "ok" "no" "-lfabric"
  if test $pass -eq 0
  then
    #test for psm incompatibility
    PSM_COMPAT_DIR=/usr/lib64/psm2-compat
    if test -d $PSM_COMPAT_DIR
    then
      add_flag CMK_LIBDIR='"$CMK_LIBDIR -Wl,-rpath=/usr/lib64/psm2-compat"' "psm2-compat lib"
      CMK_LIBDIR="$CMK_LIBDIR -Wl,-rpath=/usr/lib64/psm2-compat"
      test_linkc "whether -lfabric after adding psm2-compatible library" "ok" "no" "-lfabric"
      if test $pass -eq 0
      then
        echo "Error: -lfabric not found or not working. Pass '--basedir=/path/to/dir/' if -lfabric is located in a different directory"
        test_finish 1
      fi
    else
      CMK_LIBFABRIC_LIBONLY=`pkg-config --libs-only-l libfabric`
       if test "$?" = 0
       then
	  test_linkc "whether pkg-config fabric" "ok" "no" "-lfabric"
      else
	  echo "Error: -lfabric not working, $PSM_COMPAT_DIR not found, pkg-config libfabric not working"
	  echo "Pass '--basedir=/path/to/dir/' if -lfabric is located in a different directory"
	  test_finish 1
      fi

    fi
  fi
fi

fi

#### If we can build OFI, test if we can also build CXI ####
if test "$BUILD_OFI" = 1
then
# get path info
    CMK_LIBFABRIC_LIBONLY=`pkg-config --libs-only-l libfabric`
    CMK_LIBFABRIC_INC=`pkg-config --cflags libfabric`
    cat > $t <<EOT
#include <rdma/fabric.h>
#include <rdma/fi_cxi_ext.h>
int main(int argc, char **argv)
{
  struct fi_info *providers;
  int ret = fi_getinfo(FI_VERSION(1,0), NULL, NULL, 0ULL, NULL, &providers);
  return 0;
}
EOT
	test_cxx "whether build on CXI" "yes" "no"
	AC_DEFINE_UNQUOTED(CMK_CXI, $strictpass, [build CXI.])
	BUILD_CXI=$strictpass
fi

#### test if we can build UCX ####
if test "$CMK_BUILD_UCX" = 1
then
cat > $tc <<EOT
#include <ucp/api/ucp.h>
int main(int argc, char **argv)
{
  unsigned major, minor, number;
  ucp_get_version(&major, &minor, &number);
  return 0;
}
EOT
test_cc "whether build on UCX" "yes" "no" "-lucp"
AC_DEFINE_UNQUOTED(CMK_BUILD_ON_UCX, $strictpass, [build UCX.])
BUILD_UCX=$strictpass

if test $BUILD_UCX -eq 0
then
  echo "Error: Unable to compile UCX"
  test_finish 1
else
  test_linkc "whether -lucp" "ok" "no" "-lucp"
fi

fi

#### test if we can build MPI ####
if test "$CMK_BUILD_MPI" = 1
then

cat > $tc <<EOT
#include "mpi.h"
int main(int argc, char **argv)
{
  MPI_Init(&argc, &argv);
  return 0;
}
EOT
mv -f ../include/mpi.h ../include/mpi.h.bak 2>/dev/null
test_cc "whether build on MPI" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_BUILD_ON_MPI, $strictpass, [build MPI.])
BUILD_MPI=$strictpass

if test $BUILD_MPI -eq 0
then
  echo "Error: Cannot compile an MPI program"
  test_finish 1
fi

if test $BUILD_MPI -eq 1
then
  test_linkc "whether need to specify MPI library" "no" "yes" ""
  if test $pass -ne 1
  then
    if test -z "$CMK_MPI_LIB"
    then
      test_linkc "whether -lmpich" "ok" "no" "-lmpich"
      if test $pass -eq 1
      then
        add_flag CMK_SYSLIBS='"$CMK_SYSLIBS -lmpich"' "mpi lib"
      else
        test_linkc "whether -lmpi" "ok" "no" "-lmpi"
        if test $pass -eq 1
        then
                add_flag CMK_SYSLIBS='"$CMK_SYSLIBS -lmpi"' "mpi lib"
        else
                echo "Error: Cannot find MPI library"
                test_finish 1
        fi
      fi
    else
      add_flag CMK_SYSLIBS='"$CMK_SYSLIBS $CMK_MPI_LIB"' "mpi lib"
    fi
  fi
fi

mv -f ../include/mpi.h.bak ../include/mpi.h 2>/dev/null
fi

if test "$BUILD_MPI" = "1"
then

cat > $t <<EOT
#include <stdio.h>
#include <mpi.h>

int main (int argc, char** argv) {
  return 0;
}
EOT
test_cxx "whether macro conflicts occurs due to C++ MPI bindings" "no" "yes" ""
AC_DEFINE_UNQUOTED(CMK_CXX_MPI_BINDINGS, $strictpass, [Disables conflicting macros.])
fi


################### Syscalls and Libraries ###################

#### test for getrusage with RUSAGE_THREAD ###
cat > $tc <<EOT
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#ifndef __USE_GNU
# define __USE_GNU
#endif
#include <sys/time.h>
#include <sys/resource.h>
int main() {
  struct rusage usage;
  getrusage(RUSAGE_THREAD, &usage);
  return 0;
}
EOT
test_linkc "whether getrusage accepts RUSAGE_THREAD" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_RUSAGE_THREAD, $pass, [whether getrusage accepts RUSAGE_THREAD])

#### test for asctime ###

cat > $tc <<EOT
#include <time.h>
int main() {
  struct tm *local;
  time_t t;
  t = time(NULL);
  local = localtime(&t);
  asctime(local);
  return 0;
}
EOT
test_linkc "whether has asctime" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_ASCTIME, $pass, [whether has asctime])

#### test for log2 ###
cat > $t <<EOT
#include <math.h>
int main() {
  int i = log2(10);
  return 0;
}
EOT
test_link "whether has log2" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_LOG2, $pass, [whether has log2])

#### test for sqrtf ###
cat > $t <<EOT
#include <math.h>
int main() {
  float i = sqrtf((float)10.0);
  return 0;
}
EOT
test_link "whether has sqrtf" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_SQRTF, $pass, [whether has sqrtf])

#### test for fabsf ###
cat > $t <<EOT
#include <math.h>
int main() {
  float i = fabsf((float)10.0);
  return 0;
}
EOT
test_link "whether has fabsf" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_FABSF, $pass, [whether has fabsf])

#### test for mkstemp ###
cat > $t <<EOT
#include <stdlib.h>
#include <string.h>
int main() {
  char fname[[128]];
  strcpy(fname, "/tmp/fdXXX.XXX");
  mkstemp(fname);
  return 0;
}
EOT
test_link "whether has mkstemp" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_USE_MKSTEMP, $pass, [whether has mkstemp])

#### test for system ###
cat > $t <<EOT
#include <stdlib.h>
int main() {
  system("/bin/ls");
  return 0;
}
EOT
test_link "whether has system" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_SYSTEM, $pass, [whether has system])

#### test for sync() ###
cat > $t <<EOT
#include <unistd.h>
int main() {
  sync();
  return 0;
}
EOT
test_link "whether has sync()" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_SYNC_FUNC, $pass, [whether has sync])

#### test for fsync() ###
cat > $t <<EOT
#include <unistd.h>
int main() {
  fsync(0);
  return 0;
}
EOT
test_link "whether has fsync()" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_FSYNC_FUNC, $pass, [whether has fsync])

#### test for fdatasync() ###
cat > $t <<EOT
#include <unistd.h>
int main() {
  fdatasync(0);
  return 0;
}
EOT
test_link "whether has fdatasync()" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_FDATASYNC_FUNC, $pass, [whether has fdatasync])

#### test for sbrk ###
cat > $t <<EOT
#include <unistd.h>
int main() {
  void *ptr  = sbrk(0); 
}
EOT
test_link "whether has sbrk" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_SBRK, $pass, [whether has sbrk])

#### test for _setjmp/_longjmp ###
cat > $t <<EOT
#include <setjmp.h>
int main() {
  jmp_buf buf;
  _setjmp(buf);
  _longjmp(buf, 0);
}
EOT
test_link "whether has _setjmp/_longjmp" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_UNDERSCORE_SETJMP, $pass, [whether has _setjmp/_longjmp])

#### test for mstats ###
cat > $t <<EOT
#include <malloc.h>
int main() {
  struct mstats ms = mstats();
}
EOT
test_link "whether has mstats" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_MSTATS, $pass, [whether has mstats])

#### test for mallinfo ###
cat > $t <<EOT
#include <malloc.h>
int main() {
  struct mallinfo mi = mallinfo();
}
EOT
test_link "whether has mallinfo" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_MALLINFO, $pass, [whether has mallinfo])

#### test for cfree ###
cat > $t <<EOT
#include <stdlib.h>
int main() {
  cfree(0);
}
EOT
test_link "whether has cfree" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_CFREE, $pass, [whether has cfree])

#### test for pvalloc ###
cat > $t <<EOT
#include <malloc.h>
int main() {
  return pvalloc(4096) == 0;
}
EOT
test_link "whether has pvalloc" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_PVALLOC, $pass, [whether has pvalloc])

#### test for valloc ###
cat > $t <<EOT
#include <malloc.h>
int main() {
  return valloc(4096) == 0;
}
EOT
test_link "whether has valloc" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_VALLOC, $pass, [whether has valloc])

#### test for __morecore ###
cat > $t <<EOT
#include <cstddef>
int main() {
  extern void *(*__morecore)(ptrdiff_t);
  __morecore(0);
  return 0;
}
EOT
test_link "whether expects __morecore symbol" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_EXPECTS_MORECORE, $pass, [whether expects __morecore symbol])

#### test for popen ###
cat > $t <<EOT
#include <stdio.h>
int main() {
  FILE *p = popen("/bin/ps", "r");
  pclose(p);
}
EOT
test_link "whether has popen" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_POPEN, $pass, [whether has popen])

#### test for poll ###
cat > $t <<EOT
#include <poll.h>
void foo(void) { 
  struct pollfd pos[[3]];
  poll(pos, 1, 1);
}
EOT
test_cxx "whether has poll" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_USE_POLL, $pass, [whether the poll syscall exists])

#### check if getpagesize exists ####
cat > $tc <<EOT
#include <unistd.h>

int main(int argc, char **argv) {
    int s = getpagesize();
}
EOT
test_linkc "whether has getpagesize" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_GETPAGESIZE, $pass, [whether getpagesize exists])

#### check if getpid exists ####
cat > $tc <<EOT
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char **argv) {
    pid_t pid = getpid();
}
EOT
test_linkc "whether has getpid" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_GETPID, $pass, [whether getpid exists])

#### check if kill exists ####
cat > $tc <<EOT
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

int main(int argc, char**argv) 
{
    pid_t pid = getpid();
    kill(pid, 9);
}
EOT
test_linkc "whether has kill" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_KILL, $pass, [whether kill exists])

#### test for setpriority ###
cat > $t <<EOT
#include <sys/time.h>
#include <sys/resource.h>
void foo(void) { 
  setpriority(PRIO_PROCESS, 0, 0);
}
EOT
test_cxx "whether has setpriority" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_SETPRIORITY, $pass, [whether the setpriority exists])

#### test for system ###
cat > $t <<EOT
#include "ckdll_system.C"
EOT
test_cxx "whether to use signal-safe system() " "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_SIGSAFE_SYSTEM, $pass, [whether to use signal-safe system()])

### test sched_setaffinity ####
cat > $tc <<EOT
#define _GNU_SOURCE
#include <sched.h>

int main()
{
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
    return -1;
  }
  return 0;
}
EOT
test_linkc "whether sched_setaffinity call exists" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_SETAFFINITY, $pass, [whether the sched_setaffinity() exists])

### test pthread_setaffinity_np ####
cat > $tc <<EOT
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

int main()
{
  unsigned long mask = 0xffffffff;
  unsigned int len = sizeof(mask);

  if (pthread_setaffinity_np(pthread_self(), len, &mask) < 0) {
    return -1;
  }
  return 0;
}
EOT
test_linkc "whether pthread_setaffinity_np call exists" "yes" "no" "-lpthread"
AC_DEFINE_UNQUOTED(CMK_HAS_PTHREAD_SETAFFINITY, $pass, [whether the pthread_setaffinity_np() exists])

### test pthread_spin_lock ####
cat > $tc <<EOT
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

int main()
{
  pthread_spinlock_t  lock;

  pthread_spin_init(&lock, 0);

  pthread_spin_lock(&lock);

  return 0;
}
EOT
test_linkc "whether pthread_spin_lock exists" "yes" "no" "-lpthread"
AC_DEFINE_UNQUOTED(CMK_HAS_SPINLOCK, $pass, [whether the pthread_spin_lock exists])

### check libnuma on Linux ####
if test "$WITH_NUMA" = "yes"
then
cat > $tc <<EOT
#include <stdlib.h>
#include <stdio.h>
#include <linux/mempolicy.h>
#include <numaif.h>
#include <numa.h>

int main()
{
  if (get_mempolicy(NULL, NULL, 0, 0, 0) == 0) return 0;
  return -1;
}
EOT
test_linkc "whether libnuma exists" "yes" "no" "-lnuma"
AC_DEFINE_UNQUOTED(CMK_HAS_NUMACTRL, $pass, [whether NUMA control related functions exist])
if test $pass -eq 1
then
        add_flag 'CMK_SYSLIBS="$CMK_SYSLIBS -lnuma"' "libnuma"
fi
fi

#### check bindprocessors on AIX ####
cat > $tc <<EOT
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/processor.h>

int main()
{
  int retValue = 0;
  int pid;
  pid = getpid();
  if (bindprocessor(BINDPROCESS, pid, 0) == -1) return -1;
  return 0;
}
EOT
test_linkc "whether bindprocessor call exists" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_BINDPROCESSOR, $pass, [whether the bindprocessor()
exists])


#### check if visibility attribute works ####
cat > $t <<EOT
__attribute__((visibility("default"))) int myfunc();
int myfunc()
{
  return 0;
}
EOT
test_cxx 'whether has __attribute__((visibility("default")))' "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_ATTRIBUTE_VISIBILITY_DEFAULT, $pass, [whether has __attribute__((visibility("default")))])

#### check if dlopen works ####
skip_dl=0
test -n "$CMK_NO_DL" && skip_dl=1
dl_opt='-ldl'
#workaround for pgcc 4.0, -ldl crash linking, so use -Wl,-ldl to skip the prelinker.
case "$CMK_CXX" in
pgCC*) dl_opt='-Wl,-ldl' ;;
esac

if test $skip_dl -eq 0 
then

cat > $t <<EOT
#include "ckdll_dlopen.C"
int main() {
	CkDll dll("foo.so");
	return 0;
}
EOT
test_link "whether dlopen links without $dl_opt" "yes" "no" ""
noldl=$pass
test_link "whether dlopen links with $dl_opt" "yes" "no" "$dl_opt"
if test $pass -eq 1
then 
#dlopen requires -ldl: add it to our link line
	add_flag CMK_LIBS='"$CMK_LIBS '$dl_opt'"' "dlopen"
fi

if test $pass -eq 1 -o $noldl -eq 1
then
	CMK_HAS_DLOPEN='1'
# One version or another of dlopen worked: compile it in
	AC_DEFINE_UNQUOTED(CMK_DLL_USE_DLOPEN, 1, [dlopen])
fi

cat > $tc <<EOT
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#ifndef __USE_GNU
# define __USE_GNU
#endif
#include <dlfcn.h>
#include <stddef.h>
int main()
{
  return dlopen("", RTLD_DEEPBIND) == NULL;
}
EOT
test_cc "whether has RTLD_DEEPBIND" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_RTLD_DEEPBIND, $pass, [whether has RTLD_DEEPBIND])

cat > $tc <<EOT
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#ifndef __USE_GNU
# define __USE_GNU
#endif
#include <dlfcn.h>
#include <stddef.h>
int main()
{
  return dlsym(RTLD_DEFAULT, "main") != NULL;
}
EOT
test_cc "whether has RTLD_DEFAULT" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_RTLD_DEFAULT, $pass, [whether has RTLD_DEFAULT])

cat > $tc <<EOT
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#ifndef __USE_GNU
# define __USE_GNU
#endif
#include <dlfcn.h>
#include <stddef.h>
int main()
{
  return dlsym(RTLD_NEXT, "main") != NULL;
}
EOT
test_cc "whether has RTLD_NEXT" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_RTLD_NEXT, $pass, [whether has RTLD_NEXT])

cat > $tc <<EOT
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#ifndef __USE_GNU
# define __USE_GNU
#endif
#include <dlfcn.h>
#include <stddef.h>
int main()
{
  return dlmopen(LM_ID_NEWLM, "foo.so", 0) == NULL;
}
EOT
test_cc "whether has dlmopen" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_DLMOPEN, $pass, [whether has dlmopen])
CMK_HAS_DLMOPEN="$pass"

cat > $tc <<EOT
#include <unistd.h>
int main()
{
  char exe[[1024]];
  return readlink("/proc/self/exe", exe, sizeof(exe)-1) == -1;
}
EOT
test_cc "whether has readlink" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_READLINK, $pass, [whether has readlink])
CMK_HAS_READLINK="$pass"

cat > $tc <<EOT
#include <limits.h>
#include <stdlib.h>
int main()
{
  return realpath("/proc/self/exe", NULL) == NULL;
}
EOT
test_cc "whether has realpath" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_REALPATH, $pass, [whether has realpath])
CMK_HAS_REALPATH="$pass"

case "$CMK_VDIR" in
  *-win-*)
    CMK_CAN_GET_BINARY_PATH='1'
    CMK_CAN_OPEN_SHARED_OBJECTS_DYNAMICALLY='1'
    ;;
  *-darwin*)
    CMK_CAN_GET_BINARY_PATH='1'
    CMK_CAN_OPEN_SHARED_OBJECTS_DYNAMICALLY="$CMK_HAS_DLOPEN"
    ;;
  *)
    if test "$CMK_HAS_READLINK" = '1' -o "$CMK_HAS_REALPATH" = '1'
    then
      CMK_CAN_GET_BINARY_PATH='1'
    else
      CMK_CAN_GET_BINARY_PATH='0'
    fi
    CMK_CAN_OPEN_SHARED_OBJECTS_DYNAMICALLY="$CMK_HAS_DLOPEN"
    ;;
esac

if test "$CMK_CAN_OPEN_SHARED_OBJECTS_DYNAMICALLY" = '1' -a "$CMK_CAN_GET_BINARY_PATH" = '1'
then
  CMK_SUPPORTS_FSGLOBALS='1'
else
  CMK_SUPPORTS_FSGLOBALS='0'
fi
AC_DEFINE_UNQUOTED(CMK_SUPPORTS_FSGLOBALS, $CMK_SUPPORTS_FSGLOBALS, [whether supports filesystem globals])
add_flag "CMK_SUPPORTS_FSGLOBALS=$CMK_SUPPORTS_FSGLOBALS" 'filesystem globals'
add_make_flag "CMK_SUPPORTS_FSGLOBALS:=$CMK_SUPPORTS_FSGLOBALS" 'filesystem globals'

if test "$CMK_HAS_DLMOPEN" = '1' -a "$CMK_CAN_GET_BINARY_PATH" = '1'
then
  CMK_SUPPORTS_PIPGLOBALS='1'
else
  CMK_SUPPORTS_PIPGLOBALS='0'
fi
AC_DEFINE_UNQUOTED(CMK_SUPPORTS_PIPGLOBALS, $CMK_SUPPORTS_PIPGLOBALS, [whether supports PiP globals])
add_flag "CMK_SUPPORTS_PIPGLOBALS=$CMK_SUPPORTS_PIPGLOBALS" 'PiP globals'
add_make_flag "CMK_SUPPORTS_PIPGLOBALS:=$CMK_SUPPORTS_PIPGLOBALS" 'PiP globals'

if test "$CMK_CAN_OPEN_SHARED_OBJECTS_DYNAMICALLY" = '1' -a "$CMK_CAN_GET_BINARY_PATH" = '1' -a "$CMK_HAS_DL_ITERATE_PHDR" = '1'
then
  CMK_SUPPORTS_PIEGLOBALS='1'
else
  CMK_SUPPORTS_PIEGLOBALS='0'
fi
AC_DEFINE_UNQUOTED(CMK_SUPPORTS_PIEGLOBALS, $CMK_SUPPORTS_PIEGLOBALS, [whether supports PIE globals])
add_flag "CMK_SUPPORTS_PIEGLOBALS=$CMK_SUPPORTS_PIEGLOBALS" 'PIE globals'
add_make_flag "CMK_SUPPORTS_PIEGLOBALS:=$CMK_SUPPORTS_PIEGLOBALS" 'PIE globals'

#### check if getProcAddress works ####
cat > $t <<EOT
#include "ckdll_win32.C"
EOT
test_cxx "whether getProcAddress works" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_DLL_USE_WIN32, $pass, [whether getProcAddress works])

fi    # end of skip_dl

#### check if socklen_t exists ####
cat > $t <<EOT
#include <sys/types.h>
#include <sys/socket.h>

void foo(void) {
	int i;
	socklen_t s=sizeof(i);
}
EOT
test_cxx "whether has socklen_t" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_SOCKLEN, $pass, [whether has socklen_t])

### test getifaddrs ####
cat > $tc <<EOT
#include <netinet/in.h> /* for sockaddr_in */
#include <ifaddrs.h> /* for getifaddrs */
#include <net/if.h> /* for IFF_RUNNING */

int main()
{
  struct ifaddrs *interfaces=0;
  if( getifaddrs(&interfaces) == 0 ) {
        struct ifaddrs *interface;
        for( interface=interfaces; interface; interface=interface->ifa_next ) {
            if( (interface->ifa_flags & IFF_UP) && ! (interface->ifa_flags & IFF_LOOPBACK) ) {
                const struct sockaddr_in *addr = (const struct sockaddr_in*)interface->ifa_addr;
                if( addr && addr->sin_family==AF_INET ) {
                    break;
                }
            }
        }
        freeifaddrs(interfaces);
  }
}
EOT
test_linkc "whether getifaddrs call exists" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_GETIFADDRS, $pass, [whether getifaddrs() exists])

#### check if mmap exists ####
cat > $t <<EOT
#include <sys/types.h>
#include <sys/mman.h>

void *foo(void *a,int l,int fd) {
	return mmap((caddr_t)a,l,PROT_READ+PROT_WRITE,
                 MAP_FIXED+MAP_PRIVATE,fd,0);
}
EOT
test_cxx "whether the mmap() syscall exists" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_MMAP, $pass, [whether the mmap() syscall exists])
add_flag "CMK_HAS_MMAP=$pass"
add_make_flag "CMK_HAS_MMAP:=$pass"

#### check if mmap accepts MAP_ANON ####
cat > $t <<EOT
#include <sys/types.h>
#include <sys/mman.h>

void *foo(void *a,int l) {
	return mmap((caddr_t)a,l,PROT_READ+PROT_WRITE,
                MAP_FIXED+MAP_PRIVATE+MAP_ANON,-1,0);
}
EOT
test_cxx "whether mmap() accepts MAP_ANON" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_MMAP_ANON, $pass, [whether mmap() accepts MAP_ANON])

#### check if mmap accepts MAP_NORESERVE ####
cat > $t <<EOT
#include <sys/types.h>
#include <sys/mman.h>

void *foo(void *a,int l) {
	return mmap((caddr_t)a,l,PROT_READ+PROT_WRITE,
                MAP_FIXED+MAP_PRIVATE+MAP_NORESERVE,-1,0);
}
EOT
test_cxx "whether mmap() accepts MAP_NORESERVE" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_MMAP_NORESERVE, $pass, [whether mmap() accepts MAP_NORESERVE])

#### check if get_myaddress exists ####
cat > $t <<EOT
#include <rpc/rpc.h>

void foo(void *a) {
    get_myaddress((struct sockaddr_in*)a);
}
EOT
test_cxx "whether has get_myaddress" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_GET_MYADDRESS, $pass, [whether has get_myaddress])

#### check if mprotect exists ####
cat > $t <<EOT
#include <stdio.h>
#include <sys/mman.h>

void foo(void *a,int l,int fd) {
     void *pg = NULL;
     size_t pagesize = 4096;
     mprotect(pg, pagesize, PROT_READ | PROT_WRITE);
}
EOT
test_cxx "whether has mprotect" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_MPROTECT, $pass, [whether has mprotect])

#### check if support for SHM through CMA exists ####
cat > $t <<EOT
#define _GNU_SOURCE
#include <sys/uio.h>
#include <errno.h>
int main() {
  pid_t pid;
  struct iovec *local, *remote;
  int nread = process_vm_readv(pid, local, 1, remote, 1, 0);
  nread = process_vm_writev(pid, local, 1, remote, 1, 0);
  return errno;
}
EOT
test_link "whether has support for shm transport using Cross Memory Attach" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_CMA, $pass, [whether supports cma])
CMK_HAS_CMA="$pass"

test "$CMK_HAS_CMA" = '1' -a "$CMK_MULTICORE" != '1' && CMK_USE_CMA='1' || CMK_USE_CMA='0'
add_make_flag "CMK_USE_CMA:=$CMK_USE_CMA" 'Cross Memory Attach'

test "$CMK_WINDOWS" != '1' && CMK_SUPPORTS_MEMORY_ISOMALLOC='1' || CMK_SUPPORTS_MEMORY_ISOMALLOC='0'
add_flag "CMK_SUPPORTS_MEMORY_ISOMALLOC='$CMK_SUPPORTS_MEMORY_ISOMALLOC'" '-memory isomalloc'
add_make_flag "CMK_SUPPORTS_MEMORY_ISOMALLOC:=$CMK_SUPPORTS_MEMORY_ISOMALLOC" '-memory isomalloc'

#### check if -rdynamic works ####
cat > $t <<EOT
int main() {
  return 0;
}
EOT
TRACE_LINK_FLAG=''
CAN_EXPORT_SYMBOLS='0'
for i in '-rdynamic' '-Wl,--export-dynamic'; do
  test_link "whether has $i" "yes" "no" "" "$i"
  if test "$strictpass" = '1'; then
    TRACE_LINK_FLAG="$i"
    CAN_EXPORT_SYMBOLS='1'
    break
  fi
done

if test "$CAN_EXPORT_SYMBOLS" = '1'; then
  add_flag 'CMK_LD="$CMK_LD '$TRACE_LINK_FLAG'"' "exporting program symbols to shared objects"
  add_flag 'CMK_LDXX="$CMK_LDXX '$TRACE_LINK_FLAG'"' "exporting program symbols to shared objects"
fi

test_link "whether has -Wl,-undefined,dynamic_lookup" "yes" "no" "" "-Wl,-undefined,dynamic_lookup"
add_flag "CMK_LINKER_KNOWS_UNDEFINED='$strictpass'" "importing program symbols from shared objects"

#### check if glibc backtrace exists ####
cat > $t <<EOT
#include "cmibacktrace.C"

int main() {
	int nLevels=1;
	void *stack;
	CmiBacktraceRecord(&stack,0,&nLevels);
	return 0;
}
EOT
test_link "whether glibc backtrace works" "yes" "no" "-DCMK_USE_BACKTRACE=1 $TRACE_LINK_FLAG"
AC_DEFINE_UNQUOTED(CMK_USE_BACKTRACE, $pass, [whether glibc backtrace works])

#### test sleep ####
cat > $t <<EOT
#include <unistd.h>
int main() {
  sleep(1);
  return 0;
}
EOT
test_cxx "whether has sleep " "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_SLEEP, $pass, [whether has sleep])

#### test usleep ####
cat > $t <<EOT
#include <unistd.h>
int main() {
  usleep(100);
  return 0;
}
EOT
test_cxx "whether has usleep " "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_USLEEP, $pass, [whether has usleep])

#### test personality() and ADDR_NO_RANDOMIZE ####
# Only works on modern Linux systems
cat > $t <<EOT
#include <sys/personality.h>
int main() {
    int orig_persona = personality(0xffffffff);
    personality(orig_persona | ADDR_NO_RANDOMIZE);
    return 0;
}
EOT
test_cxx "whether personality() and ADDR_NO_RANDOMIZE exist" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_ADDR_NO_RANDOMIZE, $pass, [whether personality() and ADDR_NO_RANDOMIZE exist])

#### test posix_spawn() ####
cat > $t <<EOT
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <spawn.h>
int main() {
    return posix_spawn(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
}
EOT
test_cxx "whether posix_spawn() exists" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_POSIX_SPAWN, $pass, [whether posix_spawn() exists])


# enable/disable zlib
AC_ARG_ENABLE([zlib],
            [AS_HELP_STRING([--enable-zlib],
              [enable zlib support])],
            [enable_zlib=$enableval],
            [enable_zlib=yes])

#### test if has zlib ####
if test "$enable_zlib" = "yes"
then

cat > $t <<EOT
#include <zlib.h>
int main() { 
  gzFile f = gzopen("/tmp/x","r");
  gzprintf(f, "test...\n");
  gzclose(f);
  return 0;
}
EOT
test_link "whether has zlib" "yes" "no" "-lz"
AC_DEFINE_UNQUOTED(CMK_USE_ZLIB, $pass, [whether has zlib])
if test $pass -eq 1
then 
	add_flag 'CMK_SYSLIBS="$CMK_SYSLIBS -lz"' "zlib"
fi

fi

#### test if has elf.h ####
cat > $t <<EOT
#include <elf.h>
typedef Elf32_Addr ELF_TYPE_Addr;
int main() { 
}
EOT
test_cxx "whether has elf.h " "yes" "no" ""
AC_DEFINE_UNQUOTED([CMK_HAS_ELF_H], $pass, [whether has elf.h])
add_flag "CMK_HAS_ELF_H='$pass'" "elf.h"

CMK_SUPPORTS_SWAPGLOBALS='0'
CMK_SUPPORTS_TLSGLOBALS='0'
if [[ "$CMK_MACOSX" = '1' ]]
then
  CMK_SUPPORTS_TLSGLOBALS='1'
elif test $pass -eq 1
then
  # swapglobals
  if [[ "$CMK_SMP" != '1' ]]
  then
    LD_VERSION_OUTPUT="$($CMK_LD -Wl,--version,-v 2>/dev/null)"
    if ! $(printf "$LD_VERSION_OUTPUT" | grep -q 'GNU ld')
    then
      CMK_SUPPORTS_SWAPGLOBALS='2'
    else
      LD_VERSION="$(printf "$LD_VERSION_OUTPUT" | grep -Eo 'GNU ld (\([[^)]]*\) )?(version )?[[-0-9._a-zA-Z]]+' | awk '{print $NF}')"
      LD_VERSION_MAJOR="$(printf $LD_VERSION | cut -d. -f1)"
      LD_VERSION_MINOR="$(printf $LD_VERSION | cut -d. -f2 | cut -d- -f1)"
      if [[ "$LD_VERSION_MAJOR" -gt 2 ]] || [[ "$LD_VERSION_MAJOR" -eq 2 -a "$LD_VERSION_MINOR" -gt 23 ]]
      then
        CMK_SUPPORTS_SWAPGLOBALS='3'
      else
        CMK_SUPPORTS_SWAPGLOBALS='1'
      fi
    fi
  fi

  # tlsglobals
  if [[ "$CMK_VDIR" = "${CMK_VDIR%x86_64}x86_64" -o "$CMK_VDIR" = "${CMK_VDIR%i386}i386" ]]
  then
    if [[ "$CMK_COMPILER_KNOWS_TLSDIRECTSEGREFS" != '1' -a "$CMK_COMPILER" != 'clang' ]]
    then
      CMK_SUPPORTS_TLSGLOBALS='2'
    else
      CMK_SUPPORTS_TLSGLOBALS='1'
    fi
  fi
fi

if test "$enable_tls" = "yes"
then
    add_flag 'CMK_USER_DISABLED_TLS="0"' "tlsglobals"
else
    add_flag 'CMK_USER_DISABLED_TLS="1"' "tlsglobals"
    CMK_SUPPORTS_TLSGLOBALS='0'
fi

AC_DEFINE_UNQUOTED(CMK_SUPPORTS_SWAPGLOBALS, $CMK_SUPPORTS_SWAPGLOBALS, [whether supports swapglobals])
add_flag "CMK_SUPPORTS_SWAPGLOBALS=$CMK_SUPPORTS_SWAPGLOBALS" 'swapglobals'
add_make_flag "CMK_SUPPORTS_SWAPGLOBALS:=$CMK_SUPPORTS_SWAPGLOBALS" 'swapglobals'
AC_DEFINE_UNQUOTED(CMK_SUPPORTS_TLSGLOBALS, $CMK_SUPPORTS_TLSGLOBALS, [whether supports tlsglobals])
add_flag "CMK_SUPPORTS_TLSGLOBALS=$CMK_SUPPORTS_TLSGLOBALS" 'tlsglobals'
add_make_flag "CMK_SUPPORTS_TLSGLOBALS:=$CMK_SUPPORTS_TLSGLOBALS" 'tlsglobals'

#### test if has Multiprocessing.h for apple ####
cat > $t <<EOT
#include <Carbon/Carbon.h>
#include <Multiprocessing.h>
int main() {
  int a = MPProcessorsScheduled();
}
EOT
test_cxx "whether has Multiprocessing.h for Apple " "yes" "no" ""
AC_DEFINE_UNQUOTED([CMK_HAS_MULTIPROCESSING_H], $pass, [whether has Multiprocessing.h])

#### test if "ntohl" is available ####
cat > $tc <<EOT
#ifdef _WIN32
#include <Winsock2.h>
#else
#include <stdint.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

int main() {
  int i = 100;
  i = ntohl(i);
  return 0;
}
EOT
test_cc "whether ntohl is available" "yes" "no" ""
AC_DEFINE_UNQUOTED(CMK_HAS_NTOHL, $pass, [whether ntohl is available])

### test for libjpeg ####
cat > $t <<EOT
#include <stdio.h>
#include <stdlib.h>
#include "jpeglib.h"

int main()
{
  struct jpeg_compress_struct cinfo;
  jpeg_create_compress(&cinfo);
  return 0;
}
EOT
test_link "whether has libjpeg" "yes" "no" "-ljpeg"
AC_DEFINE_UNQUOTED(CMK_USE_LIBJPEG, $pass, [whether has libjpeg])
if test $pass -eq 1
then
        add_flag 'CMK_LIBJPEG="-ljpeg"' "libjpeg"
        add_make_flag 'CMK_LIBJPEG:=-ljpeg' 'libjpeg'
fi

#### check if PAPI exists ####
if test -n "$CMK_USE_PAPI"
then
cat > $t <<EOT
#include <stdio.h>
#include <papi.h>
                                                                                
int main() {
    if (PAPI_library_init(PAPI_VER_CURRENT) != PAPI_VER_CURRENT) return 1;
    return 0;
}
EOT
test_link "whether PAPI exists" "yes" "no" "-lpapi"
AC_DEFINE_UNQUOTED(CMK_HAS_COUNTER_PAPI, $pass, [whether PAPI exists])
if test "$enable_tracing" = "no"
then
	echo "Error: Cannot build papi version when tracing is disabled, build with --enable-tracing"
	test_finish 1
elif test $fail -eq 1
then
	echo "Error: Cannot find papi library"
	test_finish 1
else
	add_flag 'CMK_LIBS="$CMK_LIBS -lpapi"' "papi"
fi
fi

#### test if Python headers are installed ####
PYTHON_VERSION=`python2 -V 2>&1 | awk {'print $2'} | awk -F. {'print $1"."$2'}`
cat > $t <<EOT
#include "python${PYTHON_VERSION}/Python.h"
#include "python${PYTHON_VERSION}/compile.h"
#include "python${PYTHON_VERSION}/eval.h"
#include "python${PYTHON_VERSION}/node.h"

int main() {
    Py_Initialize();
    PyEval_InitThreads();
    struct _node* programNode = PyParser_SimpleParseString("return 1\n",Py_file_input);
    PyCodeObject *program = PyNode_Compile(programNode, "");
}
EOT
test_link "whether Python is installed" "yes" "no" "-lpython$PYTHON_VERSION -lpthread -lutil -ldl"
AC_DEFINE_UNQUOTED(CMK_HAS_PYTHON, $pass, [whether Python is installed])
AC_DEFINE_UNQUOTED(CMK_PYTHON_VERSION, ${PYTHON_VERSION}, [Python version])
if test $pass -eq 1
then
	add_flag "CMK_BUILD_PYTHON=$PYTHON_VERSION" "python"
	add_make_flag "CMK_BUILD_PYTHON:=$PYTHON_VERSION" 'python'
fi

## Cray specific test
if test "$CMK_BUILD_CRAY" = "1"
then
#echo "Test for known incompatible compiler versions"

 if test "$CRAY_CC_VERSION" = "8.1.4"
 then
	echo "CCE 8.1.4 produces incorrect Charm++ code"
	echo "Please use a newer version of the CCE compiler"
	echo "e.g. module load cce/8.1.7"
	test_finish 1
 fi

#### test if Cray node topology will work ####
cat > $tc <<EOT
#include <pmi.h>

int main() {
    int nid;
    PMI_Get_nid(0, &nid);

    return 0;
}
EOT
test_linkc "whether PMI_Get_nid exists" "yes" "no" "$CMK_CRAY_LIBS"
AC_DEFINE_UNQUOTED(CMK_HAS_PMI_GET_NID, $pass, [whether PMI_Get_nid exists])

#### test if Cray mesh topology will work ####
cat > $tc <<EOT
#include <rca_lib.h>

int main() {
    rca_mesh_coord_t xyz;
    rca_get_meshcoord(0, &xyz);

    return 0;
}
EOT
test_linkc "whether Cray rca library is available" "yes" "no" "$CRAY_RCA_POST_LINK_OPTS -lrca"
AC_DEFINE_UNQUOTED(CMK_HAS_RCALIB, $pass, [whether Cray rca library is available])

#### test if Cray mesh dimension query function will work ####
cat > $tc <<EOT
#include <rca_lib.h>

int main() {
    rca_mesh_coord_t xyz;
    rca_get_max_dimension(&xyz);

    return 0;
}
EOT
test_linkc "whether Cray rca_has get_max_dimension" "yes" "no" "$CRAY_RCA_POST_LINK_OPTS -lrca"
AC_DEFINE_UNQUOTED(CMK_HAS_RCA_MAX_DIMENSION, $pass, [whether Cray rca has rca_get_max_dimension])

#### query Cray machine max NID if get_max_dimension not exist ####

if test $pass -eq 0
then
cmd=`which xtprocadmin 2>/dev/null`
if test -n "$cmd"
then
    #maxnid=`xtprocadmin  | tail -1  | awk '{print $1}'`
    # workaround for hopper
    (export SHELL=/bin/csh; xtprocadmin >& t.$$)
    maxnid=`cat t.$$  | tail -1  | awk '{print $1}'`
    /bin/rm -f t.$$
    AC_MSG_CHECKING(Cray machine maxnid)
    AC_MSG_RESULT("$maxnid")
    if test -n "$maxnid"
    then
    AC_DEFINE_UNQUOTED(CMK_CRAY_MAXNID, $maxnid, [Cray MAXNID])
    fi
fi
fi




#### query Cray machine supports BANDWIDTH_INJECTION controls####

if test "$CMK_BUILD_MPI" = "1"
then
echo "BALANCED_INJECTION test disabled on MPI"
elif test "$GNI_CRAYXC" = "1"
then
echo "BALANCED_INJECTION test disabled on Aries network"
else
cat > $tc <<EOT
#include <stdint.h>
#include <gni_pub.h>

int main() {
    gni_bi_desc_t gni_bi_desc;
    uint32_t gni_device_id = 0;
    gni_return_t gni_rc = GNI_GetBIConfig(gni_device_id, &gni_bi_desc);
    if (gni_rc == GNI_RC_SUCCESS) {
    }
    return 0;
}
EOT
test_linkc "whether GNI_GetBIConfig exists" "yes" "no" "$CMK_CRAY_LIBS"
AC_DEFINE_UNQUOTED(CMK_BALANCED_INJECTION_API, $pass, [whether Cray gni_pub has GNI_GetBIConfig])
fi # end of BIConfig test


fi  # end of Cray specific test

#### test if it can build shared library ####
cat > $t <<EOT
#include "stdlib.h"
extern int foo();
int foo1() { 
  void * t= malloc(2);
  foo();
  return 0;
}
EOT
test_linkso "whether can build shared library" "yes" "no" ""
if test $pass -eq 0
then 
	add_flag 'CMK_NO_BUILD_SHARED="true"' "build-shared"
	add_make_flag 'CMK_NO_BUILD_SHARED:=true' 'build-shared'
	CMK_NO_BUILD_SHARED='true'
else
	BUILD_SHARED=1
        if test "$BUILD_MPI" = "1"
	then
cat > $t <<EOT
#include "stdlib.h"
#include "mpi.h"
extern int foo();
int foo(int argc, char ** argv) {
  void * t= malloc(2);
  foo();
  MPI_Init(&argc, &argv);
  return 0;
}
EOT
		test_linkso "whether can build shared library with MPI" "yes" "no" ""
		BUILD_SHARED=$pass
	fi
	if test $BUILD_SHARED -eq 0
	then
		add_flag 'CMK_NO_BUILD_SHARED="true"' "build-shared"
		add_make_flag 'CMK_NO_BUILD_SHARED:=true' 'build-shared'
		CMK_NO_BUILD_SHARED='true'
	fi
fi

#### test the if command sync exists ####
AC_CHECK_PROG(SYNC, sync, sync )
if test -n "$SYNC"
then
AC_DEFINE_UNQUOTED(CMK_HAS_SYNC, 1, [sync program])
fi

################## Fortran #########################

#echo "set F77 compiler as: $CMK_CF77"
AC_MSG_CHECKING("F77 compiler as")
AC_MSG_RESULT("$CMK_CF77")

AC_MSG_CHECKING("whether Fortran 77 compiler works")
### test fortran 77 compiler ###
cat > conftest.f <<EOF
      SUBROUTINE FOO_foo
      END
EOF
$CMK_CF77 -c conftest.f > /dev/null 2> /dev/null
if test ! -r conftest.o
then
  AC_MSG_RESULT("no")
else
  AC_MSG_RESULT("yes")
fi

### test fortran 90 compiler ###
#echo "set F90 compiler as: $CMK_CF90"
AC_MSG_CHECKING("F90 compiler as")
AC_MSG_RESULT("$CMK_CF90")

AC_MSG_CHECKING("whether Fortran 90 compiler works")
cat > conftest2.f90 <<EOF
      SUBROUTINE FOO_foo
      END
EOF
$CMK_CF90 -c conftest2.f90 > /dev/null 2> /dev/null
if test ! -r conftest2.o
then
  AC_MSG_RESULT("no")
else
  AC_MSG_RESULT("yes")
  mv -f conftest2.o conftest.o
  USE_FORTRAN90=1
fi

### check fortran name mangling
AC_MSG_CHECKING(subroutine name used by Fortran 90 compiler)

if test -r conftest.o
then
  AC_CACHE_VAL(_cv_fortran_postfix,

  NAME=`$CMK_NM conftest.o | grep "foo_foo__"`
  if test "$NAME" != ""
  then
    _cv_fortran_postfix=TWOSCORE
    AC_DEFINE_UNQUOTED(CMK_FORTRAN_USES_TWOSCORE, 1, [TWOSCORE])
  else
    NAME=`$CMK_NM conftest.o | grep "foo_foo_"`
    if test "$NAME" != ""
    then
      _cv_fortran_postfix=ONESCORE
      AC_DEFINE_UNQUOTED(CMK_FORTRAN_USES_ONESCORE, 1, [ONESCORE])
    else
      NAME=`$CMK_NM conftest.o | grep "foo_foo"`
      if test "$NAME" != ""
      then
        _cv_fortran_postfix=NOSCORE
        AC_DEFINE_UNQUOTED(CMK_FORTRAN_USES_NOSCORE, 1, [NOSCORE])
      else
        NAME=`$CMK_NM conftest.o | grep "FOO_FOO"`
        if test "$NAME" != ""
        then
          _cv_fortran_postfix=ALLCAPS
          AC_DEFINE_UNQUOTED(CMK_FORTRAN_USES_ALLCAPS, 1, [ALLCAPS])
        else
          echo "#################################################"
          echo "FORTRAN compiler generated name not supported yet"
          echo "#################################################"
        fi
      fi
    fi
  fi
  rm -f conftest.f conftest.o
  )
  AC_MSG_RESULT($_cv_fortran_postfix)
else
  AC_MSG_RESULT("Fortran compiler not working")
fi

### check module name ###

if test "$USE_FORTRAN90" = 1
then
  cat > conftest.f90 <<EOF
      MODULE testmod
        interface 
          function fpup_issizing(p)
          INTEGER :: p
          logical fpup_issizing
          end function
        end interface
      END MODULE
EOF
  AC_MSG_CHECKING(Fortran 90 mod name is capital)
  $CMK_CF90 -c conftest.f90 > /dev/null 2> /dev/null
  name=`ls TESTMOD.* 2>/dev/null`
  if test -n "$name"
  then
        AC_MSG_RESULT("yes")
	add_flag 'CMK_MOD_NAME_ALLCAPS=1' "mod name capital"
  else
        AC_MSG_RESULT("no")
	name=`ls testmod.* 2>/dev/null`
  fi
  if test -n "$name"
  then
  	AC_MSG_CHECKING(Fortran 90 mod name extension)
    	ext=`echo $name | sed -e 's/^[[^.]]*\.//'`
	add_flag 'CMK_MOD_EXT="'$ext'"' "mod name extension"
        AC_MSG_RESULT("$ext")
        /bin/rm -f $name
  fi
  /bin/rm -f conftest.f90
fi

### test if the C++ linker succeeds with a Fortran entry point ##

cat > conftest.f90 <<EOF
      PROGRAM empty
      END PROGRAM
EOF
test_link_fortran_main() {
  AC_MSG_CHECKING("$1")
  echo "### $1" >> $charmout
  cat conftest.f90 >> $charmout
  echo $CMK_CF90 -c conftest.f90 -o test.o $4 >> $charmout
  $CMK_CF90 -c conftest.f90 -o test.o $4 > out 2>&1
  if test $? -ne 0
  then
    test_result 1 "$1" "$2" "$3"
  else
    echo $CMK_LDXX $CMK_LDXX_FLAGS $CMK_LINK_BINARY -o testlink test.o $CMK_LIBDIR $OPTS_LD $CMK_SYSLIBS $CMK_F90LIBS $CMK_F90MAINLIBS $4 $5 >> $charmout
    $CMK_LDXX $CMK_LDXX_FLAGS $CMK_LINK_BINARY -o testlink test.o $CMK_LIBDIR $OPTS_LD $CMK_SYSLIBS $CMK_F90LIBS $CMK_F90MAINLIBS $4 $5 >> out 2>&1
    ret=$?
    test ! -x testlink && ret=1
    test_result $ret "$1" "$2" "$3"
  fi
  cat out >> $charmout
}
test_link_fortran_main "whether the C++ linker succeeds with a Fortran entry point" "yes" "no" ""
/bin/rm -f conftest.f90 test.o testlink out
if test $pass -eq 1
then
  add_flag "CMK_CAN_LINK_FORTRAN='1'" "Fortran"
  add_make_flag "CMK_CAN_LINK_FORTRAN:=1" "Fortran"
fi

### check for OpenMP availability ###
cat > $tc <<EOT
#include <stdio.h>
#include <omp.h>

int main(void)
{
  int iam = 0, np = 1;

  #pragma omp parallel default(shared) private(iam, np)
  {
    np = omp_get_num_threads();
    iam = omp_get_thread_num();
    printf("Hello from thread %d of %d\n", iam, np);
  }

  return 0;
}
EOT
test_linkc "whether OpenMP is supported" "yes" "no" "$CMK_C_OPENMP"
add_flag "CMK_HAS_OPENMP='$pass'" "OpenMP"
add_make_flag "CMK_HAS_OPENMP:=$pass" "OpenMP"

### check for Lustre FS availability ###
cat > $tc <<EOT
#include <stdio.h>
#include <lustre/lustreapi.h>
#include <lustre/lustre_user.h>

int main() {
  llapi_printf(LLAPI_MSG_NORMAL, "Lustre FS is available");
  return 0;
}
EOT
test_linkc "whether has lustre fs" "yes" "no" "-llustreapi"
AC_DEFINE_UNQUOTED(CMK_HAS_LUSTREFS, $pass, [whether has lustrefs])
if test $pass -eq 1
then
  add_make_flag 'CMK_LUSTREAPI:=-llustreapi' "lustreapi"
else
  add_make_flag 'CMK_LUSTREAPI:=' "lustreapi"
fi


eval `"$CHARMBIN/charmc" $OPTS -print-building-blocks -standalone`
add_make_flag "HWLOC_CC:=$(get_full_command_name "$CHARM_CC")" "hwloc"
add_make_flag "HWLOC_CXX:=$(get_full_command_name "$CHARM_CXX")" "hwloc"
add_make_flag "HWLOC_CC_FLAGS:=$CHARM_CC_FLAGS" "hwloc"
add_make_flag "HWLOC_CXX_FLAGS:=$CHARM_CXX_FLAGS" "hwloc"

AC_OUTPUT
