// To run: gcc -O3 step4_correct_avg_speed_by_hour.c && time ./a.out | tee >(md5sum)
//
// The corrected computation for the average moving speed by hour.
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "schema.h"

struct PerHourCounter {
  double total_miles;
  double total_seconds;
};
  
struct PerHourCounter per_hour_counters[24];  // Zero-initialized as global. -- D.K.

void UpdateAverageSpeed(struct PerHourCounter* self, double miles, double seconds) {
  self->total_miles+= miles;
  self->total_seconds += seconds;
}

double ComputeAverageSpeed(const struct PerHourCounter* self) {
  return self->total_seconds ? (60.0 * 60.0 * self->total_miles / self->total_seconds) : 0.0;
}

void Run(const struct Ride* data, size_t n) {
  int total_considered = 0;
  for (size_t i = 0; i < n; ++i) {
    const int trip_duration_seconds = data[i].dropoff.epoch - data[i].pickup.epoch;
    if (data[i].trip_distance > 0 && trip_duration_seconds > 0) {
      const double cost_per_mile = data[i].fare_amount / data[i].trip_distance;
      if (cost_per_mile >= 2 && cost_per_mile <= 10) {
        ++total_considered;
        UpdateAverageSpeed(&per_hour_counters[data[i].pickup.hour], data[i].trip_distance, trip_duration_seconds);
      }
    }
  }
  if (total_considered) {
    for (int hour = 0; hour < 24; ++hour) {
      printf("%02d\t%.2lf\n", hour, ComputeAverageSpeed(&per_hour_counters[hour]));
    }
    fprintf(stderr, "Total rides considered: %d (%.1lf%%)\n", total_considered, 100.0 * total_considered / n);
  }
}

int main(int argc, char** argv) {
  const char* filename = argc >= 2 ? argv[1] : "../cooked.bin";

  struct stat st;
  stat(filename, &st);
  const size_t length = st.st_size;

  const int fd = open(filename, O_RDONLY, 0);
  if (fd == -1) {
    return -1;
  }

  char* readonly_buffer = (char*)(mmap(NULL, length, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0));
  if (readonly_buffer == MAP_FAILED) {
    return -1;
  }

  if (!((length % sizeof(struct Ride)) == 0)) {
    fprintf(stderr, "Wrong file size.\n");
    return -1;
  }

  Run((const struct Ride*)readonly_buffer, length / (sizeof(struct Ride)));

  munmap((void*)readonly_buffer, length);
  close(fd);
}
