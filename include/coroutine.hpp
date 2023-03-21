//
// Copyright (c) 2023 lobby (smartao9527@163.com)
//
// Original repository: https://github.com/tmp-username/CoroutineCpp
// 

#include <iostream>
#include <utility>
#include <unistd.h>
#include <functional>
#include <type_traits>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <atomic>
#include <coroutine>
#include <thread>
#include <future>
#include <mutex>
#include <queue>
#include <fstream>
#include <cstdarg>
#include <condition_variable>
#include <boost/thread/thread.hpp>

using namespace std::chrono_literals;

using ExecutorEvent = std::function<void()>;
using SchedulerEvent = std::function<void()>;
using ThreadTaskCB = std::function<void()>;
using Duration = std::chrono::duration<double, std::ratio<1, 1000000>>;
using Seconds = std::chrono::duration<double, std::ratio<1, 1>>;
using MilliSeconds = std::chrono::duration<double, std::ratio<1, 1000>>;
using MicroSeconds = std::chrono::duration<double, std::ratio<1, 1000000>>;
using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

/// Log record
#define LOG(...) PrintLog(__FILE__, __FUNCTION__, __VA_ARGS__)

void FormatData(char *buf) {
    time_t now_time = time(NULL);
    struct tm *now = localtime(&now_time);
    if (now) {
        sprintf(buf, "%d-%02d-%02d %02d:%02d:%02d", now->tm_year + 1900, now->tm_mon + 1, now->tm_mday,
                now->tm_hour, now->tm_min, now->tm_sec);
    }
}

void PrintLog(const char *file, const char *func, const char *fmt, ...) {
    std::ofstream ofs("/tmp/coroutine.log", std::ofstream::out | std::ofstream::app);
    if (ofs.good()) {
        char data[64] = {'\0'};
        char buf[256] = {'\0'};
        FormatData(data);
        ofs << "[" << data << "]";
        ofs << "[" << file << ":" << func << "]";
        ofs << "[" << std::this_thread::get_id() << "] ";
        va_list list;
        va_start(list, fmt);
        vsnprintf(buf, sizeof(buf), fmt, list);
        va_end(list);
        ofs << buf << "\n";
    }
}

template <typename T, typename U>
class Task;

template <typename T, typename U>
class TaskWaitable;

template <typename T, typename U>
class SleepWaitable;
        
/// Thread pool
static TimePoint TimeNow() { return std::chrono::steady_clock::now(); }
struct SchedTask {
    SchedTask() = default;

    explicit SchedTask(SchedulerEvent &&sched_event, Duration duration)
        : sched_event_{std::move(sched_event)}, duration_{duration} {
        complete_time_ = TimeNow() + std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    }

    TimePoint CompleteTime() const {
        return complete_time_;
    }

    void operator()() const {
        sched_event_();
    }

    bool operator<(const SchedTask &sched_task) const {
        return complete_time_ > sched_task.CompleteTime();
    }

private:
    SchedulerEvent sched_event_;
    Duration duration_;
    TimePoint complete_time_;
};
using SchedTaskQueue = std::priority_queue<SchedTask>;

class ThreadTaskQueueMgr {
    public:
        struct ThreadInfo {
            std::variant<std::deque<ThreadTaskCB>, SchedTaskQueue> th_info_queue_;
            int th_info_queue_len_{0};
            std::condition_variable th_info_cond_;
            std::mutex th_info_mtx_;
        };
        using ThreadInfoPtr = std::shared_ptr<ThreadInfo>;

        ThreadTaskQueueMgr(int thread_num = 2) {
            work_thread_num_ = thread_num;
            th_grp_.create_thread([this] () {
                ManagerThread();
            });
            th_info_.emplace_back(new ThreadInfo());
            th_grp_.create_thread(boost::bind(&ThreadTaskQueueMgr::CoroutineWorkThread, this, th_info_[0]));
            for (int i=1; i<thread_num; i++) {
                th_info_.emplace_back(new ThreadInfo());
                th_grp_.create_thread(boost::bind(&ThreadTaskQueueMgr::WorkThread, this, th_info_[i]));
            }
        }

        ~ThreadTaskQueueMgr() {
            stop_ = true;
            {
                std::unique_lock<std::mutex> lk(mgr_mtx_);
                task_queue_cond_.notify_one();
            }
            {
                for (int i=0; i<work_thread_num_; i++) {
                    std::unique_lock<std::mutex> lk{th_info_[i]->th_info_mtx_};
                    th_info_[i]->th_info_cond_.notify_all();
                }
            }
            th_grp_.join_all();
        }

        int GetWorkThreadNum() const { return work_thread_num_; }

        template <typename Func, typename ...Args, typename RetType = std::invoke_result_t<Func, Args...>>
        typename std::enable_if_t<std::is_same_v<RetType, void>>
        AddTask(Func &&f, Args &&...args) {
            auto task = std::make_shared<std::packaged_task<RetType()>>(std::bind(std::forward<Func>(f), std::forward<Args>(args)...));
            {
                std::unique_lock<std::mutex> lk(mgr_mtx_);
                task_queue_.emplace_back([task] () {
                    (*task)();
                });
            }
            mgr_cond_.notify_one();
        }

        template <typename Func, typename ...Args, typename RetType = std::invoke_result_t<Func, Args...>>
        typename std::enable_if_t<!std::is_same_v<RetType, void>, std::future<RetType>>
        AddTask(Func &&f, Args &&...args) {
            auto task = std::make_shared<std::packaged_task<RetType()>>(std::bind(std::forward<Func>(f), std::forward<Args>(args)...));
            auto res = task->get_future();
            {
                std::unique_lock<std::mutex> lk(mgr_mtx_);
                task_queue_.emplace_back([task] () {
                    (*task)();
                });
            }
            mgr_cond_.notify_one();
            return res;
        }

        void AddCoroutineTask(const SchedTask &sched_task) {
            {
                std::unique_lock lk{th_info_[0]->th_info_mtx_};
                if (std::holds_alternative<SchedTaskQueue>(th_info_[0]->th_info_queue_)) {
                    auto &&prior_queue = std::get<SchedTaskQueue>(th_info_[0]->th_info_queue_);
                    prior_queue.push(sched_task);
                } else {
                    SchedTaskQueue prior_queue;
                    prior_queue.push(sched_task);
                    th_info_[0]->th_info_queue_ = std::move(prior_queue);
                }
            }
            th_info_[0]->th_info_cond_.notify_one();
        }

        void ManagerThread() {
            while (!stop_) {
                decltype(task_queue_) task_queue;
                {
                    std::unique_lock<std::mutex> lk(mgr_mtx_);
                    mgr_cond_.wait(lk, [this] () { return !task_queue_.empty() || stop_; });
                }
                if (stop_) { continue; }
                if (!task_queue_.empty()) { task_queue_.swap(task_queue); }
                LOG("Manager thread start dispatch");
                {
                    for (auto &task : task_queue) {
                        int idx = 0, max_len = INT_MAX;
                        for (int i=1; i<th_info_.size(); i++) {
                            if (th_info_[i]->th_info_queue_len_ == 0) {
                                idx = i;
                                break;
                            }
                            if (th_info_[i]->th_info_queue_len_ < max_len) {
                                idx = i;
                                max_len = th_info_[i]->th_info_queue_len_;
                            }
                        }
                        {
                            std::unique_lock<std::mutex> lk(th_info_[idx]->th_info_mtx_);
                            if (std::holds_alternative<std::deque<ThreadTaskCB>>(th_info_[idx]->th_info_queue_)) {
                                auto &&queue = std::get<0>(th_info_[idx]->th_info_queue_);
                                queue.push_back(task);
                            } else {
                                std::deque<ThreadTaskCB> queue;
                                queue.push_back(task);
                                th_info_[idx]->th_info_queue_ = std::move(queue);
                            }
                            th_info_[idx]->th_info_queue_len_++;
                        }
                        th_info_[idx]->th_info_cond_.notify_one();
                    }
                }
            }
        }

        void CoroutineWorkThread(ThreadInfoPtr &thread_info) {
            LOG("coroutine work thread started");
            while (!stop_) {
                {
                    std::unique_lock<std::mutex> lk(thread_info->th_info_mtx_);
                    thread_info->th_info_cond_.wait(lk, [&thread_info, this] () {
                        auto prior_queue = std::get_if<1>(&(thread_info->th_info_queue_));
                        return (prior_queue && !prior_queue->empty()) || stop_; 
                    });
                }
                if (stop_) { continue; }
                {
                    double delta_milli{0.0};
                    SchedTask front_task;
                    {
                        std::unique_lock<std::mutex> lk(thread_info->th_info_mtx_);
                        auto &&sched_task_queue = std::get<1>(thread_info->th_info_queue_);
                        LOG("sched task queue size : %d", sched_task_queue.size());
                        front_task = sched_task_queue.top();
                        delta_milli = std::chrono::duration_cast<std::chrono::milliseconds>(TimeNow() - front_task.CompleteTime()).count();
                        if (delta_milli >= 0.0) {
                            sched_task_queue.pop();
                            thread_info->th_info_queue_len_--;
                        } else {
                            if (auto status = thread_info->th_info_cond_.wait_for(lk, std::abs(delta_milli) * 1ms);
                                status == std::cv_status::no_timeout) {
                                continue;
                            } else {    /// timeout
                                sched_task_queue.pop();
                                thread_info->th_info_queue_len_--;
                            }
                        }
                    }
                    front_task();
                }
            } 
        }

        void WorkThread(ThreadInfoPtr &thread_info) {
            while (!stop_) {
                std::deque<ThreadTaskCB> th_info_queue;
                {
                    std::unique_lock<std::mutex> lk(thread_info->th_info_mtx_);
                    thread_info->th_info_cond_.wait(lk, [&thread_info, this] () {
                        auto queue = std::get_if<0>(&(thread_info->th_info_queue_));
                        return (queue && !queue->empty()) || stop_;
                    });
                    if (stop_) { continue; }
                    std::swap(std::get<0>(thread_info->th_info_queue_), th_info_queue);
                }
                for (auto &task : th_info_queue) {
                    task();
                    {
                        std::unique_lock<std::mutex> lk(thread_info->th_info_mtx_);
                        thread_info->th_info_queue_len_--;
                        th_info_queue.pop_front();
                    }
                }
            }
        }

    private:
        int work_thread_num_{0};
        std::atomic<bool> stop_{false};
        std::vector<ThreadTaskCB> task_queue_;
        std::vector<ThreadInfoPtr> th_info_;
        std::condition_variable task_queue_cond_;
        std::condition_variable mgr_cond_;
        std::mutex mgr_mtx_;
        boost::thread_group th_grp_;
};
using ThreadTaskQueueMgrPtr = std::shared_ptr<ThreadTaskQueueMgr>;
ThreadTaskQueueMgrPtr g_th_task_queue_mgr{nullptr};

/// Task executor
template <typename ExecutorType>
class BaseExecutor {
    public:
        using executor_type = ExecutorType;

        template <typename EventType>
        void execute(EventType &&func) {
            return GetExecutor().execute(std::forward<EventType>(func));
        }

    private:
        ExecutorType &GetExecutor() {
            return *static_cast<ExecutorType *>(this);
        }
};

class ThreadExecutor : public BaseExecutor<ThreadExecutor> {
    public:
        ThreadExecutor() = default;

        ~ThreadExecutor() {}

        template <typename EventType>
        void execute(EventType &&event_cb) {
            if constexpr (std::is_same_v<EventType, ExecutorEvent>) {
                g_th_task_queue_mgr->AddTask(std::forward<EventType>(event_cb));
            } else if constexpr (std::is_same_v<EventType, SchedTask>) {
                g_th_task_queue_mgr->AddCoroutineTask(std::forward<EventType>(event_cb)); 
            } else {
                LOG("wrong event type");
            }
        }
};

/// Coroutine function
template <typename RetType, typename ExecutorType>
class TaskPromise {
        ExecutorType executor_;
        RetType m_value{};
        std::atomic_bool is_completed_{false};
        std::mutex complete_mtx_;
        std::condition_variable complete_cond_;
    public:
        Task<RetType, ExecutorType> get_return_object() {
            return Task{std::coroutine_handle<TaskPromise>::from_promise(*this)};
        }

        auto initial_suspend() noexcept { return std::suspend_never{}; }
        auto final_suspend() noexcept { return std::suspend_always{}; }
        void unhandled_exception() {}

        auto await_transform(Task<RetType, ExecutorType> task) {
            return TaskWaitable<RetType, ExecutorType>(std::move(task), &executor_);
        }

        auto await_transform(Duration duration) {
            return SleepWaitable<ExecutorType, Duration>(duration, &executor_);
        }

        std::suspend_always yield_value(RetType value) {
            m_value = value;
            return {};
        }

        void return_value(RetType value) {
            m_value = value; 
            is_completed_.store(true);
            {
                std::unique_lock lk{complete_mtx_};
                complete_cond_.notify_one(); 
            }
        }

        RetType GetTaskResult() {
            if (is_completed_.load(std::memory_order_relaxed)) { return m_value; }
            {
                std::unique_lock lk{complete_mtx_};
                complete_cond_.wait(lk); 
            }
            return m_value;
        }
};

template <typename ExecutorType>
class TaskPromise<void, ExecutorType> {
        std::atomic_bool is_completed_{false};
        std::mutex complete_mtx_;
        std::condition_variable complete_cond_;
        ExecutorType executor_;
    public:
        Task<void, ExecutorType> get_return_object() {
            return Task{std::coroutine_handle<TaskPromise>::from_promise(*this)};
        }

        auto initial_suspend() noexcept { return std::suspend_never{}; }
        auto final_suspend() noexcept { return std::suspend_always{}; }
        void unhandled_exception() {}

        auto await_transform(Task<void, ExecutorType> task) {
            return TaskWaitable<void, ExecutorType>(std::move(task), &executor_);
        }

        auto await_transform(Duration duration) {
            return SleepWaitable<ExecutorType, Duration>(duration, &executor_);
        }

        void return_void() {
            is_completed_.store(true);
            {
                std::unique_lock lk{complete_mtx_};
                complete_cond_.notify_one(); 
            }
        }

        void GetTaskResult() {
            if (is_completed_.load(std::memory_order_relaxed)) { return; }
            {
                std::unique_lock lk{complete_mtx_};
                complete_cond_.wait(lk); 
            }
        }
};

template <typename ExecutorType>
class SleepWaitable<ExecutorType, Duration> {
    public:
        SleepWaitable(Duration duration, ExecutorType *executor)
            : duration_{duration}, base_executor_{executor} {}

        bool await_ready() { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            base_executor_->execute(SchedTask{[this, h] () {
                LOG("Resume after sleep duration");
                h.resume();
            }, duration_});
        }

        void await_resume() {}

    private:
        Duration duration_;
        BaseExecutor<ExecutorType> *base_executor_;
};

template <typename RetType, typename ExecutorType>
class TaskWaitable {
    public:
        explicit TaskWaitable(Task<RetType, ExecutorType> &&task, ExecutorType *executor)
            : task_{std::move(task)}, base_executor_{executor} {}

        bool await_ready() { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            base_executor_->execute(ExecutorEvent([h] () {
                h.resume();
            }));
        }

        RetType await_resume() {
            return task_.GetTaskResult(); 
        }

    private:
        Task<RetType, ExecutorType> task_;
        BaseExecutor<ExecutorType> *base_executor_;
};

template <typename RetType, typename ExecutorType>
class Task {
    public:
        using promise_type = TaskPromise<RetType, ExecutorType>;

        explicit Task(std::coroutine_handle<promise_type> handle)
            : coro_handle_{handle} {}

        ~Task() {
            if (coro_handle_)
                coro_handle_.destroy();
        }

        Task(Task &&task_exec)
            : coro_handle_{std::exchange(task_exec.coro_handle_, {})} {}

        Task(const Task &) = delete;
        Task &operator=(const Task &) = delete;

        RetType GetTaskResult() {
            return coro_handle_.promise().GetTaskResult();
        }

        void Resume() {
            coro_handle_.resume();
        }

    private:
        std::coroutine_handle<promise_type> coro_handle_;
};

template <typename ExecutorType>
class Task<void, ExecutorType> {
    public:
        using promise_type = TaskPromise<void, ExecutorType>;

        explicit Task(std::coroutine_handle<promise_type> handle)
            : coro_handle_{handle} {}

        ~Task() {
            if (coro_handle_)
                coro_handle_.destroy();
        }

        Task(Task &&task_exec)
            : coro_handle_{std::exchange(task_exec.coro_handle_, {})} {}

        Task(const Task &) = delete;
        Task &operator=(const Task &) = delete;

        void GetTaskResult() {
            return coro_handle_.promise().GetTaskResult();
        }

        void Resume() {
            coro_handle_.resume();
        }

    private:
        std::coroutine_handle<promise_type> coro_handle_;
};

/// Calling interface
template <typename RetType, typename ExecutorType>
Task<RetType, ExecutorType> AsyncResCall(std::function<Task<RetType, ExecutorType>()> &&func) {
    auto f = std::move(func);
    auto res = co_await f();
    co_return res;
}

template <typename ExecutorType>
Task<void, ExecutorType> AsyncCall(std::function<Task<void, ExecutorType>()> &&func) {
    auto f = std::move(func);
    co_await f();
    co_return;
}


#if 0
/// Usage example
/// asynchronous call with task result
void usage_example1() {
    auto time_start = std::chrono::steady_clock::now();
    auto res = AsyncResCall(std::function([] () -> Task<int, ThreadExecutor> {
        std::cout << std::this_thread::get_id() << " : coroutine1 started\n";
        co_await 3s;
        std::cout << std::this_thread::get_id() << " : coroutine1 finished\n";
        co_return 20;
    }));
    auto res2 = AsyncResCall(std::function([] () -> Task<std::string, ThreadExecutor> {
        std::cout << std::this_thread::get_id() << " : coroutine2 started\n";
        co_await 5s;
        std::cout << std::this_thread::get_id() << " : coroutine2 finished\n";
        co_return "hello world";
    }));
    std::cout << res.GetTaskResult() << "\n";
    std::cout << res2.GetTaskResult() << "\n";

    auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - time_start).count();
    std::cout << "delta = " << delta << "ms\n";
}
/// asynchronous call without result
void usage_example2() {
    auto res1 = AsyncCall(std::function([] () -> Task<void, ThreadExecutor> {
        std::cout << std::this_thread::get_id() << " : func1 started\n";
        co_await 5s;
        std::cout << std::this_thread::get_id() << " : func1 finished\n";
    }));
    auto res2 = AsyncCall(std::function([] () -> Task<void, ThreadExecutor> {
        std::cout << std::this_thread::get_id() << " : func3 started\n";
        co_await 3s;
        std::cout << std::this_thread::get_id() << " : func3 finished\n";
    }));

    /// Avoid res1/res2 lapse
    while(1) {
        std::this_thread::sleep_for(100ms);
    }
}

void Test()
{
    g_th_task_queue_mgr = std::make_shared<ThreadTaskQueueMgr>(2);
    /// Waiting for sub-thread creation 
    std::this_thread::sleep_for(100ms);

    usage_example1();
    std::cout << "\n";
    usage_example2();
}
#endif
