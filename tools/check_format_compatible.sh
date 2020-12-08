#!/usr/bin/env bash
# Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
#
# A shell script to load some pre generated data file to a DB using ldb tool
# ./ldb needs to be avaible to be executed.
#
# Usage: <SCRIPT> [checkout]
# `checkout` can be a tag, commit or branch name. Will build using it and check DBs generated by all previous branches (or tags for very old versions without branch) can be opened by it.
# Return value 0 means all regression tests pass. 1 if not pass.

scriptpath=`dirname $BASH_SOURCE`
test_dir=${TEST_TMPDIR:-"/tmp"}"/format_compatible_check"
script_copy_dir=$test_dir"/script_copy"
input_data_path=$test_dir"/test_data_input/"

python_bin=$(which python3 || which python || echo python3)

mkdir $test_dir || true
mkdir $input_data_path || true
rm -rf $script_copy_dir
cp $scriptpath $script_copy_dir -rf

# Generate random files.
for i in {1..6}
do
  input_data[$i]=$input_data_path/data$i
  echo == Generating random input file ${input_data[$i]}
  $python_bin - <<EOF
import random
random.seed($i)
symbols=['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9']
with open('${input_data[$i]}', 'w') as f:
  for i in range(1,1024):
    k = ""
    for j in range(1, random.randint(1,32)):
      k=k + symbols[random.randint(0, len(symbols) - 1)]
    vb = ""
    for j in range(1, random.randint(0,128)):
      vb = vb + symbols[random.randint(0, len(symbols) - 1)]
    v = ""
    for j in range(1, random.randint(1, 5)):
      v = v + vb
    print(k + " ==> " + v, file=f)
EOF
done

# Generate file(s) with sorted keys.
sorted_input_data=$input_data_path/sorted_data
echo == Generating file with sorted keys ${sorted_input_data}
$python_bin - <<EOF
with open('${sorted_input_data}', 'w') as f:
  for i in range(0,10):
    k = str(i)
    v = "value" + k
    print(k + " ==> " + v, file=f)
EOF

declare -a backward_compatible_checkout_objs=("2.2.fb.branch" "2.3.fb.branch" "2.4.fb.branch" "2.5.fb.branch" "2.6.fb.branch" "2.7.fb.branch" "2.8.1.fb" "3.0.fb.branch" "3.1.fb" "3.2.fb" "3.3.fb" "3.4.fb" "3.5.fb" "3.6.fb" "3.7.fb" "3.8.fb" "3.9.fb" "4.2.fb" "4.3.fb" "4.4.fb" "4.5.fb" "4.6.fb" "4.7.fb" "4.8.fb" "4.9.fb" "4.10.fb" "4.11.fb" "4.12.fb" "4.13.fb" "5.0.fb" "5.1.fb" "5.2.fb" "5.3.fb" "5.4.fb" "5.5.fb" "5.6.fb" "5.7.fb" "5.8.fb" "5.9.fb" "5.10.fb" "5.11.fb" "5.12.fb" "5.13.fb" "5.14.fb" "5.15.fb")
declare -a forward_compatible_checkout_objs=() # N/A at the moment
declare -a forward_compatible_with_options_checkout_objs=("5.16.fb" "5.17.fb" "5.18.fb" "6.0.fb" "6.1.fb" "6.2.fb" "6.3.fb" "6.4.fb" "6.5.fb" "6.6.fb" "6.7.fb" "6.8.fb" "6.9.fb" "6.10.fb" "6.11.fb" "6.12.fb" "6.13.fb" "6.14.fb" "6.15.fb")
declare -a checkout_objs=(${backward_compatible_checkout_objs[@]} ${forward_compatible_checkout_objs[@]} ${forward_compatible_with_options_checkout_objs[@]})
declare -a extern_sst_ingestion_compatible_checkout_objs=("5.16.fb" "5.17.fb" "5.18.fb" "6.0.fb" "6.1.fb" "6.2.fb" "6.3.fb" "6.4.fb" "6.5.fb" "6.6.fb" "6.7.fb" "6.8.fb" "6.9.fb" "6.10.fb" "6.11.fb" "6.12.fb" "6.13.fb" "6.14.fb" "6.15.fb")

generate_db()
{
    set +e
    $script_copy_dir/generate_random_db.sh $1 $2
    if [ $? -ne 0 ]; then
        echo ==== Error loading data from $2 to $1 ====
        exit 1
    fi
    set -e
}

compare_db()
{
    set +e
    $script_copy_dir/verify_random_db.sh $1 $2 $3 $4 $5
    if [ $? -ne 0 ]; then
        echo ==== Read different content from $1 and $2 or error happened. ====
        exit 1
    fi
    set -e
}

write_external_sst()
{
    set +e
    $script_copy_dir/write_external_sst.sh $1 $2 $3
    if [ $? -ne 0 ]; then
        echo ==== Error writing external SST file using data from $1 to $3 ====
        exit 1
    fi
    set -e
}

ingest_external_sst()
{
    set +e
    $script_copy_dir/ingest_external_sst.sh $1 $2
    if [ $? -ne 0 ]; then
        echo ==== Error ingesting external SST in $2 to DB at $1 ====
        exit 1
    fi
    set -e
}

# Sandcastle sets us up with a remote that is just another directory on the same
# machine and doesn't have our branches. Need to fetch them so checkout works.
# Remote add may fail if added previously (we don't cleanup).
git remote add github_origin "https://github.com/facebook/rocksdb.git"
set -e
git fetch github_origin

# Compatibility test for external SST file ingestion
for checkout_obj in "${extern_sst_ingestion_compatible_checkout_objs[@]}"
do
  echo == Generating DB with extern SST file in "$checkout_obj" ...
  git checkout github_origin/$checkout_obj -b $checkout_obj
  make clean
  DISABLE_WARNING_AS_ERROR=1 make ldb -j32
  write_external_sst $input_data_path $test_dir/$checkout_obj $test_dir/$checkout_obj
  ingest_external_sst $test_dir/$checkout_obj $test_dir/$checkout_obj
done

checkout_flag=${1:-"master"}

echo == Building $checkout_flag debug
git checkout github_origin/$checkout_flag -b tmp-$checkout_flag
make clean
DISABLE_WARNING_AS_ERROR=1 make ldb -j32
compare_base_db_dir=$test_dir"/base_db_dir"
write_external_sst $input_data_path $compare_base_db_dir $compare_base_db_dir
ingest_external_sst $compare_base_db_dir $compare_base_db_dir

for checkout_obj in "${extern_sst_ingestion_compatible_checkout_objs[@]}"
do
  echo == Build "$checkout_obj" and try to open DB generated using $checkout_flag
  git checkout $checkout_obj
  make clean
  DISABLE_WARNING_AS_ERROR=1 make ldb -j32
  compare_db $test_dir/$checkout_obj $compare_base_db_dir db_dump.txt 1 1
  git checkout tmp-$checkout_flag
  # Clean up
  git branch -D $checkout_obj
done

echo == Finish compatibility test for SST ingestion.

for checkout_obj in "${checkout_objs[@]}"
do
   echo == Generating DB from "$checkout_obj" ...
   git checkout github_origin/$checkout_obj -b $checkout_obj
   make clean
   DISABLE_WARNING_AS_ERROR=1 make ldb -j32
   generate_db $input_data_path $test_dir/$checkout_obj
done

checkout_flag=${1:-"master"}

echo == Building $checkout_flag debug
git checkout tmp-$checkout_flag
make clean
DISABLE_WARNING_AS_ERROR=1 make ldb -j32
compare_base_db_dir=$test_dir"/base_db_dir"
echo == Generate compare base DB to $compare_base_db_dir
generate_db $input_data_path $compare_base_db_dir

for checkout_obj in "${checkout_objs[@]}"
do
   echo == Opening DB from "$checkout_obj" using debug build of $checkout_flag ...
   compare_db $test_dir/$checkout_obj $compare_base_db_dir db_dump.txt 1 0
done

for checkout_obj in "${forward_compatible_checkout_objs[@]}"
do
   echo == Build "$checkout_obj" and try to open DB generated using $checkout_flag...
   git checkout $checkout_obj
   make clean
   DISABLE_WARNING_AS_ERROR=1 make ldb -j32
   compare_db $test_dir/$checkout_obj $compare_base_db_dir forward_${checkout_obj}_dump.txt 0
done

for checkout_obj in "${forward_compatible_with_options_checkout_objs[@]}"
do
   echo == Build "$checkout_obj" and try to open DB generated using $checkout_flag with its options...
   git checkout $checkout_obj
   make clean
   DISABLE_WARNING_AS_ERROR=1 make ldb -j32
   compare_db $test_dir/$checkout_obj $compare_base_db_dir forward_${checkout_obj}_dump.txt 1 1
done

echo ==== Compatibility Test PASSED ====
