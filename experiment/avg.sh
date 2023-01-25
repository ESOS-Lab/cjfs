cat /var/log/syslog | grep t_conflict_count | awk 'BEGIN{TOTAL=0;COUNT=0} {TOTAL+=$11;COUNT+=1.0}END{printf"%f/%f => %.3f\n",TOTAL,COUNT,TOTAL/COUNT}'

echo > /var/log/syslog
