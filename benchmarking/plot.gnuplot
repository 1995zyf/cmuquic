#!/usr/bin/gnuplot -p
# "table" be generated by tablify_ifstats.sh
set term png
set output "plot.png"
set logscale y
set xlabel "Transmission size parameter (approx. number of packets)"
set ylabel "Average bandwidth achieved (kilobits per second)"
plot 'table' \
        using 2:xtic(1) title 'Apache' with lines, \
     '' using 3:xtic(1) title 'DPDK/DPDK' with lines, \
     '' using 4:xtic(1) title 'DPDK/Linux' with lines, \
     '' using 5:xtic(1) title 'Linux/DPDK' with lines, \
     '' using 6:xtic(1) title 'Linux/Linux' with lines, \
