#pragma once

#include <exception>
#include <iostream>
#include <boost/stacktrace.hpp>
#include <boost/exception/all.hpp>

typedef boost::error_info<struct tag_stacktrace, boost::stacktrace::stacktrace> traced;

template <class E>
void throw_with_trace(const E& e) {
  throw boost::enable_error_info(e) << traced(boost::stacktrace::stacktrace());
}

// Include first in main(), so it's called before other static initializers.
class GlobalErrorHandler
{
  public:
    GlobalErrorHandler()
    {
      std::set_terminate([](){
        const std::exception_ptr eptr = std::current_exception();
        if (eptr) {
          try {
            std::rethrow_exception(eptr);
          } catch (const std::exception& e) {
            std::cerr << "Uncaught exception: " << e.what() << '\n';
            std::cerr << '\n';
            std::cerr << "Stack trace:" << '\n';
            const boost::stacktrace::stacktrace* trace = boost::get_error_info<traced>(e);
            if (trace) {
              std::cerr << *trace << '\n';
            } else {
              std::cerr << "<not available>" << '\n';
            }
          } catch (...) {
            std::cerr << "Uncaught exception: <unknown>\n";
          }
        } else {
          std::cerr << "std::terminate() called, but no std::current_exception().";
        }
        std::abort();
      });
    }
};
