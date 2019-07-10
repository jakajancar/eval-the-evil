#include <iostream>
#include <thread>

#include <boost/program_options.hpp>
#include <boost/asio.hpp>

#include "evaluation.h"

namespace po = boost::program_options;
using boost::asio::ip::tcp;

int main(int argc, char *argv[])
{
  // Process arguments
  int port;
  int num_threads;
  po::options_description desc("Allowed options");
  desc.add_options()
      ("help", "produce help message")
      ("port", po::value<int>(&port)->default_value(1101), "port to listen on")
      ("threads", po::value<int>(&num_threads)->default_value(std::thread::hardware_concurrency()), "number of threads (defaults to hardware concurrency)")
  ;
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);
  if (vm.count("help")) {
      std::cout << desc << "\n";
      return 1;
  }

  // Prepare global evaluation context
  eval::GlobalContext global_eval_context;

  // Start threads
  std::thread threads[num_threads];
  for (int i = 0; i < num_threads; i++) {
    threads[i] = std::thread([port]{
      // Listen for TCP connections.
      boost::asio::io_service io_service;
      tcp::acceptor acceptor(io_service);
      acceptor.open(tcp::v4());
      acceptor.set_option(boost::asio::socket_base::reuse_address(true));
      acceptor.set_option(boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true));
      acceptor.bind(tcp::endpoint(tcp::v4(), port));
      acceptor.listen();

      // Prepare thread-level evaluation context
      eval::ThreadContext thread_eval_context;
      while (true)
      {
        // Prepare request-specific evaluation context
        eval::RequestContext request_eval_context(&thread_eval_context);

        // Accept a connection
        tcp::socket sock(io_service);
        acceptor.accept(sock);

        // Read the request
        boost::system::error_code error;
        std::string *request_blob = new std::string();
        std::size_t read_bytes = boost::asio::read(sock, boost::asio::dynamic_buffer(*request_blob), boost::asio::transfer_all(), error);
        if (error != boost::asio::error::eof)
          throw boost::system::system_error(error);
        assert(request_blob->length() == read_bytes);

        // Evaluate
        char *response_blob;
        size_t response_blob_length;
        request_eval_context.handle_request(request_blob->c_str(), response_blob, response_blob_length);
        delete request_blob; // no longer needed now

        // Send response
        std::size_t written_bytes = boost::asio::write(sock, boost::asio::buffer(response_blob, response_blob_length));
        assert(written_bytes == response_blob_length);
        sock.close();

      } // run loop
      acceptor.close();
    }); // thread
  }
  std::cout << "eval-the-evil listening on port " << port << ".\n";

  // Wait for threads to exit
  for (auto &thread : threads) {
    thread.join();
  }
  return 0;
}
