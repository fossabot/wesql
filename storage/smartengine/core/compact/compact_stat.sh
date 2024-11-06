#cat /tmp/xdump.sh
#!/bin/bash

RED="\e[1;31m"
BLUE="\e[1;34m"
GREEN="\e[1;32m"
RED_BLINK="\e[1;31;5m"
GREEN_BLINK="\e[1;32;5m"
NC="\e[0m"

get_key_value() {
  echo "$1" | sed 's/^--[a-zA-Z_-]*=//'
}

usage() {
cat <<EOF
usage: $0 [-t frag|compact_sk|show_sk] [-i input_stats_file]
   or: $0 [-h | --help]

 -t command
    frag                calc fragment of extents;
    show_sk             show secondary key's extents status;
    compact_sk          compact all secondary key;
 -i input_stats_file    use input_stats_file instead of fetch again
 -h                     Show this help message.
EOF
}

parse_options() {
  while test $# -gt 0 ; do
    case "$1" in
    -t=*)
      command_type=`get_key_value "$1"`;;
    -t)
      shift
      command_type=`get_key_value "$1"`;;
    -i=*)
      input_stats_file=`get_key_value "$1"`;;
    -i)
      shift
      input_stats_file=`get_key_value "$1"`;;
    -h | --help)
      usage
      exit 0;;
    *)
      echo "Unknown option '$1'"
      exit 1;;
    esac
    shift
  done

}

# total_size: select sum(DATA), sum(INDEX), sum(EXTENTS) from information_schema.SMARTENGINE_SUBTABLE;
# l0_size: select "l0_size", sum(DATA), sum(SMARTENGINE_SUBTABLE.INDEX), sum(EXTENTS) from information_schema.SMARTENGINE_SUBTABLE where LEVEL=0;
# l1_size: select "l1_size", sum(DATA), sum(SMARTENGINE_SUBTABLE.INDEX), sum(EXTENTS) from information_schema.SMARTENGINE_SUBTABLE where LEVEL=1;
# l2_size: select "l2_size", sum(DATA), sum(SMARTENGINE_SUBTABLE.INDEX), sum(EXTENTS) from information_schema.SMARTENGINE_SUBTABLE where LEVEL=2;
# 内外部碎片率计算，每层数据size和碎片率计算
calc_frag() {
  if [ ${input_stats_file}'x' == 'x' ]; then
    sudo rm -rf ${cf_size}
    ${mysql} -uroot -S /u01/my3306/run/mysql.sock -e 'select "total_size", sum(DATA), sum(SMARTENGINE_SUBTABLE.INDEX), sum(EXTENTS) from information_schema.SMARTENGINE_SUBTABLE; select "l0_size", sum(DATA), sum(SMARTENGINE_SUBTABLE.INDEX), sum(EXTENTS) from information_schema.SMARTENGINE_SUBTABLE where LEVEL=0; select "l1_size", sum(DATA), sum(SMARTENGINE_SUBTABLE.INDEX), sum(EXTENTS) from information_schema.SMARTENGINE_SUBTABLE where LEVEL=1;select "l2_size", sum(DATA), sum(SMARTENGINE_SUBTABLE.INDEX), sum(EXTENTS) from information_schema.SMARTENGINE_SUBTABLE where LEVEL=2;' > ${cf_size}

    input_stats_file=${cf_size}
  fi

  sst_num=`sudo find /u01/my3306/data/smartengine/ -name "*.sst" | wc -l`
  #echo "sst file: $sst_num"
  printf "${RED}sst file num:${NC} %s\n" "${sst_num}"
  grep -E "total_size" ${input_stats_file} | grep -v "sum" | awk -v sst_num="${sst_num}" 'BEGIN{total_data=0;total_index=0;total_size=0;total_extent=0;} \
      { total_data+=$2/1024/1024; \
        total_index+=$3/1024/1024; \
        total_extent+=$4;\
        total_size+=total_extent*2;\
      } \

      END{ print "\033[34mAll:\033[0m TotalSize:", int(total_size), "MB TotalData:", int(total_data), "MB TotalIndex:", int(total_index), "MB"; \
      print "   TotalExtent:", int(total_extent), " Real SST:", sst_num, "\033[31m SST usage=\033[0m ", (total_extent+511)/512*100/sst_num, "%"; \
      print "   \033[31m(Data + Index) / Size = \033[0m", 100*(total_data+total_index)/total_size, "%"  }'
  grep "l0_size" ${input_stats_file} | grep -v "sum" | awk 'BEGIN{total_data=0;total_index=0;total_size=0;total_extent=0;} \
      { total_data+=$2/1024/1024; \
        total_index+=$3/1024/1024; \
        total_extent+=$4;\
        total_size+=total_extent*2;\
      } \

          END{ print "\033[34m L0:\033[0m TotalSize:", int(total_size), "MB TotalData:", int(total_data), "MB TotalIndex:", int(total_index), "MB"; \
      print "   \033[31mExtent: \033[0m", int(total_extent); \
      print "   \033[31m(Data + Index) / Size = \033[0m", 100*(total_data+total_index)/total_size, "%"  }'
  grep "l1_size" ${input_stats_file} | grep -v "sum" | awk 'BEGIN{total_data=0;total_index=0;total_size=0;total_extent=0;} \
      { total_data+=$2/1024/1024; \
        total_index+=$3/1024/1024; \
        total_extent+=$4;\
        total_size+=total_extent*2;\
      } \
            END{ print "\033[34m L1:\033[0m TotalSize:", int(total_size), "MB TotalData:", int(total_data), "MB TotalIndex:", int(total_index), "MB"; \
      print "   \033[31mExtent: \033[0m", int(total_extent); \
      print "   \033[31m(Data + Index) / Size = \033[0m", 100*(total_data+total_index)/total_size, "%"  }'
  grep "l2_size" ${input_stats_file} | grep -v "sum" | awk 'BEGIN{total_data=0;total_index=0;total_size=0;total_extent=0;} \
      { total_data+=$2/1024/1024; \
        total_index+=$3/1024/1024; \
        total_extent+=$4;\
        total_size+=total_extent*2;\
      } \

            END{ print "\033[34m L2:\033[0m TotalSize:", int(total_size), "MB TotalData:", int(total_data), "MB TotalIndex:", int(total_index), "MB"; \
      print "   \033[31mExtent: \033[0m", int(total_extent); \
      print "   \033[31m(Data + Index) / Size = \033[0m", 100*(total_data+total_index)/total_size, "%"  }'
}

#  INVALID_TYPE_TASK = 0,
#  FLUSH_TASK = 1,
#  // There are many kinds of Compaction types.
#  // Each type has specified input and output level
#  INTRA_COMPACTION_TASK = 2,                  // L0 layers intra compaction
#  MINOR_COMPACTION_TASK = 3,                  // L0 compaction to L1 (FPGA)
#  SPLIT_TASK = 4,                             // L1 split extent to L1
#  MAJOR_COMPACTION_TASK = 5,                  // L1 compaction to L2
#  MAJOR_SELF_COMPACTION_TASK = 6,             // L2 compaction to L2
#  BUILD_BASE_INDEX_TASK = 7,                 //build base index, append data to level2
#  //
#  // Note we should resever these orders to be backwords compatible.
#  STREAM_COMPACTION_TASK = 8,                 // L0 compaction to L1
#  MINOR_DELETE_COMPACTION_TASK = 9,           // L0 compaction to L1 (Delete triggered)
#  MAJOR_DELETE_COMPACTION_TASK = 10,           // L1 compaction to L2 (Delete triggered)
#  MAJOR_AUTO_SELF_COMPACTION_TASK = 11,       // L2 compaction to L2 (Auto triggered with time period)
#  MANUAL_MAJOR_TASK = 12,                     // L1 compaction to L2 (manual trigger, all extents to L2)
#  MANUAL_FULL_AMOUNT_TASK = 13,               // compact all extents to level2
#  SHRINK_EXTENT_SPACE_TASK = 14,              // reclaim the free extent space
#  DELETE_MAJOR_SELF_TASK = 15,                // compact all delete records in level2
#  FLUSH_LEVEL1_TASK = 16,                     // flush memtables to level1
#  MINOR_AUTO_SELF_TASK = 17,                  // level1 compaction to level1
#  DUMP_TASK = 18,                             // dump memtable to M0
#  SWITCH_M02L0_TASK = 19,                     // switch M0 to L0
#  MAX_TYPE_TASK
#  set global smartengine_compact=cf_id+(task_type << 32)

compact_sk() {
  ${mysql} -uroot -S /u01/my3306/run/mysql.sock -e "select COLUMN_FAMILY from information_schema.se_ddl where index_type=2" > ${cf_file}
  cnt=0
  while read line ; do
    cnt=$((cnt+1))
    echo -e "Pharse 1: ${cnt} cf=${line}\n"
    ${mysql} -uroot -S /u01/my3306/run/mysql.sock -e "set global smartengine_compact=${line}+(13 << 32)";

    #echo `sed -n "/${line}/{n;n;n;n;n;n;n;p}" $cf_file`
    #echo -n `grep -A 5 -nr "\[${line}\]" $cf_file`
  done < ${cf_file}

  sleep 1800s
  #sleep 3600s
  cnt=0
  while read line ; do
    cnt=$((cnt+1))
    echo -e "Pharse 2: ${cnt} cf=$line\n"
    ${mysql} -uroot -S /u01/my3306/run/mysql.sock -e "set global smartengine_compact=${line}+(6<<32)";
  done < ${cf_file}
}

show_sk() {
  if [ ! -f "$cf_stats" ]; then
    sudo rm -rf ${cf_stats}
    ${mysql} -uroot -S /u01/my3306/run/mysql.sock -e 'select * from information_schema.smartengine_subtable' > ${cf_stats}
  fi
  ${mysql} -uroot -S /u01/my3306/run/mysql.sock -e "select COLUMN_FAMILY from information_schema.smartengine_ddl where index_type=2" > ${cf_file}
  cnt=0
  while read line ; do
    cnt=$((cnt+1))
    echo -e "${cnt}: cf_id=${line}"
    #grep ${line} $cf_file
    #echo -e `grep  ${line} $cf_file`
    echo "`sed -n "/${line}/{n;n;n;n;n;N;N;p}" "$cf_stats"`"
    #echo `sed -n "/${line}/{n;n;n;n;n;p;n;p;n;p}" $cf_stats`
    #echo -n `grep -A 5 -nr "\[${line}\]" $cf_file`
    #echo -n `grep -A 5 -nr "\[${line}\]" $cf_file`
  done < ${cf_file}
  exit
}

command_type="frag"
cf_file="/tmp/cf0"
cf_size="/tmp/cf_size"
cf_stats="/tmp/cf_stats"
mysql="/u01/mysql/bin/mysql"

parse_options "$@"

if [ ${command_type} == "frag" ]; then
   calc_frag
elif [ ${command_type} == "compact_sk" ]; then
   compact_sk
elif [ ${command_type} == "show_sk" ]; then
   show_sk
fi
