## Introduction
This coroutine implementation is based on the c++20 standard, which includes a thread pool and encapsulates the implementation of the original api of the c++ coroutine. It provides two ways to use coroutines, and you can make asynchronous calls through these two methods.
- If you care about the result of the call, you can use it like this:
```c++
/// The template class `Task` is the coroutine function, the second template parameter `ThreadExecutor` is the default coroutine scheduler implemented
auto res = AsyncResCall(std::function([] () -> Task<int, ThreadExecutor> {
        std::cout << std::this_thread::get_id() << " : coroutine1 started\n";
        co_await 3s;
        std::cout << std::this_thread::get_id() << " : coroutine1 finished\n";
        co_return 20;
}));
std::cout << res.GetTaskResult() << "\n";
```
- If you don't care about the call result, you can refer to [this](https://github.com/tmp-username/CoroutineCpp/blob/7f9929e960fec45021ef423c94d268122241f33d/coroutine.hpp#L568)

## Requirements
- c++20 At least support the coroutine mechanism
- Boost Boost thread group used here