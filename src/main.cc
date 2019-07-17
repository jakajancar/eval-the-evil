#include <iostream>
#include <thread>
#include <boost/program_options.hpp>
#include <boost/asio.hpp>
#include "error-handling.h"
#include "evaluation.h"
#include "timer.h"
#include <cmath>

namespace po = boost::program_options;
using boost::asio::ip::tcp;

uint64_t clock_gettime_nanos(clockid_t clockid)
{
  struct timespec time;
  if (clock_gettime(clockid, &time) != 0)
    throw_with_trace(std::system_error(errno, std::generic_category(), "Cannot get time"));
  return time.tv_sec * 1e9 + time.tv_nsec;
}

static inline timespec duration_to_timespec(std::chrono::nanoseconds duration)
{
    using namespace std::chrono;
    seconds secs = duration_cast<seconds>(duration);
    duration -= secs;
    return timespec{secs.count(), duration.count()};
}

double bench_timer_thread_id(uint64_t timeout, int num_iterations, clockid_t clockid, bool use_mutex)
{
  uint64_t accumulator = 0;

  for (int i = 0; i < num_iterations; i++) {
    std::mutex stop_mutex;
    bool stop = false;

    auto timer_cb = [&]{
      if (use_mutex) {
        std::lock_guard<std::mutex> lock(stop_mutex);
        stop = true;
      } else {
        stop = true;
      }
    };
    eval::Timer<decltype(timer_cb)> timer(std::chrono::nanoseconds(timeout), clockid, timer_cb);
    uint64_t start = clock_gettime_nanos(clockid);
    while (true) {
      if (use_mutex) {
        std::lock_guard<std::mutex> lock(stop_mutex);
        if (stop)
          break;
      } else {
        if (stop)
          break;
      }
    }
    uint64_t end = clock_gettime_nanos(clockid);
    uint64_t duration = end - start;
    int64_t overshoot = duration - timeout;
    accumulator += std::abs(overshoot);
    // printf("overshoot = %f ms\n", (double)overshoot / 1e6);
  }
  return (double)accumulator/num_iterations;
}

double bench_timer_thread_id_mutex(uint64_t timeout, int num_iterations, clockid_t clockid)
{
  return bench_timer_thread_id(timeout, num_iterations, clockid, true);
}

double bench_timer_thread_id_nomutex(uint64_t timeout, int num_iterations, clockid_t clockid)
{
  return bench_timer_thread_id(timeout, num_iterations, clockid, false);
}

bool stop = false; // unsafe
void bench_timer_signal_handler(int signo)
{
  stop = true;
}

double bench_timer_signal(uint64_t timeout, int num_iterations, clockid_t clockid)
{
  uint64_t accumulator = 0;

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = &bench_timer_signal_handler;
  sigaction(SIGUSR1, &action, NULL);

  for (int i = 0; i < num_iterations; i++) {
    stop = false;

    // Timer
    struct sigevent event;
    memset(&event, 0, sizeof(event));
    event.sigev_notify = SIGEV_SIGNAL;
    event.sigev_signo = SIGUSR1;

    timer_t timer;
    if (timer_create(clockid, &event, &timer) != 0) {
      std::cout << "timer_create failed\n";
      abort();
    }

    struct itimerspec timing;
    memset(&timing, 0, sizeof(timing));
    timing.it_value = duration_to_timespec(std::chrono::nanoseconds(timeout));
    if (timer_settime(timer, 0, &timing, NULL) != 0) {
      std::cout << "timer_settime failed\n";
      abort();
    }

    uint64_t start = clock_gettime_nanos(clockid);
    while (true) {
      if (stop)
        break;
    }

    uint64_t end = clock_gettime_nanos(clockid);
    if (timer_delete(timer) != 0) {
      std::cout << "timer_delete failed\n";
      abort();
    }
    uint64_t duration = end - start;
    int64_t overshoot = duration - timeout;
    accumulator += std::abs(overshoot);
    // printf("overshoot = %f ms\n", (double)overshoot / 1e6);
  }
  return (double)accumulator/num_iterations;
}

typedef double(*bench_method_ptr)(uint64_t, int, clockid_t);

int main(int argc, char *argv[])
{
  GlobalErrorHandler eh;

  int num_clocks = 4;
  clockid_t clock_ids[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID};
  const char *clock_names[] = {"CLOCK_REALTIME", "CLOCK_MONOTONIC", "CLOCK_PROCESS_CPUTIME_ID", "CLOCK_THREAD_CPUTIME_ID"};

  int num_methods = 3;
  bench_method_ptr method_ptrs[] = {
    &bench_timer_thread_id_mutex,
    &bench_timer_thread_id_nomutex,
    &bench_timer_signal};
  const char *method_names[] = {
    "bench_timer_thread_id_mutex",
    "bench_timer_thread_id_nomutex",
    "bench_timer_signal"
  };

  for (int c = 0; c < num_clocks; c++) {
    printf("%s:\n", clock_names[c]);

    for (int m = 0; m < num_methods; m++) {
      double avg_overshoot = (*(method_ptrs[m]))(10 * 1e6, 100, clock_ids[c]);
      printf("  - %s = %f ms\n", method_names[m], (double)avg_overshoot / 1e6);
    }
  }
}
