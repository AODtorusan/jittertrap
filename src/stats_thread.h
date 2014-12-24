#ifndef STATS_THREAD_H
#define STATS_THREAD_H

/* Sleep time in milliseconds */
#define SAMPLE_PERIOD_MS 10

struct byte_counts {
  long long timestamp;
  long long rx_bytes;
  long long tx_bytes;
  int rx_bytes_delta;
  int tx_bytes_delta;
};


int stats_thread_init(void (*stats_handler)(struct byte_counts *counts));
void stats_monitor_iface(const char *_iface);

#endif
