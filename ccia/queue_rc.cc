// https://github.com/anthonywilliams/ccia_code_samples/blob/main/listings/listing_7.14.cpp
// https://github.com/anthonywilliams/ccia_code_samples/blob/main/listings/listing_7.16.cpp
// https://github.com/anthonywilliams/ccia_code_samples/blob/main/listings/listing_7.17.cpp
// https://github.com/anthonywilliams/ccia_code_samples/blob/main/listings/listing_7.18.cpp
// https://github.com/anthonywilliams/ccia_code_samples/blob/main/listings/listing_7.19.cpp
// https://github.com/anthonywilliams/ccia_code_samples/blob/main/listings/listing_7.20.cpp
// https://github.com/anthonywilliams/ccia_code_samples/blob/main/listings/listing_7.21.cpp
// https://github.com/anthonywilliams/ccia_code_samples/blob/main/listings/listing_7.22.cpp

#include <atomic>
#include <memory>

template <typename T>
class lock_free_queue
{
private:
    struct node;
    struct counted_node_ptr
    {
        int external_count;  // control block ptr for shared_ptr
        node* ptr;
    };
    std::atomic<counted_node_ptr> head;
    std::atomic<counted_node_ptr> tail;
    struct node_counter
    {
        unsigned internal_count : 30;
        unsigned external_counters : 2;
    };
    struct node
    {
        std::atomic<T*> data;
        std::atomic<node_counter> count;
        counted_node_ptr next;
        node()
        {
            node_counter new_count;
            new_count.internal_count = 0;
            new_count.external_counters = 2;
            count.store(new_count);
            next.ptr = nullptr;
            next.external_count = 0;
        }
        node(T const& data_) : data(std::make_shared<T>(data_)), count(0) {}

        void release_ref()
        {
            node_counter old_counter = count.load(std::memory_order_relaxed);
            node_counter new_counter;
            do {
                new_counter = old_counter;
                --new_counter.internal_count;
            } while (!count.compare_exchange_strong(old_counter, new_counter,
                                                    std::memory_order_acquire,
                                                    std::memory_order_relaxed));
            if (!new_counter.internal_count && !new_counter.external_counters) {
                delete this;
            }
        }
    };

    static void increase_external_count(std::atomic<counted_node_ptr>& counter,
                                        counted_node_ptr& old_counter)
    {
        counted_node_ptr new_counter;
        do {
            new_counter = old_counter;
            ++new_counter.external_count;
        } while (!counter.compare_exchange_strong(
            old_counter, new_counter,
            std::memory_order_acquire,  // counter->ptr will be read when
                                        // success
            std::memory_order_relaxed));
        old_counter.external_count = new_counter.external_count;
    }

    static void free_external_counter(counted_node_ptr& old_node_ptr)
    {
        node* const ptr = old_node_ptr.ptr;

        int const count_increase = old_node_ptr.external_count -
                                   2;  // 2 = 1 (current thread) + 1 (tail)
        node_counter old_counter = ptr->count.load(std::memory_order_relaxed);
        node_counter new_counter;
        do {
            new_counter = old_counter;
            // old_tail will no longer be referenced by tail any more
            --new_counter.external_counters;
            new_counter.internal_count += count_increase;
        } while (!ptr->count.compare_exchange_strong(
            old_counter, new_counter,
            std::memory_order_acquire,  // std::memory_order_relaxed is enough:
                                        // old_head.ptr->data has already been
                                        // acquired by increase_external_count()
                                        // in pop()
            std::memory_order_relaxed));
        if (!new_counter.internal_count && !new_counter.external_counters) {
            delete ptr;
        }
    }

    void set_new_tail(counted_node_ptr& old_tail,
                      counted_node_ptr const& new_tail)
    {
        node* const current_tail_ptr = old_tail.ptr;
        while (!tail.compare_exchange_weak(old_tail, new_tail /*, std::memory_order_release, std::memory_order_relaxed*/) &&
               old_tail.ptr == current_tail_ptr); // acquire-release payload is new_tail itself
        if (old_tail.ptr ==
            current_tail_ptr)  // tail has been set by current thread
            free_external_counter(old_tail);
        else
            // --internal_count only because external_count has been merged by
            // the succeeded thread
            current_tail_ptr->release_ref();
    }

public:
    lock_free_queue()
        : head(new node), tail(head.load(/*std::memory_order_relaxed*/))
    {}
    lock_free_queue(const lock_free_queue& other) = delete;
    lock_free_queue& operator=(const lock_free_queue& other) = delete;
    ~lock_free_queue()
    {
        while (node* const old_head =
                   head.load(/*std::memory_order_relaxed*/)) {
            head.store(old_head->next);
            delete old_head;
        }
    }

    void push(T new_value)
    {
        std::unique_ptr<T> new_data(new T(new_value));
        counted_node_ptr new_next;
        new_next.ptr = new node;
        new_next.external_count = 1;
        counted_node_ptr old_tail = tail.load(/*std::memory_order_relaxed*/);
        for (;;) {
            increase_external_count(tail, old_tail);
            T* old_data = nullptr;
            if (old_tail.ptr->data.compare_exchange_strong(
                    old_data, new_data.get() /*, std::memory_order_relaxed*/)) {
                // now tail.ptr->data != nullptr, so nobody else could make
                // any progress util new tail is set
                counted_node_ptr old_next = {0};
                if (!old_tail.ptr->next.compare_exchange_strong(
                        old_next, new_next /*, std::memory_order_relaxed*/)) {
                    delete new_next.ptr;
                    new_next = old_next;
                }
                set_new_tail(old_tail, new_next);  // release when success
                new_data.release();
                break;
            } else {  // helper to prevent busy-wait
                counted_node_ptr old_next = {0};
                if (old_tail.ptr->next.compare_exchange_strong(
                        old_next, new_next /*, std::memory_order_relaxed*/)) {
                    old_next = new_next;
                    new_next.ptr = new node;
                }
                set_new_tail(old_tail, old_next);  // release when success
            }
        }
    }

    std::unique_ptr<T> pop()
    {
        counted_node_ptr old_head = head.load(std::memory_order_relaxed);
        for (;;) {
            increase_external_count(head, old_head);  // acquire when sucess
            node* const ptr = old_head.ptr;
            // must be memory_order_seq_cst to load the latest tail?, or it may
            // return nullptr even the queue is not empty because latest tail
            // was not load
            if (ptr == tail.load(/*std::memory_order_seq_cst*/).ptr) {
                return std::unique_ptr<T>();
            }
            // already acquired by increase_external_count()
            counted_node_ptr next = ptr->next.load(
                /*std::memory_order_relaxed*/);
            if (head.compare_exchange_strong(
                    old_head, next /*, std::memory_order_relaxed*/)) {
                T* const res =
                    ptr->data.exchange(nullptr /*, std::memory_order_relaxed*/);
                free_external_counter(old_head);  // no acquire-release payload
                                                  // like res.swap(ptr->data)
                return std::unique_ptr<T>(res);
            }
            // --internal_count only because external_count has been merged by
            // the succeeded thread
            ptr->release_ref();
        }
    }
};
