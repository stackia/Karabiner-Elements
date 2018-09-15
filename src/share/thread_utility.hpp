#pragma once

// `krbn::thread_utility::wait` can be used safely in a multi-threaded environment.
// `krbn::thread_utility::timer` can be used safely in a multi-threaded environment.
// `krbn::thread_utility::dispatcher` can be used safely in a multi-threaded environment.

#include "logger.hpp"
#include <chrono>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace krbn {
class thread_utility final {
public:
  static std::thread::id get_main_thread_id(void) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> guard(mutex);

    static std::thread::id id = std::this_thread::get_id();
    return id;
  }

  static void register_main_thread(void) {
    get_main_thread_id();
  }

  static bool is_main_thread(void) {
    return get_main_thread_id() == std::this_thread::get_id();
  }

  class wait final {
  public:
    wait(void) : notify_(false) {
    }

    void wait_notice(void) {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] {
        return notify_;
      });
    }

    void notify(void) {
      {
        std::lock_guard<std::mutex> lock(mutex_);

        notify_ = true;
      }

      cv_.notify_one();
    }

  private:
    bool notify_;
    std::mutex mutex_;
    std::condition_variable cv_;
  };

  class timer final {
  public:
    enum class mode {
      once,
      repeat,
    };

    timer(const std::function<std::chrono::milliseconds(size_t count)>& interval,
          mode mode,
          const std::function<void(void)>& function) : mode_(mode),
                                                       exit_(false),
                                                       graceful_exit_(false),
                                                       count_(0) {
      thread_ = std::thread([this, interval, function] {
        while (true) {
          // Wait
          {
            std::unique_lock<std::mutex> lock(timer_mutex_);
            timer_cv_.wait_for(lock, interval(count_), [this] {
              return exit_ == true;
            });
          }

          if (exit_) {
            break;
          }

          function();
          ++count_;

          if (mode_ == mode::once) {
            break;
          }

          if (graceful_exit_) {
            break;
          }
        }
      });
    }

    timer(std::chrono::milliseconds interval,
          mode mode,
          const std::function<void(void)>& function) : timer([interval](auto&& count) { return interval; },
                                                             mode,
                                                             function) {
    }

    ~timer(void) {
      graceful_exit_ = true;

      wait();
    }

    void wait(void) {
      if (thread_.joinable()) {
        thread_.join();
      }
    }

    void cancel(void) {
      exit_ = true;

      timer_cv_.notify_one();
    }

  private:
    mode mode_;

    std::thread thread_;
    std::atomic<bool> exit_;
    std::atomic<bool> graceful_exit_;
    size_t count_;

    std::mutex timer_mutex_;
    std::condition_variable timer_cv_;
  };

  class dispatcher final {
  public:
    dispatcher(void) : exit_(false) {
      wait w;

      worker_thread_ = std::thread([this, &w] {
        worker_thread_id_ = std::this_thread::get_id();
        w.notify();

        while (true) {
          std::function<void(void)> function;

          {
            std::unique_lock<std::mutex> queue_lock(queue_mutex_);
            queue_cv_.wait(queue_lock, [this] {
              return exit_ || !queue_.empty();
            });

            if (exit_ && queue_.empty()) {
              break;
            }

            if (!queue_.empty()) {
              function = queue_.front();
              queue_.pop();
            }
          }

          if (function) {
            function();
          }
        }
      });

      w.wait_notice();
    }

    ~dispatcher(void) {
      if (worker_thread_.joinable()) {
        logger::get_logger().error("Call `thread_utility::dispatcher::terminate` before destroy `thread_utility::dispatcher`");
        terminate();
      }
    }

    bool is_worker_thread(void) const {
      return std::this_thread::get_id() == worker_thread_id_;
    }

    void terminate(void) {
      // We should separate `~dispatcher` and `terminate` to ensure dispatcher exists until all jobs are processed.
      //
      // Example:
      // ----------------------------------------
      // auto q = std::unique_ptr<thread_utility::dispatcher>();
      //
      // q->enqueue([q] {
      //   // `q` might be nullptr if we call `terminate` before `q = nullptr`.
      //   q->enqueue([] {
      //     std::cout << "hello" << std::endl;
      //   }
      // });
      //
      // q->terminate();
      // q = nullptr;
      // ----------------------------------------

      if (is_worker_thread()) {
        exit_ = true;
        queue_.empty();
        worker_thread_.detach();

      } else {
        if (worker_thread_.joinable()) {
          exit_ = true;
          queue_cv_.notify_one();
          worker_thread_.join();
        }
      }
    }

    void enqueue(const std::function<void(void)>& function) {
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        queue_.push(function);
      }

      queue_cv_.notify_one();
    }

  private:
    std::thread worker_thread_;
    std::thread::id worker_thread_id_;
    std::atomic<bool> exit_;

    std::queue<std::function<void(void)>> queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
  };
};
} // namespace krbn
