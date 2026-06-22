#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <thread>

template<typename T>
class BlockingQueue{
private: 
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_producer_;
    std::condition_variable cv_consumer_;
    size_t size_max_;

public:
    BlockingQueue(size_t size_max) : size_max_(size_max){}
    ~BlockingQueue() = default;
    bool push(const T& val){
        std::unique_lock lock(mutex_);
        cv_producer_.wait(lock, 
            [this](){return queue_.size() < size_max_;});
        queue_.push(val);
        cv_consumer_.notify_one();
        return true;
    }

    bool push(T&& val){
        std::unique_lock lock(mutex_);
        cv_producer_.wait(lock, 
            [this](){return queue_.size() < size_max_;});
        queue_.push(std::move(val));
        cv_consumer_.notify_one();
        return true;
    }

    bool try_push(const T& val){
        std::lock_guard lock(mutex_);
        if(queue_.size() >= size_max_) return false;
        queue_.push(val);
        cv_consumer_.notify_one();
        return true;
    }

    bool try_push(T&& val){
        std::lock_guard lock(mutex_);
        if(queue_.size() >= size_max_) return false;
        queue_.push(std::move(val));
        cv_consumer_.notify_one();
        return true;
    }

    bool pop(T& val){
        std::unique_lock lock(mutex_);
        cv_consumer_.wait(lock, 
            [this](){
            return !queue_.empty();});
        val = queue_.front();
        queue_.pop();
        cv_producer_.notify_one();
        return true;
    }

    bool try_pop(T& val){
        std::lock_guard lock(mutex_);
        if(queue_.empty()) return false;
        val = queue_.front();
        queue_.pop();
        cv_producer_.notify_one();
        return true;
    }

    size_t size(){
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

};
