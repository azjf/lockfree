// https://github.com/anthonywilliams/ccia_code_samples/blob/main/listings/listing_7.13.cpp

#include <atomic>
#include <memory>

template <typename T>
class lock_free_stack
{
private:
    struct node;
    struct counted_node_ptr
    {
        int external_count;
        node *ptr;
    };
    struct node
    {
        std::shared_ptr<T> data;
        std::atomic<int> internal_count;
        counted_node_ptr next;
        node(T const &data_)
            : data(std::make_shared<T>(data_)), internal_count(0)
        {}
    };
    std::atomic<counted_node_ptr> head;
    void increase_head_count(counted_node_ptr &old_counter)
    {
        counted_node_ptr new_counter;
        do {
            new_counter = old_counter;
            ++new_counter.external_count;
        } while (!head.compare_exchange_strong(
            old_counter, new_counter,
            std::memory_order_acquire,  // head.ptr will be read when success
            std::memory_order_relaxed));
        old_counter.external_count = new_counter.external_count;
    }

public:
    ~lock_free_stack() { while (pop()); }
    void push(T const &data)
    {
        counted_node_ptr new_node;
        new_node.ptr = new node(data);
        new_node.external_count = 1;
        new_node.ptr->next = head.load(std::memory_order_relaxed);
        while (!head.compare_exchange_weak(
            new_node.ptr->next, new_node,
            std::memory_order_release,  // acquire-release payload is new_node
                                        // itself
            std::memory_order_relaxed));
    }
    std::shared_ptr<T> pop()
    {
        counted_node_ptr old_head = head.load(std::memory_order_relaxed);
        for (;;) {
            increase_head_count(old_head);  // acquire when success
            node *const ptr = old_head.ptr;
            if (!ptr) { return std::shared_ptr<T>(); }
            if (head.compare_exchange_strong(old_head, ptr->next,
                                             std::memory_order_relaxed)) {
                std::shared_ptr<T> res;
                res.swap(ptr->data);
                int const count_increase =
                    old_head.external_count -
                    2;  // 2 = 1 (current thread) + 1 (tail)
                if (ptr->internal_count.fetch_add(
                        count_increase,
                        std::memory_order_release)  // acquire-release payload
                                                    // is res.swap(ptr->data)
                    == -count_increase) {  // --interal_count in the else branch
                                           // may be executed before it
                    delete ptr;
                }
                return res;
            } else if (ptr->internal_count.fetch_add(
                           -1, std::memory_order_relaxed) == 1) {
                ptr->internal_count.load(
                    std::memory_order_acquire);  // ensure that ptr->data has
                                                 // already been swapped out
                delete ptr;
            }
        }
    }
};
