#!/bin/bash
#
# Conv-host for MPI:
#  Translates +pN-style conv-host options into
# mpirun -npN options.

export FROM_CHARMRUN='1'

args=()

# Arguments that need to be passed to the charm application, not mpirun (e.g. ++quiet)
charm_args=()
pes=0
nodes=0
ppn=1
machinefile=""
QUIET=0
XTERM='xterm'
DEBUG=0
DEBUG_NO_PAUSE=0
DEBUGGER=''

runCmd()
{
  [[ $QUIET -eq 0 ]] && echo "charmrun>" "$@"
  "$@"
}

while [[ $# -ne 0 ]]
do
	case "$1" in
	+n|++n|++np)
		nodes="$2"
		shift
		;;
	+n[0-9]*)
		nodes="${1#\+n}"
		;;
	++n[0-9]*)
		nodes="${1#\+\+n}"
		;;
	++np[0-9]*)
		nodes="${1#\+\+np}"
		;;
	+ppn|++ppn)
		args+=(+ppn "$2")
		ppn="$2"
		shift
		;;
	+ppn[0-9]*)
		args+=("$1")
		ppn="${1#\+ppn}"
		;;
	++ppn[0-9]*)
		args+=("$1")
		ppn="${1#\+\+ppn}"
		;;
	+p|++p)
		pes="$2"
		shift
		;;
	+p[0-9]*)
		pes="${1#\+p}"
		;;
	++p[0-9]*)
		pes="${1#\+\+p}"
		;;
	+pemap)
		args+=("$1" "$2")
		shift
		;;
	-machinefile)
		machinefile="$2"
		args=("$1" "$2" "${args[@]}")
		shift
		;;
	++quiet)
		QUIET=1
		;;
	++no-quiet)
		QUIET=0
		;;
	++local|++no-local)
		# ignore
		;;
	++xterm)
		XTERM="$2"
		shift
		;;
	++debug)
		DEBUG=1
		;;
	++no-debug)
		DEBUG=0
		;;
	++debug-no-pause)
		DEBUG_NO_PAUSE=1
		;;
	++no-debug-no-pause)
		DEBUG_NO_PAUSE=0
		;;
	++debugger)
		DEBUGGER="$2"
		shift
		;;
	*)
		args+=("$1")
		;;
	esac
	shift
done

[[ $QUIET -eq 1 ]] && charm_args+=(++quiet)

args+=("${charm_args[@]}")

if [[ "$DEBUG" = '1' || "$DEBUG_NO_PAUSE" = '1' ]]
then
  if [[ -z "$DEBUGGER" ]]
  then
    [[ "$(uname -s 2>/dev/null)" = 'Darwin' ]] && DEBUGGER='lldb' || DEBUGGER='gdb'
  fi

  DEBUG_RUN=(-ex r)
  DEBUG_POSTFIX='--args'
  if [[ "$DEBUGGER" = 'lldb' ]]
  then
    DEBUG_RUN=(-o r)
    DEBUG_POSTFIX='--'
  fi

  args=("$DEBUG_POSTFIX" "${args[@]}")
  [[ "$DEBUG_NO_PAUSE" = '1' ]] && args=("${DEBUG_RUN[@]}" "${args[@]}")
  args=("$XTERM" -e "$DEBUGGER" "${args[@]}")
fi

if [[ $nodes = 0 ]]
then
  [[ $pes = 0 ]] && pes=1
  if [[ $(( pes % ppn )) -ne 0 ]]
  then
    printf "Charmrun> Error: ++ppn %s (number of PEs per node) does not divide +p %s (number of PEs).\n" "$ppn" "$pes"
    exit 1
  else
    nodes=$(( pes / ppn ))
  fi
elif [[ $pes != 0 && $pes != $(( nodes * ppn )) ]]
then
  printf "Charmrun> Error: +n/++np %s times ++ppn %s does not equal +p %s.\n" "$nodes" "$ppn" "$pes"
  exit 1
fi

[[ $QUIET -eq 0 ]] && echo && echo "Running as $nodes OS processes: ${args[*]}"

if [[ -n "$PBS_NODEFILE" ]]
then
# we are in a job shell
  aprun="$(command -v aprun 2>/dev/null)"
  if [[ -n "$aprun" ]]
  then
    runCmd "$aprun" -n "$nodes" "${args[@]}"
  else
    mpirun_cmd="$(command -v mpirun 2>/dev/null)"
    if [[ -n "$mpirun_cmd" ]]
    then
      if echo "$mpirun_cmd" | grep 'mvapich2' >/dev/null 2>&1
      then
        # if daemon not started, start it
        if ! mpdtrace >/dev/null 2>&1
        then
          mvapich2-start-mpd
        fi
        runCmd mpirun -np "$nodes" "${args[@]}"
        #    mpdallexit
      else   # normal case
        [[ -z "$machinefile" ]] && args=(-machinefile "$PBS_NODEFILE" "${args[@]}")
        runCmd mpirun -np "$nodes" "${args[@]}"
      fi
    else
      echo "Charmrun> can not locate mpirun in order to run the program."
      exit 1
    fi
  fi
elif [[ -n "$LSB_HOSTS" ]]
then
# Tungsten
    runCmd cmpirun -lsf -poll -no_smp -gm_long 200000 "${args[@]}"
elif [[ -n "$SLURM_JOB_ID" ]]
then
    #use srun
    # srun in a cray-shasta environment should support --mpi=cray-shasta regardless, the question here is how this charm was built
    # assume built with craype  if that is loaded
    craype=`module -t list craype 2>&1 | grep craype`
    if [[ $SLURM_JOB_NUM_NODES -eq 1 ]]
    then
	NET_ARGS="--network=single_node_vni"
    else
	if [[ $pes -eq 1 ]]
	then
	    NET_ARGS="--network=single_node_vni"
	else
	    if [[ $nodes -eq 1 ]]
	    then
		NET_ARGS="--network=single_node_vni"
	    else
		NET_ARGS=""
	    fi
	fi
    fi

    if [[ $? -eq 0 ]]
    then
	runCmd srun --mpi=cray_shasta $NET_ARGS -n "$nodes" "${args[@]}"
    else
	#someday this should be pmix, but our pmix launcher needs some
	# work and cray hasn't really adopted PMIX at this time.
	# unclear what the right answer is in other OFI environments
	runCmd srun --mpi=pmi2 -n "$nodes" "${args[@]}"
    fi
elif [[ -n "$PBS_QUEUE" || -n "$LSF_QUEUE" ]]
then
# Interactive mode: create, and submit a batch job
        script="charmrun_script.$$.sh"
        indir="$(pwd)"
        output="$indir/charmrun_script.$$.stdout"
        result="$indir/charmrun_script.$$.result"
	rm -f "$result"
# Some machine specific 
	USE_LSF=0
# 10 minutes	
	walllimit=10
	queue_stat=qstat
	queue_qsub=qsub
	queue_kill=qdel
	hostname="$(hostname)"
	extra=()
	case "$hostname" in
	turing*.turing.uiuc.edu) 
		ppn='#PBS -l nodes='$nodes':ppn=1'
		extra=("-machinefile" "\$PBS_NODEFILE")
		;;
	tg-login*|honest*.ncsa.uiuc.edu)
		# always ppn=2
		nodes=$(( ( nodes + 1 ) / 2 ))
		[[ $nodes -eq 1 ]] && ppns=1 || ppns=2
		ppn='#PBS -l nodes='$nodes':ppn='$ppns
		extra=("-machinefile" "\$PBS_NODEFILE")
		;;
	co-login*.ncsa.uiuc.edu)
		mem='#PBS -l mem=500mb'
		ncpus="#PBS -l ncpus=$nodes"
		;;
	tun*)
		USE_LSF=1
		queue_stat=bjobs
		queue_qsub=bsub
		queue_kill=bkill
		;;
	abe*)
		# always ppn=2
		nodes=$(( ( nodes + 1 ) / 2 ))
		[[ $nodes -eq 1 ]] && ppns=1 || ppns=2
		ppn='#PBS -l nodes='$nodes':ppn='$ppns
		extra=("-machinefile" "\$PBS_NODEFILE")
		;;
        kraken*)
                ncores=$(( ( nodes + 11 ) / 12 * 12 ))
		ncpus="#PBS -l size=$ncores"
		ppn=''
		;;
	*)
		ncpus="#PBS -l ncpus=$nodes"
		;;
	esac
	if [[ "$USE_LSF" = '0' ]]
	then
          mpirun="$(command -v aprun 2>/dev/null)"
          npcmd="-n "
          if [[ -z "$mpirun" ]]
          then
	    mpirun="$(command -v mpirun 2>/dev/null)"
            npcmd="-np "
            if [[ -z "$mpirun" ]]
            then
              mpirun="$(command -v srun 2>/dev/null)"
              npcmd="-n "
            fi
          fi
          cat > $script << EOF
#!/bin/sh
# This is a charmrun-generated PBS batch job script.
# The lines starting with #PBS are queuing system flags:
#
$ppn
#
$ncpus
#
#PBS -l walltime=$walllimit:00
#
$mem
#
#PBS -q $PBS_QUEUE
#
#PBS -N autobuild
#
#PBS -j oe
#
#PBS -o $output

cd $indir

cat \$PBS_NODEFILE
echo $mpirun $npcmd $nodes ${extra[@]} ${args[@]}
$mpirun $npcmd $nodes ${extra[@]} ${args[@]}

# Save mpirun exit status
status=\$?
echo \$status > $result
EOF
	else
#  use LSF
	  mpirun="cmpirun -lsf -poll -no_smp -gm_long 200000"
          cat > $script << EOF
#!/bin/sh
# This is a charmrun-generated PBS batch job script.
# The lines starting with #PBS are queuing system flags:
#
#BSUB -J autobuild
#BSUB -W 0:$walllimit
#BSUB -n $nodes
#BSUB -o $output

cd $indir
echo \$LSB_MCPU_HOSTS
$mpirun ${args[@]}
# Save mpirun exit status
status=\$?
echo \$status > $result
EOF
	fi

End() {
	echo "Charmrun> $queue_kill $jobid ..."
	$queue_kill $jobid
	rm -f $script
	exit $1
}

        echo "Submitting batch job for> $mpirun -np $nodes ${args[*]}"
        echo " using the command> $queue_qsub $script"
        chmod 755 $script
	while [[ -z "$jobid" ]]
	do
	  [[ "$USE_LSF" = '0' ]] && jobid=$($queue_qsub $script|tail -1)
	  [[ "$USE_LSF" = '1' ]] && jobid=$($queue_qsub < $script|tail -1|sed -e 's/[^0-9]*//g')
	done
	echo "Job enqueued under job ID $jobid"
# kill job if interrupted
	trap 'End 1' 2 3
	retry=0
# Wait for the job to complete, by checking its status
        while [[ true ]]
        do
                $queue_stat $jobid > tmp.$$
		exitstatus=$?
                if [[ -f "$output" ]]
                then
# The job is done-- print its output
                        rm tmp.$$
# When job hangs, result file does not exist
			[[ -f "$result" ]] && status="$(cat "$result")" || status=1
			[[ $status -eq 0 ]] && status="$(grep 'End of program' "$output" > /dev/null 2>&1)"
			cat "$output"
			rm -f "$result"
			[[ -f "$status" ]] && rm -f "$script" "$output"
			exit $status
                fi
# The job is still queued or running-- print status and wait
                tail -1 tmp.$$
                rm tmp.$$
# Job ID may not exist now
		if [[ $exitstatus -ne 0 ]]
		then
# retry a few times when error occurs
			retry=$(( retry + 1 ))
			if [[ $retry -gt 6 ]]
			then
				echo "Charmrun> too many errors, abort!"
				exit 1
			else
				sleep 15
			fi
		else
# job still in queue
			retry=0
                	sleep 20
		fi
        done
elif [[ -n "$(command -v mpirun 2>/dev/null)" ]]
then
  mpirun_cmd="$(command -v mpirun 2>/dev/null)"
  if [[ -n "$mpirun_cmd" ]]
  then
    [[ -n "$MPI_MACHINEFILE" ]] && args=(-machinefile "$MPI_MACHINEFILE" "${args[@]}")
    setarch_cmd="$(command -v setarch 2>/dev/null)"
    if [[ -n "$setarch_cmd" && -x "$setarch_cmd" ]]
    then
      # Disables randomization of the virtual address  space  (turns  on
      #          ADDR_NO_RANDOMIZE).
      cur_arch="$(uname -m)"
      runCmd "$setarch_cmd" "$cur_arch" -R mpirun -np "$nodes" "${args[@]}"
    else
      runCmd mpirun -np "$nodes" "${args[@]}"
    fi
  else
    mpiexec_cmd="$(command -v mpiexec 2>/dev/null)"
    if [[ -n "$mpiexec_cmd" ]]
    then
      runCmd "$mpiexec_cmd" -n "$nodes" "${args[@]}"
    else
      echo "Don't know how to run MPI program."
      exit 1
    fi
  fi
elif [[ -n "$(command -v srun 2>/dev/null)" ]]
then
  #mpirun is checked before srun to support the Bridges supercomputer at PSC
  #as srun has a known issue and fails to successfully launch the parallel job.
  #This is required to run the nightly ofi autobuild.
  runCmd srun -n "$nodes" -c $(( ppn + 1 )) "${args[@]}"
else
  echo "No job launcher found! (tried aprun, mpirun and srun)"
  exit 1
fi
