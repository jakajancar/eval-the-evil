#pragma once

#include <time.h>
#include <signal.h>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <cassert>
#include <cstring>
#include <iostream>

namespace eval {

template<class Callback>
class Timer
{
  private:
    const Callback &callback;
    bool fired;
    timer_t underlying;

  public:
    /**
     * Calls `callback` after `delay` if `Timer` still exists.
     * It will happen on a global thread and block all other timeouts.
     */
    Timer(std::chrono::nanoseconds delay, clockid_t clockid, const Callback &callback) : callback(callback), fired(false)
    {
      std::lock_guard<std::mutex> lock(timers_mutex());

      // Add to global instance set
      bool inserted = timers().insert(this).second;
      assert(inserted);

      struct sigevent event;
      memset(&event, 0, sizeof(event));
      event.sigev_notify = SIGEV_THREAD;
      event.sigev_value.sival_ptr = this;
      event.sigev_notify_function = &global_callback;

      // timer_t timer;
      if (timer_create(clockid, &event, &(this->underlying)) != 0) {
        std::cout << "timer_create failed\n";
        abort();
      }

      struct itimerspec timing;
      memset(&timing, 0, sizeof(timing));
      timing.it_value = duration_to_timespec(delay);
      if (timer_settime(this->underlying, 0, &timing, NULL) != 0) {
        std::cout << "timer_settime failed\n";
        abort();
      }
    }

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    ~Timer()
    {
      std::lock_guard<std::mutex> lock(timers_mutex());

      if (timer_delete(this->underlying) != 0) {
        std::cout << "timer_delete failed\n";
        abort();
      }

      // Remove from global instance set
      int removed = timers().erase(this);
      assert(removed == 1);

    }

  private:
    // Wizardry to get static members into header-only files :/
    static auto& timers_mutex() { static std::mutex I; return I; }
    static auto& timers() { static std::unordered_set<Timer<Callback>*> I; return I; }

    static void global_callback(union sigval sigev_value)
    {
      std::lock_guard<std::mutex> lock(timers_mutex());
      Timer *timer = static_cast<Timer*>(sigev_value.sival_ptr);
      if (timers().count(timer)) {
        // Still relevant
        timer->callback();
      }
    }

    static inline timespec duration_to_timespec(std::chrono::nanoseconds duration)
    {
        using namespace std::chrono;
        seconds secs = duration_cast<seconds>(duration);
        duration -= secs;
        return timespec{secs.count(), duration.count()};
    }
};

}
